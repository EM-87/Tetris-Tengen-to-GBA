#!/usr/bin/env python3
"""
render_nes.py — draw the cartridge's own screens, as the NES would.

This is the reference picture. Every graphical decision in the port is
supposed to be answerable by putting this next to a screenshot of the ROM,
so it needs to be the real thing and not an impression of it: the nametable
and attribute table come from RUNNING the cartridge's own
`sendNametableToPPU` (see extract_assets.py for why slicing does not work),
the tiles come from its CHR, and the palettes from its own palette tables.

    python3 tools/render_nes.py /path/to/tetris.nes -o out/

Writes one PNG per screen at the NES's own 256x240, plus a 240x160 crop
showing exactly which part of it a GBA screen can hold.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import extract_assets as ea


# The PPU's master palette lives in hardware, not in the cartridge; this is
# the same widely-used approximation gba/palette.h carries, so the two
# pictures are compared under the same assumption.
NES_RGB = [
    (84, 84, 84), (0, 30, 116), (8, 16, 144), (48, 0, 136),
    (68, 0, 100), (92, 0, 48), (84, 4, 0), (60, 24, 0),
    (32, 42, 0), (8, 58, 0), (0, 64, 0), (0, 60, 0),
    (0, 50, 60), (0, 0, 0), (0, 0, 0), (0, 0, 0),
    (152, 150, 152), (8, 76, 196), (48, 50, 236), (92, 30, 228),
    (136, 20, 176), (160, 20, 100), (152, 34, 32), (120, 60, 0),
    (84, 90, 0), (40, 114, 0), (8, 124, 0), (0, 118, 40),
    (0, 102, 120), (0, 0, 0), (0, 0, 0), (0, 0, 0),
    (236, 238, 236), (76, 154, 236), (120, 124, 236), (176, 98, 236),
    (228, 84, 236), (236, 88, 180), (236, 106, 100), (212, 136, 32),
    (160, 170, 0), (116, 196, 0), (76, 208, 32), (56, 204, 108),
    (56, 180, 204), (60, 60, 60), (0, 0, 0), (0, 0, 0),
    (236, 238, 236), (168, 204, 236), (188, 188, 236), (212, 178, 236),
    (236, 174, 236), (236, 174, 212), (236, 180, 176), (228, 196, 144),
    (204, 210, 120), (180, 222, 120), (168, 226, 144), (152, 226, 180),
    (160, 214, 228), (160, 162, 160), (0, 0, 0), (0, 0, 0),
]

NT_W, NT_H = 32, 30


def tile_pixels(chr_data, index):
    """One 8x8 tile as 64 two-bit values."""
    base = index * ea.NES_TILE_BYTES
    return ea.tile_2bpp_to_pixels(chr_data[base:base + ea.NES_TILE_BYTES])


def render(nametable, attributes, chr_data, palette_set):
    """A full 256x240 picture, as a list of rows of (r, g, b).

    `palette_set` is four palettes of four NES colour indices, as
    read_palette_set hands them over. Entry 0 of the first is the backdrop
    every palette shares — the PPU only stores it once."""
    backdrop = palette_set[0][0]
    out = [[NES_RGB[backdrop & 0x3F]] * (NT_W * 8) for _ in range(NT_H * 8)]
    for row in range(NT_H):
        for col in range(NT_W):
            tile = nametable[row * NT_W + col]
            bank = ea.attribute_palette(attributes, col, row)
            px = tile_pixels(chr_data, tile)
            for y in range(8):
                dst = out[row * 8 + y]
                for x in range(8):
                    v = px[y * 8 + x]
                    if v == 0:
                        continue     # colour 0 is the shared backdrop
                    dst[col * 8 + x] = NES_RGB[palette_set[bank][v] & 0x3F]
    return out


def write_png(path, rows):
    """A PNG with no dependencies beyond zlib — the harness already needs no
    imaging library and this is not a reason to add one."""
    import struct
    import zlib
    h = len(rows)
    w = len(rows[0])
    raw = bytearray()
    for row in rows:
        raw.append(0)                       # filter type 0
        for r, g, b in row:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


# Which screen uses which palette set and which CHR bank. The banks matter as
# much as the palettes: the title has its own 256 tiles (bank 2) and rendering
# it against the game's (bank 0) produces a screenful of letters where the
# cathedral should be — which is a good way to waste an afternoon.
SCREENS = {
    "title": (ea.SCREEN_TITLE, ea.PALETTE_BG_TITLE, 2),
    "menu": (ea.SCREEN_MENU, ea.PALETTE_BG_MENU, 2),
    "game_1p": (ea.SCREEN_1P, ea.PALETTE_BG_GAME, 0),
    "game_2p": (ea.SCREEN_2P, ea.PALETTE_BG_GAME, 0),
    "coop": (ea.SCREEN_COOP, ea.PALETTE_BG_GAME, 0),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("-o", "--out", default="out")
    ap.add_argument("--which", default="all")
    args = ap.parse_args()

    with open(args.rom, "rb") as fh:
        rom = ea.Rom(fh.read())
    os.makedirs(args.out, exist_ok=True)

    for name, (screen, palette, bank) in SCREENS.items():
        if args.which not in ("all", name):
            continue
        nt, attr = ea.read_screen(rom, screen)
        pal = [bytearray(p) for p in ea.read_palette_set(rom, palette)]
        if name.startswith(("game", "coop")):
            # setPlayfieldPaletteFromLevel rewrites background palette 0's
            # entries 1-3 from piecePaletteIndex[level % 10]
            # (main.asm.txt:5328); level 0 is what a game starts on.
            level0 = rom.at(ea.PIECE_PALETTE_ADDR, 3)
            for i in range(3):
                pal[0][1 + i] = level0[i]
        rows = render(nt, attr, rom.chr_bank(bank), pal)
        path = os.path.join(args.out, f"nes_{name}.png")
        write_png(path, rows)
        print(f"escrito {path}  ({NT_W * 8}x{NT_H * 8})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
