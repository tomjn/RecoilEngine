#!/usr/bin/env python3
"""Crop a region out of a phase dump PPM and write a PNG.

sips --cropOffset is not an absolute top-left offset, which made every crop land
somewhere unintended. This is unambiguous.

    crop.py <in.ppm> <out.png> <left> <top> <width> <height> [scale]

Coordinates are in dump pixels. The dump is quarter resolution, so divide screen
coordinates by 4, and remember the dump is top-down while widget screen space is
bottom-up.
"""

import sys
import zlib
import struct


def read_ppm(path):
    data = open(path, "rb").read()
    fields, off = [], 0
    while len(fields) < 4:
        end = off
        while data[end:end + 1] not in (b" ", b"\n", b"\t", b"\r"):
            end += 1
        fields.append(data[off:end])
        off = end + 1
    return int(fields[1]), int(fields[2]), data[off:]


def read_png(path):
    """Minimal non-interlaced 8-bit RGB/RGBA reader, returns (w, h, rgb bytes)."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    off, idat, w = 8, bytearray(), None
    while off < len(data):
        ln = struct.unpack(">I", data[off:off + 4])[0]
        tag = data[off + 4:off + 8]
        body = data[off + 8:off + 8 + ln]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (2, 6), f"depth {depth} colour type {ctype}"
            assert body[12] == 0, "interlaced PNG not supported"
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        off += 12 + ln

    nch = 3 if ctype == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        ft = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                b = prev[i]
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line

    if nch == 4:
        rgb = bytearray(w * h * 3)
        for i in range(w * h):
            rgb[i * 3:i * 3 + 3] = out[i * 4:i * 4 + 3]
        return w, h, bytes(rgb)
    return w, h, bytes(out)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    left, top, cw, ch = (int(v) for v in sys.argv[3:7])
    scale = int(sys.argv[7]) if len(sys.argv) > 7 else 1

    w, h, px = read_png(src) if src.lower().endswith(".png") else read_ppm(src)
    left, top = max(0, left), max(0, top)
    cw, ch = min(cw, w - left), min(ch, h - top)

    out = bytearray()
    for y in range(top, top + ch):
        row = px[(y * w + left) * 3:(y * w + left + cw) * 3]
        for _ in range(scale):
            if scale == 1:
                out += row
            else:
                for x in range(cw):
                    out += row[x * 3:x * 3 + 3] * scale

    write_png(dst, cw * scale, ch * scale, bytes(out))
    print(f"{dst} {cw * scale}x{ch * scale}")


if __name__ == "__main__":
    main()
