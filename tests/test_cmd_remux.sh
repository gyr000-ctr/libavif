#!/bin/bash
# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ------------------------------------------------------------------------------
#
# tests for the container-level lossless remuxer (avifremux). The AV1 payloads
# are never re-encoded: every 'mdat' box must survive byte-for-byte.

source $(dirname "$0")/cmd_test_common.sh || exit

# Input file paths.
INPUT_PNG="${TESTDATA_DIR}/paris_icc_exif_xmp.png"
INPUT_ICC="${TESTDATA_DIR}/sRGB2014.icc"
GRID_AVIF="${TESTDATA_DIR}/color_grid_alpha_nogrid.avif"
SEQUENCE_AVIF="${TESTDATA_DIR}/colors-animated-8bpc.avif"
IDAT_AVIF="${TESTDATA_DIR}/draw_points_idat.avif"
# Output file names.
SRC="avif_test_cmd_remux_src.avif"
SRC_ALPHA="avif_test_cmd_remux_alpha.avif"
OUT="avif_test_cmd_remux_out.avif"
DEC_SRC="avif_test_cmd_remux_dec_src.png"
DEC_OUT="avif_test_cmd_remux_dec_out.png"
METADATA_BIN="avif_test_cmd_remux_metadata.bin"

# Verifies that every 'mdat' payload of the first file is copied byte-for-byte
# into an 'mdat' of the second file. Extra mdats (e.g. holding replacement
# metadata) in the second file are allowed; the AV1 payloads are what must be
# untouched.
check_mdat_preserved() {
  python3 - "$1" "$2" <<'PY'
import sys
def mdat_payloads(path):
    data = open(path, "rb").read()
    out = []
    off = 0
    while off + 8 <= len(data):
        size = int.from_bytes(data[off:off + 4], "big")
        typ = data[off + 4:off + 8]
        header = 8
        if size == 1:
            size = int.from_bytes(data[off + 8:off + 16], "big")
            header = 16
        if size == 0:
            size = len(data) - off
        if typ == b"mdat":
            out.append(data[off + header:off + size])
        off += size
    return out
src = mdat_payloads(sys.argv[1])
dst = mdat_payloads(sys.argv[2])
if not src:
    sys.exit("no mdat in source")
for payload in src:
    if payload not in dst:
        sys.exit("an mdat payload was modified or removed")
PY
}

# Cleanup
cleanup() {
  pushd ${TMP_DIR}
    rm -f -- "${SRC}" "${SRC_ALPHA}" "${OUT}" "${DEC_SRC}" "${DEC_OUT}" "${METADATA_BIN}"
  popd
}
trap cleanup EXIT

pushd ${TMP_DIR}
  echo "Testing remux setup"
  "${AVIFENC}" "${INPUT_PNG}" -o "${SRC}" --pasp 4,3
  "${AVIFENC}" "${TESTDATA_DIR}/abc.png" -o "${SRC_ALPHA}"

  echo "Testing no-op remux preserves pixels and mdat byte-for-byte"
  "${AVIFREMUX}" "${SRC}" "${OUT}"
  check_mdat_preserved "${SRC}" "${OUT}"
  "${AVIFDEC}" "${SRC}" "${DEC_SRC}"
  "${AVIFDEC}" "${OUT}" "${DEC_OUT}"
  cmp "${DEC_SRC}" "${DEC_OUT}"

  echo "Testing irot/imir/pasp/clap transforms"
  "${AVIFREMUX}" --irot 1 --imir 0 --pasp 16,9 "${SRC}" "${OUT}"
  check_mdat_preserved "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "irot (Rotation).*: 1"
  "${AVIFDEC}" -i "${OUT}" | grep -q "imir (Mirror).*: 0"
  "${AVIFDEC}" -i "${OUT}" | grep -q "pasp (Aspect Ratio).*: 16/9"
  "${AVIFREMUX}" --clap 400/1,300/1,0/1,0/1 "${SRC}" "${OUT}"
  check_mdat_preserved "${SRC}" "${OUT}"
  "${AVIFDEC}" --no-strict -i "${OUT}" | grep -q "clap"

  echo "Testing transforms also apply to the alpha item"
  "${AVIFREMUX}" --irot 3 "${SRC_ALPHA}" "${OUT}"
  check_mdat_preserved "${SRC_ALPHA}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "irot (Rotation).*: 3"

  echo "Testing ICC replace and remove"
  "${AVIFREMUX}" --icc "${INPUT_ICC}" "${SRC}" "${OUT}"
  check_mdat_preserved "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "ICC Profile.*: Present"
  "${AVIFREMUX}" --remove-icc "${SRC}" "${OUT}"
  check_mdat_preserved "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "ICC Profile.*: Absent"

  echo "Testing Exif/XMP replace, remove and add"
  # Minimal little-endian TIFF header so the payload is accepted as Exif.
  printf 'II*\x00\x08\x00\x00\x00' > "${METADATA_BIN}"
  "${AVIFREMUX}" --exif "${METADATA_BIN}" "${SRC}" "${OUT}"
  check_mdat_preserved "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "Exif Metadata.*: Present (8 bytes)"
  "${AVIFREMUX}" --remove-exif "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "Exif Metadata.*: Absent"
  printf '<x:xmpmeta xmlns:x="adobe:ns:meta/"></x:xmpmeta>' > "${METADATA_BIN}"
  "${AVIFREMUX}" --xmp "${METADATA_BIN}" "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "XMP Metadata.*: Present (48 bytes)"
  "${AVIFREMUX}" --remove-xmp "${SRC}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "XMP Metadata.*: Absent"
  # Adding metadata to a file that had none.
  EXIF_BIN="avif_test_cmd_remux_exif.bin"
  XMP_BIN="avif_test_cmd_remux_xmp.bin"
  printf 'II*\x00\x08\x00\x00\x00' > "${EXIF_BIN}"
  printf '<x:xmpmeta xmlns:x="adobe:ns:meta/"></x:xmpmeta>' > "${XMP_BIN}"
  "${AVIFREMUX}" --exif "${EXIF_BIN}" --xmp "${XMP_BIN}" "${SRC_ALPHA}" "${OUT}"
  "${AVIFDEC}" -i "${OUT}" | grep -q "Exif Metadata.*: Present"
  "${AVIFDEC}" -i "${OUT}" | grep -q "XMP Metadata.*: Present"
  rm -f -- "${EXIF_BIN}" "${XMP_BIN}"

  echo "Testing invalid Exif payload is rejected"
  printf 'not exif' > "${METADATA_BIN}"
  if "${AVIFREMUX}" --exif "${METADATA_BIN}" "${SRC}" "${OUT}"; then
    echo "ERROR: invalid Exif payload should be rejected" >&2
    exit 1
  fi

  echo "Testing grid rejection (AVIF_RESULT_REMUX_GRID_NOT_SUPPORTED)"
  if "${AVIFREMUX}" --irot 1 "${GRID_AVIF}" "${OUT}"; then
    echo "ERROR: grid image should be rejected" >&2
    exit 1
  fi
  # Dedicated error code 35.
  "${AVIFREMUX}" --irot 1 "${GRID_AVIF}" "${OUT}" 2>&1 | grep -q "(35)"

  echo "Testing sequence rejection (AVIF_RESULT_REMUX_SEQUENCE_NOT_SUPPORTED)"
  if "${AVIFREMUX}" --irot 1 "${SEQUENCE_AVIF}" "${OUT}"; then
    echo "ERROR: image sequence should be rejected" >&2
    exit 1
  fi
  "${AVIFREMUX}" --irot 1 "${SEQUENCE_AVIF}" "${OUT}" 2>&1 | grep -q "(36)"

  echo "Testing idat construction rejection (AVIF_RESULT_REMUX_UNSUPPORTED_CONSTRUCTION)"
  if "${AVIFREMUX}" --irot 1 "${IDAT_AVIF}" "${OUT}"; then
    echo "ERROR: idat construction should be rejected" >&2
    exit 1
  fi
  "${AVIFREMUX}" --irot 1 "${IDAT_AVIF}" "${OUT}" 2>&1 | grep -q "(38)"

  echo "Testing missing extent rejection (AVIF_RESULT_ITEM_MISSING_EXTENT)"
  # Build a minimal AVIF whose primary item declares zero extents.
  ZERO_EXTENT="avif_test_cmd_remux_zero_extent.avif"
  python3 - "${ZERO_EXTENT}" <<'PY'
import sys
def box(t, payload):
    return (len(payload) + 8).to_bytes(4, "big") + t + payload
def fullbox(t, ver, flags, payload):
    return box(t, bytes([ver]) + flags.to_bytes(3, "big") + payload)
ftyp = box(b"ftyp", b"avif" + (0).to_bytes(4, "big") + b"avif" + b"mif1")
hdlr = fullbox(b"hdlr", 0, 0, (0).to_bytes(4, "big") + b"pict" + b"\0" * 12 + b"\0")
pitm = fullbox(b"pitm", 0, 0, (1).to_bytes(2, "big"))
# iloc v0: offset/length nibbles are 4, one item with extent_count 0.
iloc = fullbox(b"iloc", 0, 0,
               bytes([0x44, 0x00]) + (1).to_bytes(2, "big") + (1).to_bytes(2, "big") +
               (0).to_bytes(2, "big") + (0).to_bytes(2, "big"))
infe = fullbox(b"infe", 2, 0, (1).to_bytes(2, "big") + (0).to_bytes(2, "big") + b"av01" + b"\0")
iinf = fullbox(b"iinf", 0, 0, (1).to_bytes(2, "big") + infe)
ipma = fullbox(b"ipma", 0, 0, (0).to_bytes(4, "big"))
iprp = box(b"iprp", box(b"ipco", b"") + ipma)
meta = fullbox(b"meta", 0, 0, hdlr + pitm + iloc + iinf + iprp)
open(sys.argv[1], "wb").write(ftyp + meta + box(b"mdat", b""))
PY
  if "${AVIFREMUX}" --irot 1 "${ZERO_EXTENT}" "${OUT}"; then
    echo "ERROR: item without extents should be rejected" >&2
    exit 1
  fi
  "${AVIFREMUX}" --irot 1 "${ZERO_EXTENT}" "${OUT}" 2>&1 | grep -q "(37)"
  rm -f -- "${ZERO_EXTENT}"

  echo "Testing garbage input is rejected without crash"
  printf 'this is not an avif file' > "${METADATA_BIN}"
  if "${AVIFREMUX}" --irot 1 "${METADATA_BIN}" "${OUT}"; then
    echo "ERROR: garbage input should be rejected" >&2
    exit 1
  fi

  echo "Testing argument validation"
  if "${AVIFREMUX}" --irot 9 "${SRC}" "${OUT}"; then
    echo "ERROR: invalid irot should be rejected" >&2
    exit 1
  fi
  if "${AVIFREMUX}" --pasp 0,1 "${SRC}" "${OUT}"; then
    echo "ERROR: invalid pasp should be rejected" >&2
    exit 1
  fi
popd

echo "PASS: test_cmd_remux"
