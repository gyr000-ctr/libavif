// Copyright 2025 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

// Container-level lossless remux of single-image AVIF files.
//
// A remux rewrites the ISOBMFF container (ftyp/meta and iloc/iprp/iinf/iref
// boxes) without decoding or re-encoding any AV1 payload. Every 'mdat' box of
// the input is copied to the output byte-for-byte; only clap/irot/imir/pasp
// properties, the ICC 'colr' box and Exif/XMP metadata items can change.

#include "avif/internal.h"

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The indices of the properties associated with an item. libavif writes 'ipma'
// with flags set to 0, so property indices are 7 bits wide.
#define REMUX_MAX_PROPERTY_INDEX ((1 << 7) - 1)

static const char remuxXmpContentType[] = AVIF_CONTENT_TYPE_XMP;
static const size_t remuxXmpContentTypeSize = sizeof(remuxXmpContentType);

// ---------------------------------------------------------------------------
// Internal data model
//
// All arrays below use the avifArray*() helpers (AVIF_ARRAY_DECLARE layout).

typedef struct avifRemuxExtent
{
    uint64_t offset; // Absolute file offset (construction_method 0).
    uint64_t size;
} avifRemuxExtent;
AVIF_ARRAY_DECLARE(avifRemuxExtentArray, avifRemuxExtent, extent);

typedef struct avifRemuxAssociation
{
    avifBool essential;
    uint16_t propertyIndex; // 1-based index into the ipco array, 0 = no property.
} avifRemuxAssociation;
AVIF_ARRAY_DECLARE(avifRemuxAssociationArray, avifRemuxAssociation, association);

typedef struct avifRemuxItem
{
    uint32_t id;
    uint8_t type[4];
    avifBool hidden; // infe flags bit 0.
    avifRemuxExtentArray extents;
    avifRemuxAssociationArray associations;
    avifBool constructionKnown;
    uint8_t constructionMethod; // iloc construction_method.
    avifBool infeParsed;
    avifBool isMime;
    char contentType[64]; // For "mime" items, NUL terminated.
    avifBool hasContentType;
} avifRemuxItem;
AVIF_ARRAY_DECLARE(avifRemuxItemArray, avifRemuxItem, item);

// A raw property box as found inside ipco: full box including its own header
// (size + type, and version/flags for full boxes).
typedef struct avifRemuxProperty
{
    uint8_t type[4];
    avifRWData raw;
} avifRemuxProperty;
AVIF_ARRAY_DECLARE(avifRemuxPropertyArray, avifRemuxProperty, property);

// One entry of an iref box.
typedef struct avifRemuxRef
{
    uint8_t type[4];
    uint32_t fromItemID;
    uint32_t toItemID;
} avifRemuxRef;
AVIF_ARRAY_DECLARE(avifRemuxRefArray, avifRemuxRef, ref);

// A verbatim copy of a top-level box other than ftyp/meta/mdat, or of a meta
// child box that this remux does not model.
typedef struct avifRemuxRawBox
{
    avifRWData raw;
} avifRemuxRawBox;
AVIF_ARRAY_DECLARE(avifRemuxRawBoxArray, avifRemuxRawBox, box);

// A verbatim copy of an mdat box. 'headerSize' is the number of bytes taken by
// the box header (8, 16 or 20 with largesize); 'payload' points inside the
// input buffer and is never modified.
typedef struct avifRemuxMdat
{
    const uint8_t * header;
    size_t headerSize;
    const uint8_t * payload;
    uint64_t payloadSize;
    uint64_t fileOffset; // Absolute offset of the box in the input.
    avifBool sizeZero;   // Header uses size==0 (box runs to EOF).
} avifRemuxMdat;
AVIF_ARRAY_DECLARE(avifRemuxMdatArray, avifRemuxMdat, mdat);

// Forward declarations of buildOutput() working-state types (full definitions
// appear later in the file). Kept in the context so every error path frees them.
typedef struct avifRemuxNewProperty
{
    avifRWData raw;
    uint8_t type[4];
} avifRemuxNewProperty;
AVIF_ARRAY_DECLARE(avifRemuxNewPropertyArray, avifRemuxNewProperty, property);

typedef struct IlocFixup
{
    size_t offsetField;    // Position of the extent_offset field in output.
    uint64_t targetOffset; // Final absolute offset of the payload.
    avifBool use64;
} IlocFixup;
AVIF_ARRAY_DECLARE(IlocFixupArray, IlocFixup, fixup);

typedef struct avifRemuxContext
{
    const avifROData * input;
    avifDiagnostics * diag;

    // Top-level layout.
    const uint8_t * ftyp; // Points inside input.
    size_t ftypSize;      // Total box size including header.
    avifBool hasFtyp;
    avifRemuxRawBoxArray otherTopLevelBoxes;
    avifRemuxMdatArray mdats;
    avifBool hasMoov;

    // Parsed meta contents.
    uint32_t primaryItemID;
    avifBool hasPrimaryItemID;
    avifRemuxItemArray items;
    avifRemuxPropertyArray properties;
    avifRemuxRefArray refs;
    avifRemuxRawBoxArray otherMetaBoxes; // Meta children copied verbatim.
    avifRWData hdlrBox;                  // hdlr copied verbatim if present (typically ~33 bytes).

    // New Exif/XMP payloads (with the 4-byte Exif TIFF header offset prefix for
    // Exif) that must be appended in a freshly written mdat.
    avifRWData newExifPayload;
    avifRWData newXmpPayload;

    // Owned working state of buildOutput(). Keeping it in the context ensures
    // every error path releases it via avifRemuxContextDestroy().
    avifRemuxNewPropertyArray newProperties;
    IlocFixupArray fixups;
    uint32_t * propertyMap;
    uint64_t * mdatOutputPayloadOffsets;
} avifRemuxContext;

static void avifRemuxContextDestroy(avifRemuxContext * ctx);

// ---------------------------------------------------------------------------
// Small parsing helpers

static avifBool memEquals4(const uint8_t * a, const char * b)
{
    return memcmp(a, b, 4) == 0;
}

static avifResult remuxDiagError(avifDiagnostics * diag, avifResult result, const char * format, ...)
{
    if (diag) {
        va_list args;
        va_start(args, format);
        // Avoid clobbering an already-set message.
        if (!*diag->error) {
            vsnprintf(diag->error, sizeof(diag->error), format, args);
        }
        va_end(args);
    }
    return result;
}

// Reads a UX8 value given a size in bytes (0, 4 or 8).
static avifBool readSized(avifROStream * s, uint8_t size, uint64_t * value)
{
    *value = 0;
    if (size == 0) {
        return AVIF_TRUE;
    }
    if (size == 4) {
        uint32_t v;
        AVIF_CHECK(avifROStreamReadU32(s, &v));
        *value = v;
        return AVIF_TRUE;
    }
    if (size == 8) {
        AVIF_CHECK(avifROStreamReadU64(s, value));
        return AVIF_TRUE;
    }
    return AVIF_FALSE;
}

// ---------------------------------------------------------------------------
// Item lookup / creation

static avifRemuxItem * findItem(avifRemuxContext * ctx, uint32_t id)
{
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        if (ctx->items.item[i].id == id) {
            return &ctx->items.item[i];
        }
    }
    return NULL;
}

static avifRemuxItem * getOrCreateItem(avifRemuxContext * ctx, uint32_t id)
{
    avifRemuxItem * item = findItem(ctx, id);
    if (item) {
        return item;
    }
    avifRemuxItem * created = (avifRemuxItem *)avifArrayPush(&ctx->items);
    if (!created) {
        return NULL;
    }
    memset(created, 0, sizeof(*created));
    created->id = id;
    if (!avifArrayCreate(&created->extents, sizeof(avifRemuxExtent), 2) ||
        !avifArrayCreate(&created->associations, sizeof(avifRemuxAssociation), 4)) {
        return NULL;
    }
    return created;
}

static void itemDestroy(avifRemuxItem * item)
{
    avifArrayDestroy(&item->extents);
    avifArrayDestroy(&item->associations);
}

// ---------------------------------------------------------------------------
// Meta child box parsers

static avifResult parsePitm(avifRemuxContext * ctx, const uint8_t * raw, size_t rawLen)
{
    avifROData rodata = { raw, rawLen };
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "pitm");
    uint8_t version;
    uint32_t flags;
    AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&s, &version, &flags), AVIF_RESULT_BMFF_PARSE_FAILED);
    uint32_t id;
    if (version == 0) {
        uint16_t id16;
        AVIF_CHECKERR(avifROStreamReadU16(&s, &id16), AVIF_RESULT_BMFF_PARSE_FAILED);
        id = id16;
    } else {
        AVIF_CHECKERR(avifROStreamReadU32(&s, &id), AVIF_RESULT_BMFF_PARSE_FAILED);
    }
    ctx->primaryItemID = id;
    ctx->hasPrimaryItemID = AVIF_TRUE;
    return AVIF_RESULT_OK;
}

static avifResult parseIloc(avifRemuxContext * ctx, const uint8_t * raw, size_t rawLen)
{
    avifROData rodata = { raw, rawLen };
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "iloc");
    uint8_t version;
    uint32_t flags;
    AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&s, &version, &flags), AVIF_RESULT_BMFF_PARSE_FAILED);
    if (version > 2) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iloc: unsupported version %u", version);
    }

    uint8_t offsetSize, lengthSize, baseOffsetSize, indexSize;
    uint8_t nibble;
    AVIF_CHECKERR(avifROStreamReadBitsU8(&s, &nibble, 8), AVIF_RESULT_BMFF_PARSE_FAILED);
    offsetSize = nibble >> 4;
    lengthSize = nibble & 0xF;
    AVIF_CHECKERR(avifROStreamReadBitsU8(&s, &nibble, 8), AVIF_RESULT_BMFF_PARSE_FAILED);
    baseOffsetSize = nibble >> 4;
    indexSize = nibble & 0xF;
    // Only 0/4/8 byte field widths are legal.
    if (offsetSize != 0 && offsetSize != 4 && offsetSize != 8) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iloc: invalid offset_size");
    }
    if (lengthSize != 4 && lengthSize != 8) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iloc: invalid length_size");
    }
    if (baseOffsetSize != 0 && baseOffsetSize != 4 && baseOffsetSize != 8) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iloc: invalid base_offset_size");
    }
    if (indexSize != 0 && indexSize != 4 && indexSize != 8) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iloc: invalid index_size");
    }

    uint32_t itemCount;
    if (version < 2) {
        uint16_t count16;
        AVIF_CHECKERR(avifROStreamReadU16(&s, &count16), AVIF_RESULT_BMFF_PARSE_FAILED);
        itemCount = count16;
    } else {
        AVIF_CHECKERR(avifROStreamReadU32(&s, &itemCount), AVIF_RESULT_BMFF_PARSE_FAILED);
    }

    for (uint32_t i = 0; i < itemCount; ++i) {
        uint32_t itemID;
        uint8_t constructionMethod = 0;
        if (version < 2) {
            uint16_t id16;
            AVIF_CHECKERR(avifROStreamReadU16(&s, &id16), AVIF_RESULT_BMFF_PARSE_FAILED);
            itemID = id16;
        } else {
            AVIF_CHECKERR(avifROStreamReadU32(&s, &itemID), AVIF_RESULT_BMFF_PARSE_FAILED);
        }
        if (version >= 1) {
            uint16_t reservedAndMethod;
            AVIF_CHECKERR(avifROStreamReadU16(&s, &reservedAndMethod), AVIF_RESULT_BMFF_PARSE_FAILED);
            constructionMethod = (uint8_t)(reservedAndMethod & 0xF);
        }
        uint16_t dataReferenceIndex;
        AVIF_CHECKERR(avifROStreamReadU16(&s, &dataReferenceIndex), AVIF_RESULT_BMFF_PARSE_FAILED);
        (void)dataReferenceIndex;
        uint64_t baseOffset = 0;
        AVIF_CHECKERR(readSized(&s, baseOffsetSize, &baseOffset), AVIF_RESULT_BMFF_PARSE_FAILED);

        uint16_t extentCount;
        AVIF_CHECKERR(avifROStreamReadU16(&s, &extentCount), AVIF_RESULT_BMFF_PARSE_FAILED);

        avifRemuxItem * item = getOrCreateItem(ctx, itemID);
        AVIF_CHECKERR(item != NULL, AVIF_RESULT_OUT_OF_MEMORY);
        item->constructionKnown = AVIF_TRUE;
        item->constructionMethod = constructionMethod;

        for (uint16_t e = 0; e < extentCount; ++e) {
            if (indexSize != 0) {
                uint64_t ignoredIndex;
                AVIF_CHECKERR(readSized(&s, indexSize, &ignoredIndex), AVIF_RESULT_BMFF_PARSE_FAILED);
            }
            uint64_t extentOffset;
            uint64_t extentLength;
            AVIF_CHECKERR(readSized(&s, offsetSize, &extentOffset), AVIF_RESULT_BMFF_PARSE_FAILED);
            AVIF_CHECKERR(readSized(&s, lengthSize, &extentLength), AVIF_RESULT_BMFF_PARSE_FAILED);

            avifRemuxExtent * extent = (avifRemuxExtent *)avifArrayPush(&item->extents);
            AVIF_CHECKERR(extent != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            // A 0-sized field means the value is 0.
            extent->offset = (offsetSize == 0) ? 0 : baseOffset + extentOffset;
            extent->size = (lengthSize == 0) ? 0 : extentLength;
        }
    }
    return AVIF_RESULT_OK;
}

static avifResult parseIinf(avifRemuxContext * ctx, const uint8_t * raw, size_t rawLen)
{
    avifROData rodata = { raw, rawLen };
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "iinf");
    uint8_t version;
    uint32_t flags;
    AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&s, &version, &flags), AVIF_RESULT_BMFF_PARSE_FAILED);

    if (version == 0) {
        uint16_t entryCount;
        AVIF_CHECKERR(avifROStreamReadU16(&s, &entryCount), AVIF_RESULT_BMFF_PARSE_FAILED);
    } else {
        uint32_t entryCount;
        AVIF_CHECKERR(avifROStreamReadU32(&s, &entryCount), AVIF_RESULT_BMFF_PARSE_FAILED);
    }

    while (avifROStreamHasBytesLeft(&s, 1)) {
        avifBoxHeader header;
        AVIF_CHECKERR(avifROStreamReadBoxHeader(&s, &header), AVIF_RESULT_BMFF_PARSE_FAILED);
        if (!memEquals4(header.type, "infe")) {
            AVIF_CHECKERR(avifROStreamSkip(&s, header.size), AVIF_RESULT_BMFF_PARSE_FAILED);
            continue;
        }
        const uint8_t * body = avifROStreamCurrent(&s);
        avifROData bodyData = { body, header.size };
        avifROStream bs;
        avifROStreamStart(&bs, &bodyData, ctx->diag, "infe");
        uint8_t infeVersion;
        uint32_t infeFlags;
        AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&bs, &infeVersion, &infeFlags), AVIF_RESULT_BMFF_PARSE_FAILED);
        uint32_t itemID;
        if (infeVersion < 2) {
            return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "infe: unsupported version %u", infeVersion);
        }
        if (infeVersion == 2) {
            uint16_t id16;
            AVIF_CHECKERR(avifROStreamReadU16(&bs, &id16), AVIF_RESULT_BMFF_PARSE_FAILED);
            itemID = id16;
            uint16_t protectionIndex;
            AVIF_CHECKERR(avifROStreamReadU16(&bs, &protectionIndex), AVIF_RESULT_BMFF_PARSE_FAILED);
        } else {
            AVIF_CHECKERR(avifROStreamReadU32(&bs, &itemID), AVIF_RESULT_BMFF_PARSE_FAILED);
            uint16_t protectionIndex;
            AVIF_CHECKERR(avifROStreamReadU16(&bs, &protectionIndex), AVIF_RESULT_BMFF_PARSE_FAILED);
        }
        uint8_t itemType[4];
        AVIF_CHECKERR(avifROStreamRead(&bs, itemType, 4), AVIF_RESULT_BMFF_PARSE_FAILED);
        // item_name (NUL terminated) - skip it.
        AVIF_CHECKERR(avifROStreamReadString(&bs, NULL, 0), AVIF_RESULT_BMFF_PARSE_FAILED);

        avifRemuxItem * item = getOrCreateItem(ctx, itemID);
        AVIF_CHECKERR(item != NULL, AVIF_RESULT_OUT_OF_MEMORY);
        item->infeParsed = AVIF_TRUE;
        memcpy(item->type, itemType, 4);
        item->hidden = (infeFlags & 1) ? AVIF_TRUE : AVIF_FALSE;
        item->isMime = memEquals4(itemType, "mime");
        if (item->isMime && avifROStreamHasBytesLeft(&bs, 1)) {
            // content_type (NUL terminated).
            AVIF_CHECKERR(avifROStreamReadString(&bs, item->contentType, sizeof(item->contentType)), AVIF_RESULT_BMFF_PARSE_FAILED);
            item->hasContentType = AVIF_TRUE;
        }
        AVIF_CHECKERR(avifROStreamSkip(&s, header.size), AVIF_RESULT_BMFF_PARSE_FAILED);
    }
    return AVIF_RESULT_OK;
}

static avifResult parseIprp(avifRemuxContext * ctx, const uint8_t * raw, size_t rawLen)
{
    avifROData rodata = { raw, rawLen };
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "iprp");
    avifBool foundIpco = AVIF_FALSE;

    while (avifROStreamHasBytesLeft(&s, 1)) {
        avifBoxHeader header;
        AVIF_CHECKERR(avifROStreamReadBoxHeader(&s, &header), AVIF_RESULT_BMFF_PARSE_FAILED);
        const uint8_t * body = avifROStreamCurrent(&s);

        if (memEquals4(header.type, "ipco")) {
            if (foundIpco) {
                return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iprp: multiple ipco boxes");
            }
            foundIpco = AVIF_TRUE;
            avifROData ipcoData = { body, header.size };
            avifROStream ps;
            avifROStreamStart(&ps, &ipcoData, ctx->diag, "ipco");
            while (avifROStreamHasBytesLeft(&ps, 1)) {
                const size_t propBoxStart = avifROStreamOffset(&ps);
                avifBoxHeader propHeader;
                AVIF_CHECKERR(avifROStreamReadBoxHeader(&ps, &propHeader), AVIF_RESULT_BMFF_PARSE_FAILED);
                const size_t totalSize = avifROStreamOffset(&ps) - propBoxStart + propHeader.size;
                // Sanity check: the box must fit in the remaining ipco payload.
                AVIF_CHECKERR(propBoxStart + totalSize <= header.size, AVIF_RESULT_BMFF_PARSE_FAILED);
                AVIF_CHECKERR(avifROStreamSkip(&ps, propHeader.size), AVIF_RESULT_BMFF_PARSE_FAILED);

                avifRemuxProperty * property = (avifRemuxProperty *)avifArrayPush(&ctx->properties);
                AVIF_CHECKERR(property != NULL, AVIF_RESULT_OUT_OF_MEMORY);
                memset(property, 0, sizeof(*property));
                memcpy(property->type, propHeader.type, 4);
                AVIF_CHECKRES(avifRWDataSet(&property->raw, body + propBoxStart, totalSize));
            }
        } else if (memEquals4(header.type, "ipma")) {
            avifROData ipmaData = { body, header.size };
            avifROStream as;
            avifROStreamStart(&as, &ipmaData, ctx->diag, "ipma");
            uint8_t ipmaVersion;
            uint32_t ipmaFlags;
            AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&as, &ipmaVersion, &ipmaFlags), AVIF_RESULT_BMFF_PARSE_FAILED);
            uint32_t entryCount;
            AVIF_CHECKERR(avifROStreamReadU32(&as, &entryCount), AVIF_RESULT_BMFF_PARSE_FAILED);
            const avifBool wideIndices = (ipmaFlags & 1) ? AVIF_TRUE : AVIF_FALSE;
            for (uint32_t e = 0; e < entryCount; ++e) {
                uint32_t itemID;
                if (ipmaVersion < 1) {
                    uint16_t id16;
                    AVIF_CHECKERR(avifROStreamReadU16(&as, &id16), AVIF_RESULT_BMFF_PARSE_FAILED);
                    itemID = id16;
                } else {
                    AVIF_CHECKERR(avifROStreamReadU32(&as, &itemID), AVIF_RESULT_BMFF_PARSE_FAILED);
                }
                avifRemuxItem * item = getOrCreateItem(ctx, itemID);
                AVIF_CHECKERR(item != NULL, AVIF_RESULT_OUT_OF_MEMORY);
                uint8_t associationCount;
                AVIF_CHECKERR(avifROStreamReadBitsU8(&as, &associationCount, 8), AVIF_RESULT_BMFF_PARSE_FAILED);
                for (uint8_t a = 0; a < associationCount; ++a) {
                    uint8_t essentialAndIndex;
                    AVIF_CHECKERR(avifROStreamReadBitsU8(&as, &essentialAndIndex, 8), AVIF_RESULT_BMFF_PARSE_FAILED);
                    avifBool essential = (essentialAndIndex & 0x80) ? AVIF_TRUE : AVIF_FALSE;
                    uint16_t propertyIndex = essentialAndIndex & 0x7F;
                    if (wideIndices) {
                        uint8_t second;
                        AVIF_CHECKERR(avifROStreamReadBitsU8(&as, &second, 8), AVIF_RESULT_BMFF_PARSE_FAILED);
                        propertyIndex = (uint16_t)(((uint16_t)(essentialAndIndex & 0x7F) << 8) | second);
                    }
                    // libavif writes ipma with flags 0 (7-bit indices); validate
                    // that the index is in range and representable on output.
                    // Index 0 is the special "no property" value.
                    if (propertyIndex != 0 && (propertyIndex > ctx->properties.count || propertyIndex > REMUX_MAX_PROPERTY_INDEX)) {
                        return remuxDiagError(ctx->diag,
                                              AVIF_RESULT_BMFF_PARSE_FAILED,
                                              "ipma: property index %u out of range (ipco has %u properties)",
                                              propertyIndex,
                                              ctx->properties.count);
                    }
                    avifRemuxAssociation * association = (avifRemuxAssociation *)avifArrayPush(&item->associations);
                    AVIF_CHECKERR(association != NULL, AVIF_RESULT_OUT_OF_MEMORY);
                    association->essential = essential;
                    association->propertyIndex = propertyIndex;
                }
            }
        } else {
            // The only defined children of iprp are ipco and ipma. Anything
            // else cannot be repositioned safely at the meta level, so reject
            // the file rather than emitting a malformed container.
            return remuxDiagError(ctx->diag,
                                  AVIF_RESULT_BMFF_PARSE_FAILED,
                                  "iprp: unexpected child box '%c%c%c%c'",
                                  header.type[0],
                                  header.type[1],
                                  header.type[2],
                                  header.type[3]);
        }
        AVIF_CHECKERR(avifROStreamSkip(&s, header.size), AVIF_RESULT_BMFF_PARSE_FAILED);
    }

    if (!foundIpco) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iprp: missing ipco box");
    }
    return AVIF_RESULT_OK;
}

static avifResult parseIref(avifRemuxContext * ctx, const uint8_t * raw, size_t rawLen)
{
    avifROData rodata = { raw, rawLen };
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "iref");
    uint8_t version;
    uint32_t flags;
    AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&s, &version, &flags), AVIF_RESULT_BMFF_PARSE_FAILED);
    const avifBool wideIDs = (version >= 1);

    while (avifROStreamHasBytesLeft(&s, 1)) {
        avifBoxHeader header;
        AVIF_CHECKERR(avifROStreamReadBoxHeader(&s, &header), AVIF_RESULT_BMFF_PARSE_FAILED);
        avifROData refData = { avifROStreamCurrent(&s), header.size };
        avifROStream rs;
        avifROStreamStart(&rs, &refData, ctx->diag, "iref entry");
        uint32_t fromID;
        if (wideIDs) {
            AVIF_CHECKERR(avifROStreamReadU32(&rs, &fromID), AVIF_RESULT_BMFF_PARSE_FAILED);
        } else {
            uint16_t v;
            AVIF_CHECKERR(avifROStreamReadU16(&rs, &v), AVIF_RESULT_BMFF_PARSE_FAILED);
            fromID = v;
        }
        uint16_t referenceCount;
        AVIF_CHECKERR(avifROStreamReadU16(&rs, &referenceCount), AVIF_RESULT_BMFF_PARSE_FAILED);
        for (uint16_t r = 0; r < referenceCount; ++r) {
            uint32_t toID;
            if (wideIDs) {
                AVIF_CHECKERR(avifROStreamReadU32(&rs, &toID), AVIF_RESULT_BMFF_PARSE_FAILED);
            } else {
                uint16_t v;
                AVIF_CHECKERR(avifROStreamReadU16(&rs, &v), AVIF_RESULT_BMFF_PARSE_FAILED);
                toID = v;
            }
            avifRemuxRef * ref = (avifRemuxRef *)avifArrayPush(&ctx->refs);
            AVIF_CHECKERR(ref != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            memcpy(ref->type, header.type, 4);
            ref->fromItemID = fromID;
            ref->toItemID = toID;
        }
        AVIF_CHECKERR(avifROStreamSkip(&s, header.size), AVIF_RESULT_BMFF_PARSE_FAILED);
    }
    return AVIF_RESULT_OK;
}

// Walks the children of a meta box.
static avifResult parseMetaBox(avifRemuxContext * ctx, const uint8_t * raw, size_t rawLen)
{
    avifROData rodata = { raw, rawLen };
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "meta");
    // meta is a full box.
    AVIF_CHECKERR(avifROStreamReadVersionAndFlags(&s, NULL, NULL), AVIF_RESULT_BMFF_PARSE_FAILED);

    while (avifROStreamHasBytesLeft(&s, 1)) {
        avifBoxHeader header;
        AVIF_CHECKERR(avifROStreamReadBoxHeader(&s, &header), AVIF_RESULT_BMFF_PARSE_FAILED);
        const uint8_t * body = avifROStreamCurrent(&s);
        const size_t totalSize = 8 + header.size;

        avifResult result = AVIF_RESULT_OK;
        if (memEquals4(header.type, "hdlr")) {
            AVIF_CHECKRES(avifRWDataSet(&ctx->hdlrBox, body - 8, totalSize));
        } else if (memEquals4(header.type, "pitm")) {
            result = parsePitm(ctx, body, header.size);
        } else if (memEquals4(header.type, "iloc")) {
            result = parseIloc(ctx, body, header.size);
        } else if (memEquals4(header.type, "iinf")) {
            result = parseIinf(ctx, body, header.size);
        } else if (memEquals4(header.type, "iprp")) {
            result = parseIprp(ctx, body, header.size);
        } else if (memEquals4(header.type, "iref")) {
            result = parseIref(ctx, body, header.size);
        } else if (memEquals4(header.type, "idat")) {
            // idat payloads use iloc construction_method 1, which is not supported.
            return remuxDiagError(ctx->diag,
                                  AVIF_RESULT_REMUX_UNSUPPORTED_CONSTRUCTION,
                                  "idat box present: only file-offset (mdat) construction is supported");
        } else {
            // Unknown/unmodeled meta child (grpl, auxl, ...): keep it verbatim.
            avifRemuxRawBox * rawBox = (avifRemuxRawBox *)avifArrayPush(&ctx->otherMetaBoxes);
            AVIF_CHECKERR(rawBox != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            memset(rawBox, 0, sizeof(*rawBox));
            AVIF_CHECKRES(avifRWDataSet(&rawBox->raw, body - 8, totalSize));
        }
        AVIF_CHECKRES(result);
        AVIF_CHECKERR(avifROStreamSkip(&s, header.size), AVIF_RESULT_BMFF_PARSE_FAILED);
    }
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Top-level box walk

static avifResult walkTopLevelBoxes(avifRemuxContext * ctx)
{
    avifROData rodata = *ctx->input;
    avifROStream s;
    avifROStreamStart(&s, &rodata, ctx->diag, "file");
    avifBool sawMeta = AVIF_FALSE;

    while (avifROStreamHasBytesLeft(&s, 1)) {
        const size_t boxOffset = avifROStreamOffset(&s);
        avifBoxHeader header;
        AVIF_CHECKERR(avifROStreamReadBoxHeaderPartial(&s, &header, /*topLevel=*/AVIF_TRUE), AVIF_RESULT_BMFF_PARSE_FAILED);
        size_t headerBytes = avifROStreamOffset(&s) - boxOffset;

        uint64_t payloadSize64 = header.size;
        if (header.isSizeZeroBox) {
            payloadSize64 = (uint64_t)avifROStreamRemainingBytes(&s);
        }
        if (payloadSize64 > SIZE_MAX) {
            return remuxDiagError(ctx->diag, AVIF_RESULT_NOT_IMPLEMENTED, "box too large");
        }
        size_t payloadSize = (size_t)payloadSize64;
        if (!avifROStreamHasBytesLeft(&s, payloadSize)) {
            return remuxDiagError(ctx->diag,
                                  AVIF_RESULT_TRUNCATED_DATA,
                                  "truncated top-level box '%c%c%c%c'",
                                  header.type[0],
                                  header.type[1],
                                  header.type[2],
                                  header.type[3]);
        }
        const uint8_t * boxStart = ctx->input->data + boxOffset;
        const uint8_t * payload = avifROStreamCurrent(&s);

        if (memEquals4(header.type, "ftyp")) {
            ctx->ftyp = boxStart;
            ctx->ftypSize = headerBytes + payloadSize;
            ctx->hasFtyp = AVIF_TRUE;
        } else if (memEquals4(header.type, "meta")) {
            if (sawMeta) {
                return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "multiple meta boxes");
            }
            sawMeta = AVIF_TRUE;
            AVIF_CHECKRES(parseMetaBox(ctx, payload, payloadSize));
        } else if (memEquals4(header.type, "moov") || memEquals4(header.type, "mini")) {
            ctx->hasMoov = AVIF_TRUE;
        } else if (memEquals4(header.type, "mdat")) {
            avifRemuxMdat * mdat = (avifRemuxMdat *)avifArrayPush(&ctx->mdats);
            AVIF_CHECKERR(mdat != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            memset(mdat, 0, sizeof(*mdat));
            mdat->header = boxStart;
            mdat->headerSize = headerBytes;
            mdat->payload = payload;
            mdat->payloadSize = payloadSize64;
            mdat->fileOffset = boxOffset;
            mdat->sizeZero = header.isSizeZeroBox;
        } else {
            avifRemuxRawBox * rawBox = (avifRemuxRawBox *)avifArrayPush(&ctx->otherTopLevelBoxes);
            AVIF_CHECKERR(rawBox != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            memset(rawBox, 0, sizeof(*rawBox));
            AVIF_CHECKRES(avifRWDataSet(&rawBox->raw, boxStart, headerBytes + payloadSize));
        }

        AVIF_CHECKERR(avifROStreamSkip(&s, payloadSize), AVIF_RESULT_BMFF_PARSE_FAILED);
    }

    if (!ctx->hasFtyp) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_INVALID_FTYP, "missing ftyp box");
    }
    if (!sawMeta) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "missing meta box");
    }
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Validation (dedicated error codes)

static avifResult validateItems(avifRemuxContext * ctx)
{
    // Sequence files are rejected up front.
    if (ctx->hasMoov) {
        return remuxDiagError(ctx->diag, AVIF_RESULT_REMUX_SEQUENCE_NOT_SUPPORTED, "image sequences (movie/track-based files) cannot be remuxed");
    }

    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        const avifRemuxItem * item = &ctx->items.item[i];
        if (!item->infeParsed) {
            return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "iloc references item ID %u without an infe entry", item->id);
        }
        // construction_method: 0 = file offset. Anything else cannot be remuxed.
        if (item->constructionKnown && item->constructionMethod != 0) {
            return remuxDiagError(ctx->diag,
                                  AVIF_RESULT_REMUX_UNSUPPORTED_CONSTRUCTION,
                                  "item ID %u uses iloc construction_method %u",
                                  item->id,
                                  item->constructionMethod);
        }
        if (item->extents.count == 0) {
            return remuxDiagError(ctx->diag, AVIF_RESULT_ITEM_MISSING_EXTENT, "item ID %u has zero extents", item->id);
        }
        if (memEquals4(item->type, "grid")) {
            return remuxDiagError(ctx->diag, AVIF_RESULT_REMUX_GRID_NOT_SUPPORTED, "grid image item ID %u cannot be remuxed", item->id);
        }
    }
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Output construction

static avifResult writeRaw(avifRWStream * s, const avifRWData * data)
{
    return avifRWStreamWrite(s, data->data, data->size);
}

// Writes a minimal hdlr box with the given handler type (e.g. "pict").
static avifResult writeHandlerBox(avifRWStream * s, const char handlerType[4])
{
    avifBoxMarker hdlr;
    AVIF_CHECKRES(avifRWStreamWriteFullBox(s, "hdlr", AVIF_BOX_SIZE_TBD, 0, 0, &hdlr));
    AVIF_CHECKRES(avifRWStreamWriteU32(s, 0));                // pre_defined
    AVIF_CHECKRES(avifRWStreamWriteChars(s, handlerType, 4)); // handler_type
    AVIF_CHECKRES(avifRWStreamWriteZeros(s, 12));             // reserved[3]
    AVIF_CHECKRES(avifRWStreamWriteChars(s, "", 1));          // name (NUL terminator)
    AVIF_CHECKRES(avifRWStreamFinishBox(s, hdlr));
    return AVIF_RESULT_OK;
}

// Builds a clap/irot/imir/pasp property box in 'out' (a complete box including
// its header).
static avifResult buildTransformProperty(avifRWData * out, const avifRemuxer * remuxer, const char * type)
{
    avifRWStream s;
    avifRWStreamStart(&s, out);
    if (!strcmp(type, "clap")) {
        avifBoxMarker marker;
        AVIF_CHECKRES(avifRWStreamWriteBox(&s, "clap", AVIF_BOX_SIZE_TBD, &marker));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.widthN));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.widthD));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.heightN));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.heightD));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.horizOffN));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.horizOffD));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.vertOffN));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->clap.vertOffD));
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, marker));
    } else if (!strcmp(type, "irot")) {
        avifBoxMarker marker;
        AVIF_CHECKRES(avifRWStreamWriteBox(&s, "irot", AVIF_BOX_SIZE_TBD, &marker));
        AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 6));
        AVIF_CHECKRES(avifRWStreamWriteBits(&s, remuxer->irot.angle & 0x3, 2));
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, marker));
    } else if (!strcmp(type, "imir")) {
        avifBoxMarker marker;
        AVIF_CHECKRES(avifRWStreamWriteBox(&s, "imir", AVIF_BOX_SIZE_TBD, &marker));
        AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 7));
        AVIF_CHECKRES(avifRWStreamWriteBits(&s, remuxer->imir.axis ? 1 : 0, 1));
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, marker));
    } else {
        avifBoxMarker marker;
        AVIF_CHECKRES(avifRWStreamWriteBox(&s, "pasp", AVIF_BOX_SIZE_TBD, &marker));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->pasp.hSpacing));
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, remuxer->pasp.vSpacing));
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, marker));
    }
    avifRWStreamFinishWrite(&s);
    return AVIF_RESULT_OK;
}

static avifResult buildICCProperty(avifRWData * out, const avifRWData * icc)
{
    avifRWStream s;
    avifRWStreamStart(&s, out);
    avifBoxMarker marker;
    AVIF_CHECKRES(avifRWStreamWriteBox(&s, "colr", AVIF_BOX_SIZE_TBD, &marker));
    AVIF_CHECKRES(avifRWStreamWriteChars(&s, "prof", 4));
    AVIF_CHECKRES(avifRWStreamWrite(&s, icc->data, icc->size));
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, marker));
    avifRWStreamFinishWrite(&s);
    return AVIF_RESULT_OK;
}

// Returns AVIF_TRUE if a property fourcc is a transformative property handled
// by the remuxer.
static avifBool isTransformPropertyType(const uint8_t type[4])
{
    return memEquals4(type, "clap") || memEquals4(type, "irot") || memEquals4(type, "imir") || memEquals4(type, "pasp");
}

// Whether an item is a coded image item whose payload is an AV1 OBU sequence
// ("av01", or experimental "av02"). Transformative/colour properties may only
// be associated with such items, never with derived items ("grid", "tmap",
// "sato", ...).
static avifBool isCodedImageItem(const avifRemuxItem * item)
{
    return memEquals4(item->type, "av01") || memEquals4(item->type, "av02");
}

// Finds an existing item by metadata kind.
static avifRemuxItem * findMetadataItem(avifRemuxContext * ctx, avifBool exif)
{
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        avifRemuxItem * item = &ctx->items.item[i];
        if (exif) {
            if (memEquals4(item->type, "Exif")) {
                return item;
            }
        } else if (item->isMime && item->hasContentType && strcmp(item->contentType, AVIF_CONTENT_TYPE_XMP) == 0) {
            return item;
        }
    }
    return NULL;
}

// Decides which items are removed by the requested metadata actions. A REPLACE
// action keeps the item (its extent is redirected to the new payload).
static avifBool itemIsRemoved(const avifRemuxer * remuxer, const avifRemuxItem * item)
{
    if (memEquals4(item->type, "Exif")) {
        return remuxer->exifAction == AVIF_REMUX_METADATA_REMOVE;
    }
    if (item->isMime && item->hasContentType && strcmp(item->contentType, AVIF_CONTENT_TYPE_XMP) == 0) {
        return remuxer->xmpAction == AVIF_REMUX_METADATA_REMOVE;
    }
    return AVIF_FALSE;
}

// Returns AVIF_TRUE if the reference involves an item that is removed by the
// requested metadata actions.
static avifBool refIsDead(const avifRemuxContext * ctx, const avifRemuxer * remuxer, const avifRemuxRef * ref)
{
    for (uint32_t k = 0; k < ctx->items.count; ++k) {
        const avifRemuxItem * it = &ctx->items.item[k];
        if (!itemIsRemoved(remuxer, it)) {
            continue;
        }
        if (it->id == ref->fromItemID || it->id == ref->toItemID) {
            return AVIF_TRUE;
        }
    }
    return AVIF_FALSE;
}

// Writes the whole output file. 'newMetaSize' and the appended mdat placement
// are computed from the iloc offsets.
static avifResult buildOutput(avifRemuxContext * ctx, const avifRemuxer * remuxer, avifRWData * output)
{
    // ---- Determine removed/new metadata items and new item IDs -------------
    avifBool removeExif = (remuxer->exifAction != AVIF_REMUX_METADATA_KEEP);
    avifBool removeXmp = (remuxer->xmpAction != AVIF_REMUX_METADATA_KEEP);
    avifRemuxItem * oldExif = findMetadataItem(ctx, AVIF_TRUE);
    avifRemuxItem * oldXmp = findMetadataItem(ctx, AVIF_FALSE);
    avifBool addExif = removeExif && (remuxer->exifAction == AVIF_REMUX_METADATA_REPLACE) && remuxer->exif.size > 0;
    avifBool addXmp = removeXmp && (remuxer->xmpAction == AVIF_REMUX_METADATA_REPLACE) && remuxer->xmp.size > 0;
    // If an item already exists and the action is REPLACE, keep the item and
    // point its extent at the new mdat payload.
    avifBool replaceExif = oldExif && addExif;
    avifBool replaceXmp = oldXmp && addXmp;
    avifBool createExif = addExif && !oldExif;
    avifBool createXmp = addXmp && !oldXmp;

    // Validate replacement Exif payloads up front.
    if (addExif) {
        size_t tiffOffset;
        AVIF_CHECKRES(avifGetExifTiffHeaderOffset(remuxer->exif.data, remuxer->exif.size, &tiffOffset));
        AVIF_CHECKERR(tiffOffset <= UINT32_MAX, AVIF_RESULT_INVALID_EXIF_PAYLOAD);
        const uint32_t tiffOffsetBE = avifHTONL((uint32_t)tiffOffset);
        AVIF_CHECKRES(avifRWDataRealloc(&ctx->newExifPayload, 4 + remuxer->exif.size));
        memcpy(ctx->newExifPayload.data, &tiffOffsetBE, 4);
        memcpy(ctx->newExifPayload.data + 4, remuxer->exif.data, remuxer->exif.size);
    }
    if (addXmp) {
        AVIF_CHECKRES(avifRWDataSet(&ctx->newXmpPayload, remuxer->xmp.data, remuxer->xmp.size));
    }

    // ---- Build the new ipco and the old-index -> new-index map -------------
    // Map is indexed by old 1-based property index; 0 means dropped/absent.
    // Owned by ctx so error paths free it via avifRemuxContextDestroy().
    ctx->propertyMap = (uint32_t *)avifCalloc(ctx->properties.count + 1, sizeof(uint32_t));
    AVIF_CHECKERR(ctx->propertyMap != NULL, AVIF_RESULT_OUT_OF_MEMORY);
    uint32_t * propertyMap = ctx->propertyMap;

    avifRemuxNewPropertyArray * newProperties = &ctx->newProperties;
    AVIF_CHECKERR(avifArrayCreate(newProperties, sizeof(avifRemuxNewProperty), ctx->properties.count + 8), AVIF_RESULT_OUT_OF_MEMORY);

    const avifBool changeIcc = (remuxer->iccAction != AVIF_REMUX_METADATA_KEEP);
    // Whether the input contained any ICC-type colr property. When replacing
    // ICC on a file that had none, the new colr is associated with every image
    // item (mirroring avifenc), rather than being left as an orphan property.
    avifBool anyIccColrExisted = AVIF_FALSE;

    for (uint32_t p = 0; p < ctx->properties.count; ++p) {
        const avifRemuxProperty * property = &ctx->properties.property[p];
        avifBool drop = AVIF_FALSE;
        if (isTransformPropertyType(property->type)) {
            const avifTransformFlags flag = memEquals4(property->type, "clap")   ? AVIF_TRANSFORM_CLAP
                                            : memEquals4(property->type, "irot") ? AVIF_TRANSFORM_IROT
                                            : memEquals4(property->type, "imir") ? AVIF_TRANSFORM_IMIR
                                                                                 : AVIF_TRANSFORM_PASP;
            drop = (remuxer->transformFlags & flag) ? AVIF_TRUE : AVIF_FALSE;
        } else if (memEquals4(property->type, "colr")) {
            if (property->raw.size >= 12 &&
                (memcmp(property->raw.data + 8, "prof", 4) == 0 || memcmp(property->raw.data + 8, "rICC", 4) == 0)) {
                anyIccColrExisted = AVIF_TRUE;
                if (changeIcc) {
                    drop = AVIF_TRUE;
                }
            }
        }
        if (drop) {
            propertyMap[p + 1] = 0;
            continue;
        }
        avifRemuxNewProperty * np = (avifRemuxNewProperty *)avifArrayPush(newProperties);
        AVIF_CHECKERR(np != NULL, AVIF_RESULT_OUT_OF_MEMORY);
        memset(np, 0, sizeof(*np));
        memcpy(np->type, property->type, 4);
        AVIF_CHECKRES(avifRWDataSet(&np->raw, property->raw.data, property->raw.size));
        propertyMap[p + 1] = newProperties->count;
    }

    // Append replacement/addition transformative properties, in encoder order.
    // Records the 1-based ipco index of every property newly added (as opposed
    // to copied from the input), so ipma entries can be extended below.
    uint32_t addedPropertyIndices[8];
    uint32_t addedPropertyCount = 0;
    avifBool addedPropertyEssential[8];
    // For colr (ICC) additions: only items that previously had an ICC colr get
    // associated. Transformative properties apply to every image item.
    avifBool addedPropertyIsColr[8];
    const struct
    {
        const char * type;
        avifTransformFlags flag;
        avifBool essential;
    } transformOrder[] = { { "pasp", AVIF_TRANSFORM_PASP, AVIF_FALSE },
                           { "clap", AVIF_TRANSFORM_CLAP, AVIF_TRUE },
                           { "irot", AVIF_TRANSFORM_IROT, AVIF_TRUE },
                           { "imir", AVIF_TRANSFORM_IMIR, AVIF_TRUE } };
    for (size_t t = 0; t < sizeof(transformOrder) / sizeof(transformOrder[0]); ++t) {
        if (!(remuxer->transformFlags & transformOrder[t].flag)) {
            continue;
        }
        avifRemuxNewProperty * np = (avifRemuxNewProperty *)avifArrayPush(newProperties);
        AVIF_CHECKERR(np != NULL, AVIF_RESULT_OUT_OF_MEMORY);
        memset(np, 0, sizeof(*np));
        memcpy(np->type, transformOrder[t].type, 4);
        AVIF_CHECKRES(buildTransformProperty(&np->raw, remuxer, transformOrder[t].type));
        AVIF_CHECKERR(addedPropertyCount < 8, AVIF_RESULT_NOT_IMPLEMENTED);
        addedPropertyIndices[addedPropertyCount] = newProperties->count;
        addedPropertyEssential[addedPropertyCount] = transformOrder[t].essential;
        addedPropertyIsColr[addedPropertyCount] = AVIF_FALSE;
        ++addedPropertyCount;
    }
    // Replacement ICC profile.
    if (changeIcc && remuxer->iccAction == AVIF_REMUX_METADATA_REPLACE && remuxer->icc.size > 0) {
        avifRemuxNewProperty * np = (avifRemuxNewProperty *)avifArrayPush(newProperties);
        AVIF_CHECKERR(np != NULL, AVIF_RESULT_OUT_OF_MEMORY);
        memset(np, 0, sizeof(*np));
        memcpy(np->type, "colr", 4);
        AVIF_CHECKRES(buildICCProperty(&np->raw, &remuxer->icc));
        AVIF_CHECKERR(addedPropertyCount < 8, AVIF_RESULT_NOT_IMPLEMENTED);
        addedPropertyIndices[addedPropertyCount] = newProperties->count;
        addedPropertyEssential[addedPropertyCount] = AVIF_FALSE;
        addedPropertyIsColr[addedPropertyCount] = AVIF_TRUE;
        ++addedPropertyCount;
    }
    AVIF_CHECKERR(newProperties->count <= REMUX_MAX_PROPERTY_INDEX, AVIF_RESULT_NOT_IMPLEMENTED);

    // ---- Determine surviving items and new metadata item IDs ---------------
    uint32_t maxItemID = 0;
    avifBool largeIDs = AVIF_FALSE;
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        maxItemID = AVIF_MAX(maxItemID, ctx->items.item[i].id);
    }
    uint32_t nextItemID = maxItemID + 1;
    uint32_t newExifID = 0;
    uint32_t newXmpID = 0;
    if (createExif) {
        newExifID = nextItemID++;
    }
    if (createXmp) {
        newXmpID = nextItemID++;
    }
    uint32_t totalItemCount = ctx->items.count + (createExif ? 1 : 0) + (createXmp ? 1 : 0);
    AVIF_CHECKERR(totalItemCount >= ctx->items.count, AVIF_RESULT_NOT_IMPLEMENTED);
    // A new item needs a fresh, non-zero ID.
    AVIF_CHECKERR(!((createExif || createXmp) && maxItemID == UINT32_MAX), AVIF_RESULT_NOT_IMPLEMENTED);
    largeIDs = (nextItemID - 1 > UINT16_MAX) || (totalItemCount > UINT16_MAX);

    // ---- First pass: write everything except iloc extent offsets -----------
    // The output layout is:
    //   ftyp (verbatim) | new meta | old other top-level boxes | old mdats
    //   (verbatim) | optional new mdat.
    // iloc offsets are patched once final positions are known.

    avifRWStream s;
    avifRWStreamStart(&s, output);

    AVIF_CHECKRES(avifRWStreamWrite(&s, ctx->ftyp, ctx->ftypSize));

    // iloc fixups: one per extent of every surviving item. Owned by ctx so any
    // error path releases them via avifRemuxContextDestroy().
    IlocFixupArray * fixups = &ctx->fixups;
    AVIF_CHECKERR(avifArrayCreate(fixups, sizeof(IlocFixup), 16), AVIF_RESULT_OUT_OF_MEMORY);

    // Track the output offset at which each input region starts, for shifting
    // the verbatim iloc extent offsets.

    // ---- New meta box -------------------------------------------------------
    avifBoxMarker metaMarker;
    AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "meta", AVIF_BOX_SIZE_TBD, 0, 0, &metaMarker));
    if (ctx->hdlrBox.size > 0) {
        AVIF_CHECKRES(writeRaw(&s, &ctx->hdlrBox));
    } else {
        AVIF_CHECKRES(writeHandlerBox(&s, "pict"));
    }

    if (ctx->hasPrimaryItemID) {
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "pitm", sizeof(uint32_t), 1, 0, NULL));
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, ctx->primaryItemID));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "pitm", sizeof(uint16_t), 0, 0, NULL));
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)ctx->primaryItemID));
        }
    }

    // Decide iloc field widths from the final payload layout. 64-bit offsets
    // are required if any extent lives past 4 GiB or uses an 8-byte length.
    avifBool use64Offsets = AVIF_FALSE;
    uint64_t newMdatPayloadSize = (addExif ? ctx->newExifPayload.size : 0) + (addXmp ? ctx->newXmpPayload.size : 0);
    // Estimate the output size upper bound to know whether 64-bit is needed.
    for (uint32_t m = 0; m < ctx->mdats.count; ++m) {
        const avifRemuxMdat * mdat = &ctx->mdats.mdat[m];
        if (mdat->fileOffset + mdat->payloadSize > UINT32_MAX) {
            use64Offsets = AVIF_TRUE;
        }
    }
    if (newMdatPayloadSize > UINT32_MAX) {
        use64Offsets = AVIF_TRUE;
    }
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        const avifRemuxItem * item = &ctx->items.item[i];
        for (uint32_t e = 0; e < item->extents.count; ++e) {
            if (item->extents.extent[e].size > UINT32_MAX || item->extents.extent[e].offset > UINT32_MAX) {
                use64Offsets = AVIF_TRUE;
            }
        }
    }

    // ---- iloc ---------------------------------------------------------------
    avifBoxMarker ilocMarker;
    AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "iloc", AVIF_BOX_SIZE_TBD, largeIDs ? 2 : 0, 0, &ilocMarker));
    AVIF_CHECKRES(avifRWStreamWriteBits(&s, use64Offsets ? 8 : 4, 4)); // offset_size
    AVIF_CHECKRES(avifRWStreamWriteBits(&s, use64Offsets ? 8 : 4, 4)); // length_size
    AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 4));                    // base_offset_size
    AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 4));                    // index_size/reserved

    // Count surviving items (+ newly created metadata items).
    uint32_t survivingItemCount = 0;
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        if (!itemIsRemoved(remuxer, &ctx->items.item[i])) {
            ++survivingItemCount;
        }
    }
    survivingItemCount += (createExif ? 1 : 0) + (createXmp ? 1 : 0);
    if (largeIDs) {
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, survivingItemCount));
    } else {
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)survivingItemCount));
    }

    // Helper to record an offset fixup and write the placeholder/length.
    // iloc entries for existing items.
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        avifRemuxItem * item = &ctx->items.item[i];
        if (itemIsRemoved(remuxer, item)) {
            continue;
        }
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, item->id));
            AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 12)); // reserved
            AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 4));  // construction_method = 0
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)item->id));
        }
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, 0)); // data_reference_index

        // For replaced Exif/XMP items, emit a single extent into the new mdat.
        avifBool replaced = (replaceExif && item == oldExif) || (replaceXmp && item == oldXmp);
        if (replaced) {
            const avifRWData * payload = (item == oldExif) ? &ctx->newExifPayload : &ctx->newXmpPayload;
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, 1)); // extent_count
            IlocFixup * fixup = (IlocFixup *)avifArrayPush(fixups);
            AVIF_CHECKERR(fixup != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            fixup->offsetField = avifRWStreamOffset(&s);
            fixup->use64 = use64Offsets;
            fixup->targetOffset = 0; // Patched after new mdat position is known.
            if (use64Offsets) {
                AVIF_CHECKRES(avifRWStreamWriteU64(&s, 0));
                AVIF_CHECKRES(avifRWStreamWriteU64(&s, payload->size));
            } else {
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, 0));
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, (uint32_t)payload->size));
            }
            continue;
        }

        AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)item->extents.count));
        for (uint32_t e = 0; e < item->extents.count; ++e) {
            const avifRemuxExtent * extent = &item->extents.extent[e];
            IlocFixup * fixup = (IlocFixup *)avifArrayPush(fixups);
            AVIF_CHECKERR(fixup != NULL, AVIF_RESULT_OUT_OF_MEMORY);
            fixup->offsetField = avifRWStreamOffset(&s);
            fixup->use64 = use64Offsets;
            fixup->targetOffset = extent->offset; // Shifted by the meta-size delta.
            if (use64Offsets) {
                AVIF_CHECKRES(avifRWStreamWriteU64(&s, 0));
                AVIF_CHECKRES(avifRWStreamWriteU64(&s, extent->size));
            } else {
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, 0));
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, (uint32_t)extent->size));
            }
        }
    }
    // Entries for newly created Exif/XMP items.
    const struct
    {
        uint32_t id;
        const avifRWData * payload;
    } newEntries[2] = { { newExifID, &ctx->newExifPayload }, { newXmpID, &ctx->newXmpPayload } };
    for (int n = 0; n < 2; ++n) {
        if (newEntries[n].id == 0) {
            continue;
        }
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, newEntries[n].id));
            AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 12));
            AVIF_CHECKRES(avifRWStreamWriteBits(&s, 0, 4));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)newEntries[n].id));
        }
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, 0));
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, 1));
        IlocFixup * fixup = (IlocFixup *)avifArrayPush(fixups);
        AVIF_CHECKERR(fixup != NULL, AVIF_RESULT_OUT_OF_MEMORY);
        fixup->offsetField = avifRWStreamOffset(&s);
        fixup->use64 = use64Offsets;
        fixup->targetOffset = 0;
        if (use64Offsets) {
            AVIF_CHECKRES(avifRWStreamWriteU64(&s, 0));
            AVIF_CHECKRES(avifRWStreamWriteU64(&s, newEntries[n].payload->size));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, 0));
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, (uint32_t)newEntries[n].payload->size));
        }
    }
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, ilocMarker));

    // ---- iinf ---------------------------------------------------------------
    avifBoxMarker iinfMarker;
    AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "iinf", AVIF_BOX_SIZE_TBD, largeIDs ? 1 : 0, 0, &iinfMarker));
    if (largeIDs) {
        AVIF_CHECKRES(avifRWStreamWriteU32(&s, survivingItemCount));
    } else {
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)survivingItemCount));
    }
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        const avifRemuxItem * item = &ctx->items.item[i];
        if (itemIsRemoved(remuxer, item)) {
            continue;
        }
        avifBoxMarker infeMarker;
        AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "infe", AVIF_BOX_SIZE_TBD, largeIDs ? 3 : 2, item->hidden ? 1 : 0, &infeMarker));
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, item->id));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)item->id));
        }
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, 0)); // item_protection_index
        AVIF_CHECKRES(avifRWStreamWrite(&s, item->type, 4));
        if (memEquals4(item->type, "Exif")) {
            AVIF_CHECKRES(avifRWStreamWriteChars(&s, "Exif", 5));
        } else if (item->isMime) {
            const char * name = item->hasContentType ? "XMP" : "";
            AVIF_CHECKRES(avifRWStreamWriteChars(&s, name, strlen(name) + 1));
            if (item->hasContentType) {
                AVIF_CHECKRES(avifRWStreamWriteChars(&s, item->contentType, strlen(item->contentType) + 1));
            }
        } else {
            AVIF_CHECKRES(avifRWStreamWriteChars(&s, "", 1));
        }
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, infeMarker));
    }
    // Newly created metadata items.
    if (createExif) {
        avifBoxMarker infeMarker;
        AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "infe", AVIF_BOX_SIZE_TBD, largeIDs ? 3 : 2, 0, &infeMarker));
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, newExifID));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)newExifID));
        }
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, 0));
        AVIF_CHECKRES(avifRWStreamWriteChars(&s, "Exif", 4));
        AVIF_CHECKRES(avifRWStreamWriteChars(&s, "Exif", 5));
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, infeMarker));
    }
    if (createXmp) {
        avifBoxMarker infeMarker;
        AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "infe", AVIF_BOX_SIZE_TBD, largeIDs ? 3 : 2, 0, &infeMarker));
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, newXmpID));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)newXmpID));
        }
        AVIF_CHECKRES(avifRWStreamWriteU16(&s, 0));
        AVIF_CHECKRES(avifRWStreamWriteChars(&s, "mime", 4));
        AVIF_CHECKRES(avifRWStreamWriteChars(&s, "XMP", 4));
        AVIF_CHECKRES(avifRWStreamWriteChars(&s, AVIF_CONTENT_TYPE_XMP, remuxXmpContentTypeSize));
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, infeMarker));
    }
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, iinfMarker));

    // ---- iref (drop references to removed items, add cdsc for new metadata) -
    avifBoxMarker irefMarker = 0;
    avifBool haveIref = AVIF_FALSE;

    // Emit one reference box per unique (type, from_item) pair, merging all of
    // their to_item IDs as required by ISO/IEC 14496-12.
    for (uint32_t r = 0; r < ctx->refs.count; ++r) {
        if (refIsDead(ctx, remuxer, &ctx->refs.ref[r])) {
            continue;
        }
        const avifRemuxRef * current = &ctx->refs.ref[r];
        // Skip if an earlier ref already represents this (type, from) group.
        avifBool alreadyEmitted = AVIF_FALSE;
        for (uint32_t earlier = 0; earlier < r; ++earlier) {
            if (refIsDead(ctx, remuxer, &ctx->refs.ref[earlier])) {
                continue;
            }
            const avifRemuxRef * prev = &ctx->refs.ref[earlier];
            if (prev->fromItemID == current->fromItemID && memcmp(prev->type, current->type, 4) == 0) {
                alreadyEmitted = AVIF_TRUE;
                break;
            }
        }
        if (alreadyEmitted) {
            continue;
        }
        // Gather all surviving to_item IDs of this group.
        uint32_t toIDs[256];
        uint16_t toCount = 0;
        for (uint32_t g = r; g < ctx->refs.count; ++g) {
            const avifRemuxRef * member = &ctx->refs.ref[g];
            if (member->fromItemID != current->fromItemID || memcmp(member->type, current->type, 4) != 0 ||
                refIsDead(ctx, remuxer, &ctx->refs.ref[g])) {
                continue;
            }
            AVIF_CHECKERR(toCount < 256, AVIF_RESULT_NOT_IMPLEMENTED);
            toIDs[toCount++] = member->toItemID;
        }
        if (!haveIref) {
            AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "iref", AVIF_BOX_SIZE_TBD, largeIDs ? 1 : 0, 0, &irefMarker));
            haveIref = AVIF_TRUE;
        }
        avifBoxMarker refMarker;
        AVIF_CHECKRES(avifRWStreamWriteBox(&s, (const char *)current->type, AVIF_BOX_SIZE_TBD, &refMarker));
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, current->fromItemID));
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, toCount));
            for (uint16_t t = 0; t < toCount; ++t) {
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, toIDs[t]));
            }
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)current->fromItemID));
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, toCount));
            for (uint16_t t = 0; t < toCount; ++t) {
                AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)toIDs[t]));
            }
        }
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, refMarker));
    }

    // Add cdsc references from newly created metadata items to the primary item.
    if ((createExif || createXmp) && ctx->hasPrimaryItemID) {
        if (!haveIref) {
            AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "iref", AVIF_BOX_SIZE_TBD, largeIDs ? 1 : 0, 0, &irefMarker));
            haveIref = AVIF_TRUE;
        }
        const uint32_t newIDs[2] = { newExifID, newXmpID };
        for (int n = 0; n < 2; ++n) {
            if (newIDs[n] == 0) {
                continue;
            }
            avifBoxMarker refMarker;
            AVIF_CHECKRES(avifRWStreamWriteBox(&s, "cdsc", AVIF_BOX_SIZE_TBD, &refMarker));
            if (largeIDs) {
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, newIDs[n]));
                AVIF_CHECKRES(avifRWStreamWriteU16(&s, 1));
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, ctx->primaryItemID));
            } else {
                AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)newIDs[n]));
                AVIF_CHECKRES(avifRWStreamWriteU16(&s, 1));
                AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)ctx->primaryItemID));
            }
            AVIF_CHECKRES(avifRWStreamFinishBox(&s, refMarker));
        }
    }
    if (haveIref) {
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, irefMarker));
    }

    // ---- iprp / ipco / ipma -------------------------------------------------
    avifBoxMarker iprpMarker;
    AVIF_CHECKRES(avifRWStreamWriteBox(&s, "iprp", AVIF_BOX_SIZE_TBD, &iprpMarker));
    avifBoxMarker ipcoMarker;
    AVIF_CHECKRES(avifRWStreamWriteBox(&s, "ipco", AVIF_BOX_SIZE_TBD, &ipcoMarker));
    for (uint32_t p = 0; p < newProperties->count; ++p) {
        AVIF_CHECKRES(writeRaw(&s, &newProperties->property[p].raw));
    }
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, ipcoMarker));

    avifBoxMarker ipmaMarker;
    AVIF_CHECKRES(avifRWStreamWriteFullBox(&s, "ipma", AVIF_BOX_SIZE_TBD, largeIDs ? 1 : 0, 0, &ipmaMarker));
    uint32_t ipmaEntryCount = 0;
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        const avifRemuxItem * item = &ctx->items.item[i];
        if (itemIsRemoved(remuxer, item)) {
            continue;
        }
        if (item->associations.count > 0) {
            ++ipmaEntryCount;
        }
    }
    AVIF_CHECKRES(avifRWStreamWriteU32(&s, ipmaEntryCount));
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        const avifRemuxItem * item = &ctx->items.item[i];
        if (itemIsRemoved(remuxer, item) || item->associations.count == 0) {
            continue;
        }
        if (largeIDs) {
            AVIF_CHECKRES(avifRWStreamWriteU32(&s, item->id));
        } else {
            AVIF_CHECKRES(avifRWStreamWriteU16(&s, (uint16_t)item->id));
        }
        // Whether this item was associated with an ICC colr property that was
        // dropped. A replacement ICC colr is only associated with such items.
        avifBool itemHadIccColr = AVIF_FALSE;
        if (changeIcc) {
            for (uint32_t a = 0; a < item->associations.count; ++a) {
                const uint16_t oldIndex = item->associations.association[a].propertyIndex;
                if (oldIndex > 0 && oldIndex <= ctx->properties.count && memEquals4(ctx->properties.property[oldIndex - 1].type, "colr")) {
                    const avifRWData * raw = &ctx->properties.property[oldIndex - 1].raw;
                    if (raw->size >= 12 && (memcmp(raw->data + 8, "prof", 4) == 0 || memcmp(raw->data + 8, "rICC", 4) == 0)) {
                        itemHadIccColr = AVIF_TRUE;
                    }
                }
            }
        }
        uint32_t addedForItem = 0;
        // Transformative properties (clap/irot/imir/pasp) and colr apply to all
        // coded image items (av01/av02), including alpha and gain map image
        // items: the encoder writes identical orientation/crop/pasp on the base
        // and gain map items of a tmap, and on color/alpha items. Derived items
        // (tmap/grid/sato/...) never receive them. The replacement colr has an
        // additional gating rule (itemHadIccColr) below.
        const avifBool itemIsCoded = isCodedImageItem(item);
        for (uint32_t a = 0; a < addedPropertyCount; ++a) {
            if (!itemIsCoded) {
                continue;
            }
            if (addedPropertyIsColr[a] && anyIccColrExisted && !itemHadIccColr) {
                // A replacement colr only goes to items that previously had an
                // ICC colr. If the file had no ICC colr at all, it applies to
                // every coded image item.
                continue;
            }
            ++addedForItem;
        }
        AVIF_CHECKERR((uint32_t)item->associations.count + addedForItem <= UINT8_MAX, AVIF_RESULT_NOT_IMPLEMENTED);
        AVIF_CHECKRES(avifRWStreamWriteU8(&s, (uint8_t)(item->associations.count + addedForItem)));
        for (uint32_t a = 0; a < item->associations.count; ++a) {
            const avifRemuxAssociation * assoc = &item->associations.association[a];
            AVIF_CHECKERR(assoc->propertyIndex <= ctx->properties.count, AVIF_RESULT_BMFF_PARSE_FAILED);
            const uint32_t newIndex = (assoc->propertyIndex == 0) ? 0 : propertyMap[assoc->propertyIndex];
            AVIF_CHECKERR(newIndex <= REMUX_MAX_PROPERTY_INDEX, AVIF_RESULT_INTERNAL_ERROR);
            // An association with property_index 0 means "no property"; the
            // essential bit must be 0 in that case (ISO/IEC 14496-12 8.11.14).
            const avifBool essential = (newIndex == 0) ? AVIF_FALSE : assoc->essential;
            AVIF_CHECKRES(avifRWStreamWriteBits(&s, essential ? 1 : 0, 1));
            AVIF_CHECKRES(avifRWStreamWriteBits(&s, newIndex & 0x7F, 7));
        }
        // Associate the newly added clap/irot/imir/pasp/colr properties using
        // the same per-property gating as above.
        if (itemIsCoded) {
            for (uint32_t a = 0; a < addedPropertyCount; ++a) {
                if (addedPropertyIsColr[a] && anyIccColrExisted && !itemHadIccColr) {
                    continue;
                }
                AVIF_CHECKRES(avifRWStreamWriteBits(&s, addedPropertyEssential[a] ? 1 : 0, 1));
                AVIF_CHECKRES(avifRWStreamWriteBits(&s, addedPropertyIndices[a] & 0x7F, 7));
            }
        }
    }
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, ipmaMarker));
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, iprpMarker));

    // ---- Other modeled meta children (verbatim) -----------------------------
    for (uint32_t b = 0; b < ctx->otherMetaBoxes.count; ++b) {
        AVIF_CHECKRES(writeRaw(&s, &ctx->otherMetaBoxes.box[b].raw));
    }
    AVIF_CHECKRES(avifRWStreamFinishBox(&s, metaMarker));

    // ---- Other top-level boxes and verbatim mdats ---------------------------
    for (uint32_t b = 0; b < ctx->otherTopLevelBoxes.count; ++b) {
        AVIF_CHECKRES(writeRaw(&s, &ctx->otherTopLevelBoxes.box[b].raw));
    }

    // Every mdat payload is copied byte-for-byte. The header is usually copied
    // verbatim (including the largesize form); however a size==0 mdat (which is
    // only legal as the last box of a file) is rewritten with an explicit size,
    // otherwise any box appended after it (e.g. the new metadata mdat) would be
    // swallowed by the size==0 box. Record where each payload lands so iloc
    // extent offsets can be translated per mdat, regardless of mdat/meta order.
    // Owned by ctx so error paths free it via avifRemuxContextDestroy().
    ctx->mdatOutputPayloadOffsets = (uint64_t *)avifCalloc(ctx->mdats.count ? ctx->mdats.count : 1, sizeof(uint64_t));
    AVIF_CHECKERR(ctx->mdatOutputPayloadOffsets != NULL, AVIF_RESULT_OUT_OF_MEMORY);
    uint64_t * mdatOutputPayloadOffsets = ctx->mdatOutputPayloadOffsets;
    for (uint32_t m = 0; m < ctx->mdats.count; ++m) {
        const avifRemuxMdat * mdat = &ctx->mdats.mdat[m];
        if (mdat->sizeZero) {
            const uint64_t totalSize = 8 + mdat->payloadSize;
            if (totalSize <= UINT32_MAX) {
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, (uint32_t)totalSize));
                AVIF_CHECKRES(avifRWStreamWriteChars(&s, "mdat", 4));
            } else {
                AVIF_CHECKRES(avifRWStreamWriteU32(&s, 1)); // largesize marker
                AVIF_CHECKRES(avifRWStreamWriteChars(&s, "mdat", 4));
                AVIF_CHECKRES(avifRWStreamWriteU64(&s, 16 + mdat->payloadSize));
            }
        } else {
            AVIF_CHECKRES(avifRWStreamWrite(&s, mdat->header, mdat->headerSize));
        }
        mdatOutputPayloadOffsets[m] = avifRWStreamOffset(&s);
        AVIF_CHECKRES(avifRWStreamWrite(&s, mdat->payload, (size_t)mdat->payloadSize));
    }

    // ---- Optional new mdat holding replacement/added Exif/XMP ---------------
    uint64_t newMdatPayloadOffset = 0;
    if (newMdatPayloadSize > 0) {
        avifBoxMarker mdatMarker;
        AVIF_CHECKRES(avifRWStreamWriteBox(&s, "mdat", AVIF_BOX_SIZE_TBD, &mdatMarker));
        newMdatPayloadOffset = avifRWStreamOffset(&s);
        if (addExif) {
            AVIF_CHECKRES(writeRaw(&s, &ctx->newExifPayload));
        }
        if (addXmp) {
            AVIF_CHECKRES(writeRaw(&s, &ctx->newXmpPayload));
        }
        AVIF_CHECKRES(avifRWStreamFinishBox(&s, mdatMarker));
    }

    avifRWStreamFinishWrite(&s);

    // Translates an input file offset (construction_method 0 extent) to the
    // matching output offset by locating the mdat that contains it.
    // Returns AVIF_RESULT_BMFF_PARSE_FAILED if no mdat contains the offset.
    // extents inside an mdat keep their in-mdat relative offset.
    const uint64_t exifPayloadOffset = newMdatPayloadOffset;
    const uint64_t xmpPayloadOffset = newMdatPayloadOffset + (addExif ? ctx->newExifPayload.size : 0);

    uint32_t fixupIndex = 0;
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        avifRemuxItem * item = &ctx->items.item[i];
        if (itemIsRemoved(remuxer, item)) {
            continue;
        }
        avifBool replaced = (replaceExif && item == oldExif) || (replaceXmp && item == oldXmp);
        if (replaced) {
            fixups->fixup[fixupIndex++].targetOffset = (item == oldExif) ? exifPayloadOffset : xmpPayloadOffset;
            continue;
        }
        for (uint32_t e = 0; e < item->extents.count; ++e) {
            const avifRemuxExtent * extent = &item->extents.extent[e];
            uint64_t translated = 0;
            avifBool found = AVIF_FALSE;
            for (uint32_t m = 0; m < ctx->mdats.count; ++m) {
                const avifRemuxMdat * mdat = &ctx->mdats.mdat[m];
                const uint64_t mdatPayloadEnd = mdat->fileOffset + mdat->headerSize + mdat->payloadSize;
                if (extent->offset >= mdat->fileOffset + mdat->headerSize && extent->offset + extent->size <= mdatPayloadEnd) {
                    const uint64_t relative = extent->offset - (mdat->fileOffset + mdat->headerSize);
                    translated = mdatOutputPayloadOffsets[m] + relative;
                    found = AVIF_TRUE;
                    break;
                }
            }
            if (!found) {
                // Owned resources are released by avifRemuxContextDestroy().
                return remuxDiagError(ctx->diag, AVIF_RESULT_BMFF_PARSE_FAILED, "extent of item ID %u is not contained in any mdat", item->id);
            }
            fixups->fixup[fixupIndex++].targetOffset = translated;
        }
    }
    if (createExif) {
        fixups->fixup[fixupIndex++].targetOffset = exifPayloadOffset;
    }
    if (createXmp) {
        fixups->fixup[fixupIndex++].targetOffset = xmpPayloadOffset;
    }
    assert(fixupIndex == fixups->count);

    for (uint32_t f = 0; f < fixups->count; ++f) {
        const IlocFixup * fixup = &fixups->fixup[f];
        if (fixup->use64) {
            uint64_t be = avifHTON64(fixup->targetOffset);
            memcpy(output->data + fixup->offsetField, &be, sizeof(be));
        } else {
            if (fixup->targetOffset > UINT32_MAX) {
                // Owned resources are released by avifRemuxContextDestroy().
                return remuxDiagError(ctx->diag, AVIF_RESULT_NOT_IMPLEMENTED, "an extent offset does not fit in a 32-bit iloc field");
            }
            uint32_t be = avifHTONL((uint32_t)fixup->targetOffset);
            memcpy(output->data + fixup->offsetField, &be, sizeof(be));
        }
    }

    // Owned resources are released by avifRemuxContextDestroy().
    return AVIF_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Context lifetime

static avifRemuxContext * avifRemuxContextCreate(void)
{
    avifRemuxContext * ctx = (avifRemuxContext *)avifCalloc(1, sizeof(avifRemuxContext));
    if (!ctx) {
        return NULL;
    }
    if (!avifArrayCreate(&ctx->otherTopLevelBoxes, sizeof(avifRemuxRawBox), 4) ||
        !avifArrayCreate(&ctx->mdats, sizeof(avifRemuxMdat), 4) || !avifArrayCreate(&ctx->items, sizeof(avifRemuxItem), 8) ||
        !avifArrayCreate(&ctx->properties, sizeof(avifRemuxProperty), 16) || !avifArrayCreate(&ctx->refs, sizeof(avifRemuxRef), 8) ||
        !avifArrayCreate(&ctx->otherMetaBoxes, sizeof(avifRemuxRawBox), 4)) {
        avifRemuxContextDestroy(ctx);
        return NULL;
    }
    return ctx;
}

static void avifRemuxContextDestroy(avifRemuxContext * ctx)
{
    if (!ctx) {
        return;
    }
    for (uint32_t i = 0; i < ctx->items.count; ++i) {
        itemDestroy(&ctx->items.item[i]);
    }
    avifArrayDestroy(&ctx->items);
    for (uint32_t p = 0; p < ctx->properties.count; ++p) {
        avifRWDataFree(&ctx->properties.property[p].raw);
    }
    avifArrayDestroy(&ctx->properties);
    avifArrayDestroy(&ctx->refs);
    for (uint32_t b = 0; b < ctx->otherMetaBoxes.count; ++b) {
        avifRWDataFree(&ctx->otherMetaBoxes.box[b].raw);
    }
    avifArrayDestroy(&ctx->otherMetaBoxes);
    for (uint32_t b = 0; b < ctx->otherTopLevelBoxes.count; ++b) {
        avifRWDataFree(&ctx->otherTopLevelBoxes.box[b].raw);
    }
    avifArrayDestroy(&ctx->otherTopLevelBoxes);
    avifArrayDestroy(&ctx->mdats);
    avifRWDataFree(&ctx->hdlrBox);
    avifRWDataFree(&ctx->newExifPayload);
    avifRWDataFree(&ctx->newXmpPayload);
    // buildOutput() working state. These may be zero-initialized if the build
    // never started; avifArrayDestroy() and avifFree() handle that.
    for (uint32_t p = 0; p < ctx->newProperties.count; ++p) {
        avifRWDataFree(&ctx->newProperties.property[p].raw);
    }
    avifArrayDestroy(&ctx->newProperties);
    avifArrayDestroy(&ctx->fixups);
    avifFree(ctx->propertyMap);
    avifFree(ctx->mdatOutputPayloadOffsets);
    avifFree(ctx);
}

// ---------------------------------------------------------------------------
// Public API

avifRemuxer * avifRemuxerCreate(void)
{
    avifRemuxer * remuxer = (avifRemuxer *)avifCalloc(1, sizeof(avifRemuxer));
    return remuxer;
}

void avifRemuxerDestroy(avifRemuxer * remuxer)
{
    if (!remuxer) {
        return;
    }
    avifRWDataFree(&remuxer->icc);
    avifRWDataFree(&remuxer->exif);
    avifRWDataFree(&remuxer->xmp);
    avifFree(remuxer);
}

avifResult avifRemuxerSetIrot(avifRemuxer * remuxer, uint8_t angle)
{
    AVIF_CHECKERR(remuxer != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    AVIF_CHECKERR(angle <= 3, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->irot.angle = angle;
    remuxer->transformFlags |= AVIF_TRANSFORM_IROT;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerSetImir(avifRemuxer * remuxer, uint8_t axis)
{
    AVIF_CHECKERR(remuxer != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    AVIF_CHECKERR(axis <= 1, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->imir.axis = axis;
    remuxer->transformFlags |= AVIF_TRANSFORM_IMIR;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerSetClap(avifRemuxer * remuxer, const avifCleanApertureBox * clap)
{
    AVIF_CHECKERR(remuxer != NULL && clap != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->clap = *clap;
    remuxer->transformFlags |= AVIF_TRANSFORM_CLAP;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerSetPasp(avifRemuxer * remuxer, uint32_t hSpacing, uint32_t vSpacing)
{
    AVIF_CHECKERR(remuxer != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    AVIF_CHECKERR(hSpacing != 0 && vSpacing != 0, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->pasp.hSpacing = hSpacing;
    remuxer->pasp.vSpacing = vSpacing;
    remuxer->transformFlags |= AVIF_TRANSFORM_PASP;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerSetICC(avifRemuxer * remuxer, const uint8_t * icc, size_t iccSize)
{
    AVIF_CHECKERR(remuxer != NULL && (icc != NULL || iccSize == 0), AVIF_RESULT_INVALID_ARGUMENT);
    AVIF_CHECKRES(avifRWDataSet(&remuxer->icc, icc, iccSize));
    remuxer->iccAction = AVIF_REMUX_METADATA_REPLACE;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerRemoveICC(avifRemuxer * remuxer)
{
    AVIF_CHECKERR(remuxer != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->iccAction = AVIF_REMUX_METADATA_REMOVE;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerSetExif(avifRemuxer * remuxer, const uint8_t * exif, size_t exifSize)
{
    AVIF_CHECKERR(remuxer != NULL && exif != NULL && exifSize > 0, AVIF_RESULT_INVALID_ARGUMENT);
    AVIF_CHECKRES(avifRWDataSet(&remuxer->exif, exif, exifSize));
    remuxer->exifAction = AVIF_REMUX_METADATA_REPLACE;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerRemoveExif(avifRemuxer * remuxer)
{
    AVIF_CHECKERR(remuxer != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->exifAction = AVIF_REMUX_METADATA_REMOVE;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerSetXMP(avifRemuxer * remuxer, const uint8_t * xmp, size_t xmpSize)
{
    AVIF_CHECKERR(remuxer != NULL && xmp != NULL && xmpSize > 0, AVIF_RESULT_INVALID_ARGUMENT);
    AVIF_CHECKRES(avifRWDataSet(&remuxer->xmp, xmp, xmpSize));
    remuxer->xmpAction = AVIF_REMUX_METADATA_REPLACE;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerRemoveXMP(avifRemuxer * remuxer)
{
    AVIF_CHECKERR(remuxer != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    remuxer->xmpAction = AVIF_REMUX_METADATA_REMOVE;
    return AVIF_RESULT_OK;
}

avifResult avifRemuxerRun(avifRemuxer * remuxer, const avifROData * input, avifRWData * output)
{
    AVIF_CHECKERR(remuxer != NULL && input != NULL && input->data != NULL && output != NULL, AVIF_RESULT_INVALID_ARGUMENT);
    memset(output, 0, sizeof(*output));

    avifRemuxContext * ctx = avifRemuxContextCreate();
    AVIF_CHECKERR(ctx != NULL, AVIF_RESULT_OUT_OF_MEMORY);
    ctx->input = input;
    ctx->diag = remuxer->diag;

    avifResult result = walkTopLevelBoxes(ctx);
    if (result == AVIF_RESULT_OK) {
        result = validateItems(ctx);
    }
    if (result == AVIF_RESULT_OK) {
        result = buildOutput(ctx, remuxer, output);
    }
    avifRemuxContextDestroy(ctx);
    if (result != AVIF_RESULT_OK) {
        avifRWDataFree(output);
    }
    return result;
}
