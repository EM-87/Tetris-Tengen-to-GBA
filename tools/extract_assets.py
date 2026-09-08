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
  audio_prg.h     The slice of the cartridge's PROGRAM code that contains its
                  sound engine and all of its music. The port does not
                  reimplement the engine — it runs this, on a small 6502
                  interpreter, and translates what it writes to the NES APU
                  into GBA sound registers. See gba/nes_audio.c and the
                  AUDIO section below.
  screen_1p.h     The 1P screen layout, lifted from the ROM's own nametable
                  and attribute table, reflowed from the NES's 32 columns to
                  the GBA's 30 (see BOARD LAYOUT below).
  palettes.h      The NES palette entries the game actually uses.

BOARD LAYOUT — how 32 columns become 30 without touching the game:

    cols  0-1   braided frame: the playfield's left wall
    cols  2-11  playfield: the ten playable columns x 20 rows
    cols 12-13  braided frame: the right wall
    cols 14-17  vertical decorative divider (the TETRIS banner)
    cols 18-19  braided frame again — the left wall of a SECOND playfield
                area, which 2P uses and 1P covers with its score panel
    cols 20-29  that second area: the score / stats panel in 1P
    cols 30-31  right frame

  The walls are frame ART, not blocks drawn from the playfield buffer. Three
  things say so: exactly ten blank columns sit between the frames, the ROM's
  line-clear sweep runs from x $10 to $58 (columns 2 to 11 and no further),
  and the pause plaque is blitted at column 12, right where the frame starts.

  The GBA is two tiles narrower. Those two come out of columns 18-19, the
  frame around the second playfield area — the one element the 1P screen has
  no use for, since it draws its panel over that area anyway. The playfield,
  its frames, the banner and the full 10-column panel all survive at original
  size. Nothing is scaled and nothing is cropped.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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

# The between-levels dancers. Each pose is four sprite tile ids forming a
# 2x2, i.e. one 16x16 figure; the animation driver (main.asm.txt:6392-6499)
# walks a script of pointers into this table, advancing one every 8 frames.
# The labelled extent runs $C8BC..$C9FF.
DANCER_POSE_ADDR = 0xC8BC
DANCER_POSE_END = 0xCA00

# The title screen: "TENGEN PRESENTS / THE SOVIET MIND GAME / TETRIS" over
# St Basil's Cathedral. Its nametable carries no attribute table (the next
# thing in the ROM is fireworksData00), because the whole screen uses one
# palette — bgPalette0's first entry, the blues the artwork is drawn in.
TITLE_NAMETABLE_ADDR = 0xCA00
TITLE_BG_PALETTE_ADDR = 0xA6E6  # bgPalette0

# How the 32x30 title is composed down to the GBA's 30x20. The border is four
# tiles thick on every side; dropping the top and bottom of it (and the very
# tip of the tallest spire) is enough to fit everything that matters, while
# three of the four side border columns stay. Nothing is scaled.
TITLE_KEEP_COLS = (1, 31)       # 30 columns: 3 of the border each side + content
TITLE_ROW_BLOCKS = (
    (4, 12),   # TENGEN / PRESENTS / THE SOVIET MIND GAME / the TETRIS logo
    (13, 25),  # the cathedral, from just below its topmost spire tip
)

# The menu screen the ROM uses for its selection screens: a decorative frame
# on all four sides with an empty middle it writes text into at runtime. Its
# borders are what the level selector is framed with here.
MENU_NAMETABLE_ADDR = 0xB8A8
# 30 columns: both side borders intact, the two the GBA lacks taken from the
# empty middle where they cost nothing.
MENU_COL_BLOCKS = ((0, 15), (17, 32))
# 20 rows: the top and bottom borders plus 16 rows of the empty middle. The
# ROM's SCORE/LINES/LEVEL header (rows 2-7) is skipped — this is a menu.
MENU_ROW_BLOCKS = ((0, 2), (8, 24), (28, 30))

# THE SOUND ENGINE
#
# Tengen's audio is a dense piece of 6502 with vibrato, portamento, per-channel
# envelopes and a sound-effect priority system, working almost entirely through
# unlabelled RAM. Transcribing it by hand would mean guessing, so the port
# doesn't: it runs the cartridge's own code.
#
# Two entry points are all that is needed, and they take everything else with
# them: setMusicOrSoundEffect queues a track or effect, updateAudio runs one
# frame of playback. The engine touches nothing but RAM and $4000-$4017 — no
# PPU, no mapper — which is what makes a 12KB emulator enough. That claim is
# not assumed: the extraction below runs every music track and sound effect the
# game has and records exactly which addresses were touched.
AUDIO_SET_TRACK_ADDR = 0xCFB1   # setMusicOrSoundEffect
AUDIO_UPDATE_ADDR = 0xCFCA      # updateAudio, once per frame
AUDIO_TRACK_IDS = tuple(range(0x01, 0x1A))  # MUSIC_* and SOUND_* in constants.asm.txt
AUDIO_PROBE_FRAMES = 1200       # ~20s of each, enough to reach every branch taken
AUDIO_SLICE_ALIGN = 0x100

# A golden recording of the engine's APU output, so `make gba-check` can prove
# the hand-written 6502 interpreter in gba/nes6502.c behaves exactly like the
# reference one here. Any divergence — a mis-set flag, a wrong addressing mode
# — shows up as a mismatched frame instead of as music that is subtly wrong in
# a way nobody notices.
AUDIO_GOLDEN_TRACK = 0x09       # MUSIC_TITLESCREEN, the first thing that plays
AUDIO_GOLDEN_FRAMES = 400

# Screen regions, in NES nametable columns. See BOARD LAYOUT above.
COL_FRAME_L = (0, 2)            # braid; the playfield's left wall
COL_PLAYFIELD = (2, 14)         # ten playable columns plus the right frame
COL_DIVIDER = (14, 20)          # TETRIS banner (14-17) + the second area's frame
COL_PANEL = (20, 30)
COL_BORDER_R = (30, 32)
DIVIDER_TRIM = 2                # drops cols 18-19: the second field's left frame

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

    The two columns the GBA lacks are taken from the far end of the divider
    block — columns 18-19, the braided frame around the second playfield area
    that the 1P screen covers with its panel anyway. Every other region keeps
    its exact width and its exact tiles.
    """
    keep_cols = (list(range(*COL_FRAME_L)) +
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


def extract_audio_prg(rom: "Rom"):
    """The PRG slice the sound engine needs, with its own bounds measured.

    Runs every track through the 6502 interpreter in tools/nes_cpu.py, watching
    which PRG addresses get read and asserting that nothing outside RAM and the
    APU is touched. The slice is then cut to fit what was actually used, with a
    page of margin at each end, rather than to an address guessed in advance.
    """
    from nes_cpu import Bus, CPU

    prg_banks = rom.prg_banks
    prg = rom.data[rom.prg_off:rom.prg_off + prg_banks * 16384]
    if prg_banks == 1:
        prg = prg + prg

    low, high = 0xFFFF, 0x0000

    class Watching(Bus):
        def read(self, addr):
            nonlocal low, high
            a = addr & 0xFFFF
            if a >= 0x8000:
                low = min(low, a)
                high = max(high, a)
            return super().read(addr)

    for track in AUDIO_TRACK_IDS:
        bus = Watching(prg)
        cpu = CPU(bus)
        cpu.call(AUDIO_SET_TRACK_ADDR, a=track)
        for _ in range(AUDIO_PROBE_FRAMES):
            cpu.call(AUDIO_UPDATE_ADDR)
        if bus.stray:
            raise ValueError(
                f"track ${track:02X} touched something outside RAM and the APU: "
                f"{bus.stray[:5]} — the port's audio emulator only implements those")

    if low > high:
        raise ValueError("the sound engine read no PRG at all; wrong addresses?")

    base = max(0x8000, (low - AUDIO_SLICE_ALIGN) & ~(AUDIO_SLICE_ALIGN - 1))
    end = min(0x10000, (high + AUDIO_SLICE_ALIGN + 1) & ~(AUDIO_SLICE_ALIGN - 1))
    return base, prg[base - 0x8000:end - 0x8000], (low, high)


def record_audio_golden(rom: "Rom", track, frames):
    """`frames` frames of the APU register file, as the engine leaves it."""
    from nes_cpu import Bus, CPU

    prg_banks = rom.prg_banks
    prg = rom.data[rom.prg_off:rom.prg_off + prg_banks * 16384]
    if prg_banks == 1:
        prg = prg + prg

    bus = Bus(prg)
    cpu = CPU(bus)
    cpu.call(AUDIO_SET_TRACK_ADDR, a=track)
    out = bytearray()
    for _ in range(frames):
        cpu.call(AUDIO_UPDATE_ADDR)
        out += bytes(bus.apu)
    return bytes(out)


def emit_audio_header(base, data, span, source):
    lines = [
        "/*",
        " * audio_prg.h — the cartridge's sound engine and music, as 6502 code.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * This is not a conversion of anything: it is the original program",
        " * bytes. gba/nes_audio.c runs them on a small 6502 interpreter and",
        " * turns the NES APU writes they produce into GBA sound registers, so",
        " * the music and every sound effect are the cartridge's own, mixed and",
        " * prioritised by its own code.",
        " *",
        f" * The slice is ${base:04X}..${base + len(data) - 1:04X}. It was measured, not",
        f" * guessed: running every track for {AUDIO_PROBE_FRAMES} frames reads",
        f" * ${span[0]:04X}..${span[1]:04X} and touches nothing outside RAM and $4000-$4017.",
        " */",
        "#ifndef AUDIO_PRG_H",
        "#define AUDIO_PRG_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define AUDIO_PRG_BASE 0x{base:04X}",
        f"#define AUDIO_PRG_SIZE {len(data)}",
        f"#define AUDIO_SET_TRACK_ADDR 0x{AUDIO_SET_TRACK_ADDR:04X}",
        f"#define AUDIO_UPDATE_ADDR 0x{AUDIO_UPDATE_ADDR:04X}",
        "",
        f"static const uint8_t kAudioPrg[{len(data)}] = {{",
    ]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines += ["};", "", "#endif /* AUDIO_PRG_H */", ""]
    return "\n".join(lines)


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
        " * dropped from between the TETRIS banner and the score panel: the braided",
        " * frame around the second playfield area, which the 1P screen covers with",
        " * its panel anyway. The playfield, its frames, the banner and the full",
        " * 10-column panel all survive at original size. Nothing is scaled and",
        " * nothing is cropped.",
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


def compose_title(nametable):
    """32x30 title screen -> a 30x20 layout that keeps every element.

    Returns a flat list of 600 tile ids. The vertical fit comes from dropping
    the thick top and bottom borders rather than from scaling or from cutting
    artwork: see TITLE_ROW_BLOCKS.
    """
    cols = list(range(*TITLE_KEEP_COLS))
    assert len(cols) == 30, f"title needs 30 columns, got {len(cols)}"

    rows = []
    for start, end in TITLE_ROW_BLOCKS:
        rows.extend(range(start, end))
    assert len(rows) == 20, f"title needs 20 rows, got {len(rows)}"

    return [nametable[r * 32 + c] for r in rows for c in cols]


def compose_menu(nametable, attributes):
    """The menu frame, composed to 30x20 with its borders intact."""
    cols = [c for start, end in MENU_COL_BLOCKS for c in range(start, end)]
    rows = [r for start, end in MENU_ROW_BLOCKS for r in range(start, end)]
    assert len(cols) == 30 and len(rows) == 20, \
        f"menu needs 30x20, got {len(cols)}x{len(rows)}"
    tiles = [nametable[r * 32 + c] for r in rows for c in cols]
    palettes = [attribute_palette(attributes, c, r) for r in rows for c in cols]
    return tiles, palettes


def emit_menu_header(tiles, palettes, source):
    lines = [
        "/*",
        " * screen_menu.h — the ROM's menu frame, composed to the GBA's 30x20.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * The cartridge draws its selection screens inside this frame, writing",
        " * the wording into the empty middle at runtime; the port does the same.",
        " * Both side borders survive whole — the two columns the GBA lacks come",
        " * out of that empty middle, where they cost nothing.",
        " */",
        "#ifndef SCREEN_MENU_H",
        "#define SCREEN_MENU_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SCREEN_MENU_W 30",
        "#define SCREEN_MENU_H_TILES 20",
        "",
        "static const uint8_t kScreenMenuTiles[600] = {",
    ]
    for i in range(0, len(tiles), 30):
        lines.append("    " + ", ".join(f"0x{t:02X}" for t in tiles[i:i + 30]) + ",")
    lines += ["};", "", "static const uint8_t kScreenMenuPalettes[600] = {"]
    for i in range(0, len(palettes), 30):
        lines.append("    " + ", ".join(str(p) for p in palettes[i:i + 30]) + ",")
    lines += ["};", "", "#endif /* SCREEN_MENU_H */", ""]
    return "\n".join(lines)


def emit_title_header(tiles, palette, source):
    lines = [
        "/*",
        " * screen_title.h — the title screen, composed from the ROM's own.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * The cartridge's title screen is 32x30 tiles; this is the 30x20 the GBA",
        " * shows. The rows it loses are the thick top and bottom borders plus the",
        " * single row holding the tallest spire's tip — every piece of lettering",
        " * and the whole cathedral survive at original size, and three of the four",
        " * border columns stay on each side. Nothing is scaled.",
        " *",
        " * One palette covers the screen: the ROM's own nametable has no attribute",
        " * table here, because the artwork is drawn entirely in bgPalette0's blues.",
        " */",
        "#ifndef SCREEN_TITLE_H",
        "#define SCREEN_TITLE_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SCREEN_TITLE_W 30",
        "#define SCREEN_TITLE_H_TILES 20",
        "",
        "static const uint8_t kScreenTitleTiles[600] = {",
    ]
    for i in range(0, len(tiles), 30):
        lines.append("    " + ", ".join(f"0x{t:02X}" for t in tiles[i:i + 30]) + ",")
    lines += ["};", "",
              "/* bgPalette0's first palette: the blues the title art uses. */",
              "static const uint8_t kTitlePalette[4] = {",
              "    " + ", ".join(f"0x{b:02X}" for b in palette),
              "};", "", "#endif /* SCREEN_TITLE_H */", ""]
    return "\n".join(lines)


def emit_dancer_poses_header(poses, source):
    lines = [
        "/*",
        " * dancer_poses.h — the between-levels dancers' animation poses.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source} ({len(poses)} poses)",
        " *",
        " * Each pose is four sprite tile ids that form a 2x2 block — one 16x16",
        " * Cossack dancer. The ROM keeps eight dancers, each stepping through a",
        " * script of these every 8 frames while walking sideways and flipping",
        " * horizontally (main.asm.txt:6392-6499).",
        " */",
        "#ifndef DANCER_POSES_H",
        "#define DANCER_POSES_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define DANCER_POSE_COUNT {len(poses)}",
        "",
        "static const uint8_t kDancerPoses[DANCER_POSE_COUNT][4] = {",
    ]
    for pose in poses:
        lines.append("    { " + ", ".join(f"0x{b:02X}" for b in pose) + " },")
    lines += ["};", "", "#endif /* DANCER_POSES_H */", ""]
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

    # The title composition must keep 30x20 and must not reorder columns.
    title = compose_title(bytes(range(256)) * 4)
    if len(title) != 600:
        failures.append(f"la composicion del titulo dio {len(title)} tiles, esperado 600")
    rows_kept = sum(end - start for start, end in TITLE_ROW_BLOCKS)
    if rows_kept != 20:
        failures.append(f"los bloques de filas del titulo suman {rows_kept}, esperado 20")

    menu_tiles, menu_palettes = compose_menu(bytes(range(256)) * 4, bytes(64))
    if len(menu_tiles) != 600 or len(menu_palettes) != 600:
        failures.append("la composicion del menu no dio 30x20")

    if failures:
        for f in failures:
            print(f"FALLA: {f}")
        return 1
    print("OK: conversion de tiles, atributos, reflow de pantalla y "
          "composicion del titulo verificados.")
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

    pose_bytes = rom.at(DANCER_POSE_ADDR, DANCER_POSE_END - DANCER_POSE_ADDR)
    poses = [pose_bytes[i:i + 4] for i in range(0, len(pose_bytes), 4)]

    title_nt = rom.at(TITLE_NAMETABLE_ADDR, NAMETABLE_BYTES)
    title_palette = rom.at(TITLE_BG_PALETTE_ADDR, 4)

    menu_nt = rom.at(MENU_NAMETABLE_ADDR, NAMETABLE_BYTES)
    menu_attr = rom.at(MENU_NAMETABLE_ADDR + NAMETABLE_BYTES, ATTRIBUTE_BYTES)
    menu_tiles, menu_palettes = compose_menu(menu_nt, menu_attr)

    audio_base, audio_bytes, audio_span = extract_audio_prg(rom)
    audio_golden = record_audio_golden(rom, AUDIO_GOLDEN_TRACK, AUDIO_GOLDEN_FRAMES)

    outputs = {
        "audio_prg.h": emit_audio_header(audio_base, audio_bytes, audio_span, src),
        "screen_menu.h": emit_menu_header(menu_tiles, menu_palettes, src),
        "screen_title.h": emit_title_header(compose_title(title_nt), title_palette, src),
        "tiles_title.h": emit_tiles_header(
            "kTitleTiles", "TILES_TITLE", convert_tiles(rom.chr_bank(2)), f"{src} [title]"),
        "dancer_poses.h": emit_dancer_poses_header(poses, f"{src} [dancers]"),
        "tiles_game.h": emit_tiles_header(
            "kGameTiles", "TILES_GAME", convert_tiles(rom.chr_bank(0)), f"{src} [game]"),
        "tiles_dancers.h": emit_tiles_header(
            "kDancerTiles", "TILES_DANCERS", convert_tiles(rom.chr_bank(1)), f"{src} [dancers]"),
        "screen_1p.h": emit_screen_header(tiles, palettes, keep_cols, src),
        "palettes_rom.h": emit_palette_header(bg_palette, piece_palettes, src),
    }

    for filename, content in outputs.items():
        path = os.path.join(args.outdir, filename)
        with open(path, "w") as fh:
            fh.write(content)
        print(f"escrito {path}")

    golden_path = os.path.join(args.outdir, "audio_golden.bin")
    with open(golden_path, "wb") as fh:
        fh.write(audio_golden)
    print(f"escrito {golden_path} ({AUDIO_GOLDEN_FRAMES} frames del tema de titulo)")

    print(f"motor de sonido: PRG ${audio_base:04X}..${audio_base + len(audio_bytes) - 1:04X} "
          f"({len(audio_bytes)} bytes); accesos medidos ${audio_span[0]:04X}..${audio_span[1]:04X}")
    ox, oy = playfield_origin(keep_cols)
    print(f"playfield en la pantalla reflowed: columna {ox}, fila {oy} "
          f"(10x20 jugables, con el marco del cartucho a los lados)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
