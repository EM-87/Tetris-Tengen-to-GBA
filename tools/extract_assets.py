#!/usr/bin/env python3
"""
extract_assets.py — build the GBA port's graphics from an original cartridge dump.

This repo ships no game artwork. Point this at a dump of the original NES
cartridge and it produces the headers `gba/` compiles in: the tile pixels, the
palettes, and the screen layout, all taken from the ROM rather than redrawn.

    python3 tools/extract_assets.py /path/to/tetris.nes -o gba/
    python3 tools/extract_assets.py --self-test        # no ROM needed

What comes out, and why each piece is needed:

  tiles_game.h    The 256-tile playfield/HUD tileset, converted from NES 2bpp
                  planar to GBA 4bpp packed. This is what makes the blocks,
                  the braided border and the lettering look like the original
                  instead of like programmer art.
  tiles_dancers.h The Cossack dancers that perform between levels.
  screen_1p.h     The 1P screen layout, lifted from the ROM's own nametable
                  and attribute table, reflowed from the NES's 32 columns to
                  the GBA's 30 (see BOARD LAYOUT below).
  palettes.h      The NES palette entries the game actually uses.

BOARD LAYOUT — how 32 columns become 30 without touching the game:

    cols  0-1   left border
    cols  2-13  playfield: 12 columns (10 playable + 2 walls) x 20 rows
    cols 14-19  vertical decorative divider (the TETRIS banner)
    cols 20-29  score / stats panel
    cols 30-31  right border

  The GBA is two tiles narrower. Those two come out of the decorative
  divider, which is the only element that is pure ornament: the playfield
  keeps all 12 columns at their original size, both borders survive, and the
  panel keeps its full 10. Nothing is scaled and nothing is cropped.
"""
import argparse
import sys

NES_HEADER = 16
NES_TILE_BYTES = 16
GBA_TILE_BYTES = 32
CHR_BANK = 4096

# Where things live in the PRG, verified against the disassembly in
# reference/disasm/ (see reference/NOTES.md).
NAMETABLE_1P_ADDR = 0xC028      # gameModeNametable1P
NAMETABLE_BYTES = 960           # 32 x 30 tiles
ATTRIBUTE_BYTES = 64
BG_PALETTE_ADDR = 0xA706        # bgPalette2, the in-game background palette
PIECE_PALETTE_ADDR = 0xA788     # piecePaletteIndex0..B

# Screen regions, in NES nametable columns.
COL_BORDER_L = (0, 2)
COL_PLAYFIELD = (2, 14)
COL_DIVIDER = (14, 20)
COL_PANEL = (20, 30)
COL_BORDER_R = (30, 32)
DIVIDER_TRIM = 2                # columns dropped from the divider for the GBA

ROW_PLAYFIELD = (8, 28)         # 20 rows

# The NES master palette lives in the PPU, not the cartridge, so no dump can
# supply it. This is a widely-used approximation; different references (and
# different consoles) disagree slightly.
NES_MASTER = [
    (84,84,84),(0,30,116),(8,16,144),(48,0,136),(68,0,100),(92,0,48),(84,4,0),(60,24,0),
    (32,42,0),(8,58,0),(0,64,0),(0,60,0),(0,50,60),(0,0,0),(0,0,0),(0,0,0),
    (152,150,152),(8,76,196),(48,50,236),(92,30,228),(136,20,176),(160,20,100),(152,34,32),(120,60,0),
    (84,90,0),(40,114,0),(8,124,0),(0,118,40),(0,102,120),(0,0,0),(0,0,0),(0,0,0),
    (236,238,236),(76,154,236),(120,124,236),(176,98,236),(228,84,236),(236,88,180),(236,106,100),(212,136,32),
    (160,170,0),(116,196,0),(76,208,32),(56,204,108),(56,180,204),(60,60,60),(0,0,0),(0,0,0),
    (236,238,236),(168,204,236),(188,188,236),(212,178,236),(236,174,236),(236,174,212),(236,180,176),(228,196,144),
    (204,210,120),(180,222,120),(168,226,144),(152,226,180),(160,214,228),(160,162,160),(0,0,0),(0,0,0),
]


class Rom:
    """An iNES cartridge dump, addressed the way the disassembly does."""

    def __init__(self, data: bytes):
        if data[:4] != b"NES\x1a":
            raise ValueError("not an iNES file (missing NES\\x1a magic)")
        self.prg_banks, self.chr_banks = data[4], data[5]
        if self.chr_banks == 0:
            raise ValueError("this dump uses CHR RAM, so it holds no tile data")
        self.data = data
        self.prg_off = NES_HEADER
        self.chr_off = NES_HEADER + self.prg_banks * 16384
        expected = self.chr_off + self.chr_banks * 8192
        if len(data) < expected:
            raise ValueError(f"dump is truncated: expected {expected} bytes, got {len(data)}")

    def at(self, addr: int, count: int) -> bytes:
        """Read `count` bytes from a PRG address as the 6502 sees it ($8000+)."""
        off = self.prg_off + (addr - 0x8000)
        return self.data[off:off + count]

    def chr_bank(self, index: int) -> bytes:
        off = self.chr_off + index * CHR_BANK
        return self.data[off:off + CHR_BANK]


def tile_2bpp_to_pixels(tile: bytes) -> list:
    """One 16-byte NES tile -> 64 pixel values (0-3), row-major.

    NES tiles are PLANAR: bytes 0-7 hold bit 0 of each row, bytes 8-15 hold
    bit 1. The leftmost pixel is the HIGH bit of each byte.
    """
    pixels = []
    for row in range(8):
        plane0, plane1 = tile[row], tile[row + 8]
        for bit in range(7, -1, -1):
            pixels.append(((plane0 >> bit) & 1) | (((plane1 >> bit) & 1) << 1))
    return pixels


def pixels_to_gba_4bpp(pixels: list) -> bytes:
    """64 pixel values -> 32 bytes of GBA 4bpp: two pixels per byte, low first."""
    return bytes((pixels[i] & 0xF) | ((pixels[i + 1] & 0xF) << 4)
                 for i in range(0, 64, 2))


def convert_tiles(chr_data: bytes) -> bytes:
    out = bytearray()
    for off in range(0, len(chr_data), NES_TILE_BYTES):
        tile = chr_data[off:off + NES_TILE_BYTES]
        if len(tile) < NES_TILE_BYTES:
            break
        out += pixels_to_gba_4bpp(tile_2bpp_to_pixels(tile))
    return bytes(out)


def attribute_palette(attributes: bytes, col: int, row: int) -> int:
    """Which of the four background palettes the NES uses for a given tile.

    One attribute byte covers a 4x4-tile block, two bits per 2x2 quadrant.
    The GBA picks a palette per tile, which is strictly finer, so this
    converts without losing anything.
    """
    byte = attributes[(row // 4) * 8 + (col // 4)]
    quadrant = ((row % 4) // 2) * 2 + ((col % 4) // 2)
    return (byte >> (quadrant * 2)) & 3


def reflow_screen(nametable: bytes, attributes: bytes):
    """NES 32-column screen -> GBA 30-column screen.

    Returns (tiles, palettes) as 30x20-per-row lists covering 30x30 tiles;
    only the first 20 rows are visible on a GBA, but the full height is kept
    so the caller can choose the vertical window.

    The two columns the GBA lacks are taken from the decorative divider. Every
    other region keeps its exact width and its exact tiles.
    """
    keep_cols = (list(range(*COL_BORDER_L)) +
                 list(range(*COL_PLAYFIELD)) +
                 list(range(COL_DIVIDER[0], COL_DIVIDER[1] - DIVIDER_TRIM)) +
                 list(range(*COL_PANEL)) +
                 list(range(*COL_BORDER_R)))
    assert len(keep_cols) == 30, f"expected 30 columns after trimming, got {len(keep_cols)}"

    tiles, palettes = [], []
    for row in range(30):
        for col in keep_cols:
            tiles.append(nametable[row * 32 + col])
            palettes.append(attribute_palette(attributes, col, row))
    return tiles, palettes, keep_cols


def playfield_origin(keep_cols) -> tuple:
    """Where the playfield's top-left tile ends up after the reflow."""
    return keep_cols.index(COL_PLAYFIELD[0]), ROW_PLAYFIELD[0]


def emit_tiles_header(name, guard, tiles, source):
    count = len(tiles) // GBA_TILE_BYTES
    lines = [
        "/*",
        f" * {guard.lower()}.h — GBA tile data converted from the original NES CHR ROM.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source} ({count} tiles).",
        " *",
        " * This is artwork from a cartridge dump you supplied. It is generated",
        " * locally and kept out of version control (see .gitignore).",
        " */",
        f"#ifndef {guard}_H",
        f"#define {guard}_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define {guard}_TILE_COUNT {count}",
        "",
        f"static const uint8_t {name}[{len(tiles)}] = {{",
    ]
    for i in range(0, len(tiles), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in tiles[i:i + 16]) + ",")
    lines += ["};", "", f"#endif /* {guard}_H */", ""]
    return "\n".join(lines)


def emit_screen_header(tiles, palettes, keep_cols, source):
    origin_x, origin_y = playfield_origin(keep_cols)
    lines = [
        "/*",
        " * screen_1p.h — the 1P screen layout, taken from the ROM's own nametable.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * The NES screen is 32 tiles wide and the GBA is 30, so two columns are",
        " * dropped from the decorative divider between the playfield and the score",
        " * panel — the one element that is pure ornament. The playfield keeps all",
        " * 12 of its columns at original size, both borders survive, and the panel",
        " * keeps its full 10. Nothing is scaled and nothing is cropped.",
        " *",
        " * Tiles marked 0 are blank in the ROM because the game draws over them at",
        " * runtime; the port does the same.",
        " */",
        "#ifndef SCREEN_1P_H",
        "#define SCREEN_1P_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SCREEN_1P_W 30",
        "#define SCREEN_1P_H_TILES 30",
        "",
        "/* Where the playfield's top-left cell sits in the layout above. */",
        f"#define SCREEN_1P_FIELD_TX {origin_x}",
        f"#define SCREEN_1P_FIELD_TY {origin_y}",
        "",
        "static const uint8_t kScreen1pTiles[900] = {",
    ]
    for i in range(0, len(tiles), 30):
        lines.append("    " + ", ".join(f"0x{t:02X}" for t in tiles[i:i + 30]) + ",")
    lines += ["};", "",
              "/* Which of the four NES background palettes each tile uses. The NES",
              " * stores this per 16x16 block; the GBA picks a palette per tile, so",
              " * this expands losslessly. */",
              "static const uint8_t kScreen1pPalettes[900] = {"]
    for i in range(0, len(palettes), 30):
        lines.append("    " + ", ".join(str(p) for p in palettes[i:i + 30]) + ",")
    lines += ["};", "", "#endif /* SCREEN_1P_H */", ""]
    return "\n".join(lines)


def emit_palette_header(bg_palette, piece_palettes, source):
    lines = [
        "/*",
        " * palettes_rom.h — the palette entries the game actually uses.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * These are NES colour INDICES, straight from the cartridge. Turning an",
        " * index into a colour needs the PPU's master palette, which is in the",
        " * console's hardware rather than the cartridge — gba/palette.h carries a",
        " * widely-used approximation of it.",
        " */",
        "#ifndef PALETTES_ROM_H",
        "#define PALETTES_ROM_H",
        "",
        "#include <stdint.h>",
        "",
        "/* bgPalette2: the four background palettes of the in-game screen, four",
        " * entries each (the first of each is the shared backdrop). */",
        "static const uint8_t kRomBgPalette[16] = {",
        "    " + ", ".join(f"0x{b:02X}" for b in bg_palette),
        "};",
        "",
        "/* piecePaletteIndex0..B: three colours each, indexed by piece id for a",
        " * falling piece and by the level's ones digit for the field. */",
        "static const uint8_t kRomPiecePalettes[12][3] = {",
    ]
    for i, entry in enumerate(piece_palettes):
        lines.append("    { " + ", ".join(f"0x{b:02X}" for b in entry) + f" }}, /* {i} */")
    lines += ["};", "", "#endif /* PALETTES_ROM_H */", ""]
    return "\n".join(lines)


def self_test() -> int:
    """Verifies the format conversions against hand-worked cases, so the tool
    can be trusted before it is pointed at a ROM."""
    failures = []

    if tile_2bpp_to_pixels(bytes([0xFF] * 8 + [0x00] * 8)) != [1] * 64:
        failures.append("plano 0 lleno deberia dar 64 pixeles de color 1")
    if tile_2bpp_to_pixels(bytes([0xFF] * 16)) != [3] * 64:
        failures.append("ambos planos llenos deberian dar color 3")
    if tile_2bpp_to_pixels(bytes(16)) != [0] * 64:
        failures.append("tile vacio deberia dar color 0")

    px = tile_2bpp_to_pixels(bytes([0x80] + [0x00] * 15))
    if px[0] != 1 or any(px[1:]):
        failures.append("el bit alto deberia ser el pixel de la izquierda")

    if pixels_to_gba_4bpp([1, 2] + [0] * 62)[0] != 0x21:
        failures.append("empaquetado 4bpp invertido")
    if len(convert_tiles(bytes([0xFF] * 16))) != GBA_TILE_BYTES:
        failures.append("un tile de NES deberia producir 32 bytes de GBA")

    # Attribute unpacking: byte 0b11_10_01_00 covers a 4x4 block whose four
    # 2x2 quadrants take palettes 0,1,2,3 in reading order.
    attrs = bytes([0b11100100] + [0] * 63)
    got = [attribute_palette(attrs, c, r) for r, c in ((0, 0), (0, 2), (2, 0), (2, 2))]
    if got != [0, 1, 2, 3]:
        failures.append(f"desempaquetado de atributos incorrecto: {got}")

    # The reflow must keep exactly 30 columns and must not touch the playfield.
    nt = bytes(range(256)) * 4
    tiles, palettes, keep = reflow_screen(nt[:960], bytes(64))
    if len(tiles) != 900 or len(palettes) != 900:
        failures.append("el reflow deberia producir 30x30 tiles")
    if keep[:14] != list(range(14)):
        failures.append("el reflow movio el playfield o el borde izquierdo")
    if keep[-12:] != list(range(20, 32)):
        failures.append("el reflow movio el panel derecho o el borde derecho")
    origin_x, _ = playfield_origin(keep)
    if origin_x != COL_PLAYFIELD[0]:
        failures.append("el playfield no quedo en su columna original")

    if failures:
        for f in failures:
            print(f"FALLA: {f}")
        return 1
    print("OK: conversion de tiles, atributos y reflow de pantalla verificados.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", nargs="?", help="iNES cartridge dump")
    ap.add_argument("-o", "--outdir", default="gba", help="where to write the headers")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.rom:
        ap.error("give a ROM, or --self-test")

    rom = Rom(open(args.rom, "rb").read())
    src = args.rom

    nametable = rom.at(NAMETABLE_1P_ADDR, NAMETABLE_BYTES)
    attributes = rom.at(NAMETABLE_1P_ADDR + NAMETABLE_BYTES, ATTRIBUTE_BYTES)
    tiles, palettes, keep_cols = reflow_screen(nametable, attributes)

    bg_palette = rom.at(BG_PALETTE_ADDR, 16)
    piece_palettes = [rom.at(PIECE_PALETTE_ADDR + i * 3, 3) for i in range(12)]

    outputs = {
        "tiles_game.h": emit_tiles_header(
            "kGameTiles", "TILES_GAME", convert_tiles(rom.chr_bank(0)), f"{src} [game]"),
        "tiles_dancers.h": emit_tiles_header(
            "kDancerTiles", "TILES_DANCERS", convert_tiles(rom.chr_bank(1)), f"{src} [dancers]"),
        "screen_1p.h": emit_screen_header(tiles, palettes, keep_cols, src),
        "palettes_rom.h": emit_palette_header(bg_palette, piece_palettes, src),
    }

    import os
    for filename, content in outputs.items():
        path = os.path.join(args.outdir, filename)
        with open(path, "w") as fh:
            fh.write(content)
        print(f"escrito {path}")

    ox, oy = playfield_origin(keep_cols)
    print(f"playfield en la pantalla reflowed: columna {ox}, fila {oy} (12x20 tiles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
