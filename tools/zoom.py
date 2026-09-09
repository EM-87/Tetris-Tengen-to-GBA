#!/usr/bin/env python3
"""
zoom.py — crop and magnify a PNG, so a tile can actually be looked at.

Comparing a 256x240 NES screen with a 240x160 GBA one by eye needs the eye to
be able to see the pixels. Nearest-neighbour only: a smoothed enlargement of
pixel art hides exactly the errors this is for.

    python3 tools/zoom.py in.png out.png --crop X,Y,W,H --scale 4 [--grid 8]
"""
import argparse
import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path} is not a PNG")
    pos = 8
    idat = bytearray()
    w = h = depth = ctype = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif tag == b"IDAT":
            idat += body
        elif tag == b"IEND":
            break
        pos += 12 + length
    if depth != 8 or ctype not in (2, 6):
        sys.exit(f"{path}: only 8-bit RGB/RGBA PNGs are handled here")
    stride = 3 if ctype == 2 else 4
    raw = zlib.decompress(bytes(idat))

    rows = []
    prev = bytearray(w * stride)
    pos = 0
    for _ in range(h):
        filt = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + w * stride])
        pos += 1 + w * stride
        for i in range(len(line)):
            a = line[i - stride] if i >= stride else 0
            b = prev[i]
            c = prev[i - stride] if i >= stride else 0
            if filt == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filt == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filt == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line
        rows.append([tuple(line[x * stride:x * stride + 3]) for x in range(w)])
    return rows


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw += bytes(px[:3])

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--crop", help="X,Y,W,H in source pixels")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--grid", type=int, default=0,
                     help="draw a line every N source pixels (8 = tile grid)")
    args = ap.parse_args()

    rows = read_png(args.src)
    if args.crop:
        x, y, w, h = (int(v) for v in args.crop.split(","))
        rows = [r[x:x + w] for r in rows[y:y + h]]

    s = args.scale
    out = []
    for sy, row in enumerate(rows):
        base = []
        for px in row:
            base.extend([px] * s)
        for sub in range(s):
            line = list(base)
            # A one-pixel rule ON the tile boundary, not over the whole tile:
            # it has to show where the grid falls without hiding what is in it.
            if args.grid:
                if sub == 0 and sy % args.grid == 0:
                    line = [(255, 0, 255)] * len(line)
                else:
                    for sx in range(0, len(row), args.grid):
                        line[sx * s] = (255, 0, 255)
            out.append(line)
    write_png(args.dst, out)
    print(f"escrito {args.dst} ({len(out[0])}x{len(out)})")


if __name__ == "__main__":
    main()
