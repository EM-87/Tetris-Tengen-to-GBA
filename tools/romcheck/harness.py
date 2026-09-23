"""
The machinery every check shares: an mGBA core, the ROM's own
layout and memory map, and the helpers that drive and read it.
"""
import os
import subprocess
import sys

try:
    import mgba.core
    import mgba.image
    import mgba.log
except ImportError as exc:  # pragma: no cover - environment problem, not logic
    sys.exit(f"mGBA python bindings unavailable ({exc}).\n"
             "Install with: pip install pygba && apt-get install libmgba0.10")

SCREEN_W, SCREEN_H = 240, 160
TILE = 8
SCREEN_TW_TILES = SCREEN_W // TILE

# The screen layout, in tile columns. These mirror gba/port.h and the reflow
# done by tools/extract_assets.py; if they drift apart, the checks below stop
# meaning anything, so they are asserted against the running ROM rather than
# assumed.
# The cartridge's columns are resequenced to put the playfield in the middle
# with the HUD split either side; see SCREEN_SEGMENTS in extract_assets.py.
# 10 | 10 | 10 — a closed rectangle of the board's own blue braid, the ten
# playable columns, and another rectangle. The braid that used to run down the
# board's edges as two bare strips is now those boxes' inner sides, so the
# playfield is framed exactly where it always was; the rope simply carries on
# round the HUD. Symmetric by construction.
COL_BOX_L = (0, 10)       # score / lines / level / high score / next
COL_FRAME_L = (8, 10)     # the board's left frame — the box's inner side
COL_FIELD = (10, 20)      # the ten playable columns — dead centre
COL_FRAME_R = (20, 22)    # the board's right frame — the other box's
COL_BOX_R = (20, 30)      # the piece statistics, in two ranks

# The walls are the cartridge's own frame art, drawn once with the screen, not
# blocks painted from the playfield buffer — see the note in gba/hud.c.
FIELD_TILES_W = COL_FIELD[1] - COL_FIELD[0]
FIELD_X0 = COL_FIELD[0] * TILE
FIELD_X1 = COL_FIELD[1] * TILE
WALL_L_X = COL_FRAME_L[0] * TILE
WALL_R_X = (COL_FRAME_R[1] - 1) * TILE


def load(rom_path):
    """Returns (core, screen).

    KEEP THE SCREEN. mGBA renders into that buffer and its bindings do not
    hold a Python reference to it, so dropping the returned Image — binding it
    to `_`, say — lets the garbage collector free memory the emulator is still
    writing to, and the next run_frame() segfaults. Every caller here names it
    and keeps it alive for as long as it runs frames.
    """
    mgba.log.silence()
    core = mgba.core.load_path(rom_path)
    if core is None:
        sys.exit(f"mGBA could not load {rom_path}")
    screen = mgba.image.Image(SCREEN_W, SCREEN_H)
    core.set_video_buffer(screen)
    core.reset()
    return core, screen


def pixels(screen):
    """Framebuffer as a list of rows of (r, g, b) tuples.

    mGBA hands back a raw 32-bit-per-pixel buffer whose row length is
    `stride`, not `width` — reading it as width-sized rows silently skews the
    image, so the stride has to be honoured here.
    """
    import numpy as np
    from mgba import ffi

    raw = ffi.buffer(screen.buffer, screen.stride * screen.height * 4)
    flat = np.frombuffer(raw, dtype=np.uint32).reshape(screen.height, screen.stride)
    frame = flat[:, :screen.width]
    r = (frame & 0xFF).astype(int)
    g = ((frame >> 8) & 0xFF).astype(int)
    b = ((frame >> 16) & 0xFF).astype(int)
    return [[(int(r[y][x]), int(g[y][x]), int(b[y][x])) for x in range(screen.width)]
            for y in range(screen.height)]


def run(core, frames, keys=0):
    if keys:
        core.set_keys(keys)
    for _ in range(frames):
        core.run_frame()


def describe(rows):
    distinct = {p for row in rows for p in row}
    print(f"colores distintos en pantalla: {len(distinct)}")
    non_black = sum(1 for row in rows for p in row if p != (0, 0, 0))
    print(f"pixeles no negros: {non_black} / {SCREEN_W * SCREEN_H}")

    # Column occupancy profile, split by region: the field has a fixed
    # allocation, the HUD legitimately extends past it.
    cols = [sum(1 for y in range(SCREEN_H) if rows[y][x] != (0, 0, 0))
            for x in range(SCREEN_W)]
    lit = [x for x, c in enumerate(cols) if c > 0]
    if not lit:
        print("columnas con contenido: ninguna (pantalla vacia)")
        return cols

    print(f"columnas con contenido: {min(lit)}..{max(lit)}")
    for name, bounds in (("recuadro izq", COL_BOX_L), ("marco izq", COL_FRAME_L),
                         ("campo", COL_FIELD), ("marco der", COL_FRAME_R),
                         ("recuadro der", COL_BOX_R)):
        x0, x1 = bounds[0] * TILE, bounds[1] * TILE
        print(f"  {name:10s} x={x0:3d}..{x1 - 1:3d}")
    return cols


KEY_START = 3  # set_keys takes bit indices, not a mask


def press_start(core):
    core.set_keys(KEY_START)
    run(core, 4)
    core.set_keys()
    run(core, 6)


def start_game(core, tune=0):
    """Gets past the title and the selection screens into a solo game.

    Three presses: a title screen, then GAME SELECT (whose first entry is
    1 PLAYER, so START takes it), then LEVEL SETTINGS — one page carrying the
    level, the handicap and the tune — and then play.

    `tune` picks a tune on the way through. The port now opens on NO MUSIC
    (musicSelectTable's own first entry), so a check that needs to HEAR
    something has to ask for it: the cursor goes down to the MUSIC row and
    RIGHT walks the list from there.
    """
    run(core, 8)
    press_start(core)   # title -> game select
    press_start(core)   # game select (1 PLAYER) -> LEVEL SETTINGS
    if tune:
        for _ in range(2):      # cursor: LEVEL -> HANDICAP -> MUSIC
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 8)
        for _ in range(tune):   # NO MUSIC -> LOGINSKA -> ...
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 8)
    press_start(core)   # LEVEL SETTINGS -> play
    run(core, 8)


# ---------------------------------------------------------------------------
# The line-clear animation
#
# The ROM does not simply delete completed rows: a puff of smoke crosses each
# one and leaves SINGLE / DOUBLE / TRIPLE / TETRIS written where the blocks
# were (see reference/NOTES.md). That is five sprites and twelve tile writes
# per row per step, all of it easy to get subtly wrong and impossible to see
# in a host test, so it is checked here against the running ROM.
#
# Reaching a line clear by PLAYING would need a bot that stacks well, and the
# piece sequence depends on when Start was pressed. Instead the check writes
# completed rows straight into the game's playfield — the first member of
# `g_session`, whose address comes out of the ELF — and lets the next piece to
# land trigger the clear. Nothing in the ROM is modified; this is a fixture in
# the harness, the same way a unit test constructs a board.
#
# `g_session` is a TengenLink, whose first member is the TengenGame, which in
# turn starts with the playfields. So the symbol's own address IS the board's:
# a solo game and a linked one are the same struct at the same place, which is
# what lets one fixture serve both.
# ---------------------------------------------------------------------------
PF_W, PF_H = 12, 20            # must match TENGEN_PF_WIDTH / _HEIGHT
PF_PLAYABLE = PF_W - 2         # what is actually drawn; the walls are frame art
CELL_WALL, CELL_BLOCK = 15, 1
SCREENBLOCK_ADDR = 0x0600E000  # screenblock 28, as gba/main.c sets BG0CNT
CHARBLOCK_ADDR = 0x06000000    # charblock 0, the game's tiles
# The panel's own top run: two slots above the cartridge's 256 that nothing
# but apply_skin writes. See SKIN_PANEL_RUN_BASE in gba/screen_proto.h.
PANEL_RUN_SLOTS = (0x320, 0x321)
OAM_ADDR = 0x07000000
SWEEP_TILES = (0x5B, 0x5C, 0x5D, 0x5E, 0x5F)  # main.asm.txt:1274-1338
SWEEP_PAL_BANK = 4   # gba/port.h PAL_OBJ_CLEAR; banks 0-3 are the dancers
CLEAR_WORDS = {1: "SINGLE", 2: "DOUBLE", 3: "TRIPLE", 4: "TETRIS"}

KEY_DOWN = 7


def game_state_address(rom_path, name="g_session"):
    """Address of a symbol in the built ROM, read out of the ELF beside it."""
    elf = os.path.splitext(rom_path)[0] + ".elf"
    if not os.path.exists(elf):
        return None, f"no encuentro {elf} (hace falta para localizar el estado)"
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    try:
        out = subprocess.check_output([nm, elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        return None, f"no pude ejecutar {nm}: {exc}"
    for line in out.splitlines():
        if line.endswith(" " + name):
            return int(line.split()[0], 16), None
    return None, f"el ELF no exporta {name}"


def fill_rows(core, base, rows):
    """Plant complete rows straight into the playfield.

    `base` is the address of the FIELD, which is TengenGame's first member
    today and need not stay that way — see game_offsets.
    """
    for row in rows:
        for col in range(PF_W):
            value = CELL_WALL if col in (0, PF_W - 1) else CELL_BLOCK
            core.memory.u8[base + row * PF_W + col] = value


def map_row_text(core, row):
    """The playfield row as it is actually on screen, read back from VRAM.

    Tile ids in this game's set are ASCII for letters and digits, so a row
    the sweep has written reads as text. Everything else comes back as '#'
    for a block and '.' for the empty tile, which is id 0.
    """
    out = []
    for col in range(PF_PLAYABLE):
        tile = core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + COL_FIELD[0] + col) * 2] & 0x3FF
        out.append("." if tile == 0 else (chr(tile) if 0x20 <= tile < 0x7F else "#"))
    return "".join(out)


# ---------------------------------------------------------------------------
# Pause and the cheat codes
#
# Start pauses; the codes go in while paused. The rules are tested on the host
# (tests/test_tengen.c); what is checked here is that the GBA layer wires them
# up at all and draws the ROM's own PAUSE plaque where it should.
# ---------------------------------------------------------------------------
# The plaque is CENTRED, both ways — on the cartridge its eight columns are
# 12..19 of 32, which is the middle of the screen, and that relationship is
# what the port keeps rather than the column number. gba/port.h derives these
# the same way.
# The braid's own mid blue, which the shelves are drawn in. Sampled rather
# than named: it is palette bank 2 colour 2 of the cartridge's game set.
BRAID_BLUE = (74, 156, 239)

PAUSE_W = 8
PAUSE_H = 2
PAUSE_TX = (SCREEN_TW_TILES - PAUSE_W) // 2
PAUSE_TY = ((SCREEN_H // TILE) - PAUSE_H) // 2
PAUSE_ROW0 = [0x10, 0x11, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0x12]  # main.asm.txt:8061
PAUSE_ROW1 = [0x13, 0x14, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0x15]

KEYS = {"A": 0, "B": 1, "SELECT": 2, "START": 3,
        "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7,
        # The GBA's two extra buttons. The game proper never reads them —
        # they have no NES equivalent — so they are the port's own switches:
        # L or R swaps the title skin; in a match L+R changes the cossack
        # and SELECT swaps the right-hand HUD box.
        "R": 8, "L": 9}
CODE_LEVEL_UP = "UP DOWN UP DOWN LEFT RIGHT B B A".split()
CODE_LONG_BAR = "DOWN DOWN LEFT RIGHT LEFT RIGHT B A".split()
CODE_UNDO = "LEFT DOWN RIGHT UP LEFT DOWN RIGHT B A".split()

# Offsets into g_session.game, from the structs in src/tengen_core.h. The compiler is
# arm-none-eabi with the EABI's default -fshort-enums, so a TengenTetromino is
# one byte; a mismatch would show up immediately as nonsense readings, which
# the checks below would catch.
# THE ELF KNOWS THE OFFSETS, not this file. They used to be constants here and
# a single new field in TengenGame moved all of them, which showed up as three
# unrelated cheat-code checks failing. gba/match.c exports kGameProbe; this
# reads it, the way the audio checks read kNes6502Probe.
_GAME_PROBE_CACHE = {}


def game_offsets(rom_path):
    """Byte offsets into TengenGame, read out of the built ELF."""
    if rom_path not in _GAME_PROBE_CACHE:
        addr, why = game_state_address(rom_path, "kGameProbe")
        if addr is None:
            raise RuntimeError(why)
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        (field, player, stride, cur, y, level, stats,
         paused, held, nxt, alive, x, score, lines,
         counts, orient, piece_ids, proto_rules) = (
            core.memory.u16[addr + i * 2] for i in range(18))
        _GAME_PROBE_CACHE[rom_path] = {
            "field": field, "player": player, "stride": stride,
            "current": player + cur, "y": player + y,
            "level": player + level, "stats": player + stats,
            "paused": paused, "held": player + held, "next": player + nxt,
            "active": player + alive, "x": player + x,
            "score": player + score,
            "lines": player + lines, "counts": player + counts,
            "orientation": player + orient,
            "piece_ids": piece_ids, "proto_rules": proto_rules,
        }
    return _GAME_PROBE_CACHE[rom_path]


# The pieces' own 4x4 bitmaps, from orientationTable ($86C3) — the same bytes
# src/tengen_core.c carries, written out again here ON PURPOSE: a check that
# read the core's answer for what a piece covers would be asking the code
# under test to mark its own work. Bit 15 is (row 0, column 0).
ORIENTATION_BITS = [
    [0x0000, 0x0000, 0x0000, 0x0000],   # none
    [0xF000, 0x4444, 0xF000, 0x4444],   # I
    [0xE400, 0x8C80, 0x4E00, 0x4C40],   # T
    [0xCC00, 0xCC00, 0xCC00, 0xCC00],   # O
    [0xE200, 0xC880, 0x8E00, 0x44C0],   # J
    [0xE800, 0x88C0, 0x2E00, 0xC440],   # L
    [0x6C00, 0x8C40, 0x6C00, 0x8C40],   # S
    [0xC600, 0x4C80, 0xC600, 0x4C80],   # Z
]


# ---------------------------------------------------------------------------
# The sound engine
#
# The port plays the cartridge's own music by running the cartridge's own 6502
# sound engine (see gba/nes_audio.h). The risky part of that is the hand-written
# interpreter, so this check does not listen to the output — it reads the
# emulated APU register file straight out of the running ROM and compares it,
# frame by frame and byte for byte, with a golden recording that
# tools/extract_assets.py made with the reference interpreter in nes_cpu.py.
# A wrong flag or addressing mode shows up as a mismatched frame here instead
# of as music that is subtly wrong in a way nobody notices.
# ---------------------------------------------------------------------------
APU_REGS = 0x18
GOLDEN_PATH = "gba/audio_golden.bin"
AUDIO_ALIGN_SEARCH = 90   # frames of the ROM to look through for the match
AUDIO_GOLDEN_SKIP = 60    # ...and of the golden, whose first frames the ROM
                          # covers with the screen-switch effect

# GBA sound registers, read back to confirm the translation reached them.
REG_SOUNDCNT_X = 0x04000084
REG_SOUND1CNT_H = 0x04000062


# Restarting a GBA sound channel resets its phase and reloads its volume, so
# doing it every frame chops every held note into 60Hz slices — which is what
# it sounds like, and what this measures. It looks at the AMPLITUDE ENVELOPE,
# not the signal: the signal's own 60Hz band is just bass notes. Measured at
# 41% when the port applied channels on every register WRITE, and 10% once it
# only applied them on a register CHANGE, so the limit sits between.
AUDIO_RATE = 32768


# ---------------------------------------------------------------------------
# Pausing has to actually stop the sound.
#
# This engine ends a note by clearing the channel's bit in $4015, not by
# letting a length counter run out, and suspending the music clears them all.
# A port that only re-applies a channel when its period or volume registers
# change would never notice, and the last note would drone on under the pause
# and come back layered over the music on the way out — which is what "the
# music sounds doubled when I pause" is.
#
# This reads the GBA's own sound registers rather than listening to the
# output. That is a deliberate retreat: the mGBA binding here does hand back
# real samples (with the sound switched off at REG_SOUNDCNT_X the RMS is
# exactly zero), but the same code over the same build gives a silent pause on
# one run and a loud one on the next, so a measurement of the waveform cannot
# be trusted to mean anything. The registers are reproducible and they are
# the thing the port actually controls: every channel's volume nibble at zero
# and no channel flagged as sounding IS silence, whatever the buffer says.
# ---------------------------------------------------------------------------
REG_SOUND1CNT_H = 0x04000062     # channels 1, 2 and 4 keep their volume in
REG_SOUND2CNT_L = 0x04000068     # bits 12-15 of these
REG_SOUND3CNT_L = 0x04000070     # bit 7 is the wave channel's own on/off
REG_SOUND3CNT_H = 0x04000072     # ...and bits 13-14 its volume code
REG_SOUND4CNT_L = 0x04000078
REG_SOUNDCNT_X  = 0x04000084     # bits 0-3: which channels are sounding
REG_SOUND1CNT_X = 0x04000064     # ...and channel 1's pitch, in bits 0-10


def sound_state(core):
    io = core._native.memory.io

    def reg(addr):
        return io[(addr - 0x04000000) >> 1]

    return {
        "pulso 1": (reg(REG_SOUND1CNT_H) >> 12) & 0xF,
        "pulso 2": (reg(REG_SOUND2CNT_L) >> 12) & 0xF,
        "triangulo": 1 if (reg(REG_SOUND3CNT_L) & 0x80) else 0,
        "ruido": (reg(REG_SOUND4CNT_L) >> 12) & 0xF,
        "activos": reg(REG_SOUNDCNT_X) & 0x0F,
    }


# ---------------------------------------------------------------------------
# The two-player front end, with no cable attached.
#
# A real linked match needs two consoles and a cable, which no headless
# emulator here can supply. What CAN be checked, and is worth checking, is the
# half of it that a single GBA reaches on its own: that GAME SELECT offers 2
# PLAYER, that choosing it reaches the link screen, that the ROM keeps running
# at full speed while the lobby finds nothing on the other end — a serial wait
# without a timeout would hang here and nowhere else — that it eventually says
# so instead of waiting forever, and that B gets back out.
#
# The handshake's own logic is tested on the host, two lobbies against each
# other (tests/test_tengen.c); this is the wiring around it.
# ---------------------------------------------------------------------------
LINK_MSG_ROW = 11
LINK_TIMEOUT_FRAMES = 600   # TENGEN_LOBBY_TIMEOUT in src/tengen_link.h


# The second map, four pixels along, which the menus use to centre their
# odd-length lines and the play screen uses for the statistics. See
# SCREENBLOCK_STATS in gba/port.h.
SCREENBLOCK_OFFSET_ADDR = SCREENBLOCK_ADDR + 0x800
# ...and screenblock 31, the histogram's, which outside a match carries the
# one line the port wants two pixels higher than the grid (see
# set_credit_layer in gba/video.c).
SCREENBLOCK_LIFTED_ADDR = SCREENBLOCK_ADDR + 0x1800
# ...and screenblock 30, the counters', which is where SCORE, LINES, LEVEL
# and HIGH are actually written.
SCREENBLOCK_PANEL_ADDR = SCREENBLOCK_ADDR + 0x1000


def tilemap_text(core, row, first=0, last=30):
    """The row of the tilemap as text, ACROSS EVERY TEXT LAYER.

    The tileset's letters sit at their ASCII codes (see ascii_tile in
    gba/video.c), so a tile id IS a character. A menu line of odd length is
    drawn on the offset layer instead of the main one, and the GAME SELECT
    credit on the lifted one — reading only the main map would report an empty
    row and every menu check would quietly stop checking anything.

    The one letter the cartridge does not keep at its ASCII code is the
    question mark: it lives at TILES_GAME_QUESTION (gba/tiles_game.h), so
    ascii_tile maps '?' there and this maps it back. Without that, "EXIT?"
    reads as "EXIT" and the pause menu's question looks like its EXIT line.
    """
    QUESTION = 0xF0    # TILES_GAME_QUESTION in gba/tiles_game.h
    # ...and the pause menu's headings, drawn with copies of their letters one
    # pixel higher (PMENU_RAISED_BASE / PMENU_RAISED_CHARS in gba/port.h).
    RAISED_BASE, RAISED = 960, "PAUSEXIT?"
    # ...and its arrow, moved three pixels closer across two tiles
    # (T_ARROW_TAIL / T_ARROW_HEAD): read as "->".
    ARROW = {RAISED_BASE + len(RAISED): "-", RAISED_BASE + len(RAISED) + 1: ">"}

    def readable(t):
        return (32 <= t < 127 or t == QUESTION or t in ARROW
                or RAISED_BASE <= t < RAISED_BASE + len(RAISED))

    out = []
    for x in range(first, last):
        off = (row * 32 + x) * 2
        tile = core.memory.u16[SCREENBLOCK_ADDR + off] & 0x3FF
        if not readable(tile):
            tile = core.memory.u16[SCREENBLOCK_OFFSET_ADDR + off] & 0x3FF
        if not readable(tile):
            tile = core.memory.u16[SCREENBLOCK_LIFTED_ADDR + off] & 0x3FF
        if not readable(tile):
            tile = core.memory.u16[SCREENBLOCK_PANEL_ADDR + off] & 0x3FF
        if tile == QUESTION:
            out.append("?")
        elif RAISED_BASE <= tile < RAISED_BASE + len(RAISED):
            out.append(RAISED[tile - RAISED_BASE])
        elif tile in ARROW:
            out.append(ARROW[tile])
        else:
            out.append(chr(tile) if 32 <= tile < 127 else " ")
    return "".join(out).strip()


# ---------------------------------------------------------------------------
# The title screen's sprites: the cathedral overlay and the fireworks.
#
# Neither is drawn by the port. Both are subroutines of the cartridge run on
# the same 6502 interpreter as the sound engine, filling oamStaging, which
# gba/frontend.c copies into OAM (see draw_title_sprites). What can be checked
# from outside is exactly what matters: that the eighteen cathedral sprites
# land on the picture, that bursts actually happen and animate, that the show
# ends where the ROM ends it (frameCounterHigh = 4, about 1024 frames), that
# entering the title again restarts it — initializeTitleScreen zeroes the
# frame counter, so without that it would only ever play once — and that the
# extra 6502 work still fits in a frame.
# ---------------------------------------------------------------------------
CATHEDRAL_SPRITES = 18
TITLE_SHOW_FRAMES = 1024        # frameCounterHigh reaches 4


def oam_visible(core, first, last):
    """(x, y, tile) of every visible sprite in a range of OAM slots."""
    out = []
    for i in range(first, last):
        a0 = core.memory.u16[OAM_ADDR + i * 8]
        if (a0 & 0x0300) == 0x0200:
            continue                        # the hidden bit
        a1 = core.memory.u16[OAM_ADDR + i * 8 + 2]
        a2 = core.memory.u16[OAM_ADDR + i * 8 + 4]
        out.append((a1 & 0x1FF, a0 & 0xFF, a2 & 0x3FF))
    return out


# ---------------------------------------------------------------------------
# The title-skin easter egg: L or R swaps the release's title screen for the
# prototype cartridge's, which has another cathedral, another logo and a green
# fret instead of the blue braid.
#
# A ROM built without a prototype dump has only one skin, and this says so
# rather than failing — SCREEN_PROTO_AVAILABLE is 0 and L/R do nothing.
# ---------------------------------------------------------------------------

# How far the chord is followed before a cycle that never comes home is called
# broken. The number of skins is whatever the build was given, so this is a
# ceiling rather than an expectation.
LIMIT_SKINS = 12


# ---------------------------------------------------------------------------
# The tunes that are not on the cartridge.
#
# Tengen's four are Loginska, Bradinsky, Karinka and Troika. Korobeiniki and
# Katyusha are not among them, so both are entered by hand in gba/handtunes.c
# and hidden behind L+R on the selection screen. That makes them the only
# content in the port that was not extracted from the ROM, and the only sound
# that does not come out of the cartridge's own engine, so they are worth
# checking rather than assuming:
#
#   * the menu offers the cartridge's five until the code is entered, and
#     eight after — the two tunes and MUSIC MIX;
#   * choosing one actually produces notes, and DIFFERENT notes over time (a
#     stuck channel would still read as "sounding");
#   * the two are different tunes and not one score played twice, which is
#     what a bad index into the tune table would look like;
#   * PAUSE silences them, which needs its own stop because MUSIC_SUSPEND only
#     reaches the cartridge's engine;
#   * and the cartridge's engine is still running underneath, because the
#     sound effects are still meant to be the ROM's.
# ---------------------------------------------------------------------------
# GAME SELECT's entries start at row 10, two rows apart. The port offers the
# cartridge's first three; the last two want the COMPUTER player.
GAME_SELECT_ROWS = 5
GAME_SELECT_TY = 10             # ...and they start here, one row apart
# THE LIST IS FLUSH LEFT AND PUSHED RIGHT, the cartridge's own way of setting
# it: every entry starts at the same column whatever its length, and VERSUS
# COMPUTER — the longest at fifteen characters — ends one blank column short
# of the frame. gba/port.h's GAME_SELECT_TX. The arrow is two columns before
# it, so the scan below has to start AT the list rather than a few columns
# left of it, or it reads the cursor's white as an entry in the wrong bank.
GAME_SELECT_TX = 12
GAME_SELECT_LONGEST = 15
# The bank EVERY menu line is drawn in, chosen or not. It is bank 0 of
# bgPalette1 -- kRomPalette_bg_menu's `0F 12 0F 0F`, the cartridge's menu BLUE.
# Measured off the running cartridge: on its GAME SELECT all five entries come
# out (48,50,236) to the pixel and the only white anywhere is the cursor
# arrow's (236,238,236). It was PAL_MENU_BASE + 3 here for a long while, which
# had the port writing the list in white and marking the choice in the ORANGE
# that bank 1 holds for the credit line alone.
MENU_TEXT_BANK = 8              # PAL_MENU_BASE + 0, the cartridge's menu blue
# ...and the one white on these screens, the cursor's: gba/port.h's BANK_ARROW.
# The handicap's chosen number borrows it, which is what says which of the two
# the pad is moving.
MENU_ARROW_BANK = 11            # PAL_MENU_BASE + 3
# La memoria de paletas de fondo, y el primero de los cuatro bancos del titulo
# (PAL_TITLE_BASE en gba/port.h). Una skin toma prestados tres de esos cuatro.
PALETTE_ADDR = 0x05000000
TITLE_PAL_BASE = 4
# ...and the cursor's column, gba/frontend.c's GAME_SELECT_ARROW_TX.
MENU_ARROW_TX = GAME_SELECT_TX - 2
# The shared coop board starts one column further left than the ten-wide
# one, because it is twelve wide (SCREEN_COOP_FIELD_TX in the generated
# header, and COOP_FIELD_TX in gba/port.h).
COOP_FIELD_TX = 9
# The HIGH SCORES page: the heading's row and the first entry's, as
# gba/screen_leaderboard.h generates them.
LEADER_HEAD_TY = 2
LEADER_FIRST_TY = 3
TENGEN_PF_WIDTH = 12
TENGEN_PF_HEIGHT = 20
TENGEN_ROM_ROW_ORIGIN = 6   # the ROM row the visible field starts at

# The attract demo's own clock: frameCounterHigh 5, frameCounterLow $20.
DEMO_START_FRAME = 5 * 256 + 0x20
DEMO_WATCH_FRAMES = 2000

# LEVEL SETTINGS, two rows apart and hung off HANDICAP -- gba/port.h's
# MENU_FIELD_TY(f) = 9 + 2f. Three rows apart read as three announcements
# rather than as one block to choose from.
LEVEL_ROW = 9
HANDICAP_ROW = 11               # value AND, in one player, what it buries
MUSIC_ROW = 13
# Vueltas de sobra para dar la lista de canciones entera, destapada o no.
MUSIC_COUNT_MAX = 14

# El histograma tiene mapa para el solo (SCREENBLOCK_HISTOGRAM en gba/port.h),
# asi que lo que este escrito ahi ES el histograma y no hay que saber donde
# cae la caja. Siete columnas, una por tetromino.
STATS_SCREENBLOCK = 31
STATS_COLUMNS = 7
# Las cuatro esquinas de la caja del cartel de GAME OVER (T_BOX_TL/TR/BL/BR en
# gba/port.h). Son los mismos numeros con skin y sin ella: una skin cambia el
# DIBUJO que hay en esas ranuras y el banco de paleta, nunca el numero.
PLAQUE_CORNERS = (0x29, 0x2B, 0x3A, 0x3C)
# Las etiquetas del HUD (NEXT, SCORE, LINES, LEVEL, HIGH) no son ASCII sino
# arte propio: HUD_LABEL_TILE_BASE en gba/port.h y las veintidos que siguen.
HUD_LABEL_TILE_BASE = 768
HUD_LABEL_TILE_END = 790
# Y las dos filas que el release gasta en la tira de iconos del histograma,
# con los ocho pasos que tiene una tirada de barra en las dos construcciones.
STATS_ICON_ROWS = 2
STATS_RUN_STEPS = 8
# Cuanto se escucha tras pulsar: el mas largo de los efectos dura 15 frames.
EFFECT_WATCH_FRAMES = 30
# Cuanta tinta tiene que prometer una CELDA para que su vacio sea sospechoso.
# Media celda: menos que eso puede quedar tapado por otra capa sin que pase
# nada, media letra no.
ONSCREEN_MIN_INK = 32


def to_music_page(core, settle=10):
    """Title -> GAME SELECT -> LEVEL SETTINGS, cursor on MUSIC.

    One page carries all three settings; UP/DOWN/SELECT move the cursor
    between them and LEFT/RIGHT change the one it is on, so reaching the tune
    means two STARTs and then walking the cursor down to it."""
    press_start(core); run(core, settle)    # title -> game select
    press_start(core); run(core, settle)    # -> LEVEL SETTINGS
    for _ in range(2):                      # cursor: LEVEL -> HANDICAP -> MUSIC
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, settle)


# Palette banks 12 and 13: the falling piece's colours and the preview's.
# setPiecePalette (main.asm.txt:5338) indexes kRomPiecePalettes by PIECE ID,
# so the two banks must differ exactly when the two pieces do.
PAL_PIECE_BANK, PAL_NEXT_BANK = 12, 13


# TengenAi, byte by byte — see src/tengen_ai.h, where the struct is six bytes
# of the ROM's own scratch and then the port's. There is no probe for it, so
# `soft_drop` is read as well and checked against the one value a running
# WITH COMPUTER game can have: if the layout ever moves, that is what says so
# rather than the flags quietly reading each other's bytes.
AI_TARGET_X, AI_TARGET_O = 6, 7
AI_SETTLE, AI_SOFT_DROP, AI_COOP_AWARE = 8, 9, 10


# Where the pause menu's lines land, derived the way gba/port.h derives them:
# a box PMENU_H tall centred on a 20-row screen, with the column inside it.
# The lines do NOT all live on the same background — the heading and the
# question's second line ride the counters' layer two pixels down, the
# even-length ones the offset layer three across — but tilemap_text reads all
# four, so these are just rows.
PMENU_H = 5
PMENU_TY = (SCREEN_H // TILE - PMENU_H) // 2
# SEVEN ROWS NOW, not ten: the cartridge's own PAUSE is eight columns by two
# and its GAME OVER plaque six by four, and a box half the height of the
# screen for three lines of text is out of proportion with both. The blank
# rows came out; see PMENU_H in gba/port.h.
# FIVE ROWS AND THREE LINES. The word MUSIC went — the tune's NAME is the
# entry, and a label over a value that is itself the choice says nothing —
# and with it the blank rows, so PM_MUSIC and PM_TUNE are now the same line.
# The question is one line too, "EXIT?", with the mark doing what SURE? did.
PM_HEAD = PMENU_TY + 1       # PAUSE
PM_MUSIC = PMENU_TY + 2      # ...the tune's name, which IS the music entry
PM_TUNE = PM_MUSIC
PM_EXIT = PMENU_TY + 3       # EXIT
PM_ASK = PMENU_TY + 1        # the question, "EXIT?"
PM_SURE = PM_ASK
PM_ANSWER = PMENU_TY + 2     # YES, with NO under it
# The box's own columns, which is all a check about the box should read: the
# rest of the row is the HUD, and the braid decodes as stray letters. The box
# is as wide as its lines need (pmenu_width in gba/match.c), so these are the
# WIDEST it gets and pmenu_span reads the one actually on screen.
PMENU_W_T = 18                # PMENU_W_MAX in gba/port.h
PMENU_TX = (SCREEN_TW_TILES - PMENU_W_T) // 2
PM_L = PMENU_TX + 1
PM_R = PMENU_TX + PMENU_W_T - 1


def pmenu_span(core):
    """The interior columns of the pause box on screen now, found by its top
    corners ($29 and $2B, the game-over plaque's), or the widest if none."""
    top = [c for c in range(SCREEN_TW_TILES)
           if (core.memory.u16[SCREENBLOCK_ADDR + (PMENU_TY * 32 + c) * 2]
               & 0x3FF) in (0x29, 0x2B)]
    return (top[0] + 1, top[-1]) if len(top) >= 2 else (PM_L, PM_R)


# ---------------------------------------------------------------------------
# THE CREDITS, which this port had been dropping.
#
# The cartridge prints a credit in the bottom corner of each of its four
# front-end screens — six lines in all, in the PPU patch chains at $A19B,
# $A20E, $A280 and $A2DF. The port folded three of those screens into one, so
# five of the six had nowhere to go and the sixth was never drawn; what stood
# in their place was a line the port made up. They roll at the bottom of GAME
# SELECT now, one at a time.
#
# The words are the cartridge's and this check holds them to that: all six,
# spelled its way, PAZHITNOV and all.
# ---------------------------------------------------------------------------
# MIRRORSOFT'S LINE IS DELIBERATELY NOT HERE. The cartridge spends its GAME
# SELECT corner on a licensing notice for a company that folded in 1991; this
# port spends it on whoever made the port. The other five are the people who
# made the game and are held to the cartridge's own spelling.
CREDITS = [
    ("PORTED WITH CLAUDE", "BY EDUARDO MARTINEZ"),
    ("CONCEPT BY", "ALEXEY PAZHITNOV"),
    ("DESIGN BY", "VADIM GERASIMOV"),
    ("PROGRAMMED BY", "ED LOGG"),
    ("VIDEO GRAPHICS BY", "KRIS MOSER"),
    ("AUDIO BY", "BRAD FULLER"),
]
CREDIT_TY = 16
