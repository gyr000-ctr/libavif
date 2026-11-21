#!/bin/bash
# Copyright 2026 Google LLC
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
# Tests for region (partial) decoding of grid images.
# Requires ImageMagick (magick) for cropping and pixel-exact comparisons.
# Subsampled (4:2:0) cases are compared at the YUV plane level via y4m_crop_equal.py, because
# YUV->RGB chroma upsampling at the cropped edges uses samples outside the region by design.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/cmd_test_common.sh" || exit

# Input file paths.
INPUT_PNG="${TESTDATA_DIR}/paris_icc_exif_xmp.png" # 403 x 302 px

# Output file names.
GRID_2x2="region_grid_2x2.avif"
GRID_2x2_420="region_grid_2x2_420.avif"
GRID_2x2_400="region_grid_2x2_400.avif"
GRID_7x5="region_grid_7x5.avif"
GRID_ALPHA="region_grid_alpha.avif"
# Overlapping/irregular grids (encoded tile larger than the assembly pitch) cannot be region
# decoded and must report a distinct "not supported" error.
GRID_SCALED="${TESTDATA_DIR}/color_grid_alpha_grid_shared_tiles.avif" # 80x80, 2x2, 64px tiles in 80px canvas
# Grid color item with overlapping vertical cells plus a single (non-grid) alpha item.
GRID_COLOR_SINGLE_ALPHA="${TESTDATA_DIR}/color_grid_alpha_nogrid.avif" # 80x80, 1x2 color grid
SINGLE="region_single.avif"
FULL_PNG="region_full.png"
CROP_PNG="region_crop.png"
REGION_PNG="region_out.png"
ALPHA_PNG="region_alpha_source.png"
SIZED_PNG="region_448x320.png"

cleanup() {
  pushd ${TMP_DIR}
    rm -f -- "${GRID_2x2}" "${GRID_2x2_420}" "${GRID_2x2_400}" "${GRID_7x5}" "${GRID_ALPHA}" \
             "${SINGLE}" "${FULL_PNG}" "${CROP_PNG}" "${REGION_PNG}" "${ALPHA_PNG}" "${SIZED_PNG}" \
             region_*.png region_*.txt region_*.y4m
  popd
}
trap cleanup EXIT

# Compares two images pixel by pixel; fails (returns non-zero) if any pixel differs.
expect_pixels_equal() {
  local a="$1"
  local b="$2"
  local ae
  ae=$(magick compare -metric AE "${a}" "${b}" null: 2>&1) || true
  if [[ "${ae}" != "0" && "${ae}" != "0 (0)" ]]; then
    echo "Pixel mismatch (AE=${ae}) between ${a} and ${b}" >&2
    return 1
  fi
}

# Checks that the last avifdec output decoded exactly the expected number of tiles.
expect_decoded_tiles() {
  local logfile="$1"
  local expected="$2"
  grep -q "Region decode: ${expected}/" "${logfile}"
}

# Compares a region-decoded Y4M with a crop of a full-decoded Y4M, sample by sample on every plane.
expect_y4m_crop_equal() {
  local full_y4m="$1"
  local region_y4m="$2"
  shift 2
  python3 -I "${SCRIPT_DIR}/y4m_crop_equal.py" "${full_y4m}" "${region_y4m}" "$@"
}

pushd ${TMP_DIR}
  echo "Encoding test fixtures"
  "${AVIFENC}" -s 8 "${INPUT_PNG}" --grid 2x2 --ignore-icc -o "${GRID_2x2}"
  # Even dimensions allow the 4:2:0 and 7x5 grids.
  magick "${INPUT_PNG}" -resize 400x300! "${SIZED_PNG}"
  "${AVIFENC}" -s 8 "${SIZED_PNG}" --grid 2x2 --yuv 420 --ignore-icc -o "${GRID_2x2_420}"
  "${AVIFENC}" -s 8 "${INPUT_PNG}" --grid 2x2 --yuv 400 --ignore-icc -o "${GRID_2x2_400}"
  # 448x320 with a 7x5 grid gives 64x64 tiles, one per cell.
  magick "${INPUT_PNG}" -resize 448x320! "${SIZED_PNG}"
  "${AVIFENC}" -s 8 "${SIZED_PNG}" --grid 7x5 --ignore-icc -o "${GRID_7x5}"
  # A grid with an alpha plane.
  magick -size 256x256 radial-gradient:red-blue -alpha set -channel A -evaluate set 70% +channel "${ALPHA_PNG}"
  "${AVIFENC}" -s 8 "${ALPHA_PNG}" --grid 2x2 --ignore-icc -o "${GRID_ALPHA}"
  # A non-grid (single tile) image.
  "${AVIFENC}" -s 8 "${INPUT_PNG}" --ignore-icc -o "${SINGLE}"

  echo "Testing region decode pixel equality with a full-image decode + crop"
  # Region spanning all 4 tiles: [100,80) 160x120.
  "${AVIFDEC}" "${GRID_2x2}" "${FULL_PNG}"
  "${AVIFDEC}" --region 100,80,160,120 "${GRID_2x2}" "${REGION_PNG}" > region_log.txt
  magick "${FULL_PNG}" -crop 160x120+100+80 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  # All four tiles must have been decoded (region intersects the full 2x2 grid).
  expect_decoded_tiles region_log.txt 4

  echo "Testing region contained in a single tile decodes only that tile"
  "${AVIFDEC}" --region 0,0,160,120 "${GRID_2x2}" "${REGION_PNG}" > region_log.txt
  magick "${FULL_PNG}" -crop 160x120+0+0 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  expect_decoded_tiles region_log.txt 1

  echo "Testing a region straddling the internal tile boundaries"
  # Tile boundary in this 403x302 2x2 grid is at x=202, y=151, so this crosses all four tiles.
  "${AVIFDEC}" --region 190,140,30,30 "${GRID_2x2}" "${REGION_PNG}" > region_log.txt
  magick "${FULL_PNG}" -crop 30x30+190+140 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  expect_decoded_tiles region_log.txt 4

  echo "Testing region reaching the bottom/right edges"
  # (203,151) lies entirely inside the single bottom-right tile.
  "${AVIFDEC}" --region 203,151,200,151 "${GRID_2x2}" "${REGION_PNG}" > region_log.txt
  magick "${FULL_PNG}" -crop 200x151+203+151 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  expect_decoded_tiles region_log.txt 1

  echo "Testing a single-pixel region"
  "${AVIFDEC}" --region 10,10,1,1 "${GRID_2x2}" "${REGION_PNG}"
  magick identify "${REGION_PNG}" | grep -q " 1x1 "
  magick "${FULL_PNG}" -crop 1x1+10+10 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"

  echo "Testing that the decoded tile count equals the tile coverage (7x5 grid, 64x64 tiles)"
  # 300x200 at (60,50): columns 0..5 (6), rows 0..3 (4) -> 24 tiles.
  "${AVIFDEC}" --region 60,50,300,200 "${GRID_7x5}" "${REGION_PNG}" > region_log.txt
  expect_decoded_tiles region_log.txt 24
  "${AVIFDEC}" "${GRID_7x5}" "${FULL_PNG}"
  magick "${FULL_PNG}" -crop 300x200+60+50 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  # Single column of 5 tiles.
  "${AVIFDEC}" --region 0,0,64,320 "${GRID_7x5}" "${REGION_PNG}" > region_log.txt
  expect_decoded_tiles region_log.txt 5
  magick "${FULL_PNG}" -crop 64x320+0+0 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  # Single tile only.
  "${AVIFDEC}" --region 64,64,64,64 "${GRID_7x5}" "${REGION_PNG}" > region_log.txt
  expect_decoded_tiles region_log.txt 1

  echo "Testing region decode on a 4:2:0 grid at the YUV plane level (even coordinates)"
  "${AVIFDEC}" "${GRID_2x2_420}" region_full.y4m > /dev/null
  "${AVIFDEC}" --region 100,80,160,120 "${GRID_2x2_420}" region_region.y4m > /dev/null
  expect_y4m_crop_equal region_full.y4m region_region.y4m 100 80 160 120
  # A single 4:2:0 tile and a full-image region.
  "${AVIFDEC}" --region 0,0,200,150 "${GRID_2x2_420}" region_region.y4m > /dev/null
  expect_y4m_crop_equal region_full.y4m region_region.y4m 0 0 200 150
  "${AVIFDEC}" --region 0,0,400,300 "${GRID_2x2_420}" region_region.y4m > /dev/null
  expect_y4m_crop_equal region_full.y4m region_region.y4m 0 0 400 300

  echo "Testing region decode on a monochrome (4:0:0) grid with an odd origin"
  "${AVIFDEC}" "${GRID_2x2_400}" "${FULL_PNG}"
  "${AVIFDEC}" --region 101,81,150,110 "${GRID_2x2_400}" "${REGION_PNG}"
  magick "${FULL_PNG}" -crop 150x110+101+81 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"

  echo "Testing region decode with an alpha plane"
  "${AVIFDEC}" "${GRID_ALPHA}" "${FULL_PNG}"
  "${AVIFDEC}" --region 100,80,100,100 "${GRID_ALPHA}" "${REGION_PNG}" > region_log.txt
  magick "${FULL_PNG}" -crop 100x100+100+80 +repage "${CROP_PNG}"
  expect_pixels_equal "${REGION_PNG}" "${CROP_PNG}"
  expect_decoded_tiles region_log.txt 4

  echo "Testing overlapping grid layouts report a distinct not-supported error"
  "${AVIFDEC}" --region 10,10,50,50 "${GRID_SCALED}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "not supported" region_err.txt
  "${AVIFDEC}" --region 0,0,1,1 "${GRID_COLOR_SINGLE_ALPHA}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "not supported" region_err.txt
  # Full-image decoding of these files still works (region decoding is opt-in).
  "${AVIFDEC}" "${GRID_SCALED}" "${FULL_PNG}" > /dev/null
  "${AVIFDEC}" "${GRID_COLOR_SINGLE_ALPHA}" "${FULL_PNG}" > /dev/null

  echo "Testing a region covering the whole image is equivalent to a full decode"
  "${AVIFDEC}" "${GRID_2x2}" "${FULL_PNG}" > /dev/null
  "${AVIFDEC}" --region 0,0,403,302 "${GRID_2x2}" "${REGION_PNG}" > region_log.txt
  expect_decoded_tiles region_log.txt 4
  expect_pixels_equal "${REGION_PNG}" "${FULL_PNG}"

  echo "Testing full-image path output is unchanged without --region"
  "${AVIFDEC}" "${GRID_2x2}" "region_full_a.png"
  "${AVIFDEC}" "${GRID_2x2}" "region_full_b.png"
  cmp -s region_full_a.png region_full_b.png

  echo "Testing out-of-bounds regions report a distinct invalid-region error"
  "${AVIFDEC}" --region 300,0,200,100 "${GRID_2x2}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "Invalid decode region" region_err.txt
  "${AVIFDEC}" --region 0,250,100,100 "${GRID_2x2}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "Invalid decode region" region_err.txt

  echo "Testing empty (zero-sized) regions report a distinct error"
  "${AVIFDEC}" --region 0,0,0,10 "${GRID_2x2}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "invalid region" region_err.txt
  "${AVIFDEC}" --region 0,0,10,0 "${GRID_2x2}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "invalid region" region_err.txt

  echo "Testing chroma-misaligned region on a 4:2:0 grid"
  "${AVIFDEC}" --region 101,80,100,100 "${GRID_2x2_420}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "Invalid decode region" region_err.txt

  echo "Testing region decoding on an unsupported (non-grid) image"
  "${AVIFDEC}" --region 0,0,50,50 "${SINGLE}" "${REGION_PNG}" 2> region_err.txt && exit 1
  grep -qi "not supported" region_err.txt
popd

exit 0
