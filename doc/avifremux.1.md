% AVIFREMUX(1) | General Commands Manual
%
% 2025-01-01

<!--
This man page is written in pandoc's Markdown.
See: https://pandoc.org/MANUAL.html#pandocs-markdown
-->

# NAME

avifremux - losslessly rewrite the container of a single-image AVIF file

# SYNOPSIS

**avifremux** [_options_] _input.avif_ _output.avif_

# DESCRIPTION

**avifremux** rewrites the ISOBMFF container of a single-image AVIF file
without decoding or re-encoding the AV1 payloads. Every **mdat** box of the
input is copied to the output byte-for-byte, so the encoded image quality and
bytes are preserved exactly.

Only the following container elements can be changed: the transformative
properties **clap**, **irot**, **imir** and **pasp**, the ICC profile
(**colr** box) and the **Exif** and **XMP** metadata items.

Grid images, image sequences (track-based files), items that use an
**idat** construction method and items without extents are rejected with
dedicated error codes rather than being partially rewritten.

# OPTIONS

**-h**, **\--help**
:   Show syntax help.

**\--irot** _0|1|2|3_
:   Set the rotation. The value times 90 gives the angle in degrees, applied
    counterclockwise.

**\--imir** _0|1_
:   Set the mirror axis. 0 exchanges top and bottom, 1 exchanges left and
    right.

**\--pasp** _hSpacing_,_vSpacing_
:   Set the pixel aspect ratio. Both values must be non-zero.

**\--clap** _wN_/_wD_,_hN_/_wD_,_hoN_/_hoD_,_voN_/_voD_
:   Set the clean aperture as four fractions: clean aperture width, height,
    and the horizontal and vertical offsets.

**\--icc** _file_
:   Replace the ICC profile with the contents of _file_. An existing **nclx**
    colour box is kept.

**\--remove-icc**
:   Remove the ICC profile (keeping any **nclx** box).

**\--exif** _file_
:   Replace the Exif payload with the contents of _file_. The file must
    contain a valid TIFF header.

**\--remove-exif**
:   Remove the Exif metadata item.

**\--xmp** _file_
:   Replace the XMP payload with the contents of _file_.

**\--remove-xmp**
:   Remove the XMP metadata item.

# NOTES

The AV1 payloads are never touched: remuxing is fast even for very large
files and cannot introduce generation loss. Because the **mdat** boxes are
copied verbatim, only the metadata-bearing boxes (**meta** and its children)
are regenerated.
