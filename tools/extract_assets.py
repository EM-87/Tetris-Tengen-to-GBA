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
  screen_coop.h   ...and the COOPERATIVE one, its twelve-wide field included
                  and attribute table, reflowed from the NES's 32 columns to
                  the GBA's 30 (see BOARD LAYOUT below).
  palettes.h      The NES palette entries the game actually uses.

BOARD LAYOUT — the NES screen, and how its pieces are rearranged for the GBA.

  The cartridge's 32 columns:

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

  On the NES the playfield sits well left of centre, because the screen is
  really two board areas side by side and 1P only plays in one. On a GBA
  showing one player that reads as lopsided, so the pieces are RESEQUENCED
  (never scaled, never cropped) to put the playfield dead centre with the HUD
  split around it — see SCREEN_SEGMENTS. Each segment is a run of the
  cartridge's own columns, moved whole.
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

NES_HEADER = 16
NES_TILE_BYTES = 16
GBA_TILE_BYTES = 32
CHR_BANK = 4096

# Where things live in the PRG, verified against the disassembly in
# reference/disasm/ (see reference/NOTES.md).
NAMETABLE_BYTES = 960           # 32 x 30 tiles
ATTRIBUTE_BYTES = 64

# HOW THE SCREENS ARE READ, and why not simply by slicing the ROM.
#
# A screen is a nametable (960 tile ids) plus an ATTRIBUTE table (64 bytes
# saying which of four palettes each 2x2 block of tiles uses). The obvious
# assumption — that the 64 bytes sit right after the 960 — is WRONG here, and
# quietly so: the screens are packed 960 bytes apart, so slicing that way
# hands you the next screen's first two rows and calls them palettes. The
# result looks almost right and is subtly broken everywhere: this port had the
# playfield frame changing colour down its length, and a monochrome title.
#
# So the screens are not sliced, they are RUN. sendNametableToPPU is the
# cartridge's own routine for putting one on screen, it writes 1024 bytes
# straight to the PPU, and it needs nothing but RAM and PRG — so the 6502
# interpreter in tools/nes_cpu.py can execute it and the video memory read
# back afterwards is exactly what the console would display.
SCREEN_UPLOAD_ADDR = 0xB3E9     # sendNametableToPPU
SCREEN_TITLE = 0
SCREEN_MENU = 1
SCREEN_COOP = 5
SCREEN_1P = 6
SCREEN_LEADER = 8
SCREEN_2P = 7

# The menu screen ships with its GAME SELECT wording baked into the nametable
# (rows 14-20: the heading and 1 PLAYER / 2 PLAYER / COOPERATIVE / VERSUS
# COMPUTER / WITH COMPUTER); the ROM's other selection screens overwrite those
# rows at runtime. This port fills that space with its own selection, so those
# rows come across blank — the frame, the big TETRIS logo above it and the
# borders are all kept.
MENU_TEXT_ROWS = (14, 21)
MENU_TEXT_COLS = (2, 30)

# updatePalette(n) (main.asm.txt:5268-5287) writes 16 bytes — four palettes —
# from bgPalette0 + n*16, to the background palettes for n<3 and the sprite
# palettes for n>=3. Which screen calls it with what is traced:
#   title  (:4492-4494) -> bgPalette0 + spritePalette1
#   menu   (:4547-4549) -> bgPalette1 + spritePalette0
#   game   (:3308-3310) -> bgPalette2 + spritePalette0
#   level-up interlude (:2044) -> spritePalette2, which is what the dancers
#                                 are drawn in.
PALETTE_TABLE_ADDR = 0xA6E6     # bgPalette0; the sets follow every 16 bytes
PALETTE_SET_BYTES = 16
PALETTE_BG_TITLE = 0            # bgPalette0
PALETTE_BG_MENU = 1             # bgPalette1
PALETTE_BG_GAME = 2             # bgPalette2
PALETTE_OBJ_GAME = 3            # spritePalette0
PALETTE_OBJ_TITLE = 4           # spritePalette1
PALETTE_OBJ_DANCERS = 5         # spritePalette2
PIECE_PALETTE_ADDR = 0xA788     # piecePaletteIndex0..B

# The between-levels dancers. Each pose is four sprite tile ids forming a
# 2x2, i.e. one 16x16 figure; the animation driver (main.asm.txt:6392-6499)
# walks a script of pointers into this table, advancing one every 8 frames.
# The labelled extent runs $C8BC..$C9FF.
DANCER_POSE_ADDR = 0xC8BC
DANCER_POSE_END = 0xCA00

# Where the dancers stand, and what they stand on.
#
# The level-up blit (`levelUpAnimationColsRows1` at $B7FF: 4 columns x 18 rows
# at nametable (14,10)) does not just clear the TETRIS banner — it draws the
# dancers' STAGE into it: five ledges of tile $9D, one every three rows. Their
# spacing is exactly the 24px spacing of the six dancer positions below, which
# is what pins the two tables to each other.
DANCER_STAGE_ADDR = 0xC82C      # LC82C, the blit's tile data
DANCER_STAGE_COLS = 4
DANCER_STAGE_ROWS = 18
# Per-dancer starting X, Y and OAM attributes ($8E5C / $8E6A / $8E78). Entries
# 0-5 are the six a 1P or 2P game uses, stacked in one column; 6-13 are coop's,
# in pairs down the two sides. main.asm.txt:2085-2108 picks the range: 1P/2P
# take 0..min(count,6), coop takes 6..count+6.
DANCER_POS_X_ADDR = 0x8E5C
DANCER_POS_Y_ADDR = 0x8E6A
DANCER_ATTR_ADDR = 0x8E78
DANCER_POS_COUNT = 14
DANCER_SOLO_COUNT = 6

# The title screen: "TENGEN PRESENTS / THE SOVIET MIND GAME / TETRIS" over
# St Basil's Cathedral, drawn in all four of bgPalette0's palettes.
# How the 32x30 title is composed down to the GBA's 30x20.
#
# The screen is a framed PICTURE of St Basil's Cathedral under TENGEN TETRIS,
# and what has to survive is the cathedral WHOLE — its spire, its domes and
# its bodies, at 1:1 — inside a frame that is closed on all four sides.
#
# THE FRAME IS TWO FRAMES, and the screen is not square. Outside there is a
# band of gold ingots with red and green jewels set into it, two tiles thick;
# inside that, a blue braid, another two tiles thick. Eight tiles of frame on
# every side is more than a 30x20 screen can carry — but it is not square, and
# that is the whole trick: SIDEWAYS there is room for both, VERTICALLY only for
# the braid. So the picture is framed all the way round by the braid, the way
# the cartridge frames it, and the two side bands carry the ingots and jewels
# as well.
#
# Two earlier passes each kept one band and threw the other away, and both were
# wrong the same way: the braid alone left the frame with no jewels at all, the
# ingots alone left the picture without the frame it actually sits in.
#
# BOTH BANDS ARE A TWO-TILE PATTERN — a jewel (tiles 00 01 / 04 05) then an
# ingot (08 09 / 11 12) — so every row and column kept below is kept in its
# PAIR. Take one row of a jewel and you get half a jewel.
#
# COLUMNS: ingots 0-1, braid 2-3, twenty-two of the picture, braid 28-29,
# ingots 30-31. The two the GBA lacks come off the picture's left margin, which
# is blank once the subtitle and the first copyright line are gone.
#
# ROWS: the braid at 2-3 and 26-27, and sixteen for the picture. It wants
# TENGEN (2) + the TETRIS logo (4) + the cathedral (12) = 18, so two come out
# of it. What goes, in the order the cartridge's owner allowed:
#
#   rows 0-1, 28-29   the ingot band top and bottom. No room, and the sides
#                     carry it.
#   rows 6-7          "PRESENTS" and "THE SOVIET MIND GAME".
#   rows 24-25        the two copyright lines. Not lost: the credit moved to
#                     GAME SELECT.
#   rows 12-13        the top two rows of the cathedral's thin central spire —
#                     one tile wide, and the only part of the picture that can
#                     go without leaving a cut edge. Row 14 stays, so the tip
#                     is still there, right under the TETRIS logo.
# WHICH TWO COLUMNS THE TITLE LOSES, and it has to be ONE FROM EACH SIDE.
# The cartridge's picture is centred on source column 15.5: TENGEN sits at
# columns 10-21, the cathedral at 8-23 and the spire at 15-16, all with the
# same middle, inside a frame whose interior is columns 4-27. Taking both
# spare columns off the left (which is what this did) keeps every element
# where it was but moves the frame two columns in behind them, so the whole
# picture ends up one column left of its own frame — visible, and exactly the
# "ligeramente desalineado a la izquierda" it was. Dropping 4 and 27 instead
# leaves the interior at 5-26, centred on 15.5 again.
#
# Neither column costs anything: inside the rows this layout keeps, both are
# blank in every one. Column 27 carries the last letter of the copyright line
# at source row 24, and that row is not among the rows kept either.
TITLE_COL_BLOCKS = (
    (0, 4),     # ingots and jewels, then the braid
    (5, 27),    # the picture
    (28, 32),   # the braid again, and the ingots
)
# THE TWO SPIRE TILES PUT BACK OVER THE LOGO, and the logo rows they go in.
# Each entry is (source row, source column, the row it is printed into, the
# tile that row must already hold). That last field is the guard: these two
# cells are the only places in the logo where a whole tile can be replaced
# without losing lettering, so a different dump has to fail loudly rather than
# quietly paint over a letter. See compose_title.
#
#   (12,16) $7C  the finial: a thin pole with its base flaring at the bottom
#   (13,16) $7E  the ball under it, which is what JOINS the finial to the tent
#
# Printing only the first left the finial floating eight pixels above the roof
# with black in between — the gap that read as a graphical glitch. Row 11's
# $73 at column 16 is three pixels of two letters' bottom serif and nothing
# else, so the ball fits there and the spire comes out whole: finial, ball,
# tent, on three consecutive rows exactly as the cartridge stacks them.
#
# Its left neighbour, $7D at (13,15), is ONE pixel of the ball's left edge and
# it does not come: row 11 column 15 is a solid bar of lettering.
#
# THE SPIRE'S TIP IS DRAWN AS SPRITES, not printed into the logo.
#
# The cathedral's tallest spire is one tile wide and the reflow drops the row
# its top three tiles live on: the finial $7C, the ball $7E and the ball's lit
# left rim $7D. Printing them back into the logo cells underneath is what the
# port did, and each one costs whatever ink that cell held — $6E is the right
# half of the second T's bottom serif, and $73 is three pixels of two more:
# one of the T's and two of the Я's. Compositing does not help, because a
# background cell carries ONE palette and the cathedral's turns a serif gold.
#
# As objects there is no cost at all: the logo tiles underneath are untouched
# and the tip brings its own palette. And they go ONE PIXEL LOWER than the
# grid, which is not a fudge — the ball's own artwork has a blank bottom row,
# so on the grid it stopped a pixel short of the tent it is supposed to be
# sitting on. A pixel down closes that and frees the serif row whole.
TITLE_SPIRE_OVERLAY = ()

# (source tile, source column, source row, the background palette its colours
# come from). Their screen position is where the cartridge's own reflow would
# have put that source cell, plus TITLE_SPIRE_LIFT.
TITLE_SPIRE_SPRITES = (
    (0x7C, 16, 12, 2),   # the finial
    (0x7D, 15, 13, 2),   # the ball's left rim
    (0x7E, 16, 13, 2),   # the ball
)
TITLE_SPIRE_LIFT = 1     # pixels DOWN, so the ball meets the tent

TITLE_BLANK_TILE = 0x1D

TITLE_ROW_BLOCKS = (
    (2, 6),     # the braid's top band, then TENGEN
    (8, 12),    # the TETRIS logo
    (14, 24),   # the spire's tip, and the whole cathedral under it
    (26, 28),   # the braid's bottom band
)

# The menu screen the ROM uses for its selection screens: a decorative frame
# on all four sides with an empty middle it writes text into at runtime. Its
# borders are what the level selector is framed with here.
# 30 columns: both side borders intact, the two the GBA lacks taken from the
# empty middle where they cost nothing.
# WHICH TWO COLUMNS THE MENU LOSES. Its horizontal TETRIS logo lives at rows
# 10-12, columns 4-27: six letters of exactly four columns each. Taking the two
# spare columns out of the middle (15 and 16) cut the third letter's last
# column and the fourth's first, which mashed the T and the R together — the
# logo has no empty middle to borrow from. The blank padding at columns 2 and
# 29 does, and costs nothing.
MENU_COL_BLOCKS = ((0, 2), (3, 29), (30, 32))
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
# MUSIC_TITLESCREEN, the first thing that plays — and it has to be the first,
# because the sound engine carries state between tracks (envelope phases,
# vibrato counters) and only a track started from a fresh engine can be matched
# against a reference started the same way. Recording a later tune and trying
# to meet it mid-session matches nothing at all.
AUDIO_GOLDEN_TRACK = 0x09
AUDIO_GOLDEN_FRAMES = 400
AUDIO_SILENCE_TRACK = 0x08      # MUSIC_SILENCE, constants.asm.txt:44

# THE TITLE SCREEN'S SPRITES ARE RUN, NOT REIMPLEMENTED — for the same reason
# the music is.
#
# Two routines draw everything that moves on the title screen, and both work by
# filling `oamStaging` ($0500) rather than by touching the PPU:
#
#   drawCathedralSprites ($B369, main.asm.txt:6850) lays eighteen sprites over
#     the cathedral, from a table the disassembly itself calls obfuscated —
#     y and x are packed across two bytes and unpacked with a shift, a rotate
#     and a subtract that its own worked example (`in $02,$03,$CF,$01` ->
#     `out $6F,$02,$03,$78`) is the only readable description of.
#
#   LA9CE ($A9CE, main.asm.txt:5730) is the fireworks, once per frame. It runs
#     a little program: `relatedToFireworksTable0` is a list of (frame, extra)
#     triples, each naming one of nine 8x6 tile blocks (`fireworksData00`..
#     `08`, main.asm.txt:8256) or a drift/recolour step, and it ends the show
#     on the title screen once frameCounterHigh reaches 4 — about seventeen
#     seconds, which is the cartridge's own idea of how long fireworks last.
#     Its bursts also call setMusicOrSoundEffect, so it is literally the same
#     program as the sound engine and shares its RAM. Running them on one
#     interpreter is not a convenience: it is why the burst makes a noise.
#
# Both are traced below exactly as the audio is, and the slice covers both.
FIREWORKS_UPDATE_ADDR = 0xA9CE  # LA9CE, once per frame
CATHEDRAL_ADDR = 0xB369         # drawCathedralSprites
GAMESTATE_TITLE = 0xFA          # constants.asm.txt
RAM_GAMESTATE = 0x0029
RAM_FRAME_LOW = 0x0032
RAM_FRAME_HIGH = 0x0033
RAM_RNG_SEED = 0x0034           # reset leaves it $5A (main.asm.txt:5712)
RAM_RNG_SEED_VALUE = 0x5A
RAM_OAM_STAGING = 0x0500
TITLE_PROBE_FRAMES = 2000       # past frameCounterHigh = 4, where the show ends

# Screen regions, in NES nametable columns. See BOARD LAYOUT above.
COL_FRAME_L = (0, 2)            # braid; the playfield's left wall
COL_PLAYFIELD_PLAY = (2, 12)    # the ten playable columns
COL_FRAME_R = (12, 14)          # braid; the right wall
COL_BANNER = (14, 18)           # the vertical TETRIS banner
COL_PANEL = (20, 30)            # the second board area: blank canvas in 1P

# How the GBA's 30 columns are built from the cartridge's 32, as
# (first NES column, how many). Read down, this IS the screen:
#
#   port  0-7   blank panel canvas -> the LEFT box
#   port  8-9   the board's left frame  (NES 0-1)
#   port 10-19  the ten playable columns — dead centre, 80px either side
#   port 20-21  the board's right frame (NES 12-13)
#   port 22-29  blank panel canvas -> the RIGHT box
#
# 8 + 2 + 10 + 2 + 8 = 30, and it reads the same from either end: the board
# in the middle with its own two-column braid, and a box of the same width on
# each side. That symmetry is the point. An earlier arrangement put six
# columns of HUD on the left and the TETRIS banner on the right, which left
# the screen visibly lopsided and — worse — cut the cartridge's row of piece
# icons in half, stranding two of them in the opposite corner.
#
# WHAT THIS COSTS: the vertical TETRIS banner (NES 14-17) has no place on the
# play screen any more. Thirty columns cannot hold a ten-wide board, its
# braid, two boxes wide enough for a six-digit score and a seven-column
# histogram, AND a four-column banner. The banner survives on the title
# screen, and the dancers still get their stage — the right-hand box becomes
# it during a level-up, exactly as the cartridge's own blit takes over the
# banner.
SCREEN_SEGMENTS = (
    (20, 8),
    (0, 2),
    (2, 10),
    (12, 2),
    (22, 8),
)

# Two fix-ups the runs above cannot express, both because a column is not
# uniform down its whole length:
#
#  - NES rows 8 AND 9 are where the banner's box begins, so columns 12-13 hold
#    its two top-corner rows ($97 $98 then $9B $9C) rather than the braid
#    ($73 $74) they hold on every other row. Lifted verbatim they put a hook on
#    top of the board's right wall. The braid pair is what belongs there: the
#    port has no banner for that box to be the top of.
#  - NES rows 26-27, columns 21-27 hold the cartridge's row of piece icons
#    for the statistics. The port draws those itself, where its own histogram
#    is, so they are cleared out of the canvas rather than left to show
#    through in both boxes at once.
SCREEN_WALL_FIX = ((20, 0x73), (21, 0x74))   # port column -> tile
SCREEN_WALL_FIX_ROWS = (8, 10)               # NES rows, the banner box's top
SCREEN_BLANK = ((26, 28), (20, 30))          # (row range, NES column range)

ROW_PLAYFIELD = (8, 28)         # 20 rows

# ---------------------------------------------------------------------------
# COOP, which is the cartridge's own screen and needs almost nothing done to it
#
# Screen 5 is laid out the way the GBA wants already, because coop is the one
# mode the NES also draws symmetrically:
#
#   cols  0-7   left panel: LEVEL, and five of the dancers' ledges
#   cols  8-9   braid: the wide field's left wall
#   cols 10-21  playfield: TWELVE playable columns (the walls are left open,
#               main.asm.txt:3480-3489) x 20 rows, starting at row 8 like 1P
#   cols 22-23  braid: the right wall
#   cols 24-31  right panel: HIGH and SCORE, and the other five ledges
#
# So there is no resequencing to do, only the two columns every screen has to
# give up to fit thirty — and here they come one from each END, which leaves
# the field dead centre (GBA columns 9-20, x 72..167, centred on 120) and
# seven columns of panel either side.
#
# What IS dropped is the cartridge's own LEVEL / HIGH / SCORE lettering: the
# port writes its own labels from the same tiles, in its own places, the way
# it does on every other screen.
# THE HIGH SCORE TABLE (screen 8). Its frame is two tiles thick on each side
# and its content sits between columns 7 and 24, so the two columns every
# screen gives up are taken from the BLANK gutters just inside the frame —
# one from each — which leaves the frame whole and the table centred.
LEADER_SEGMENTS = ((0, 2), (3, 26), (30, 2))

# ...and the twenty rows of it a GBA can show, out of the cartridge's thirty.
# The table IS the page — fifteen entries and the heading are sixteen rows of
# it — so what the port gives up is the arch at rows 2-7, which is the title
# screen's own art and is on the title screen. The frame's cap and sill come
# with their rows; everything else is the cartridge's, untouched, in the
# cartridge's order.
LEADER_ROWS = (0, 1, 10) + tuple(range(12, 27)) + (28, 29)

COOP_SEGMENTS = ((1, 15), (16, 15))
COOP_BLANK = (((11, 13), (2, 7)), ((11, 13), (25, 30)))
COOP_PLAY_COL = 10

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


def read_screen(rom: "Rom", index: int):
    """(nametable, attributes) for one screen, as the console would show it.

    Runs the cartridge's own sendNametableToPPU on the 6502 interpreter and
    reads the video memory back. See the note beside SCREEN_UPLOAD_ADDR for
    why this is not a slice of the ROM.

    """
    from nes_cpu import Bus, CPU

    prg = rom.data[rom.prg_off:rom.prg_off + rom.prg_banks * 16384]
    if rom.prg_banks == 1:
        prg = prg + prg

    bus = Bus(prg, ppu=True)
    cpu = CPU(bus)
    cpu.call(SCREEN_UPLOAD_ADDR, a=index)
    nametable = bytes(bus.vram[0:NAMETABLE_BYTES])
    attributes = bytes(bus.vram[NAMETABLE_BYTES:NAMETABLE_BYTES + ATTRIBUTE_BYTES])
    if not any(nametable):
        raise ValueError(f"screen {index} came back blank; wrong entry point?")
    return nametable, attributes


def read_palette_set(rom: "Rom", index: int):
    """One updatePalette set: four palettes of four NES colour indices."""
    raw = rom.at(PALETTE_TABLE_ADDR + index * PALETTE_SET_BYTES, PALETTE_SET_BYTES)
    return [raw[i * 4:(i + 1) * 4] for i in range(4)]


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


def reflow_screen(nametable: bytes, attributes: bytes, segments=None,
                   blanks=None, wall_fix=True, play_col=None):
    """NES 32-column screen -> GBA 30-column screen.

    Returns (tiles, palettes) as 30-per-row lists covering 30 rows; only the
    first 20 are visible on a GBA, but the full height is kept so the caller
    can choose the vertical window.

    Columns are RESEQUENCED, not scaled or cropped: SCREEN_SEGMENTS lists runs
    of the cartridge's own columns in the order the GBA shows them, which is
    what puts the playfield in the middle with the HUD split around it.
    """
    keep_cols = [c for start, count in (segments or SCREEN_SEGMENTS)
                 for c in range(start, start + count)]
    assert len(keep_cols) == 30, f"expected 30 columns, got {len(keep_cols)}"

    if blanks is None:
        blanks = (SCREEN_BLANK,)

    tiles, palettes = [], []
    for row in range(30):
        for port_col, col in enumerate(keep_cols):
            tile = nametable[row * 32 + col]
            for blank_rows, blank_cols in blanks:
                if (blank_rows[0] <= row < blank_rows[1] and
                        blank_cols[0] <= col < blank_cols[1]):
                    tile = 0                   # the port draws this itself
            if wall_fix and SCREEN_WALL_FIX_ROWS[0] <= row < SCREEN_WALL_FIX_ROWS[1]:
                for fix_col, fix_tile in SCREEN_WALL_FIX:
                    if port_col == fix_col:
                        tile = fix_tile        # braid, not the banner's corner
            tiles.append(tile)
            palettes.append(attribute_palette(attributes, col, row))
    return tiles, palettes, keep_cols


def playfield_origin(keep_cols, play_col=None) -> tuple:
    """Where the playfield's first playable column ends up after the reflow."""
    return keep_cols.index(COL_PLAYFIELD_PLAY[0] if play_col is None else play_col), \
        ROW_PLAYFIELD[0]


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

    # ...and the title screen's two sprite routines, on the same interpreter and
    # into the same slice. See the note beside FIREWORKS_UPDATE_ADDR.
    bus = Watching(prg)
    cpu = CPU(bus)
    bus.write(RAM_GAMESTATE, GAMESTATE_TITLE)
    bus.write(RAM_RNG_SEED, RAM_RNG_SEED_VALUE)
    for frame in range(TITLE_PROBE_FRAMES):
        bus.write(RAM_FRAME_LOW, frame & 0xFF)
        bus.write(RAM_FRAME_HIGH, (frame >> 8) & 0xFF)
        cpu.call(CATHEDRAL_ADDR)
        cpu.call(FIREWORKS_UPDATE_ADDR)
    if bus.stray:
        raise ValueError(
            "the title screen's sprites touched something outside RAM and the "
            f"APU: {bus.stray[:5]}")

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
    # LA035's order, which is the port's: MUSIC_SILENCE and then the track
    # (main.asm.txt:4730-4735). Recording the golden without the silence made
    # it a recording of something the ROM never does, and the two drifted
    # apart around frame 139 — one bit of $4015, the noise channel's enable.
    cpu.call(AUDIO_SET_TRACK_ADDR, a=AUDIO_SILENCE_TRACK)
    cpu.call(AUDIO_SET_TRACK_ADDR, a=track)
    out = bytearray()
    for _ in range(frames):
        cpu.call(AUDIO_UPDATE_ADDR)
        out += bytes(bus.apu)
    return bytes(out)


def emit_audio_header(base, data, span, source):
    lines = [
        "/*",
        " * audio_prg.h — the part of the cartridge the port RUNS, as 6502 code.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * This is not a conversion of anything: it is the original program",
        " * bytes, run on the small 6502 interpreter in gba/nes6502.c. Two",
        " * things in the port are the cartridge executing rather than the port",
        " * imitating it, and they share this slice and one 2KB of RAM because",
        " * on the cartridge they are one program:",
        " *",
        " *   the SOUND ENGINE — gba/nes_audio.c turns the NES APU writes it",
        " *     produces into GBA sound registers, so the music, every effect,",
        " *     and the priority rules deciding which channel an effect may",
        " *     steal are the cartridge's own.",
        " *",
        " *   the TITLE SCREEN'S SPRITES — drawCathedralSprites and the",
        " *     fireworks fill oamStaging ($0500), which gba/main.c reads back",
        " *     and blits to GBA OAM. The fireworks call the sound engine for",
        " *     their bursts, which is why they must be the same machine.",
        " *",
        f" * The slice is ${base:04X}..${base + len(data) - 1:04X}. It was measured, not",
        f" * guessed: running every track for {AUDIO_PROBE_FRAMES} frames and the title",
        f" * screen for {TITLE_PROBE_FRAMES} reads ${span[0]:04X}..${span[1]:04X} and touches nothing",
        " * outside RAM and $4000-$4017.",
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
        f"#define NES_FIREWORKS_ADDR 0x{FIREWORKS_UPDATE_ADDR:04X}",
        f"#define NES_CATHEDRAL_ADDR 0x{CATHEDRAL_ADDR:04X}",
        f"#define NES_GAMESTATE_TITLE 0x{GAMESTATE_TITLE:02X}",
        f"#define NES_RAM_GAMESTATE 0x{RAM_GAMESTATE:04X}",
        f"#define NES_RAM_FRAME_LOW 0x{RAM_FRAME_LOW:04X}",
        f"#define NES_RAM_FRAME_HIGH 0x{RAM_FRAME_HIGH:04X}",
        f"#define NES_RAM_RNG_SEED 0x{RAM_RNG_SEED:04X}",
        f"#define NES_RAM_RNG_SEED_VALUE 0x{RAM_RNG_SEED_VALUE:02X}",
        f"#define NES_RAM_OAM_STAGING 0x{RAM_OAM_STAGING:04X}",
        "",
        f"static const uint8_t kAudioPrg[{len(data)}] = {{",
    ]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    lines += ["};", "", "#endif /* AUDIO_PRG_H */", ""]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# THE ONE GLYPH THE CARTRIDGE DOES NOT HAVE.
#
# The game's tile set is ASCII-indexed, which is why the port can print words
# at all — but only where the cartridge needed a character. It has no
# parentheses ($28/$29 are border art), no colon ($3A-$3C are the plaque's
# frame), and no QUESTION MARK: $3F, where ASCII puts one, is the settings
# screen's LEFT ARROW. Tengen's Tetris never asks the player anything, so
# nothing in the ROM ever needed one.
#
# The pause menu does: EXIT asks SURE? before it throws a game away, and a
# question with no question mark is not a question. So this is the one piece of
# ART in the port that is entered by hand rather than extracted — the tunes in
# gba/handtunes.c are the others — and it is kept honest the same way: it
# is declared here rather than smuggled into a generated header, it is built
# to the ROM font's own metrics (ink in rows 1-7, two-pixel strokes, colour
# index 1, column 7 clear), and it takes a slot the cartridge left EMPTY
# rather than overwriting any of its art.
#
# $EF-$FF are blank in the release's CHR bank 0. $F0 is the one used; the
# check below refuses to plant anything on a dump where it is not blank, so a
# different cartridge cannot lose a tile to this without saying so.
QUESTION_TILE = 0xF0
QUESTION_GLYPH = (
    "........",
    ".XXXXX..",
    "XX...XX.",
    ".....XX.",
    "...XXX..",
    "...XX...",
    "........",
    "...XX...",
)


def plant_question_mark(tiles: bytes) -> bytes:
    """Writes the '?' above into QUESTION_TILE of a converted tile page."""
    out = bytearray(tiles)
    base = QUESTION_TILE * GBA_TILE_BYTES
    if len(out) < base + GBA_TILE_BYTES:
        raise SystemExit(f"la pagina de tiles no llega a ${QUESTION_TILE:02X}")
    if any(out[base:base + GBA_TILE_BYTES]):
        raise SystemExit(
            f"el tile ${QUESTION_TILE:02X} no esta vacio en este volcado: "
            "el signo de interrogacion borraria arte del cartucho")
    for row, bits in enumerate(QUESTION_GLYPH):
        for col in range(0, 8, 2):
            lo = 1 if bits[col] == "X" else 0
            hi = 1 if bits[col + 1] == "X" else 0
            out[base + row * 4 + col // 2] = lo | (hi << 4)
    return bytes(out)


def emit_tiles_header(name, guard, tiles, source, extra=()):
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
    ]
    lines += [f"#define {key} 0x{value:02X}" for key, value in extra]
    lines += [
        "",
        f"static const uint8_t {name}[{len(tiles)}] = {{",
    ]
    for i in range(0, len(tiles), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in tiles[i:i + 16]) + ",")
    lines += ["};", "", f"#endif /* {guard}_H */", ""]
    return "\n".join(lines)


# The piece-statistics histogram.
#
# The cartridge keeps a running count per piece and draws it as a vertical bar
# (`L9997`, main.asm.txt:3752-3798). Two things come out of the ROM here:
#
#  - the ICONS. Seven little tetromino pictures, one per piece, two tiles tall,
#    sitting at nametable rows 26-27 columns 21-27 with the bars growing up out
#    of them. They are read from the nametable rather than typed in.
#  - the BAR tiles. `lda pieceStatistics,x / and #$07 / adc #$21` — the tile is
#    $21 plus the count's low three bits, so $21..$28 are eight steps of fill
#    within one tile, and every eighth piece moves the write one row up
#    (the address arithmetic at :3773-3790 subtracts 4*(count & $F8), and a
#    nametable row is 32 bytes, so 8 counts == 32 bytes == one row).
#
# The cap is the ROM's too: `cmp #$90 / bcs` stops drawing at 144, which is the
# 18 rows its panel is tall. The port's box is shorter, so it caps at whatever
# it has room for — same rule, less room.
# The GAME OVER plaque, and the thin frame the port borrows from it.
#
# `gameOverTiles` ($C800, main.asm.txt:8064-8067) is six columns by four rows,
# blitted at nametable (4,12) in 1P — the middle of the playfield — by
# `gameOver1pColsRows1` ($86,$04) and `gameOver1pPPUAddr1` ($2184), in
# background palette 3 (`gameOverAttrs` = $FF,$33). It reads:
#
#     29 2A 2A 2A 2A 2B        a box top
#     2C 47 41 4D 45 2F        | G  A  M  E |
#     2C 4F 56 45 52 2F        | O  V  E  R |
#     3A 3B 3B 3B 3B 3C        a box bottom
#
# so the cartridge's own thin frame is in there: corners 29/2B/3A/3C, a
# horizontal edge 2A on top and 3B underneath, and sides 2C and 2F. The port
# draws its HUD panels with those, which is how the panels get to be boxes
# without anything being invented. What it had before was tile $79 used as a
# right-hand cap for the header strip's rules — and $79 is not a cap, it is an
# unrelated block, which is what those grey stubs beside SCORE / LINES / LEVEL
# were.
# The vertical TETRIS banner, NES columns 14-17 of rows 10-27: six letters on
# their own grey plaques, three rows each. The reflow does not carry it (the
# port's play screen has no room for it beside two usable boxes), so it is
# emitted on its own and the port draws it into the right-hand box when the
# player asks for it with L+R.
BANNER_COLS = (14, 18)
BANNER_ROWS = (10, 28)

GAMEOVER_TILES_ADDR = 0xC800
GAMEOVER_COLS = 6
GAMEOVER_ROWS = 4

# statsTiles1, the BONUS heading of the level-up tally, immediately after the
# GAME OVER plaque in the same table. displayStatsP1ColsRows2 reads it as ten
# columns by two rows with the high bit set, which means the source runs on
# across the rows rather than restarting (main.asm.txt:7563-7570).
BONUS_TILES_ADDR = GAMEOVER_TILES_ADDR + GAMEOVER_COLS * GAMEOVER_ROWS
BONUS_COLS = 10
BONUS_ROWS = 2

STATS_ICON_ROWS = (26, 28)
STATS_ICON_COLS = (21, 28)      # seven pieces, I T O J L S Z
STATS_BAR_TILE = 0x21           # $21 + (count & 7); $28 is a full tile


def read_banner(nametable, attributes):
    """The vertical TETRIS banner as (tiles, palette banks), row-major."""
    tiles, banks = [], []
    for r in range(*BANNER_ROWS):
        tiles.append([nametable[r * 32 + c] for c in range(*BANNER_COLS)])
        banks.append([attribute_palette(attributes, c, r)
                      for c in range(*BANNER_COLS)])
    return tiles, banks


# ---------------------------------------------------------------------------
# THE BRAID, AS A FRAME.
#
# The blue rope that runs down either side of the playfield is the same weave
# the cartridge borders its whole 1P screen with, and that border has
# everything a closed rectangle needs: two-tile-thick runs along all four
# sides and a 2x2 corner at each end. Every tile of it is in background
# palette bank 2.
#
# TWO TILES THICK IS NOT A CHOICE. Each tile is one half of the rope cut
# lengthwise (render tiles $6A and $6B side by side and it is obvious), so a
# one-tile border would be half a braid. That is what fixes the arithmetic of
# the HUD: a ten-column box spends four columns on its frame and leaves six.
BRAID_CORNERS = {           # (column, row) of each corner's top-left tile
    "tl": (0, 0), "tr": (30, 0), "bl": (0, 28), "br": (30, 28),
}
BRAID_RUNS = {              # a repeating cell of each side, and its shape
    "top": ((10, 0), (1, 2)),      # one column, two rows
    "bottom": ((10, 28), (1, 2)),
    "left": ((0, 10), (2, 1)),     # two columns, one row
    "right": ((30, 10), (2, 1)),
}


def read_braid_frame(nametable, attributes):
    """The braid's corners and runs, straight off the 1P screen's border."""
    def tile(c, r):
        return nametable[r * 32 + c]

    banks = set()
    for c, r in list(BRAID_CORNERS.values()):
        for dy in range(2):
            for dx in range(2):
                banks.add(attribute_palette(attributes, c + dx, r + dy))
    if len(banks) != 1:
        raise ValueError(f"the braid border spans palette banks {banks}, not one")

    corners = {name: [[tile(c + dx, r + dy) for dx in range(2)] for dy in range(2)]
               for name, (c, r) in BRAID_CORNERS.items()}
    runs = {name: [[tile(c + dx, r + dy) for dx in range(w)] for dy in range(h)]
            for name, ((c, r), (w, h)) in BRAID_RUNS.items()}
    return corners, runs, banks.pop()


# ---------------------------------------------------------------------------
# THE HEADER GRID, AND THE STUBS IT LEAVES IN THE LABELS
#
# The cartridge's 1P panel is a GRID: nametable rows 2-7 read label, rule,
# value, rule, label, rule, and the rule is tile $76 — four rows of colour 3
# across the full width of the tile. Where a vertical grid line crosses it
# there are junction tiles ($77, $78); where one runs beside a label, the
# label's own end tiles carry a piece of it.
#
# That last part is the glitch. SCORE's first tile ($6D) has the vertical line
# in its columns 0-2 and its last ($72) has one in columns 4-6, and the port's
# layout has no vertical grid for them to belong to — so they came out as grey
# stubs at the start and end of every word.
#
# They can be removed EXACTLY, without touching a pixel of the lettering,
# because the two are different colours: the grid is colour 3 and the letters
# are colour 1. So the labels below are the cartridge's own tiles with colour 3
# masked out — a subtraction, not a redrawing — and the rule they were a
# fragment of is put back where the cartridge puts it, between the rows.
HUD_LABELS = {
    "SCORE": (0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72),
    "LINES": (0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F),
    "LEVEL": (0x7A, 0x80, 0x81, 0x82, 0x83, 0x84),
    "NEXT":  (0x91, 0x92, 0x93, 0x94),
}
HUD_GRID_COLOUR = 3       # what the grid is drawn in; the letters are colour 1
HUD_RULE_TILE = 0x76      # the horizontal rule itself


def strip_grid(chr_data, tile):
    """One tile with every colour-3 pixel cleared, as 16 bytes of 2bpp."""
    px = tile_2bpp_to_pixels(chr_data[tile * NES_TILE_BYTES:
                                      (tile + 1) * NES_TILE_BYTES])
    px = [0 if v == HUD_GRID_COLOUR else v for v in px]
    out = bytearray(NES_TILE_BYTES)
    for y in range(8):
        lo = hi = 0
        for x in range(8):
            v = px[y * 8 + x]
            lo = (lo << 1) | (v & 1)
            hi = (hi << 1) | ((v >> 1) & 1)
        out[y] = lo
        out[y + 8] = hi
    return bytes(out)


def read_hud_labels(rom: "Rom"):
    """(names in order, their tile runs, the cleaned tile data)."""
    chr_data = rom.chr_bank(0)
    names = list(HUD_LABELS)
    tiles = bytearray()
    runs = []
    index = 0
    for name in names:
        runs.append((index, len(HUD_LABELS[name])))
        for t in HUD_LABELS[name]:
            tiles += strip_grid(chr_data, t)
            index += 1
    # ...and out through the same 2bpp-to-4bpp conversion every other tile
    # block goes through. Emitting the NES bytes straight is how these came out
    # as a scatter of faint dots: the GBA reads a tile as 32 bytes, not 16.
    return names, runs, convert_tiles(bytes(tiles))


def read_gameover_tiles(rom):
    """gameOverTiles as GAMEOVER_ROWS rows of GAMEOVER_COLS tile ids."""
    raw = rom.at(GAMEOVER_TILES_ADDR, GAMEOVER_COLS * GAMEOVER_ROWS)
    return [list(raw[r * GAMEOVER_COLS:(r + 1) * GAMEOVER_COLS])
            for r in range(GAMEOVER_ROWS)]


def read_bonus_tiles(rom):
    """statsTiles1, the BONUS heading, as BONUS_ROWS rows of BONUS_COLS."""
    raw = rom.at(BONUS_TILES_ADDR, BONUS_COLS * BONUS_ROWS)
    return [list(raw[r * BONUS_COLS:(r + 1) * BONUS_COLS])
            for r in range(BONUS_ROWS)]


def read_stats_icons(nametable, attributes):
    """The cartridge's seven piece icons: two rows of seven tiles, plus the
    palette each column is drawn in.

    The colours are not decoration to be picked: the attribute table gives the
    I its own palette (bank 3), T/O/J/L a second (bank 1) and S/Z a third
    (bank 2), which is why the row is not seven olive tetrominoes. The bars
    above them are all bank 1."""
    tiles = [[nametable[r * 32 + c] for c in range(*STATS_ICON_COLS)]
             for r in range(*STATS_ICON_ROWS)]
    banks = [attribute_palette(attributes, c, STATS_ICON_ROWS[0])
             for c in range(*STATS_ICON_COLS)]
    bar_bank = attribute_palette(attributes, STATS_ICON_COLS[0],
                                  STATS_ICON_ROWS[0] - 2)
    return tiles, banks, bar_bank


def emit_coop_screen_header(tiles, palettes, keep_cols, source):
    """screen_coop.h — the cartridge's own coop layout, two columns narrower.

    Nothing is resequenced here: screen 5 already puts a twelve-wide field in
    the middle with a panel either side, so the only change is the two columns
    every screen gives up to fit thirty, taken one from each end. See
    COOP_SEGMENTS.
    """
    origin_x, origin_y = playfield_origin(keep_cols, COOP_PLAY_COL)
    lines = [
        "/*",
        " * screen_coop.h — the COOPERATIVE screen, from the ROM's own nametable.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * Coop is the one mode the cartridge already draws symmetrically: a",
        " * TWELVE-column field in the middle (its wall nibbles are left open,",
        " * main.asm.txt:3480-3489) with a seven-column panel either side, and the",
        " * dancers' ledges down both of them. So this is the cartridge's screen",
        " * with one column dropped from each end, nothing moved.",
        " *",
        " * Tiles marked 0 are blank in the ROM because the game draws over them at",
        " * runtime; the port does the same — including the ROM's own LEVEL /",
        " * HIGH / SCORE lettering, which the port writes itself from the same",
        " * tiles.",
        " */",
        "#ifndef SCREEN_COOP_H",
        "#define SCREEN_COOP_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SCREEN_COOP_W 30",
        "#define SCREEN_COOP_H_TILES 30",
        "",
        "/* Where the twelve-wide playfield's top-left cell sits. */",
        f"#define SCREEN_COOP_FIELD_TX {origin_x}",
        f"#define SCREEN_COOP_FIELD_TY {origin_y}",
        "",
        "static const uint8_t kScreenCoopTiles[900] = {",
    ]
    for i in range(0, len(tiles), 30):
        lines.append("    " + ", ".join(f"0x{t:02X}" for t in tiles[i:i + 30]) + ",")
    lines += ["};", "",
              "static const uint8_t kScreenCoopPalettes[900] = {"]
    for i in range(0, len(palettes), 30):
        lines.append("    " + ", ".join(str(p) for p in palettes[i:i + 30]) + ",")
    lines += ["};", "", "#endif /* SCREEN_COOP_H */", ""]
    return "\n".join(lines)


def emit_leaderboard_header(tiles, palettes, keep_cols, source):
    """screen_leaderboard.h — the HIGH SCORES page, from the ROM's nametable.

    The ROM draws each row's contents at runtime starting at PPU $218B
    (initializeLeaderboard, main.asm.txt:2996-3010): three initials, a blank,
    six score digits, a blank, three line digits. Those columns are measured
    off the reflow rather than typed in, so moving a segment moves them.
    """
    def port_col(c):
        return keep_cols.index(c)

    rows = list(LEADER_ROWS)
    out = []
    for r in rows:
        out.append((tiles[r * 30:(r + 1) * 30], palettes[r * 30:(r + 1) * 30]))

    lines = [
        "/*",
        " * screen_leaderboard.h — the HIGH SCORES page, from the ROM's own",
        " * nametable (screen 8).",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * Two columns narrower and ten rows shorter than the cartridge's, and",
        " * both are taken from places that hold nothing: the columns out of the",
        " * blank gutters inside the frame, the rows out of the arch above the",
        " * table. See LEADER_SEGMENTS and LEADER_ROWS.",
        " */",
        "#ifndef SCREEN_LEADERBOARD_H",
        "#define SCREEN_LEADERBOARD_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SCREEN_LEADER_W 30",
        f"#define SCREEN_LEADER_H_TILES {len(rows)}",
        "",
        "/* The row the heading is on, and the first of the fifteen entries. */",
        f"#define SCREEN_LEADER_HEAD_TY {rows.index(10)}",
        f"#define SCREEN_LEADER_FIRST_TY {rows.index(12)}",
        "#define SCREEN_LEADER_ENTRIES 15",
        "",
        "/* ...and the columns the ROM writes each row's own text into. The",
        " * rank is not among them: '1.' to '15.' are in the nametable, art like",
        " * the frame around them. */",
        f"#define SCREEN_LEADER_NAME_TX {port_col(11)}",
        f"#define SCREEN_LEADER_SCORE_TX {port_col(15)}",
        f"#define SCREEN_LEADER_LINES_TX {port_col(22)}",
        "",
        f"static const uint8_t kScreenLeaderTiles[{len(rows) * 30}] = {{",
    ]
    for row, _pal in out:
        lines.append("    " + ", ".join(f"0x{t:02X}" for t in row) + ",")
    lines += ["};", "",
              f"static const uint8_t kScreenLeaderPalettes[{len(rows) * 30}] = {{"]
    for _row, pal in out:
        lines.append("    " + ", ".join(str(p) for p in pal) + ",")
    lines += ["};", "", "#endif /* SCREEN_LEADERBOARD_H */", ""]
    return "\n".join(lines)


def emit_screen_header(tiles, palettes, keep_cols, source, stats):
    origin_x, origin_y = playfield_origin(keep_cols)
    lines = [
        "/*",
        " * screen_1p.h — the 1P screen layout, taken from the ROM's own nametable.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        f" * Source: {source}",
        " *",
        " * The cartridge's columns, resequenced so the ten playable ones land in",
        " * the middle of the GBA's screen with the HUD split either side. Every",
        " * run of columns moves whole; nothing is scaled and nothing is cropped.",
        " * See SCREEN_SEGMENTS in tools/extract_assets.py for the order.",
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
    lines += [
        "};",
        "",
        "/* The piece-statistics histogram, from the ROM: seven icons two tiles",
        " * tall (nametable rows 26-27, columns 21-27) with a bar growing up out",
        " * of each. A bar cell is STATS_BAR_TILE + (count & 7), so eight pieces",
        " * fill one tile and move the next write a row up (main.asm.txt:3752-3798).",
        " * Indexed [row][piece], piece 0 = I. */",
        "#define SCREEN_1P_STATS_PIECES 7",
        f"#define SCREEN_1P_STATS_BAR_TILE 0x{STATS_BAR_TILE:02X}",
        "static const uint8_t kStatsIcons[2][SCREEN_1P_STATS_PIECES] = {",
    ]
    icons, icon_banks, bar_bank, gameover, bonus, banner, braid, labels = stats
    for row in icons:
        lines.append("    { " + ", ".join(f"0x{t:02X}" for t in row) + " },")
    lines += [
        "};",
        "",
        "/* Which of the four background palettes each icon is drawn in, off the",
        " * ROM's own attribute table. The bars all share one. */",
        "static const uint8_t kStatsIconBanks[SCREEN_1P_STATS_PIECES] = { "
        + ", ".join(str(b) for b in icon_banks) + " };",
        f"#define SCREEN_1P_STATS_BAR_BANK {bar_bank}",
        "",
        "/* gameOverTiles: the plaque, six by four, and with it the cartridge's",
        " * own thin box frame — see the note in tools/extract_assets.py. */",
        f"#define SCREEN_1P_GAMEOVER_W {GAMEOVER_COLS}",
        f"#define SCREEN_1P_GAMEOVER_H {GAMEOVER_ROWS}",
        "static const uint8_t kGameOverTiles[SCREEN_1P_GAMEOVER_H]"
        "[SCREEN_1P_GAMEOVER_W] = {",
    ] + [
        "    { " + ", ".join(f"0x{v:02X}" for v in gameover[r]) + " },"
        for r in range(GAMEOVER_ROWS)
    ] + [
        "};",
        "",
        "/* statsTiles1: the BONUS heading the level-up tally is written",
        " * under, ten by two. Its own tiles, the same table as the plaque. */",
        f"#define SCREEN_1P_BONUS_W {BONUS_COLS}",
        f"#define SCREEN_1P_BONUS_H {BONUS_ROWS}",
        "static const uint8_t kBonusTiles[SCREEN_1P_BONUS_H][SCREEN_1P_BONUS_W] = {",
    ] + [
        "    { " + ", ".join(f"0x{v:02X}" for v in bonus[r]) + " },"
        for r in range(BONUS_ROWS)
    ] + [
        "};",
        "",
        "/* The vertical TETRIS banner, NES columns 14-17 rows 10-27. */",
        f"#define SCREEN_1P_BANNER_W {BANNER_COLS[1] - BANNER_COLS[0]}",
        f"#define SCREEN_1P_BANNER_H {BANNER_ROWS[1] - BANNER_ROWS[0]}",
        "static const uint8_t kBannerTiles[SCREEN_1P_BANNER_H][SCREEN_1P_BANNER_W] = {",
    ] + [
        "    { " + ", ".join(f"0x{v:02X}" for v in row) + " }," for row in banner[0]
    ] + [
        "};",
        "static const uint8_t kBannerBanks[SCREEN_1P_BANNER_H][SCREEN_1P_BANNER_W] = {",
    ] + [
        "    { " + ", ".join(str(v) for v in row) + " }," for row in banner[1]
    ] + [
        "};",
        "",
        "/* The same frame, named for what each piece does. */",
        f"#define T_BOX_TL 0x{gameover[0][0]:02X}",
        f"#define T_BOX_TOP 0x{gameover[0][1]:02X}",
        f"#define T_BOX_TR 0x{gameover[0][GAMEOVER_COLS - 1]:02X}",
        f"#define T_BOX_L 0x{gameover[1][0]:02X}",
        f"#define T_BOX_R 0x{gameover[1][GAMEOVER_COLS - 1]:02X}",
        f"#define T_BOX_BL 0x{gameover[3][0]:02X}",
        f"#define T_BOX_BOTTOM 0x{gameover[3][1]:02X}",
        f"#define T_BOX_BR 0x{gameover[3][GAMEOVER_COLS - 1]:02X}",
        "",
        "/* THE BRAID, AS A FRAME. The blue rope beside the playfield is the",
        " * same weave the cartridge borders its whole screen with, so it has",
        " * corners and horizontal runs as well as the vertical ones. Two tiles",
        " * thick on every side, because each tile is half the rope cut",
        " * lengthwise. See read_braid_frame in tools/extract_assets.py. */",
        f"#define BRAID_BANK {braid[2]}",
        "",
        "/* THE HEADER GRID'S RULE, and the labels with its stubs taken out of",
        " * them. See read_hud_labels in tools/extract_assets.py: the grid is",
        " * colour 3 and the lettering is colour 1, so this is a subtraction",
        " * from the cartridge's own tiles, not a redrawing of them. */",
        f"#define T_GRID_RULE 0x{HUD_RULE_TILE:02X}",
        f"#define HUD_LABEL_COUNT {len(labels[0])}",
    ] + [
        f"#define HUD_LABEL_{name} {first}, {run}"
        for name, (first, run) in zip(labels[0], labels[1])
    ] + [
        # ...and the width on its own, for the callers that centre a label
        # rather than just placing it: the pair above cannot be indexed.
        f"#define HUD_LABEL_{name}_W {run}"
        for name, (first, run) in zip(labels[0], labels[1])
    ] + [
        f"static const uint8_t kHudLabelTiles[{len(labels[2])}] = {{",
    ] + [
        "    " + ", ".join(f"0x{b:02X}" for b in labels[2][i:i + 16]) + ","
        for i in range(0, len(labels[2]), 16)
    ] + [
        "};",
    ] + [
        f"static const uint8_t kBraid{name.upper()}[2][2] = {{ "
        + ", ".join("{ " + ", ".join(f"0x{v:02X}" for v in row) + " }"
                    for row in braid[0][name]) + " };"
        for name in ("tl", "tr", "bl", "br")
    ] + [
        "/* One repeating cell of each side: the top and bottom are one column",
        " * by two rows, the sides two columns by one row. */",
    ] + [
        f"static const uint8_t kBraid{name.capitalize()}"
        f"[{len(braid[1][name])}][{len(braid[1][name][0])}] = {{ "
        + ", ".join("{ " + ", ".join(f"0x{v:02X}" for v in row) + " }"
                    for row in braid[1][name]) + " };"
        for name in ("top", "bottom", "left", "right")
    ] + [
        "",
        "#endif /* SCREEN_1P_H */",
        "",
    ]
    return "\n".join(lines)


def _title_col(src_col):
    """Where a cartridge column ends up on the composed title screen."""
    cols = [c for start, end in TITLE_COL_BLOCKS for c in range(start, end)]
    return cols.index(src_col)


def _spire_y(src_row):
    """Where a dropped spire row lands, in pixels.

    Its own row is not in the layout, so it goes where the cell it is printed
    over would be: the logo rows the cartridge's own composition already uses
    for the tip, two rows up from the source. Plus the lift."""
    return _title_row(src_row - 2) * 8 + TITLE_SPIRE_LIFT


def _title_row(src_row):
    rows = [r for start, end in TITLE_ROW_BLOCKS for r in range(start, end)]
    return rows.index(src_row)


def compose_title(nametable, attributes):
    """32x30 title screen -> a 30x20 layout that keeps every element.

    Returns (tiles, palette banks), both flat lists of 600. The vertical fit
    comes from dropping the thick top and bottom borders rather than from
    scaling or from cutting artwork: see TITLE_ROW_BLOCKS.
    """
    cols = [c for start, end in TITLE_COL_BLOCKS for c in range(start, end)]
    assert len(cols) <= 30, f"title asks for {len(cols)} columns, screen has 30"

    rows = []
    for start, end in TITLE_ROW_BLOCKS:
        rows.extend(range(start, end))
    assert len(rows) == 20, f"title needs 20 rows, got {len(rows)}"

    tiles = [nametable[r * 32 + c] for r in rows for c in cols]
    banks = [attribute_palette(attributes, c, r) for r in rows for c in cols]

    # THE SPIRE, PUT BACK OVER THE LOGO. Dropping source rows 12-13 takes the
    # top of the cathedral's one-tile-wide spire with them, and the tip is the
    # thing the eye misses. Both tiles go back into the logo — see
    # TITLE_SPIRE_OVERLAY for which cells will take them and why — so the
    # spire runs up behind the lettering and comes out at the top whole,
    # rather than as a finial floating over a gap.
    for src_row, src_col, dst_row, expect in TITLE_SPIRE_OVERLAY:
        if dst_row not in rows or src_col not in cols:
            continue
        if nametable[dst_row * 32 + src_col] != expect:
            raise ValueError(
                f"the spire would land on unexpected artwork at ({src_col},"
                f"{dst_row}): tile {nametable[dst_row * 32 + src_col]:#04x}, "
                f"expected {expect:#04x}")
        i = rows.index(dst_row) * len(cols) + cols.index(src_col)
        tiles[i] = nametable[src_row * 32 + src_col]
        banks[i] = attribute_palette(attributes, src_col, src_row)

    return tiles, banks


# ---------------------------------------------------------------------------
# THE PROTOTYPE TITLE SCREENS — the easter egg skins.
#
# Tengen shipped this game twice, and the cartridges it made BEFORE the
# release — while the licence was still Nintendo's — carry other title screens
# entirely. This pulls them out of prototype dumps so the port can offer them.
#
# HOW, and why it changed. The first pass read one screen out of a fixed PRG
# address ($A3C4), having found it by rendering every pointer in one dump's
# upload table and looking. That worked for exactly that dump and left a note
# in CLAUDE.md saying the other two "do not store their title flat" and would
# need a disassembly each. THAT NOTE WAS WRONG, and the mistake was method,
# not fact: it looked for the screens in the ROM instead of asking the ROM for
# them. Every one of these builds draws its own title at boot, so the way to
# get it is the way read_screen already gets the release's — RUN THE
# CARTRIDGE. Reset vector, nes_cpu with a PPU behind it, an NMI every so many
# instructions so the vblank handler gets to do the uploads the reset code
# only stages in RAM (the palette is one of them), and then read $2000-$23FF
# and $3F00 back. No address to guess and no format to recognise: all three
# dumps give up complete, distinct screens, and two of them are screens the
# old method could not see at all.
#
# It is also more faithful for the dump the old method DID read. That flat
# blob is the picture only; "TM (c)1987 ACADEMYSOFT-ELORG." and "(c)1988
# TENGEN." are written afterwards by a separate text routine, so the shipped
# skin was missing both lines and the composition below had been dropping
# their rows as blank.
#
# The screens are stable: the ROM sits on the title waiting for Start, and the
# capture is byte-identical at 500k, 1M, 1.5M and 3M instructions.
PROTO_BOOT_STEPS = 1_200_000
PROTO_NMI_EVERY = 30_000
PROTO_SCREEN_BYTES = 0x400      # 960 nametable + 64 attributes


def boot_prototype(data: bytes):
    """Run a prototype dump to its title screen and read the PPU back.

    Returns (screen, palette, chr_rom) — 1024 bytes of nametable plus
    attributes, sixteen background colour indices, and the whole CHR.
    """
    from nes_cpu import Bus, CPU

    prg_banks, chr_banks = data[4], data[5]
    prg = data[NES_HEADER:NES_HEADER + prg_banks * 16384]
    chr_off = NES_HEADER + prg_banks * 16384
    chr_rom = data[chr_off:chr_off + chr_banks * 8192]
    if prg_banks == 1:
        prg = prg + prg          # NROM mirrors its single bank into $C000

    bus = Bus(prg, strict=False, ppu=True)
    cpu = CPU(bus)
    cpu.pc = bus.read(0xFFFC) | (bus.read(0xFFFD) << 8)
    cpu.sp = 0xFD
    nmi = bus.read(0xFFFA) | (bus.read(0xFFFB) << 8)
    try:
        for step in range(PROTO_BOOT_STEPS):
            cpu.step()
            if step and step % PROTO_NMI_EVERY == 0:
                cpu.push((cpu.pc >> 8) & 0xFF)
                cpu.push(cpu.pc & 0xFF)
                cpu.push(cpu.p | 0x20)
                cpu.pc = nmi
    except Exception:
        # A build that wanders off is still worth reading: the title is up
        # long before it does. An empty screen is caught by the caller.
        pass
    return bytes(bus.vram[:PROTO_SCREEN_BYTES]), bytes(bus.pal[:16]), chr_rom


# WHICH PATTERN TABLE the background reads from is NOT in PPUCTRL when the
# screen is drawn: all three dumps upload their nametables during forced blank
# with PPUCTRL clear, and one of them never sets bit 4 at all in this
# interpreter. So it is measured instead — a screen rendered out of the wrong
# half of the CHR leaves tiles it uses BLANK, and the right half leaves only
# the two the screens really do use blank (tile $00 for the black field and
# $20 for the space in the text). Ties go to the upper half, which is where
# all three of these builds keep their title art; a tie only happens for a
# dump this file has no recipe for, and the generated header says so.
def proto_pattern_table(screen: bytes, chr_rom: bytes) -> int:
    used = set(screen[:960])
    best, best_score = 1, None
    for bank in (1, 0):
        base = bank * CHR_BANK
        if base + CHR_BANK > len(chr_rom):
            continue
        score = sum(1 for t in used
                    if not any(chr_rom[base + t * 16:base + t * 16 + 16]))
        if best_score is None or score < best_score:
            best, best_score = bank, score
    return best


# ---------------------------------------------------------------------------
# FITTING 32x30 INTO 30x20.
#
# The GBA is two columns and ten rows short of an NES screen, and which ten
# rows go is a composition decision, not an extraction one — so it is made by
# hand, per screen, and keyed to the screen itself. The key is the MD5 of the
# captured 1024 bytes: it identifies the screen rather than the file, so a
# redump or a rename still matches, and a screen this file has never seen is
# never silently squeezed by a recipe meant for another.
#
# Two columns come off every one of them the same way, because all three put
# their content inside columns 1-30: the outer column each side, which on the
# bordered screens is the thin rule outside the fret and on the unbordered one
# is empty.
PROTO_COLS = tuple(range(1, 31))


def _rows(*spans):
    return tuple(r for a, b in spans for r in range(a, b))


PROTO_RECIPES = {
    # "TETRIS" over a Moscow skyline, in a plain grey box, over four lines of
    # licence text ending "NINTENDO OF AMERICA INC." — the screen from before
    # the lawsuit. It is the only one of the three that FITS: its picture is
    # eleven rows and its text four, so the ten rows dropped are all blank and
    # nothing of it is lost. Two blank rows above the box, two below it and
    # one under the last line of text, which is as close to centred as a
    # fifteen-row subject gets in twenty.
    "c229f45adcfb7b385472dca79636bed7": {
        "label": "TETRIS, Nintendo-licensed",
        "rows": _rows((4, 19), (24, 29)),
        "bank": 1,
    },
    # "TENGEN PRESENTS / TETRIS" over St Basil's, in the green fret border —
    # the skin this port already had, now with the two copyright lines the
    # flat-blob method could not see. Everything here is wanted and twenty-one
    # rows want to be kept, so ONE row of cathedral goes: row 13, the shoulder
    # where the central tower's tent starts to widen. The four rows above it
    # are the thin spire and its finial, which go the way the release's own
    # composition gives up its spire; the blank row between PRESENTS and the
    # logo STAYS, because buying it leaves the big letters' ascenders sitting
    # in the word above.
    "ac327eca3d9f9c169210adc9ccfe8344": {
        "label": "TENGEN PRESENTS TETRIS",
        "rows": _rows((1, 9), (14, 23), (24, 25), (26, 27), (28, 29)),
        "bank": 1,
    },
    # "TENGEN PRESENTS / THE SOVIET MIND GAME" over the same cathedral, which
    # is the same screen with the logo replaced by a line of text. Its heading
    # is one row taller and its sky four rows of nothing, so two of those stay
    # as breathing room and the spire goes whole.
    "087bde3f4dd561c60fd03b65d7b21fca": {
        "label": "THE SOVIET MIND GAME",
        "rows": _rows((1, 8), (14, 25), (26, 27), (28, 29)),
        "bank": 1,
    },
}


def blank_tile_ids(chr_rom: bytes, bank: int) -> set:
    base = bank * CHR_BANK
    return {t for t in range(256)
            if not any(chr_rom[base + t * 16:base + t * 16 + 16])}


def _drop_evenly(droppable, keep, need):
    """Drop `need` indices from `droppable`, taking from the longest runs.

    Runs are what a screen's empty space comes in — a band of sky, a margin —
    and taking from the middle of the longest one each time spreads the loss
    instead of gutting one band.
    """
    keep = list(keep)
    for _ in range(need):
        runs, run = [], []
        for i in keep:
            if i in droppable:
                run.append(i)
            elif run:
                runs.append(run)
                run = []
        if run:
            runs.append(run)
        if not runs:
            return None
        longest = max(runs, key=len)
        keep.remove(longest[len(longest) // 2])
    return keep


def auto_compose(screen: bytes, chr_rom: bytes, bank: int):
    """A best effort for a dump with no recipe: drop only what renders blank."""
    blank = blank_tile_ids(chr_rom, bank)
    rows = [r for r in range(30)
            if all(screen[r * 32 + c] in blank for c in range(32))]
    cols = [c for c in range(32)
            if all(screen[r * 32 + c] in blank for r in range(30))]
    keep_rows = _drop_evenly(set(rows), range(30), 10)
    keep_cols = _drop_evenly(set(cols), range(32), 2)
    if keep_rows is None or keep_cols is None:
        return None
    return tuple(keep_rows), tuple(keep_cols)


def compose_proto_title(screen: bytes, rows, cols):
    """A prototype title, composed to 30x20 out of the captured screen."""
    nametable, attributes = screen[:960], screen[960:1024]
    assert len(cols) == 30 and len(rows) == 20, \
        f"a prototype title needs 30x20, got {len(cols)}x{len(rows)}"
    tiles = [nametable[r * 32 + c] for r in rows for c in cols]
    banks = [attribute_palette(attributes, c, r) for r in rows for c in cols]
    return tiles, banks


def read_prototype(path):
    """One skin, as a dict, or (None, reason) if this dump cannot give one."""
    try:
        data = open(path, "rb").read()
    except OSError as exc:
        return None, f"no pude leer {path}: {exc}"
    if data[:4] != b"NES\x1a":
        return None, f"{path} no es un fichero iNES"
    if data[5] == 0:
        return None, f"{path} usa CHR RAM, asi que no lleva tiles"
    screen, palette, chr_rom = boot_prototype(data)
    if not any(screen[:960]):
        return None, (f"{path} no dibujo ninguna pantalla al arrancar "
                      f"({PROTO_BOOT_STEPS} instrucciones)")
    key = hashlib.md5(screen).hexdigest()
    recipe = PROTO_RECIPES.get(key)
    if recipe:
        rows, cols, bank = recipe["rows"], PROTO_COLS, recipe["bank"]
        label, how = recipe["label"], "recipe"
    else:
        bank = proto_pattern_table(screen, chr_rom)
        fit = auto_compose(screen, chr_rom, bank)
        if fit is None:
            return None, (f"{path}: su pantalla (md5 {key}) no tiene diez filas "
                          "y dos columnas en blanco que quitar, y este fichero "
                          "no trae receta para ella")
        rows, cols = fit
        label, how = f"unknown screen (md5 {key})", "automatic"
    if (bank + 1) * CHR_BANK > len(chr_rom):
        return None, f"{path} no tiene banco CHR {bank}"
    tiles, banks = compose_proto_title(screen, rows, cols)
    return {
        "label": label,
        "how": how,
        "source": path,
        "tiles": tiles,
        "banks": banks,
        "palette": palette,
        "chr": convert_tiles(chr_rom[bank * CHR_BANK:(bank + 1) * CHR_BANK]),
        "bank": bank,
    }, None


def _table(lines, decl, rows, fmt, per):
    lines.append(decl)
    for skin_rows in rows:
        lines.append("    {")
        for i in range(0, len(skin_rows), per):
            lines.append("        " + ", ".join(fmt(v)
                                                for v in skin_rows[i:i + per]) + ",")
        lines.append("    },")
    lines += ["};", ""]


def emit_proto_header(skins, notes):
    chr_bytes = len(skins[0]["chr"])
    lines = [
        "/*",
        " * screen_proto.h — the PROTOTYPE cartridges' title screens.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        " *",
        " * An easter egg: L+R on the title cycles through the builds Tengen",
        " * made before the release, each with its own art, tiles and palettes.",
        " * They are CAPTURED, not read out of the ROM: each dump is booted on",
        " * the 6502 interpreter and its own code draws its own screen. See",
        " * tools/extract_assets.py.",
        " *",
    ]
    for i, skin in enumerate(skins):
        lines += [f" * {i}: {skin['label']}",
                  f" *    {skin['source']}  (CHR bank {skin['bank']}, fitted by {skin['how']})"]
    lines += [note for note in notes]
    lines += [
        " */",
        "#ifndef SCREEN_PROTO_H",
        "#define SCREEN_PROTO_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SCREEN_PROTO_AVAILABLE 1",
        f"#define SCREEN_PROTO_COUNT {len(skins)}",
        "#define SCREEN_PROTO_W 30",
        "#define SCREEN_PROTO_H_TILES 20",
        f"#define TILES_PROTO_BYTES {chr_bytes}",
        "",
    ]
    _table(lines,
           f"static const uint8_t kRomPalette_bg_proto[SCREEN_PROTO_COUNT][16] = {{",
           [s["palette"] for s in skins], lambda b: f"0x{b:02X}", 16)
    _table(lines,
           "static const uint8_t kScreenProtoTiles[SCREEN_PROTO_COUNT][600] = {",
           [s["tiles"] for s in skins], lambda t: f"0x{t:02X}", 30)
    _table(lines,
           "static const uint8_t kScreenProtoPalettes[SCREEN_PROTO_COUNT][600] = {",
           [s["banks"] for s in skins], str, 30)
    _table(lines,
           "static const uint8_t kProtoTiles[SCREEN_PROTO_COUNT]"
           "[TILES_PROTO_BYTES] = {",
           [s["chr"] for s in skins], lambda b: f"0x{b:02X}", 16)
    lines += ["#endif /* SCREEN_PROTO_H */", ""]
    return "\n".join(lines)


def emit_proto_absent_header(why):
    return "\n".join([
        "/*",
        " * screen_proto.h — the prototype title skins are NOT in this build.",
        " *",
        " * GENERATED by tools/extract_assets.py — do not edit by hand.",
        " *",
    ] + [f" * {line}" for line in why] + [
        " *",
        " * Rebuild with prototype dumps to get them — as many as you like:",
        " *     make assets ROM=tetris.nes PROTO=\"proto_a.nes proto_b.nes\"",
        " *",
        " * The port compiles either way; without them, L+R on the title",
        " * simply has nothing to switch to.",
        " */",
        "#ifndef SCREEN_PROTO_H",
        "#define SCREEN_PROTO_H",
        "",
        "#define SCREEN_PROTO_AVAILABLE 0",
        "#define SCREEN_PROTO_COUNT 0",
        "",
        "#endif /* SCREEN_PROTO_H */",
        "",
    ])


def build_proto_header(paths):
    """The prototype skins' header, or one that says why there are none."""
    if not paths:
        return emit_proto_absent_header(
            ["No se paso ninguna ROM de prototipo (PROTO=...)."])
    skins, rejected = [], []
    for path in paths:
        skin, why = read_prototype(path)
        if skin is None:
            rejected.append(why)
            print(f"  prototipo descartado: {why}")
        else:
            skins.append(skin)
            print(f"  prototipo: {skin['label']} <- {path} "
                  f"(CHR {skin['bank']}, encaje {skin['how']})")
    if not skins:
        return emit_proto_absent_header(rejected)
    notes = [" *", " * Descartados:"] + [f" *   {w}" for w in rejected] if rejected else []
    return emit_proto_header(skins, notes)



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


def title_col_map():
    """NES nametable column -> the column it occupies on the port's title.

    The composition drops one column from each side of the picture (see
    TITLE_COL_BLOCKS), so a sprite's column is no longer a fixed offset from
    the nametable's: the picture keeps its place, and only the frame's right
    half moves two columns left. 0xFF for a dropped column."""
    out = [0xFF] * 32
    col = 0
    for start, end in TITLE_COL_BLOCKS:
        for src in range(start, end):
            out[src] = col
            col += 1
    return out


def title_row_map():
    """NES nametable row -> the row it occupies on the port's title screen.

    0xFF for the rows TITLE_ROW_BLOCKS leaves out. This is the same list the
    artwork is cut with, so the sprites cannot drift away from the picture
    they are drawn over."""
    out = [0xFF] * 30
    row = 0
    for start, end in TITLE_ROW_BLOCKS:
        for src in range(start, end):
            out[src] = row
            row += 1
    return out


def emit_title_header(tiles, banks, source):
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
        " * The screen uses ALL FOUR of bgPalette0's palettes, chosen per 2x2",
        " * block by its attribute table — that is where the cathedral's reds and",
        " * greens come from. An earlier pass here assumed one palette covered it",
        " * and produced a monochrome title.",
        " */",
        "#ifndef SCREEN_TITLE_H",
        "#define SCREEN_TITLE_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SCREEN_TITLE_W {sum(e - s for s, e in TITLE_COL_BLOCKS)}",
        "",
        "/* The spire's tip, drawn as OBJECTS over the logo rather than into",
        " * it — see TITLE_SPIRE_SPRITES. Each entry is a tile id, the pixel",
        " * it goes at, and the background palette its colours come from; the",
        " * rows the source tiles live on are dropped by the reflow, so these",
        " * are where those cells WOULD have landed, a pixel lower. */",
        f"#define SCREEN_TITLE_SPIRE_COUNT {len(TITLE_SPIRE_SPRITES)}",
        "static const struct { uint8_t tile; uint8_t x, y, bank; }",
        "    kTitleSpire[SCREEN_TITLE_SPIRE_COUNT] = {"] + [
        f"    {{ 0x{t:02X}, {_title_col(c) * 8}, "
        f"{_spire_y(r)}, {b} }},"
        for t, c, r, b in TITLE_SPIRE_SPRITES
    ] + [
        "};",
        "#define SCREEN_TITLE_H_TILES 20",
        f"#define SCREEN_TITLE_KEEP_COL0 {TITLE_COL_BLOCKS[0][0]}",
        "",
        "/* WHERE A NES SPRITE LANDS ON THIS SCREEN.",
        " *",
        " * The title art is not a window onto the NES picture, it is a",
        " * COMPOSITION: ten rows come out of the middle of the artwork (see",
        " * TITLE_ROW_BLOCKS) so the frame and both copyright lines survive on",
        " * twenty rows. The cathedral overlay and the fireworks are placed in",
        " * NES screen pixels by the cartridge's own code, so they have to be",
        " * put through the same rearrangement: this maps a NES tile row to the",
        " * row it ended up on, or 0xFF for a row the composition dropped.",
        " */",
        "#define SCREEN_TITLE_ROW_DROPPED 0xFF",
        "static const uint8_t kTitleRowMap[30] = {",
        "    " + ", ".join(f"0x{v:02X}" for v in title_row_map()) + ",",
        "};",
        "/* ...and the same for columns, because the two the composition drops",
        " * come out of the MIDDLE: everything right of the gap moves left. */",
        "static const uint8_t kTitleColMap[32] = {",
        "    " + ", ".join(f"0x{v:02X}" for v in title_col_map()) + ",",
        "};",
        "",
        f"static const uint8_t kScreenTitleTiles[{len(tiles)}] = {{",
    ]
    width = sum(e - s for s, e in TITLE_COL_BLOCKS)
    for i in range(0, len(tiles), width):
        lines.append("    " + ", ".join(f"0x{t:02X}" for t in tiles[i:i + width]) + ",")
    lines += ["};", "",
              "/* Which of bgPalette0's four palettes each tile uses. */",
              f"static const uint8_t kScreenTitlePalettes[{len(banks)}] = {{"]
    for i in range(0, len(banks), width):
        lines.append("    " + ", ".join(str(b) for b in banks[i:i + width]) + ",")
    lines += ["};", "", "#endif /* SCREEN_TITLE_H */", ""]
    return "\n".join(lines)


def emit_dancer_poses_header(poses, stage_rows, pos_x, pos_y, attrs, source):
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
        f"#define DANCER_SOLO_COUNT {DANCER_SOLO_COUNT}",
        f"#define DANCER_STAGE_COLS {DANCER_STAGE_COLS}",
        f"#define DANCER_STAGE_ROWS {DANCER_STAGE_ROWS}",
        "",
        "static const uint8_t kDancerPoses[DANCER_POSE_COUNT][4] = {",
    ]
    for pose in poses:
        lines.append("    { " + ", ".join(f"0x{b:02X}" for b in pose) + " },")
    lines += [
        "};",
        "",
        "/* Their stage, straight out of the level-up blit: five ledges of tile",
        " * $9D, one every three rows, 24px apart — the same 24px the dancer",
        " * positions below are spaced by. */",
        f"static const uint8_t kDancerStage[{DANCER_STAGE_ROWS}][{DANCER_STAGE_COLS}] = {{",
    ]
    for row in stage_rows:
        lines.append("    { " + ", ".join(f"0x{b:02X}" for b in row) + " },")
    lines += [
        "};",
        "",
        "/* Where each dancer starts, in NES screen pixels. They walk right from",
        " * here, one pixel every four frames, onto the ledges. */",
        f"static const uint8_t kDancerStartX[DANCER_SOLO_COUNT] = {{ "
        + ", ".join(f"0x{b:02X}" for b in pos_x[:DANCER_SOLO_COUNT]) + " };",
        f"static const uint8_t kDancerStartY[DANCER_SOLO_COUNT] = {{ "
        + ", ".join(f"0x{b:02X}" for b in pos_y[:DANCER_SOLO_COUNT]) + " };",
        "",
        "/* COOP'S OWN EIGHT, entries 6-13 of the same tables: two COLUMNS",
        " * rather than one, at NES x $40 and $B1 — just inside each panel —",
        " * and four heights, so the pair on each ledge walks out to its own",
        " * side. Attribute bit 6 is set on the left-hand ones: they are the",
        " * same sprite MIRRORED, which is what makes a column walking left",
        " * face the way it is going. Coop is the only mode that uses all",
        " * eight, which is why tengen_dancer_count caps at six elsewhere. */",
        f"#define DANCER_COOP_FIRST {DANCER_SOLO_COUNT}",
        f"#define DANCER_COOP_COUNT {DANCER_POS_COUNT - DANCER_SOLO_COUNT}",
        "static const uint8_t kDancerCoopX[DANCER_COOP_COUNT] = { "
        + ", ".join(f"0x{b:02X}" for b in pos_x[DANCER_SOLO_COUNT:]) + " };",
        "static const uint8_t kDancerCoopY[DANCER_COOP_COUNT] = { "
        + ", ".join(f"0x{b:02X}" for b in pos_y[DANCER_SOLO_COUNT:]) + " };",
        "static const uint8_t kDancerCoopAttr[DANCER_COOP_COUNT] = { "
        + ", ".join(f"0x{b:02X}" for b in attrs[DANCER_SOLO_COUNT:]) + " };",
        "",
        "/* The OAM attribute byte each one is given: its low two bits pick one",
        " * of spritePalette2's four palettes, which is why the six are not all",
        " * the same colour. */",
        f"static const uint8_t kDancerAttr[DANCER_SOLO_COUNT] = {{ "
        + ", ".join(f"0x{b:02X}" for b in attrs[:DANCER_SOLO_COUNT]) + " };",
        "",
        "#endif /* DANCER_POSES_H */",
        "",
    ]
    return "\n".join(lines)


def emit_palette_header(palette_sets, piece_palettes, source):
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
        "/* Each set is the four palettes one updatePalette call installs, four",
        " * entries each (the first of each is the shared backdrop). Which screen",
        " * uses which is traced in reference/NOTES.md:",
        " *   title -> bg_title + obj_title, menu -> bg_menu + obj_game,",
        " *   game  -> bg_game  + obj_game,  level-up interlude -> obj_dancers. */",
    ]
    for name, entries in palette_sets.items():
        flat = [b for entry in entries for b in entry]
        lines.append(f"static const uint8_t kRomPalette_{name}[16] = {{")
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in flat))
        lines.append("};")
    lines += [
        "",
        "/* The in-game background set, under the name the renderer already used. */",
        "#define kRomBgPalette kRomPalette_bg_game",
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
    # The board must arrive whole and in order: its left braid, the ten
    # playable columns and its right braid — 14 of the cartridge's columns
    # unbroken and in the cartridge's own order.
    board = list(range(0, 14))
    if keep[8:22] != board:
        failures.append(f"el tablero no llego entero: {keep[8:22]}")
    origin_x, _ = playfield_origin(keep)
    # The whole point of the resequencing: the ten playable columns centred.
    if origin_x * 8 + 40 != 120:
        failures.append(f"el playfield quedo centrado en {origin_x * 8 + 40}px, "
                        "esperado 120 (el centro de la pantalla)")
    # ...and the two boxes either side of it exactly the same width, which is
    # the other half of the point. A layout that centres the board but leaves
    # six columns on one side and four on the other still reads as lopsided.
    left_box = keep.index(0)
    right_box = 30 - (left_box + len(board))
    if left_box != right_box:
        failures.append(f"los recuadros no son simetricos: {left_box} y {right_box}")
    # The board's own columns may not be duplicated anywhere. The boxes are
    # allowed to share source columns — both are cut from the same ten blank
    # ones of the cartridge's score panel, and blank is blank.
    board_cols = [c for c in keep if c < 14]
    if len(set(board_cols)) != len(board_cols):
        failures.append("el reflow repite alguna columna del tablero")

    # The attribute reader, against a byte worked out by hand: $1B is
    # 00 01 10 11 -> top-left 3, top-right 2, bottom-left 1, bottom-right 0
    # reading the pairs from the high bits down.
    attr = bytes([0x1B] + [0] * 63)
    got = (attribute_palette(attr, 0, 0), attribute_palette(attr, 2, 0),
           attribute_palette(attr, 0, 2), attribute_palette(attr, 2, 2))
    if got != (3, 2, 1, 0):
        failures.append(f"lectura de atributos dio {got}, esperado (3, 2, 1, 0)")

    # The title composition must keep 30x20 and must not reorder columns.
    #
    # THE SPIRE'S GUARD HAS TO BE SATISFIED, not stepped around: the fake
    # nametable below is a counter, so its logo cells hold whatever the count
    # lands on rather than the tiles the overlay insists on finding there. The
    # guard is the point of the overlay — it is what stops a different dump
    # being quietly painted over — so the fixture plants the tiles it expects
    # instead of the check being loosened. (Without this the self-test raised
    # rather than reporting, and `make assets-check` had been failing on it.)
    fake = bytearray(bytes(range(256)) * 4)
    for _src_row, src_col, dst_row, expect in TITLE_SPIRE_OVERLAY:
        fake[dst_row * 32 + src_col] = expect
    title, title_banks = compose_title(bytes(fake), bytes(64))
    title_w = sum(e - s for s, e in TITLE_COL_BLOCKS)
    if len(title) != title_w * 20 or len(title_banks) != len(title):
        failures.append(f"la composicion del titulo dio {len(title)} tiles, "
                        f"esperado {title_w * 20}")
    if title_w > 30:
        failures.append(f"el titulo pide {title_w} columnas y la pantalla tiene 30")
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
    ap.add_argument("--proto", action="append", default=[], metavar="DUMP",
                    help="a prototype dump, for the title-skin easter egg; "
                         "repeat it for as many skins as you want to cycle")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.rom:
        ap.error("give a ROM, or --self-test")

    rom = Rom(open(args.rom, "rb").read())
    src = args.rom

    nametable, attributes = read_screen(rom, SCREEN_1P)
    tiles, palettes, keep_cols = reflow_screen(nametable, attributes)

    lead_nt, lead_attr = read_screen(rom, SCREEN_LEADER)
    lead_tiles, lead_palettes, lead_cols = reflow_screen(
        lead_nt, lead_attr, segments=LEADER_SEGMENTS, wall_fix=False)

    coop_nt, coop_attr = read_screen(rom, SCREEN_COOP)
    coop_tiles, coop_palettes, coop_cols = reflow_screen(
        coop_nt, coop_attr, segments=COOP_SEGMENTS, blanks=COOP_BLANK,
        wall_fix=False)

    bg_palette = rom.at(PALETTE_TABLE_ADDR + PALETTE_BG_GAME * PALETTE_SET_BYTES, 16)
    palette_sets = {
        "bg_game": read_palette_set(rom, PALETTE_BG_GAME),
        "bg_title": read_palette_set(rom, PALETTE_BG_TITLE),
        "bg_menu": read_palette_set(rom, PALETTE_BG_MENU),
        "obj_game": read_palette_set(rom, PALETTE_OBJ_GAME),
        "obj_title": read_palette_set(rom, PALETTE_OBJ_TITLE),
        "obj_dancers": read_palette_set(rom, PALETTE_OBJ_DANCERS),
    }
    piece_palettes = [rom.at(PIECE_PALETTE_ADDR + i * 3, 3) for i in range(12)]

    pose_bytes = rom.at(DANCER_POSE_ADDR, DANCER_POSE_END - DANCER_POSE_ADDR)
    poses = [pose_bytes[i:i + 4] for i in range(0, len(pose_bytes), 4)]

    stage = rom.at(DANCER_STAGE_ADDR, DANCER_STAGE_COLS * DANCER_STAGE_ROWS)
    stage_rows = [stage[r * DANCER_STAGE_COLS:(r + 1) * DANCER_STAGE_COLS]
                  for r in range(DANCER_STAGE_ROWS)]
    dancer_x = rom.at(DANCER_POS_X_ADDR, DANCER_POS_COUNT)
    dancer_y = rom.at(DANCER_POS_Y_ADDR, DANCER_POS_COUNT)
    dancer_attr = rom.at(DANCER_ATTR_ADDR, DANCER_POS_COUNT)

    title_nt, title_attr = read_screen(rom, SCREEN_TITLE)
    title_tiles, title_banks = compose_title(title_nt, title_attr)

    menu_nt, menu_attr = read_screen(rom, SCREEN_MENU)
    menu_nt = bytearray(menu_nt)
    for r in range(*MENU_TEXT_ROWS):
        for c in range(*MENU_TEXT_COLS):
            menu_nt[r * 32 + c] = 0
    menu_tiles, menu_palettes = compose_menu(menu_nt, menu_attr)

    audio_base, audio_bytes, audio_span = extract_audio_prg(rom)
    audio_golden = record_audio_golden(rom, AUDIO_GOLDEN_TRACK, AUDIO_GOLDEN_FRAMES)

    outputs = {
        "audio_prg.h": emit_audio_header(audio_base, audio_bytes, audio_span, src),
        "screen_menu.h": emit_menu_header(menu_tiles, menu_palettes, src),
        "screen_title.h": emit_title_header(title_tiles, title_banks, src),
        "tiles_title.h": emit_tiles_header(
            "kTitleTiles", "TILES_TITLE",
            convert_tiles(rom.chr_bank(2)),
            f"{src} [title]"),
        "dancer_poses.h": emit_dancer_poses_header(
            poses, stage_rows, dancer_x, dancer_y, dancer_attr, f"{src} [dancers]"),
        # ...and the one glyph that is not the cartridge's, planted in a slot
        # the cartridge left empty. See plant_question_mark.
        "tiles_game.h": emit_tiles_header(
            "kGameTiles", "TILES_GAME",
            plant_question_mark(convert_tiles(rom.chr_bank(0))), f"{src} [game]",
            extra=(("TILES_GAME_QUESTION", QUESTION_TILE),)),
        "tiles_dancers.h": emit_tiles_header(
            "kDancerTiles", "TILES_DANCERS", convert_tiles(rom.chr_bank(1)), f"{src} [dancers]"),
        # CHR bank 3 is the title screen's SPRITE bank: the cathedral overlay
        # at tiles $02-$13, the sparkles at $14-$17 and the firework bursts
        # filling everything from $90 up. Nothing else in the port uses it, and
        # without it drawCathedralSprites and the fireworks would be drawing
        # the dancers' tile ids by mistake.
        "tiles_title_obj.h": emit_tiles_header(
            "kTitleObjTiles", "TILES_TITLE_OBJ", convert_tiles(rom.chr_bank(3)),
            f"{src} [title sprites]"),
        "screen_coop.h": emit_coop_screen_header(coop_tiles, coop_palettes,
                                                  coop_cols, src),
        "screen_leaderboard.h": emit_leaderboard_header(lead_tiles, lead_palettes,
                                                         lead_cols, src),
        "screen_1p.h": emit_screen_header(tiles, palettes, keep_cols, src,
                                           read_stats_icons(nametable, attributes)
                                           + (read_gameover_tiles(rom),
                                              read_bonus_tiles(rom),
                                              read_banner(nametable, attributes),
                                              read_braid_frame(nametable, attributes),
                                              read_hud_labels(rom))),
        "palettes_rom.h": emit_palette_header(palette_sets, piece_palettes, src),
        "screen_proto.h": build_proto_header(args.proto),
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
