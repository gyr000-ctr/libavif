// Copyright 2025 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

// avifremux: container-level lossless rewrite of a single-image AVIF file.
//
// The AV1 payloads are never decoded or re-encoded: 'mdat' boxes are copied
// byte-for-byte. Only clap/irot/imir/pasp properties, the ICC profile ('colr')
// and Exif/XMP metadata items are modified.

#include "avif/avif.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void syntax(void)
{
    printf("avifremux: lossless AVIF container remuxer (no AV1 re-encode)\n\n");
    printf("Syntax:\n");
    printf("    avifremux [options] input.avif output.avif\n\n");
    printf("Options:\n");
    printf("    --irot <0-3>            Set rotation (angle * 90 degrees, counterclockwise)\n");
    printf("    --imir <0|1>            Set mirror axis (0=vertical flip, 1=horizontal flip)\n");
    printf("    --pasp <h>,<v>          Set pixel aspect ratio (hSpacing:vSpacing)\n");
    printf("    --clap <n>/<d>,...      Set clean aperture: wN/wD,hN/hD,hoN/hoD,voN/voD\n");
    printf("    --icc <file>            Replace the ICC profile with the contents of file\n");
    printf("    --remove-icc            Remove the ICC profile (keep nclx if present)\n");
    printf("    --exif <file>           Replace the Exif payload with the contents of file\n");
    printf("    --remove-exif           Remove the Exif metadata item\n");
    printf("    --xmp <file>            Replace the XMP payload with the contents of file\n");
    printf("    --remove-xmp            Remove the XMP metadata item\n");
    printf("    -h, --help              Show this help\n");
}

static avifBool readWholeFile(const char * filename, avifRWData * raw)
{
    FILE * f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s\n", filename);
        return AVIF_FALSE;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return AVIF_FALSE;
    }
    long pos = ftell(f);
    if (pos <= 0) {
        fprintf(stderr, "ERROR: cannot determine size of %s\n", filename);
        fclose(f);
        return AVIF_FALSE;
    }
    size_t fileSize = (size_t)pos;
    if (avifRWDataRealloc(raw, fileSize) != AVIF_RESULT_OK) {
        fclose(f);
        return AVIF_FALSE;
    }
    if (fseek(f, 0, SEEK_SET) != 0 || fread(raw->data, 1, fileSize, f) != fileSize) {
        fprintf(stderr, "ERROR: failed to read %s\n", filename);
        fclose(f);
        return AVIF_FALSE;
    }
    fclose(f);
    return AVIF_TRUE;
}

static avifBool writeWholeFile(const char * filename, const avifRWData * raw)
{
    FILE * f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s for writing\n", filename);
        return AVIF_FALSE;
    }
    avifBool ok = (fwrite(raw->data, 1, raw->size, f) == raw->size);
    if (!ok) {
        fprintf(stderr, "ERROR: failed to write %s\n", filename);
    }
    fclose(f);
    return ok;
}

// Parses "a/b" into two uint32 values.
static avifBool parseFraction(const char * text, uint32_t * n, uint32_t * d)
{
    char * end = NULL;
    unsigned long nv = strtoul(text, &end, 10);
    if (end == text || *end != '/' || nv > UINT32_MAX) {
        return AVIF_FALSE;
    }
    char * start = end + 1;
    unsigned long dv = strtoul(start, &end, 10);
    if (end == start || *end != '\0' || dv > UINT32_MAX || dv == 0) {
        return AVIF_FALSE;
    }
    *n = (uint32_t)nv;
    *d = (uint32_t)dv;
    return AVIF_TRUE;
}

int main(int argc, char ** argv)
{
    const char * inputFilename = NULL;
    const char * outputFilename = NULL;
    const char * iccFilename = NULL;
    const char * exifFilename = NULL;
    const char * xmpFilename = NULL;
    avifBool removeIcc = AVIF_FALSE;
    avifBool removeExif = AVIF_FALSE;
    avifBool removeXmp = AVIF_FALSE;
    int irot = -1;
    int imir = -1;
    avifCleanApertureBox clap;
    avifBool clapSet = AVIF_FALSE;
    uint32_t paspH = 0;
    uint32_t paspV = 0;
    avifBool paspSet = AVIF_FALSE;

    for (int i = 1; i < argc; ++i) {
        const char * arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            syntax();
            return 0;
        } else if (!strcmp(arg, "--irot")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --irot requires an argument\n");
                return 1;
            }
            irot = atoi(argv[i]);
            if (irot < 0 || irot > 3) {
                fprintf(stderr, "ERROR: --irot angle must be in [0,3]\n");
                return 1;
            }
        } else if (!strcmp(arg, "--imir")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --imir requires an argument\n");
                return 1;
            }
            imir = atoi(argv[i]);
            if (imir != 0 && imir != 1) {
                fprintf(stderr, "ERROR: --imir axis must be 0 or 1\n");
                return 1;
            }
        } else if (!strcmp(arg, "--pasp")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --pasp requires an argument\n");
                return 1;
            }
            char comma;
            if (sscanf(argv[i], "%" SCNu32 ",%" SCNu32 "%c", &paspH, &paspV, &comma) != 2 || paspH == 0 || paspV == 0) {
                fprintf(stderr, "ERROR: invalid --pasp value (expected h,v): %s\n", argv[i]);
                return 1;
            }
            paspSet = AVIF_TRUE;
        } else if (!strcmp(arg, "--clap")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --clap requires an argument\n");
                return 1;
            }
            // wN/wD,hN/hD,hoN/hoD,voN/voD. Split on commas only so that the
            // '/' fraction separators remain intact for parseFraction().
            char buf[128];
            snprintf(buf, sizeof(buf), "%s", argv[i]);
            char * parts[4];
            int partCount = 0;
            char * cursor = buf;
            while (partCount < 4) {
                parts[partCount++] = cursor;
                char * comma = strchr(cursor, ',');
                if (!comma) {
                    break;
                }
                *comma = '\0';
                cursor = comma + 1;
            }
            if (partCount != 4 || !parseFraction(parts[0], &clap.widthN, &clap.widthD) ||
                !parseFraction(parts[1], &clap.heightN, &clap.heightD) || !parseFraction(parts[2], &clap.horizOffN, &clap.horizOffD) ||
                !parseFraction(parts[3], &clap.vertOffN, &clap.vertOffD)) {
                fprintf(stderr, "ERROR: invalid --clap value: %s\n", argv[i]);
                return 1;
            }
            clapSet = AVIF_TRUE;
        } else if (!strcmp(arg, "--icc")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --icc requires a filename\n");
                return 1;
            }
            iccFilename = argv[i];
        } else if (!strcmp(arg, "--remove-icc")) {
            removeIcc = AVIF_TRUE;
        } else if (!strcmp(arg, "--exif")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --exif requires a filename\n");
                return 1;
            }
            exifFilename = argv[i];
        } else if (!strcmp(arg, "--remove-exif")) {
            removeExif = AVIF_TRUE;
        } else if (!strcmp(arg, "--xmp")) {
            if (++i >= argc) {
                fprintf(stderr, "ERROR: --xmp requires a filename\n");
                return 1;
            }
            xmpFilename = argv[i];
        } else if (!strcmp(arg, "--remove-xmp")) {
            removeXmp = AVIF_TRUE;
        } else if (arg[0] == '-') {
            fprintf(stderr, "ERROR: unrecognized option %s\n\n", arg);
            syntax();
            return 1;
        } else if (!inputFilename) {
            inputFilename = arg;
        } else if (!outputFilename) {
            outputFilename = arg;
        } else {
            fprintf(stderr, "ERROR: too many positional arguments: %s\n", arg);
            return 1;
        }
    }

    if (!inputFilename || !outputFilename) {
        syntax();
        return 1;
    }
    if (iccFilename && removeIcc) {
        fprintf(stderr, "ERROR: --icc and --remove-icc are mutually exclusive\n");
        return 1;
    }
    if (exifFilename && removeExif) {
        fprintf(stderr, "ERROR: --exif and --remove-exif are mutually exclusive\n");
        return 1;
    }
    if (xmpFilename && removeXmp) {
        fprintf(stderr, "ERROR: --xmp and --remove-xmp are mutually exclusive\n");
        return 1;
    }

    int returnCode = 1;
    avifRWData input = { NULL, 0 };
    avifRWData output = { NULL, 0 };
    avifRWData icc = { NULL, 0 };
    avifRWData exif = { NULL, 0 };
    avifRWData xmp = { NULL, 0 };
    avifRemuxer * remuxer = NULL;

    if (!readWholeFile(inputFilename, &input)) {
        goto cleanup;
    }
    if (iccFilename && !readWholeFile(iccFilename, &icc)) {
        goto cleanup;
    }
    if (exifFilename && !readWholeFile(exifFilename, &exif)) {
        goto cleanup;
    }
    if (xmpFilename && !readWholeFile(xmpFilename, &xmp)) {
        goto cleanup;
    }

    remuxer = avifRemuxerCreate();
    if (!remuxer) {
        fprintf(stderr, "ERROR: memory allocation failure\n");
        goto cleanup;
    }
    avifDiagnostics diag;
    memset(&diag, 0, sizeof(diag));
    remuxer->diag = &diag;

    avifResult result = AVIF_RESULT_OK;
    if (irot >= 0) {
        result = avifRemuxerSetIrot(remuxer, (uint8_t)irot);
    }
    if (result == AVIF_RESULT_OK && imir >= 0) {
        result = avifRemuxerSetImir(remuxer, (uint8_t)imir);
    }
    if (result == AVIF_RESULT_OK && paspSet) {
        result = avifRemuxerSetPasp(remuxer, paspH, paspV);
    }
    if (result == AVIF_RESULT_OK && clapSet) {
        result = avifRemuxerSetClap(remuxer, &clap);
    }
    if (result == AVIF_RESULT_OK && iccFilename) {
        result = avifRemuxerSetICC(remuxer, icc.data, icc.size);
    }
    if (result == AVIF_RESULT_OK && removeIcc) {
        result = avifRemuxerRemoveICC(remuxer);
    }
    if (result == AVIF_RESULT_OK && exifFilename) {
        result = avifRemuxerSetExif(remuxer, exif.data, exif.size);
    }
    if (result == AVIF_RESULT_OK && removeExif) {
        result = avifRemuxerRemoveExif(remuxer);
    }
    if (result == AVIF_RESULT_OK && xmpFilename) {
        result = avifRemuxerSetXMP(remuxer, xmp.data, xmp.size);
    }
    if (result == AVIF_RESULT_OK && removeXmp) {
        result = avifRemuxerRemoveXMP(remuxer);
    }
    if (result != AVIF_RESULT_OK) {
        fprintf(stderr, "ERROR: %s\n", avifResultToString(result));
        goto cleanup;
    }

    avifROData roInput = { input.data, input.size };
    result = avifRemuxerRun(remuxer, &roInput, &output);
    if (result != AVIF_RESULT_OK) {
        fprintf(stderr, "ERROR: remux failed: %s (%d)\n", avifResultToString(result), (int)result);
        if (*diag.error) {
            fprintf(stderr, "       %s\n", diag.error);
        }
        goto cleanup;
    }

    if (!writeWholeFile(outputFilename, &output)) {
        goto cleanup;
    }
    printf("Remuxed %s -> %s (%" PRIu64 " -> %" PRIu64 " bytes, AV1 payload untouched)\n",
           inputFilename,
           outputFilename,
           (uint64_t)input.size,
           (uint64_t)output.size);
    returnCode = 0;

cleanup:
    avifRWDataFree(&input);
    avifRWDataFree(&output);
    avifRWDataFree(&icc);
    avifRWDataFree(&exif);
    avifRWDataFree(&xmp);
    avifRemuxerDestroy(remuxer);
    return returnCode;
}
