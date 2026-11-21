#!/usr/bin/env python3
"""Compare a region-decoded Y4M against a crop of a full-decoded Y4M, plane by plane.

Usage: y4m_crop_equal.py FULL.y4m REGION.y4m X Y W H

Exits 0 if every Y/U/V sample of REGION equals the corresponding (X,Y,W,H) crop
of FULL (chroma coordinates are derived from the pixel format's subsampling).
No third-party modules are used; 8-bit single-frame Y4M only.
"""
import sys


def parse_y4m(path):
    with open(path, "rb") as f:
        data = f.read()
    nl = data.index(b"\n")
    header = data[:nl].decode("ascii").split()
    assert header[0] == "YUV4MPEG2", "not a Y4M file: %s" % path
    width = height = None
    chroma = "420"
    for token in header[1:]:
        if token.startswith("W"):
            width = int(token[1:])
        elif token.startswith("H"):
            height = int(token[1:])
        elif token.startswith("C"):
            chroma = token[1:]
    # Locate the single frame: header line, then a "FRAME" line, then raw planes.
    frame_marker = b"FRAME"
    fi = data.index(frame_marker)
    frame_start = data.index(b"\n", fi) + 1
    planes = data[frame_start:]
    if chroma.startswith("444"):
        sx = sy = 1
        mono = False
    elif chroma.startswith("422"):
        sx, sy, mono = 2, 1, False
    elif chroma.startswith("420"):
        sx, sy, mono = 2, 2, False
    elif chroma.startswith("400") or chroma == "mono":
        sx = sy = 1
        mono = True
    else:
        raise SystemExit("unsupported chroma: %s" % chroma)
    cw, ch = (width + sx - 1) // sx, (height + sy - 1) // sy
    y_size = width * height
    c_size = 0 if mono else cw * ch
    y = planes[:y_size]
    u = planes[y_size:y_size + c_size] if not mono else b""
    v = planes[y_size + c_size:y_size + 2 * c_size] if not mono else b""
    return width, height, sx, sy, mono, (y, u, v)


def plane_crop(plane, pw, ph, x, y, w, h):
    out = bytearray(w * h)
    for row in range(h):
        src = (y + row) * pw + x
        dst = row * w
        out[dst:dst + w] = plane[src:src + w]
    return bytes(out)


def main():
    if len(sys.argv) != 7:
        raise SystemExit(__doc__)
    full_path, region_path = sys.argv[1], sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:])

    fw, fh, fsx, fsy, fmono, (fy, fu, fv) = parse_y4m(full_path)
    rw, rh, rsx, rsy, rmono, (ry, ru, rv) = parse_y4m(region_path)
    if (rw, rh, fsx, fsy, fmono) != (w, h, rsx, rsy, rmono):
        raise SystemExit("format/size mismatch: full %dx%d s%d/%d mono=%s vs region %dx%d"
                         % (fw, fh, fsx, fsy, fmono, rw, rh))

    checks = [("Y", fy, ry, fw, rw, x, y, w, h, 1, 1)]
    if not fmono:
        checks.append(("U", fu, ru, (fw + fsx - 1) // fsx, (rw + rsx - 1) // rsx,
                       x // fsx, y // fsy, w // fsx, h // fsy, fsx, fsy))
        checks.append(("V", fv, rv, (fw + fsx - 1) // fsx, (rw + rsx - 1) // rsx,
                       x // fsx, y // fsy, w // fsx, h // fsy, fsx, fsy))

    for name, fp, rp, fpw, _rpw, px, py, pw_, ph_, _sx, _sy in checks:
        expected = plane_crop(fp, fpw, None, px, py, pw_, ph_)
        if expected != rp:
            diffs = sum(1 for a, b in zip(expected, rp) if a != b)
            raise SystemExit("plane %s mismatch: %d differing samples" % (name, diffs))
    print("Y4M crop equality OK (%s, %dx%d at %d,%d)" % ("mono" if fmono else "4%d%d" % (fsx, fsy), w, h, x, y))


if __name__ == "__main__":
    main()
