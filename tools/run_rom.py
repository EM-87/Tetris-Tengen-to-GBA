#!/usr/bin/env python3
"""
run_rom.py — boot the built ROM in mGBA, drive it for a while, and report
what actually ended up on screen.

This exists because "the ROM links" and "the ROM runs" are very different
claims, and the second one is the interesting one. It runs headless, so it
works in CI or over a terminal with no display.

Usage:
    python3 tools/run_rom.py build/tengen.gba [--frames N] [--png OUT.png]
    python3 tools/run_rom.py build/tengen.gba --selftest
    python3 tools/run_rom.py build/tengen.gba --lineclear
    python3 tools/run_rom.py build/tengen.gba --pause
    python3 tools/run_rom.py build/tengen.gba --audio
    python3 tools/run_rom.py build/tengen.gba --link

--selftest checks the things a broken port would get wrong: that the screen
isn't blank, that the playfield frame is where the resolution mapping says it
should be, and that a piece actually falls.

--lineclear watches the line-clear animation happen, sprite by sprite and
tile by tile. --pause pauses the game and types in the cheat codes. Both need
`build/tengen.elf` next to the ROM (for the address of the game state) and an
`arm-none-eabi-nm` to read it with.

--link walks the two-player mode as far as one console alone can go: to the
link screen, with nothing plugged in. A linked MATCH needs two consoles, and
that is tools/run_link.py, which puts a simulated cable between two cores.

Requires: pip install pygba  (pulls in the mGBA bindings)
          plus the mGBA shared library, e.g. apt-get install libmgba0.10
"""
import argparse
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


def selftest(rom_path):
    core, screen = load(rom_path)
    failures = []

    # The title screen must come up first and must not be blank. Boot takes
    # a few frames (the sound engine's 64KB code view is laid out before
    # anything is drawn) and the title's own tile swap hides behind one black
    # frame, so the check waits for it rather than looking at frame 8 exactly.
    run(core, 8)
    for _ in range(30):
        title = pixels(screen)
        if any(p != (0, 0, 0) for row in title for p in row):
            break
        core.run_frame()
    if all(p == (0, 0, 0) for row in title for p in row):
        failures.append("la pantalla de titulo quedo en negro")

    # And START must take it to GAME SELECT, not straight to play.
    press_start(core)
    menu = pixels(screen)
    if menu == title:
        failures.append("START no llevo del titulo al menu")
    core.reset()

    start_game(core)
    rows = pixels(screen)
    cols = describe(rows)

    if all(p == (0, 0, 0) for row in rows for p in row):
        failures.append("la pantalla quedo completamente negra")
    if rows == title:
        failures.append("START no arranco la partida (la pantalla no cambio)")

    # The panels' top run lives in slots the boot upload never touches, so
    # the tilemap can be right while the art is all zeros: the rope along the
    # top of both panels simply missing on a fresh boot, and back the moment
    # a prototype skin has been put on and taken off. Read the ART, not the
    # map — the map was correct all along.
    for slot in PANEL_RUN_SLOTS:
        art = [core.memory.u16[CHARBLOCK_ADDR + slot * 32 + i]
               for i in range(0, 32, 2)]
        if not any(art):
            failures.append(f"la greca superior de los paneles (tile {slot:#x}) "
                            "esta en blanco en el primer arranque")

    def region(bounds):
        return sum(cols[x] for x in range(bounds[0] * TILE, bounds[1] * TILE))

    # Each region must actually have been drawn. A blank one means a tile
    # upload, a palette or a layout index went wrong.
    for name, bounds in (("recuadro izquierdo", COL_BOX_L),
                         ("marco izquierdo", COL_FRAME_L),
                         ("marco derecho", COL_FRAME_R),
                         ("recuadro derecho", COL_BOX_R)):
        painted = region(bounds)
        if painted == 0:
            failures.append(f"{name} quedo vacio")
        else:
            print(f"  {name}: {painted} px")

    # ...and the two boxes must be the SAME WIDTH and the board centred
    # between them. This is the layout's whole claim, so it is asserted
    # against the running ROM rather than left to the extractor's self-test.
    left_margin = COL_FRAME_L[0]
    right_margin = SCREEN_TW_TILES - COL_FRAME_R[1]
    if left_margin != right_margin:
        failures.append(f"los recuadros no son simetricos: {left_margin} "
                        f"columnas a la izquierda y {right_margin} a la derecha")

    # The field runs the FULL height of the screen — that is the whole point
    # of the 160px vertical fit, and the first thing a bad window offset
    # breaks. Checked on the frame columns, which are the walls, per tile ROW
    # rather than per pixel: the braid has transparent corners, so the
    # invariant is that no row of it is missing, not that every pixel is lit.
    for name, x in (("muro izquierdo", WALL_L_X), ("muro derecho", WALL_R_X)):
        empty_rows = [ty for ty in range(SCREEN_H // TILE)
                      if not any(rows[ty * TILE + dy][x + dx] != (0, 0, 0)
                                 for dy in range(TILE) for dx in range(TILE))]
        if empty_rows:
            failures.append(f"al {name} le faltan filas de tiles: {empty_rows}")

    # The playfield must be centred: that is the whole point of resequencing
    # the cartridge's columns, and an off-by-one in SCREEN_SEGMENTS would show
    # up here and nowhere else.
    centre = (FIELD_X0 + FIELD_X1) // 2
    if centre != SCREEN_W // 2:
        failures.append(f"el campo esta centrado en {centre}px, esperado {SCREEN_W // 2}")

    # A piece must actually fall: the screen has to change over time without
    # any input at all.
    before = pixels(screen)
    run(core, 120)
    after = pixels(screen)
    if before == after:
        failures.append("la pantalla no cambio en 120 frames (la pieza no cae)")

    if failures:
        for f in failures:
            print(f"FALLA: {f}")
        return 1
    print("OK: la ROM arranca, dibuja el campo donde corresponde y la pieza cae.")
    return 0


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


def sweep_sprites(core):
    """Visible sweep sprites as {row: [(column, tile), ...]}."""
    rows = {}
    for i in range(128):
        attr0 = core.memory.u16[OAM_ADDR + i * 8]
        if attr0 & 0x0200:
            continue
        attr1 = core.memory.u16[OAM_ADDR + i * 8 + 2]
        attr2 = core.memory.u16[OAM_ADDR + i * 8 + 4]
        if (attr2 >> 12) != SWEEP_PAL_BANK:
            continue
        row = (attr0 & 0xFF) // TILE
        col = (attr1 & 0x1FF) // TILE - COL_FIELD[0]
        rows.setdefault(row, []).append((col, attr2 & 0x3FF))
    for cols in rows.values():
        cols.sort()
    return rows


def lineclear_check(rom_path, row_count):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    core, screen = load(rom_path)
    start_game(core)

    rows = list(range(PF_H - row_count, PF_H))
    fill_rows(core, base + game_offsets(rom_path)["field"], rows)
    word = CLEAR_WORDS[row_count]
    print(f"filas {rows[0]}..{rows[-1]} completas -> deberia decir {word}")

    # Soft drop until the next piece lands on them and the sweep starts.
    core.set_keys(KEY_DOWN)
    for _ in range(240):
        core.run_frame()
        if sweep_sprites(core):
            break
    else:
        print("FALLA: la animacion nunca arranco")
        return 1
    core.set_keys()

    failures = []
    heads, tails_seen, final_text = [], set(), None
    watched = rows[0]
    for _ in range(60):
        seen = sweep_sprites(core)
        if not seen:
            break
        if sorted(seen) != rows:
            failures.append(f"escobas en las filas {sorted(seen)}, esperaba {rows}")
        for row, cols in seen.items():
            # Adjacent columns carrying consecutive tiles, head ($5F) on the
            # right: the trail the ROM builds up and then lets run off the
            # field, so it is shorter than five at both ends of the sweep.
            expected = [(cols[0][0] + i, cols[0][1] + i) for i in range(len(cols))]
            if cols != expected or not set(t for _, t in cols) <= set(SWEEP_TILES):
                failures.append(f"la escoba de la fila {row} esta rota: {cols}")
            tails_seen.update(t for _, t in cols)
        heads.append(max(c for c, _ in seen[watched]))
        final_text = map_row_text(core, watched)
        print(f"  columna {heads[-1]:2d}  |{final_text}|")
        core.run_frame()

    if tails_seen != set(SWEEP_TILES):
        failures.append(f"tiles usados {sorted(hex(t) for t in tails_seen)}, "
                        f"esperaba {[hex(t) for t in SWEEP_TILES]}")
    if heads != sorted(heads):
        failures.append("la escoba retrocede en algun momento")
    if not heads or max(heads) < PF_PLAYABLE - 1:
        failures.append(f"la escoba solo llego a la columna {max(heads, default=-1)}")
    if final_text is None or word not in final_text:
        failures.append(f"la fila decia |{final_text}|, esperaba que dijera {word}")

    # And then the rows really do come down.
    for _ in range(10):
        core.run_frame()
    after = map_row_text(core, watched)
    if "." not in after:
        failures.append(f"la fila |{after}| sigue completa despues de la animacion")
    if sweep_sprites(core):
        failures.append("quedaron sprites de la escoba en pantalla")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print(f"OK: la escoba cruza {row_count} fila(s), escribe {word} y la fila colapsa.")
    return 0


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
         counts, orient) = (core.memory.u16[addr + i * 2] for i in range(16))
        _GAME_PROBE_CACHE[rom_path] = {
            "field": field, "player": player, "stride": stride,
            "current": player + cur, "y": player + y,
            "level": player + level, "stats": player + stats,
            "paused": paused, "held": player + held, "next": player + nxt,
            "active": player + alive, "x": player + x,
            "score": player + score,
            "lines": player + lines, "counts": player + counts,
            "orientation": player + orient,
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


def pause_box(core, row):
    return [core.memory.u16[SCREENBLOCK_ADDR + ((PAUSE_TY + row) * 32 + PAUSE_TX + x) * 2] & 0x3FF
            for x in range(PAUSE_W)]


def pause_check(rom_path):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    m = core.memory
    off = game_offsets(rom_path)
    OFF_Y, OFF_LEVEL, OFF_CURRENT = off["y"], off["level"], off["current"]
    failures = []

    def tap(name):
        core.set_keys(KEYS[name])
        core.run_frame()
        core.set_keys()
        core.run_frame()

    y_before = m.u8[base + OFF_Y]
    tap("START")
    run(core, 120)
    if m.u8[base + OFF_Y] != y_before:
        failures.append("la pieza siguio cayendo con el juego en pausa")
    if pause_box(core, 0) != PAUSE_ROW0 or pause_box(core, 1) != PAUSE_ROW1:
        failures.append(f"la placa de PAUSE no se dibujo: {pause_box(core, 0)}")
    else:
        print(f"  placa de PAUSE en ({PAUSE_TX},{PAUSE_TY}), tiles de la ROM")

    level = m.u8[base + OFF_LEVEL]
    for button in CODE_LEVEL_UP:
        tap(button)
    if m.u8[base + OFF_LEVEL] != level + 1:
        failures.append(f"el codigo de nivel dejo el nivel en {m.u8[base + OFF_LEVEL]}, "
                        f"esperaba {level + 1}")
    tap("A")   # the ROM leaves the cursor on the last byte, so A repeats it
    if m.u8[base + OFF_LEVEL] != level + 2:
        failures.append("pulsar A otra vez no repitio el codigo de nivel")
    else:
        print(f"  codigo de nivel: {level} -> {m.u8[base + OFF_LEVEL]} (y repite con A)")

    # A press that breaks a sequence is swallowed, so a neutral button is
    # needed before the next code -- that is the ROM's matcher, not a hack.
    tap("SELECT")
    for button in CODE_LONG_BAR:
        tap(button)
    if m.u8[base + OFF_CURRENT] != 1:
        failures.append(f"el codigo de barra larga dio la pieza "
                        f"{m.u8[base + OFF_CURRENT]}, esperaba 1 (I)")
    else:
        print("  codigo de barra larga: la pieza en juego pasa a ser la I")

    # THE THIRD CODE. It is the one that needs a piece to have LANDED, because
    # what it does is take the last locked piece back out of the field — so
    # unpause, hold Down until something settles, and only then ask for it.
    # Once per game and wiped by a line clear (L94E4), which is why this is the
    # last of the three and on a board that has cleared nothing.
    tap("START")
    run(core, 4)
    core.set_keys(KEYS["DOWN"])
    run(core, 240)
    core.set_keys()
    run(core, 12)
    field = base + off["field"]
    settled = sum(1 for i in range(TENGEN_PF_WIDTH * TENGEN_PF_HEIGHT)
                  if core.memory.u8[field + i])
    tap("START")
    run(core, 4)
    tap("SELECT")           # a neutral press, as above
    for button in CODE_UNDO:
        tap(button)
    undone = sum(1 for i in range(TENGEN_PF_WIDTH * TENGEN_PF_HEIGHT)
                 if core.memory.u8[field + i])
    if undone >= settled:
        failures.append(f"el codigo de deshacer no saco la pieza del tablero: "
                         f"{settled} celdas antes, {undone} despues")
    else:
        print(f"  codigo de deshacer: la ultima pieza vuelve al aire "
               f"({settled} -> {undone} celdas)")

    tap("START")
    run(core, 4)
    if pause_box(core, 0) == PAUSE_ROW0:
        failures.append("la placa de PAUSE se quedo despues de despausar")
    run(core, 120)
    if m.u8[base + OFF_Y] == y_before:
        failures.append("el juego no siguio despues de despausar")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: START pausa y despausa, y los codigos de trucos responden.")
    return 0


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
# Where the APU register file and the fault flag sit inside Nes6502. These
# move whenever that struct changes, so the ROM exports them rather than
# letting a constant here drift out of date: kNes6502Probe is three halfwords,
# {offsetof(bus.apu), offsetof(faulted), how many registers}. Reading a stale
# offset does not fail loudly — it reads a neighbouring byte and quietly
# reports the wrong thing, which is how a real check turns into a green light
# that means nothing.
def nes6502_probe(rom_path, core):
    addr, why = game_state_address(rom_path, "kNes6502Probe")
    if addr is None:
        return None, why
    return tuple(core.memory.u16[addr + i * 2] for i in range(3)), None
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


def pause_audio_check(rom_path):
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    start_game(core, tune=1)         # LOGINSKA: a silent pause proves nothing

    # The music has to have got going, or a silent pause proves nothing.
    heard = set()
    for _ in range(180):
        core.run_frame()
        state = sound_state(core)
        if state["activos"]:
            heard.add(state["activos"])

    core.set_keys(KEYS["START"])
    run(core, 4)
    core.set_keys()
    run(core, 8)

    worst = {k: 0 for k in sound_state(core)}
    for _ in range(120):
        core.run_frame()
        for k, v in sound_state(core).items():
            worst[k] = max(worst[k], v)

    print(f"  sonando: se oyeron los canales {sorted(heard)}")
    print("  en pausa: " + ", ".join(f"{k}={v}" for k, v in worst.items()))

    failures = []
    if not heard:
        failures.append("no habia musica que pausar; la medida no prueba nada")
    for name in ("pulso 1", "pulso 2", "triangulo", "ruido", "activos"):
        if worst[name]:
            failures.append(f"en pausa {name} sigue sonando ({worst[name]})")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: al pausar todos los canales quedan a cero.")
    return 0


def audio_check(rom_path):
    base, why = game_state_address(rom_path, "g_cpu")
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    if not os.path.exists(GOLDEN_PATH):
        print(f"SALTADO: falta {GOLDEN_PATH} (lo genera `make assets ROM=...`)")
        return 0

    raw = open(GOLDEN_PATH, "rb").read()
    golden = [raw[i:i + APU_REGS] for i in range(0, len(raw), APU_REGS)]

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    probe, why = nes6502_probe(rom_path, core)
    if probe is None:
        print(f"SALTADO: {why}")
        return 0
    apu_offset, fault_offset, apu_regs = probe
    if apu_regs != APU_REGS:
        print(f"FALLA: la ROM dice {apu_regs} registros de APU, el golden trae {APU_REGS}")
        return 1
    iwram = core.memory.iwram
    apu_off = base + apu_offset - 0x03000000
    fault_off = base + fault_offset - 0x03000000

    # THE FIREWORKS HAVE TO BE HELD OFF FOR THIS. Every burst calls
    # setMusicOrSoundEffect of its own (LACA0, main.asm.txt:6104-6109), so the
    # title's APU carries bangs the reference recording — which is a tune and
    # nothing else — knows nothing about. That is the cartridge working
    # correctly; it just cannot be inside the measurement.
    #
    # The cartridge's own lever for it is player2FallTimer ($6B): LA9DE counts
    # it down once a frame and starts a burst when it reaches zero (:5740). The
    # harness holds it away from zero, which is a fixture, never anything the
    # ROM knows about — the same shape as planting completed rows for the
    # line-clear check.
    ram, why = game_state_address(rom_path, "g_nes_ram")
    if ram is None:
        print(f"SALTADO: {why}")
        return 0
    fireworks_timer = ram + 0x6B

    seen = []
    for _ in range(len(golden) + AUDIO_ALIGN_SEARCH):
        core.memory.u8[fireworks_timer] = 200
        core.run_frame()
        seen.append(bytes(iwram[apu_off:apu_off + APU_REGS]))

    failures = []
    if iwram[fault_off]:
        failures.append("el interprete 6502 se detuvo por un opcode que no conoce")

    # WHERE THE TWO LINE UP, in both directions. The ROM reaches this screen
    # through a SOUND_SCREEN_SWITCH effect that is still ringing when the tune
    # starts, and the reference recording has no effect in it, so the first
    # frames of the golden have no counterpart in the ROM at all — searching
    # only for a shift in one of the two could never find the match. Skipping
    # a few frames of each finds it, and everything after has to be identical.
    start, gstart, matched = 0, 0, 0
    for gskip in range(AUDIO_GOLDEN_SKIP):
        for offset in range(AUDIO_ALIGN_SEARCH):
            n = 0
            while (gskip + n < len(golden) and offset + n < len(seen)
                    and seen[offset + n] == golden[gskip + n]):
                n += 1
            if n > matched:
                matched, start, gstart = n, offset, gskip

    want = len(golden) - gstart
    print(f"  motor de sonido alineado en el frame {start} de la ROM y el "
          f"{gstart} del golden; {matched} de {want} frames identicos")
    if matched < want:
        failures.append(f"el APU emulado se desvia en el frame {matched}: "
                        f"ROM {seen[start + matched].hex(' ')} "
                        f"vs referencia {golden[gstart + matched].hex(' ')}")

    if not (core.memory.u16[REG_SOUNDCNT_X] & 0x0080):
        failures.append("el sonido del GBA nunca se encendio")
    if core.memory.u16[REG_SOUND1CNT_H] == 0:
        failures.append("el canal de pulso 1 quedo sin configurar")

    # And it has to fit in a frame. Emulating a few thousand 6502 instructions
    # every frame is the one thing in this port that could plausibly overrun
    # its budget, and the symptom would be a missed vsync — which shows up as
    # gravity running slow. Level 0 drops the piece exactly every 33 frames
    # (possibleFallTimerTable entry 0), so any other interval means a frame was
    # lost to the sound engine.
    game_base, why = game_state_address(rom_path)
    if game_base is None:
        print(f"  (sin comprobar el presupuesto de CPU: {why})")
    else:
        core.reset()
        start_game(core)
        piece_y = game_base + game_offsets(rom_path)["y"]
        seen_y = []
        for _ in range(400):
            core.run_frame()
            seen_y.append(core.memory.u8[piece_y])
        drops = [i for i in range(1, len(seen_y)) if seen_y[i] != seen_y[i - 1]]
        gaps = sorted({drops[i] - drops[i - 1] for i in range(1, len(drops))})
        if gaps != [33]:
            failures.append(f"la gravedad cayo cada {gaps} frames en vez de 33: "
                            "el bucle esta perdiendo vsyncs")
        else:
            print(f"  presupuesto de CPU: {len(drops)} caidas, todas a 33 frames exactos")

    # The choppiness this check used to measure — how much of the envelope
    # moves at exactly the frame rate — came out of the emulator's audio
    # buffer, and that measurement is not reproducible here: the same build
    # over the same frames gives 2% on one run and 4% on the next. A number
    # that changes when nothing changed is not evidence, so it is gone rather
    # than quietly reported. What replaced it is `--pause-audio`, which reads
    # the sound registers instead and gives the same answer every time.

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la ROM ejecuta el motor de sonido del cartucho, nota por nota.")
    return 0


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
    """
    out = []
    for x in range(first, last):
        off = (row * 32 + x) * 2
        tile = core.memory.u16[SCREENBLOCK_ADDR + off] & 0x3FF
        if not (32 <= tile < 127):
            tile = core.memory.u16[SCREENBLOCK_OFFSET_ADDR + off] & 0x3FF
        if not (32 <= tile < 127):
            tile = core.memory.u16[SCREENBLOCK_LIFTED_ADDR + off] & 0x3FF
        if not (32 <= tile < 127):
            tile = core.memory.u16[SCREENBLOCK_PANEL_ADDR + off] & 0x3FF
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


def title_check(rom_path):
    core, screen = load(rom_path)           # `screen` must stay alive; see load()
    failures = []

    run(core, 30)
    # NOT ALL EIGHTEEN HAVE TO SHOW. The overlay is placed in NES screen
    # pixels over NES rows the composition may not carry: it drops the top of
    # the cathedral's thin spire (see TITLE_ROW_BLOCKS), and a sprite whose row
    # went with it has nothing left to overlay, so kTitleRowMap hides it rather
    # than dropping it somewhere it does not belong. Two is the most the
    # current composition can account for; more than that means the map is
    # wrong, not that the artwork was recut.
    cathedral = oam_visible(core, 0, CATHEDRAL_SPRITES)
    if len(cathedral) < CATHEDRAL_SPRITES - 2:
        failures.append(
            f"la catedral pone {len(cathedral)} de {CATHEDRAL_SPRITES} sprites")
    off = [c for c in cathedral if not (0 <= c[0] < SCREEN_W and 0 <= c[1] < SCREEN_H)]
    if off:
        failures.append(f"sprites de la catedral fuera de pantalla: {off[:3]}")
    if cathedral:
        print(f"  catedral: {len(cathedral)} sprites, de ({cathedral[0][0]},"
              f"{cathedral[0][1]}) a ({cathedral[-1][0]},{cathedral[-1][1]})")

    # The fireworks live in slots 19 and up. Watch a while: bursts come every
    # 8-71 frames and each lasts a few, so a couple of hundred frames sees
    # several, and the tile ids have to CHANGE — a burst that froze on one
    # frame of its animation would still be a lot of sprites.
    seen_tiles, peak, bursts, was_up = set(), 0, 0, False
    for _ in range(300):
        core.run_frame()
        vis = oam_visible(core, 19, 64)
        peak = max(peak, len(vis))
        for v in vis:
            seen_tiles.add(v[2])
        up = len(vis) > 0
        if up and not was_up:
            bursts += 1
        was_up = up
    if peak < 20:
        failures.append(f"los fuegos artificiales nunca pasan de {peak} sprites")
    if bursts < 2:
        failures.append(f"solo {bursts} explosion(es) en 300 frames")
    if len(seen_tiles) < 20:
        failures.append(f"los fuegos no se animan: solo {len(seen_tiles)} tiles distintos")
    print(f"  fuegos: {bursts} explosiones en 300 frames, hasta {peak} sprites, "
          f"{len(seen_tiles)} tiles distintos")

    # The show has to STOP, the way the ROM stops it.
    run(core, TITLE_SHOW_FRAMES)
    still = max(len(oam_visible(core, 19, 64)) for _ in [core.run_frame() for _ in range(120)])
    if still:
        failures.append("los fuegos siguen despues de que la ROM los termina")
    else:
        print(f"  la funcion termina sola pasados {TITLE_SHOW_FRAMES} frames, como en el cartucho")

    # ...and start again on the next visit to the title.
    for name in ("START", "B"):
        core.set_keys(KEYS[name]); run(core, 4); core.set_keys(); run(core, 8)
    seen = 0
    for _ in range(300):
        core.run_frame()
        seen = max(seen, len(oam_visible(core, 19, 64)))
    if seen < 20:
        failures.append("al volver al titulo los fuegos no vuelven a empezar")
    else:
        print("  al volver al titulo la funcion vuelve a empezar")

    # And the budget. Running two more of the ROM's subroutines on top of the
    # sound engine, every frame, is the kind of thing that quietly costs a
    # vblank; the symptom would be the show advancing slower than the screen.
    # g_title_frame is the counter draw_title_sprites feeds the ROM, so if it
    # does not go up exactly once per emulated frame, a frame was missed.
    addr, why = game_state_address(rom_path, "g_title_frame")
    if addr is None:
        print(f"  (sin comprobar el presupuesto de CPU: {why})")
    else:
        before = core.memory.u16[addr]
        run(core, 300)
        advanced = (core.memory.u16[addr] - before) & 0xFFFF
        if advanced != 300:
            failures.append(
                f"la pantalla de titulo avanzo {advanced} frames de 300: se pierden vblanks")
        else:
            print("  presupuesto de CPU: 300 frames de pantalla, 300 de la funcion")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la catedral y los fuegos son el codigo del cartucho, corriendo.")
    return 0


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


def skin_check(rom_path):
    core, screen = load(rom_path)           # `screen` must stay alive; see load()
    failures = []

    # WHAT TELLS THE TWO SCREENS APART: the TILEMAP, not the picture. The
    # release title has fireworks on it, so two frames of it are rarely
    # identical and comparing pixels would call an unchanged screen "changed"
    # every time; and both frames now reach the screen's edges, so counting lit
    # edge pixels cannot tell them apart either. Their frames are drawn from
    # different tiles, and tiles do not animate.
    def frame_tiles():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2]
                for r in range(2, 18) for c in (0, 1, SCREEN_TW_TILES - 1)]

    def tap(*keys, settle=12):
        core.set_keys(*keys)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 40)
    release = frame_tiles()

    # L+R LA ENCUENTRA; L Y R A SOLAS LA MANEJAN DESPUES. Antes de que suene
    # el acorde, un gatillo suelto no hace absolutamente nada: la skin es algo
    # que se descubre, y el titulo no se cambia por apoyar un dedo.
    tap(KEYS["L"])
    if frame_tiles() != release:
        failures.append("L a solas cambia la skin sin haber sonado el acorde")
    tap(KEYS["R"])
    if frame_tiles() != release:
        failures.append("R a solas cambia la skin sin haber sonado el acorde")

    tap(KEYS["L"], KEYS["R"])
    proto = frame_tiles()
    if proto == release:
        print("  esta ROM se construyo sin prototipo: L+R no tiene skin que poner")
        for f in failures:
            print(f"FALLA: {f}")
        if failures:
            return 1
        print("OK: sin skin de prototipo, y el titulo no se rompe por pulsar L o R.")
        return 0
    print("  L+R pone el marco del prototipo, que es de otros tiles; "
           "antes del acorde un hombro suelto no hace nada")

    # The release's fireworks and cathedral overlay belong to the release
    # picture; on the prototype's they must be gone.
    if oam_visible(core, 0, 64):
        failures.append("los sprites del release siguen encima de la skin")
    else:
        print("  los sprites del release (catedral y fuegos) se retiran con ella")

    # HOWEVER MANY SKINS the build was given, R must walk all of them and come
    # back — and each must be its OWN screen, not the same tiles twice, which
    # is what a swap that forgot to re-upload the prototype's pattern table
    # would look like.
    seen = [release, proto]
    for _ in range(LIMIT_SKINS):
        tap(KEYS["R"])
        now = frame_tiles()
        if now == release:
            break
        if now in seen:
            failures.append("dos skins del ciclo dibujan los mismos tiles")
            break
        seen.append(now)
    else:
        failures.append(f"el ciclo de skins no vuelve al release en "
                         f"{LIMIT_SKINS} pulsaciones de R")
    if not failures:
        print(f"  tras el acorde, R recorre {len(seen) - 1} skin(s) distinta(s) "
               "y vuelve al titulo del release")

    # ...Y L VUELVE, que es de lo que iba el cambio: con cuatro skins, volver a
    # la que acabas de pasar no puede costar dar la vuelta a las otras tres.
    order = seen + [release]
    tap(KEYS["R"])
    if frame_tiles() != order[1]:
        failures.append("R no avanza a la primera skin desde el release")
    tap(KEYS["L"])
    if frame_tiles() != release:
        failures.append("L no vuelve a la skin anterior")
    else:
        tap(KEYS["L"])
        if frame_tiles() != order[-2]:
            failures.append("L desde el release no da la vuelta a la ultima skin")
        else:
            print("  L retrocede y R avanza, y la lista da la vuelta por los dos "
                   "lados")

    # ...and the choice must survive leaving the title and coming back.
    tap(KEYS["R"])
    proto = frame_tiles()
    for name in ("START", "B"):
        core.set_keys(KEYS[name]); run(core, 4); core.set_keys(); run(core, 10)
    if frame_tiles() != proto:
        failures.append("la skin se pierde al salir del titulo y volver")
    else:
        print("  la skin se mantiene al salir del titulo y volver")

    # AND IT OPENS NOTHING ELSE. The skin is its own chord on its own screen:
    # a player who finds the prototype title has not thereby found the hidden
    # tunes, which are still four until the chord is rung on a menu.
    core.set_keys(KEYS["START"]); run(core, 4); core.set_keys(); run(core, 12)
    core.set_keys(KEYS["START"]); run(core, 4); core.set_keys(); run(core, 16)
    for _ in range(2):      # the cursor: LEVEL -> HANDICAP -> MUSIC
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 8)
    if "NO MUSIC" not in tilemap_text(core, MUSIC_ROW):
        failures.append(f"la fila de MUSIC no dice NO MUSIC: "
                         f"{tilemap_text(core, MUSIC_ROW)!r}")
    else:
        seen = set()
        for _ in range(12):
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 8)
            seen.add(tilemap_text(core, MUSIC_ROW))
        if len(seen) != 5:
            failures.append(f"el acorde del titulo destapo las canciones: "
                             f"el menu ofrece {len(seen)}, no 5")
        else:
            print("  ...y no destapa nada mas: el menu sigue ofreciendo las "
                   "cinco entradas del cartucho")

    # ...AND IT REACHES THE BOARD, which is the half of the feature that was
    # missing for a long time: the skin was the title art and nothing else,
    # and the player who had gone looking for it got the release's blue braid
    # and shaded blocks the moment the game started.
    #
    # Measured off the tilemap on both counts. The FRAME: the panel's wall
    # beside the board and the elbow over it are the release's own $6A $6B /
    # $95 $96 slots in either skin — the art in them changes, not the number —
    # so this compares the PIXELS of those columns instead. The BLOCKS: a
    # prototype writes all four cells of a settled piece with ONE tile (its
    # piece id) where the release writes up to four different joined-block
    # graphics, so a stacked board in a skin has fewer distinct tile ids in it.
    def stack_a_board(chords):
        """A fresh console taken into a game with `chords` skin swaps first,
        with enough soft drop behind it to leave a stack to look at.

        Returns the pixels of the board's left wall and the set of tile ids
        the settled blocks are drawn with.
        """
        this, this_screen = load(rom_path)
        _ = this_screen                      # must stay alive; see load()
        run(this, 40)
        for _i in range(chords):
            this.set_keys(KEYS["L"], KEYS["R"]); run(this, 4)
            this.set_keys(); run(this, 20)
        press_start(this); run(this, 20)      # title -> GAME SELECT
        press_start(this); run(this, 20)      # -> LEVEL SETTINGS
        press_start(this); run(this, 60)      # -> playing
        this.set_keys(KEYS["DOWN"]); run(this, 700)
        this.set_keys(); run(this, 40)
        px = pixels(this_screen)
        wall = [tuple(px[y][x] for x in range(64, 80)) for y in range(16, 144)]
        blocks = {this.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2] & 0x3FF
                  for r in range(10, 20) for c in range(10, 20)} - {0}
        return wall, blocks

    # ...AND THE FRONT END WEARS IT TOO. The menu frame and the HIGH SCORES
    # frame are the same twenty-four charblock slots the board's is, so a skin
    # that stopped at the board left them two thirds in the release's blue
    # braid and one third in the prototype's fret — half-changed, which is
    # worse than either. Counted by colour rather than by tile, because the
    # slots are the same numbers in both and only the art in them differs.
    def menu_colours(chords):
        this, this_screen = load(rom_path)
        _ = this_screen
        run(this, 40)
        for _i in range(chords):
            this.set_keys(KEYS["L"], KEYS["R"]); run(this, 4)
            this.set_keys(); run(this, 20)
        press_start(this); run(this, 24)          # -> GAME SELECT
        px = pixels(this_screen)
        # The frame's own columns, top to bottom, down the left edge.
        return [tuple(px[y][x] for x in range(0, 16)) for y in range(0, 160)]

    plain_menu = menu_colours(0)
    skin_menu = menu_colours(1)
    same = sum(1 for a, b in zip(plain_menu, skin_menu) if a == b)
    if same > len(plain_menu) // 4:
        failures.append(f"la greca del MENU no cambia con la skin: {same} de "
                         f"{len(plain_menu)} filas identicas")
    else:
        print("  ...y la greca de los menus cambia con ella")

    plain_frame, plain_blocks = stack_a_board(0)
    skin_frame, skin_blocks = stack_a_board(1)

    same = sum(1 for a, b in zip(plain_frame, skin_frame) if a == b)
    if same > len(plain_frame) // 4:
        failures.append(f"la greca del tablero no cambia con la skin: "
                         f"{same} de {len(plain_frame)} filas identicas")
    else:
        print("  ...y la greca del tablero es la del prototipo, no la trenza")
    if not plain_blocks or not skin_blocks:
        failures.append("no se asento nada: esta comprobacion no prueba nada")
    elif skin_blocks - set(range(1, 8)):
        failures.append(f"la skin asienta tiles fuera de $01-$07 "
                         f"({sorted(skin_blocks)}): sus bloques solo llegan a $07")
    elif not (plain_blocks - set(range(1, 8))):
        failures.append("el release no asento ningun tile por encima de $07: "
                         "los dos modos estarian usando la misma codificacion")
    else:
        print(f"  ...y sus piezas son una sola por tetromino "
              f"({sorted(skin_blocks)}) donde el release usa "
              f"{len(plain_blocks)} graficos unidos")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: L+R recorre el titulo del release y los de los prototipos, "
          "y la skin llega al tablero.")
    return 0


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
MENU_ARROW_TX = 5
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


# ---------------------------------------------------------------------------
# Leaving the title: two things that were wrong for a whole build and that
# nothing here would have caught, so they get their own check.
#
#   * The title is the only screen with SPRITES on it — the cathedral overlay
#     and the fireworks. Nothing else ever writes OAM, so nothing else ever
#     cleared it, and the cathedral's central tower stood in the middle of
#     GAME SELECT and every screen after it.
#
#   * The title's music has to stop. The cartridge stops it by PREVIEWING each
#     tune as the cursor moves over it (LA035 from $A00A), and LA035 always
#     sends MUSIC_SILENCE before the track — handing the engine a new track
#     without silencing the old one leaves both playing.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# THE BRAID HAS A DIRECTION, AND IT MUST NOT CHANGE WITH THE HUD.
#
# The rope is woven and the weave leans, so its runs and its corners only fit
# each other one way round. This has now been got wrong in both directions:
# once with the corners right and the runs mirrored against the cartridge's,
# once the other way, and each time the tell was the HUD swap — the banner
# redraws the
# two columns beside the board as a plain strip, and if that strip is not the
# same tile the panel puts there, the weave visibly flips as the box comes and
# goes.
#
# So this asks the only question that matters: are those two columns THE SAME
# PIXELS in both modes? It reads them off the screen rather than off the map,
# because a matching map with a mismatched palette bank would still look wrong.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# THE STARTING HANDICAP reaches the board.
#
# The rule itself has its own host tests; this asks the other half of the
# question — that the menu row is there, that a shoulder button moves it, and
# that what it says is what the playfield comes up buried under.
# ---------------------------------------------------------------------------
def handicap_check(rom_path):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    failures = []
    for steps, expect_rows in ((0, 0), (1, 3), (3, 9)):
        core, screen = load(rom_path)
        run(core, 20)
        press_start(core); run(core, 10)      # title -> game select
        press_start(core); run(core, 10)      # -> LEVEL SETTINGS
        # The cursor onto HANDICAP, which is what puts the depth line up.
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        # EL MANDO, NO LOS GATILLOS. La NES no tenia L ni R, asi que la pagina
        # se maneja con los cuatro botones que si tenia: arriba/abajo mueven el
        # cursor, izquierda/derecha cambian el valor, y SELECT (abajo) elige
        # cual de los dos numeros en una carrera.
        for _ in range(steps):
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 8)
        # One line: "HANDICAP  r   ROWS", and r is the CARTRIDGE'S OWN NUMBER
        # — its handicap screen offers 0, 3, 6, 9, 12, which are rows. The
        # step (nought to four) is what the ROM counts in and is not on
        # screen; the word BURIES went with it.
        row = tilemap_text(core, HANDICAP_ROW)
        if "HANDICAP" not in row:
            failures.append(f"la fila {HANDICAP_ROW} no es la del handicap: {row!r}")
        if f"HANDICAP {expect_rows}" not in row or "ROWS" not in row:
            failures.append(f"handicap {steps}: la fila dice {row!r}, "
                             f"esperaba HANDICAP {expect_rows} ... ROWS")
        press_start(core); run(core, 40)      # into the game
        # Count the rows of the playfield that came up with anything in them.
        filled = 0
        field = base + game_offsets(rom_path)["field"]
        for y in range(20):
            if any(core.memory.u8[field + y * PF_W + c] for c in range(1, 11)):
                filled += 1
        if filled != expect_rows:
            failures.append(f"handicap {steps}: el campo empieza con {filled} "
                             f"filas ocupadas, no {expect_rows}")
        elif expect_rows:
            # ...and none of them complete, or they would clear on frame one.
            whole = 0
            for y in range(20):
                if all(core.memory.u8[field + y * PF_W + c] for c in range(1, 11)):
                    whole += 1
            if whole:
                failures.append(f"handicap {steps}: {whole} filas llegan completas")
            else:
                print(f"  handicap {steps}: {filled} filas de basura, "
                       "ninguna completa")
        else:
            print("  handicap 0: el campo empieza vacio")

    failures += handicap_two_check(rom_path)

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el handicap entierra tres filas por paso y deja paso en todas.")
    return 0


def handicap_two_check(rom_path):
    """EN UNA CARRERA HAY DOS HANDICAPS, Y SE ELIGEN CON SELECT.

    Antes se ponian con los gatillos, uno por lado del mando, y una Nintendo
    Entertainment System no tenia gatillos. Arriba y abajo ya mueven el cursor
    e izquierda y derecha ya cambian el valor, asi que queda SELECT, que en
    esta pagina ya significa "el otro"; sobre la linea de HANDICAP salta entre
    los dos numeros en vez de abandonarla, y el que esta elegido se escribe en
    el blanco del cursor para que se vea cual de los dos mueve el mando.

    Se comprueba lo que se ve y lo que hace: que los gatillos ya no tocan nada
    aqui, que SELECT cambia de numero sin cambiar de linea, que el blanco se
    muda con el, y que DERECHA sube el numero blanco y no el otro.
    """
    failures = []
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=10):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def banks():
        """Los bancos de paleta de la fila del handicap, columna a columna."""
        return [(core.memory.u16[SCREENBLOCK_ADDR + (HANDICAP_ROW * 32 + c) * 2]
                 >> 12) & 0xF for c in range(30)]

    def white_digits():
        """Que numeros de la fila estan escritos en el blanco del cursor."""
        text = tilemap_text(core, HANDICAP_ROW)
        b = banks()
        return "".join(ch for ch, bank in zip(text, b)
                       if ch.isdigit() and bank == MENU_ARROW_BANK)

    run(core, 20)
    press_start(core); run(core, 10)      # title -> GAME SELECT
    # VERSUS COMPUTER: dos tableros, dos handicaps, y sin segunda consola.
    for _ in range(3):
        tap("DOWN")
    press_start(core); run(core, 12)       # -> LEVEL SETTINGS
    tap("DOWN")                            # el cursor sobre HANDICAP
    row = tilemap_text(core, HANDICAP_ROW)
    if "HANDICAP 0 0" not in row:
        failures.append(f"en VERSUS la fila no trae dos numeros: {row!r}")
        return failures

    if white_digits() != "0":
        failures.append(f"al abrir, el blanco no marca un solo numero: "
                         f"{white_digits()!r} en {tilemap_text(core, HANDICAP_ROW)!r}")
    else:
        print("  en VERSUS el primer numero abre en blanco: es el que mueve el mando")

    # LOS GATILLOS YA NO PONEN NADA. Uno solo, que es como se hacia antes.
    before = tilemap_text(core, HANDICAP_ROW)
    tap("L"); tap("R")
    if tilemap_text(core, HANDICAP_ROW) != before:
        failures.append(f"un gatillo sigue moviendo el handicap: {before!r} -> "
                         f"{tilemap_text(core, HANDICAP_ROW)!r}")
    else:
        print("  ni L ni R mueven ya el handicap")

    # DERECHA SUBE EL NUMERO BLANCO, que es el primero.
    tap("RIGHT")
    if "HANDICAP 3 0" not in tilemap_text(core, HANDICAP_ROW):
        failures.append(f"DERECHA no sube el primer handicap: "
                         f"{tilemap_text(core, HANDICAP_ROW)!r}")
    else:
        print("  DERECHA sube el del jugador: 3 filas")

    # SELECT CAMBIA DE NUMERO SIN CAMBIAR DE LINEA.
    tap("SELECT")
    if ">" not in tilemap_text(core, HANDICAP_ROW):
        failures.append("SELECT sobre HANDICAP se llevo el cursor de la linea")
    elif white_digits() != "0":
        failures.append(f"SELECT no pasa el blanco al segundo numero: "
                         f"{white_digits()!r} en "
                         f"{tilemap_text(core, HANDICAP_ROW)!r}")
    else:
        print("  SELECT pasa el blanco al handicap de la maquina, "
               "sin salir de la linea")

    tap("RIGHT"); tap("RIGHT")
    if "HANDICAP 3 6" not in tilemap_text(core, HANDICAP_ROW):
        failures.append(f"con el blanco en el segundo, DERECHA no lo sube a el: "
                         f"{tilemap_text(core, HANDICAP_ROW)!r}")
    else:
        print("  ...y DERECHA sube el suyo: 3 para el jugador, 6 para la maquina")

    # Y SELECT OTRA VEZ VUELVE, que si no la linea seria una trampa.
    tap("SELECT")
    if white_digits() != "3":
        failures.append(f"SELECT no devuelve el blanco al primero: "
                         f"{white_digits()!r}")
    # ...y ABAJO sigue siendo la salida de la linea.
    tap("DOWN")
    if ">" in tilemap_text(core, HANDICAP_ROW):
        failures.append("ABAJO ya no saca el cursor de la linea del handicap")
    else:
        print("  ABAJO sigue sacando el cursor de la linea")
    return failures


def panel_check(rom_path):
    """The left panel's five compartments: one shelf apart, and square in them.

    The panel is the coop screen's now — a tall cell at the top for NEXT and
    four short ones under it for the counters, ruled off with the cartridge's
    own blue LEDGE instead of the 1P header grid's grey line. So there are
    three things to measure and none of them is a constant in this file:

      * the four counters open with the SAME air under their shelf. They ride
        a background scrolled two pixels down (SCREENBLOCK_PANEL) precisely so
        that SCORE, whose shelf is the braid, matches the other three;
      * NEXT is CENTRED in the big cell, which is why it alone is drawn on the
        main layer — two pixels of panel offset is the difference between
        centred and five pixels low;
      * and the shelves are the rope's blue, not the grid's grey.
    """
    core, screen = load(rom_path)
    start_game(core)
    run(core, 40)
    rows = pixels(screen)

    # The left box's interior, in pixels: eight tiles from column 2.
    def ink(y):
        return sum(1 for p in rows[y][16:80] if p != (0, 0, 0))

    def content_ink(y):
        """Lit pixels in the panel's CONTENT columns, clear of the braid."""
        return sum(1 for p in rows[y][0:56] if p != (0, 0, 0))

    def band(y):
        """A shelf: the content columns lit wall to wall, with no gaps."""
        return content_ink(y) == 56

    failures = []

    # The shelves, off the screen rather than off a row number. From y=16,
    # which is under the braid's own bottom edge — that fills the width too.
    shelves = [y for y in range(16, 160) if band(y)]
    runs = []
    for y in shelves:
        if runs and y == runs[-1][-1] + 1:
            runs[-1].append(y)
        else:
            runs.append([y])
    if len(runs) != 4:
        failures.append(f"se esperaban 4 baldas en el cajon izquierdo, se ven "
                         f"{len(runs)}: {[r[0] for r in runs]}")
    else:
        colours = set().union(*({rows[y][x] for y in r for x in range(0, 56)}
                                 for r in runs))
        if BRAID_BLUE not in colours:
            failures.append(f"las baldas no son azules como la greca: "
                             f"{sorted(colours)}")
        else:
            print(f"  cuatro baldas azules, en y={[r[0] for r in runs]}")

    # Each counter's headroom: from the bottom of its shelf to its label's ink.
    floor = min(ink(y) for y in range(60, 158))
    gaps = []
    for r in runs:
        y = r[-1] + 1
        n = 0
        while ink(y + n) <= floor:
            n += 1
        gaps.append((r[-1] + 1, n))

    if len(gaps) == 4 and len({n for _, n in gaps}) != 1:
        failures.append("los contadores no tienen el mismo hueco bajo su balda: "
                         + ", ".join(f"y={y} -> {n}px" for y, n in gaps))
    elif len(gaps) == 4:
        print(f"  los cuatro contadores abren con {gaps[0][1]} pixeles bajo su "
               f"balda (en y={', '.join(str(y) for y, _ in gaps)})")

    # NEXT, centred in the big cell: from the braid's last row to the first
    # shelf, with the label and the preview somewhere in between.
    if runs:
        # The cell runs from under the braid to the shelf's own tile row.
        top, bot = 16, (runs[0][0] // TILE) * TILE
        lit = [y for y in range(top, bot) if content_ink(y)]
        if not lit:
            failures.append("la celda grande del cajon izquierdo esta vacia: "
                             "NEXT no se dibuja donde debe")
        else:
            above, below = lit[0] - top, bot - 1 - lit[-1]
            if abs(above - below) > 1:
                failures.append(f"NEXT no esta centrado en su celda: {above}px "
                                 f"por arriba, {below}px por abajo")
            else:
                print(f"  NEXT centrado en la celda grande: {above}px arriba, "
                       f"{below}px abajo")

    # ...AND THE TWO PANELS ARE MIRRORS. Both are inverted Ls now — rope along
    # the top and down the side facing the board, open at the bottom — so the
    # right one's rope has to run to the screen's last line exactly as the
    # left one's does, and the histogram standing in it has to reach the
    # bottom without anything closing it off.
    core.set_keys(KEYS["SELECT"])
    run(core, 5)
    core.set_keys()
    run(core, 40)
    rows = pixels(screen)

    def rope(x0, x1):
        """The lowest scanline the rope is drawn on in those columns.

        Nearly all of them, not all: the braid's outermost pixel column is
        transparent and shows the backdrop through, on both sides.
        """
        return max((y for y in range(100, SCREEN_H)
                    if sum(1 for x in range(x0, x1)
                           if rows[y][x] != (0, 0, 0)) >= (x1 - x0) - 2),
                   default=None)

    left, right = rope(64, 80), rope(160, 176)
    if left != SCREEN_H - 1 or right != SCREEN_H - 1:
        failures.append(f"la greca no llega al borde inferior en los dos "
                         f"cajones: izq acaba en y={left}, der en y={right}")
    else:
        print("  las dos grecas bajan hasta la ultima linea: los cajones son "
               "L invertidas, espejo la una de la otra")

    icons = max((y for y in range(100, SCREEN_H)
                 if any(rows[y][x] != (0, 0, 0) for x in range(176, 240))),
                default=None)
    if icons is None:
        failures.append("el cajon derecho no dibuja las estadisticas")
    elif icons < SCREEN_H - 4:
        failures.append(f"las estadisticas acaban en y={icons} y el cajon "
                         f"llega a {SCREEN_H - 1}: sobra panel debajo")
    else:
        print(f"  las estadisticas llegan a y={icons}, al pie del cajon")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: SCORE respira igual que LINES, LEVEL y HIGH, y las stats no "
           "pisan la greca.")
    return 0


# Palette banks 12 and 13: the falling piece's colours and the preview's.
# setPiecePalette (main.asm.txt:5338) indexes kRomPiecePalettes by PIECE ID,
# so the two banks must differ exactly when the two pieces do.
PAL_PIECE_BANK, PAL_NEXT_BANK = 12, 13


def palette_bank(core, n):
    return tuple(core.memory.u16[0x05000000 + (n * 16 + i) * 2] for i in (1, 2, 3))


def next_palette_check(rom_path):
    """The preview must be painted in the NEXT piece's colours.

    Both were drawn out of bank 12, which setPiecePalette loads with the piece
    IN PLAY, so the preview wore the falling piece's colours and changed under
    you every time one locked. The bug is invisible whenever the two pieces
    happen to share a palette, which is why this walks a whole game rather
    than sampling once.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    run(core, 20)

    # What each piece id's colours ARE, learned from the bank the falling
    # piece owns — so this never has to hard-code the table.
    known = {}
    failures = []
    pairs = set()
    was = None
    for _ in range(2500):
        core.set_keys(KEYS["DOWN"])
        core.run_frame()
        cur = core.memory.u8[base + off["current"]]
        nxt = core.memory.u8[base + off["next"]]
        stable, was = (cur, nxt) == was, (cur, nxt)
        # mGBA hands the frame back between the port's step and its draw, so
        # on the frame a piece locks the state has moved on and the palettes
        # have not. Only a pair that survived a second frame is settled.
        if not stable:
            continue
        if not (1 <= cur <= 7) or not (1 <= nxt <= 7):
            continue
        known[cur] = palette_bank(core, PAL_PIECE_BANK)
        pairs.add((cur, nxt))
        want = known.get(nxt)
        if want is None:
            continue        # this piece has not fallen yet; nothing to compare
        got = palette_bank(core, PAL_NEXT_BANK)
        if got != want:
            failures.append(f"con {cur} cayendo y {nxt} en NEXT, la vista previa "
                             f"usa {got} y no {want}")
            break
    core.set_keys()

    mixed = sum(1 for c, n in pairs if known.get(c) != known.get(n))
    if not mixed:
        failures.append("no se vio ninguna pareja de piezas con paletas "
                         "distintas: la prueba no prueba nada")
    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print(f"  {len(pairs)} parejas pieza/siguiente, {mixed} de ellas con "
           "paletas distintas")
    print("OK: NEXT se pinta con los colores de la pieza que viene.")
    return 0


def demo_check(rom_path):
    """The attract demo: the title starts playing by itself.

    demoStart is reached from the title's own clock at frameCounterHigh 5,
    frameCounterLow $20 (main.asm.txt:4154-4160) — 1312 frames — and sets
    playMode 0, suspends the music and drops into the ordinary game init. So
    what this checks is the three things that make it a demo: it starts on
    its own, the board fills without the pad being touched, and a press on
    the pad is the way OUT rather than a move.
    """
    flag, why = game_state_address(rom_path, "g_demo")
    if flag is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    started = None
    for f in range(DEMO_WATCH_FRAMES):
        core.run_frame()
        if core.memory.u8[flag]:
            started = f
            break
    if started is None:
        failures.append(f"la demo no arranca sola en {DEMO_WATCH_FRAMES} frames")
        print("FALLA:", failures[-1])
        return 1
    if not (1250 <= started <= 1400):
        failures.append(f"la demo arranca en el frame {started}, no cerca de "
                         f"{DEMO_START_FRAME} (frameCounterHigh 5, low $20)")
    else:
        print(f"  arranca sola en el frame {started}, con el titulo sin tocar")

    # Nothing touches the pad from here: every cell that appears was the
    # computer's.
    run(core, 6000)
    cells = sum(1 for y in range(TENGEN_PF_HEIGHT)
                for x in range(1, TENGEN_PF_WIDTH - 1)
                if core.memory.u8[base + off["field"] + y * TENGEN_PF_WIDTH + x])
    if cells < 4:
        failures.append(f"la demo solo asento {cells} celdas: no esta jugando")
    else:
        print(f"  juega sola: {cells} celdas asentadas sin tocar el mando")

    # ...and START is the way out, to GAME SELECT, not a pause.
    core.set_keys(KEYS["START"])
    run(core, 4)
    core.set_keys()
    run(core, 20)
    if core.memory.u8[flag]:
        failures.append("START no saca de la demo")
    elif "GAME SELECT" not in tilemap_text(core, 8):
        failures.append(f"START saca de la demo a {tilemap_text(core, 8)!r}, "
                         "no a GAME SELECT")
    else:
        print("  START sale de la demo a GAME SELECT, como en el cartucho")
    del core, screen

    # ...Y TAMBIEN DESDE UN TITULO CON SKIN, que es donde no llegaba. El reloj
    # del titulo vivia dentro de la rutina de sprites, de la que una skin sale
    # antes de tiempo, asi que sobre una pantalla de prototipo el contador no
    # se movia y la demo no llegaba nunca: el titulo del release se ponia a
    # jugar solo a los veintidos segundos y el del prototipo se quedaba ahi
    # para siempre.
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    run(core, 40)
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4); core.set_keys(); run(core, 20)
    started = None
    for f in range(DEMO_WATCH_FRAMES):
        core.run_frame()
        if core.memory.u8[flag]:
            started = f
            break
    if started is None:
        failures.append(f"con skin la demo no arranca en {DEMO_WATCH_FRAMES} "
                         f"frames: el reloj del titulo no corre bajo ella")
    else:
        print(f"  y con la skin puesta arranca igual, en el frame {started}")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el titulo se pone a jugar solo y se sale con un boton.")
    return 0


def computer_check(rom_path):
    """VERSUS and WITH COMPUTER: the two modes that need no second console.

    The computer is player 2 in both (main.asm.txt:3736-3749), and nothing
    here touches the pad — so every cell that appears on its board was placed
    by computerMove. What this asks is that it PLAYS: pieces land, they do not
    all land in one column, and the board it lands them on is the right one
    for the mode (its own in a race, the shared twelve-wide one in WITH).
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH

    def start(entry, frames):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        run(core, 8)
        press_start(core)               # title -> game select
        run(core, 10)
        for _ in range(entry):          # down to the mode
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)   # -> LEVEL SETTINGS
        press_start(core); run(core, frames)
        return core, screen

    def board(core, which):
        addr = base + off["field"] + which * PF
        return [[core.memory.u8[addr + y * TENGEN_PF_WIDTH + x]
                 for x in range(TENGEN_PF_WIDTH)] for y in range(TENGEN_PF_HEIGHT)]

    # HOW LONG TO WATCH, AND WHY IT IS NOT LONGER. This used to run six
    # thousand frames — a hundred seconds at level 0 — which was fine while a
    # finished game sat on its plaque for ever. It does not any more: the
    # plaque counts down to the high scores and the high scores count down to
    # the title (GAMEOVER_HOLD_FRAMES), and from the title the attract demo's
    # own clock starts a fresh 1 PLAYER game, whose player-2 field is walled.
    # So a board that filled up before the sample came back with forty cells
    # in the second field and this read it as "two boards". Long enough for
    # the computer to stack a corner, short enough that it is still stacking.
    WATCH = 2500

    # VERSUS: two boards. The computer plays its own; ours stays as it was
    # apart from the piece gravity drops on it.
    core, screen = start(3, WATCH)
    comp = board(core, 1)
    cols = {x for row in comp for x in range(1, TENGEN_PF_WIDTH - 1) if row[x]}
    cells = sum(1 for row in comp for x in range(1, TENGEN_PF_WIDTH - 1) if row[x])
    if cells < 8:
        failures.append(f"en VERSUS el ordenador solo asento {cells} celdas: no juega")
    elif len(cols) < 3:
        failures.append(f"el ordenador amontona todo en {len(cols)} columna(s): "
                         "no esta eligiendo")
    else:
        print(f"  VERSUS: el ordenador asento {cells} celdas en {len(cols)} columnas")

    # WITH: one twelve-wide board, and the computer plays into it.
    core2, screen2 = start(4, WATCH)
    shared = board(core2, 0)
    other = sum(1 for row in board(core2, 1) for v in row if v)
    wide = sum(1 for row in shared if row[0] or row[TENGEN_PF_WIDTH - 1])
    if "HIGH SCORES" in tilemap_text(core2, LEADER_HEAD_TY, 0, 30):
        failures.append("la partida de WITH COMPUTER ya habia terminado cuando "
                         "se miro el tablero: no prueba nada")
    elif other:
        failures.append(f"WITH COMPUTER usa dos campos ({other} celdas en el "
                         "segundo): deberia compartir uno")
    elif not any(v for row in shared for v in row):
        failures.append("en WITH COMPUTER no se asento nada")
    else:
        print(f"  WITH COMPUTER: un solo campo compartido, {wide} filas "
               "llegan a las columnas que solo existen en coop")

    # ...AND IT STILL PLAYS AFTER THE ATTRACT DEMO HAS RUN. The demo puts the
    # computer on PLAYER 1, which is the cartridge's own arrangement, and for
    # a while nothing here put it back — so a console left alone for the
    # twenty-two seconds the demo's clock takes started VERSUS and WITH
    # COMPUTER with a computer that never moved. Every check above drove the
    # menus faster than that clock, so every one of them passed.
    core3, screen3 = load(rom_path)
    run(core3, DEMO_START_FRAME + 400)
    if "TETRIS" in tilemap_text(core3, 4, 0, 30):
        failures.append("la demo no arranco: esta comprobacion no prueba nada")
    press_start(core3); run(core3, 20)        # out of the demo -> GAME SELECT
    for _ in range(4):
        core3.set_keys(KEYS["DOWN"]); run(core3, 4)
        core3.set_keys(); run(core3, 10)
    press_start(core3); run(core3, 12)
    press_start(core3); run(core3, 30)
    run(core3, 2500)
    addr = base + off["field"]
    after = sum(1 for i in range(PF) if core3.memory.u8[addr + i])
    if after < 40:
        failures.append(f"tras la demo el ordenador no juega: solo {after} "
                         "celdas en el tablero compartido")
    else:
        print(f"  ...y sigue jugando despues de la demo de atraccion "
               f"({after} celdas)")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el jugador COMPUTER juega solo, en el tablero de cada modo.")
    return 0


def points_check(rom_path):
    """THE POINTS THE PIECE WAS WORTH, beside the piece.

    L8129 stages three sprites the moment a piece rests and
    stageDropPointSprites keeps them up for $3C frames
    (main.asm.txt:218-313). Three things have to be true of them and all
    three are the cartridge's:

      * the number is the award, which is what the score just went up by;
      * the HEIGHT is the landing height, because in this game the height IS
        the score (the award grows the higher the piece rests); and
      * the SIDE is the player — on a coop board, one to each side of it, in
        each player's own palette.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    POINTS_FIRST, POINTS_COUNT = 118, 6

    def digits(core, first, count):
        """(x, y, value) of a run of point sprites, or None."""
        out = oam_visible(core, first, first + count)
        if not out:
            return None
        out.sort()
        value = 0
        for _x, _y, tile in out:
            value = value * 10 + ((tile - 512) & 0xF)
        return out[0][0], out[0][1], value

    # 1 PLAYER: hold Down, watch a piece land on the floor.
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    score = base + off["score"]

    def read_score():
        return sum(core.memory.u8[score + i] << (8 * i) for i in range(4))

    # The score changes inside tengen_step and the sprites are written at the
    # end of draw_match, and mGBA's frame boundary need not fall between the
    # two — so the award is measured as the last CHANGE in the score rather
    # than as a difference across the frame the sprites turned up on.
    last = read_score()
    gained = 0
    shown = None
    for _ in range(2000):
        core.set_keys(KEYS["DOWN"]); run(core, 1)
        now = read_score()
        if now != last:
            gained = now - last
            last = now
        got = digits(core, POINTS_FIRST, 3)
        if got:
            shown = got
            break
    core.set_keys(); run(core, 2)

    if shown is None:
        failures.append("una pieza se asento y no aparecio su puntuacion al lado")
    else:
        x, y, value = shown
        if value != gained:
            failures.append(f"los sprites dicen {value} y el marcador subio {gained}")
        elif x != (COL_FIELD[1]) * TILE:
            failures.append(f"la puntuacion sale en x={x}, no al borde derecho "
                             f"del tablero ({COL_FIELD[1] * TILE})")
        elif y != (TENGEN_PF_HEIGHT - 1) * TILE:
            failures.append(f"una pieza asentada en el suelo muestra sus puntos "
                             f"en y={y}, no en la ultima fila")
        else:
            print(f"  1 PLAYER: {value} puntos, junto al tablero, a la altura "
                   "en que se poso la pieza")

    # WITH COMPUTER: one board, two players, one to each side of it.
    core2, screen2 = load(rom_path)
    run(core2, 8); press_start(core2); run(core2, 10)
    for _ in range(4):
        core2.set_keys(KEYS["DOWN"]); run(core2, 4); core2.set_keys(); run(core2, 10)
    press_start(core2); run(core2, 12)
    press_start(core2); run(core2, 30)

    sides = {}
    for _ in range(4000):
        run(core2, 1)
        for slot in (0, 1):
            got = digits(core2, POINTS_FIRST + slot * 3, 3)
            if got and slot not in sides:
                sides[slot] = got
        if len(sides) == 2:
            break

    if len(sides) < 2:
        failures.append(f"en el tablero compartido solo salieron los puntos de "
                         f"{len(sides)} jugador(es)")
    else:
        left = sides[0][0] + 8 * 3
        if sides[0][0] >= COOP_FIELD_TX * TILE:
            failures.append("los puntos del jugador 1 no salen a la izquierda "
                             "del tablero compartido")
        elif sides[1][0] < (COOP_FIELD_TX + TENGEN_PF_WIDTH) * TILE:
            failures.append("los puntos del ordenador no salen a la derecha "
                             "del tablero compartido")
        else:
            print(f"  WITH COMPUTER: {sides[0][2]} a la izquierda y "
                   f"{sides[1][2]} a la derecha, uno por jugador (left={left})")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada pieza dice lo que vale, donde y cuando lo dice el cartucho.")
    return 0


def _report(failures):
    for f in failures:
        print("FALLA:", f)
    return 1 if failures else 0


def leaderboard_check(rom_path):
    """THE HIGH SCORES TABLE, which the cartridge keeps in memory.

    Four things, all of them the ROM's:

      * the table it comes up with cold — @resetHighScores builds fifteen
        entries of AAA from 17000 down to 3000 in thousands
        (main.asm.txt:5670-5700), which is why HIGH SCORE opens at 017000 and
        not at nothing;
      * a score that beats one of them goes IN, at the right row, pushing the
        rest down and the last off the bottom (L81FF, :342-378);
      * the three initials are typed with Left and Right and taken with A or B
        (L9234, :2709-2762); and
      * the table is still there for the next game.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []

    def rows(core):
        return [tilemap_text(core, LEADER_FIRST_TY + i, 0, 30)
                for i in range(15)]

    def to_gameover(core, score):
        """Plant a score, bury the board, and take the plaque's way out."""
        for i in range(4):
            core.memory.u8[base + off["score"] + i] = (score >> (8 * i)) & 0xFF
        for r in range(TENGEN_PF_HEIGHT):
            for c in range(TENGEN_PF_WIDTH):
                core.memory.u8[base + off["field"] + r * TENGEN_PF_WIDTH + c] = (
                    CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                    else (0 if c == 5 else CELL_BLOCK))
        run(core, 240)
        press_start(core); run(core, 30)

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)

    # The cold table, seen through the panel's own HIGH counter first.
    to_gameover(core, 20)
    table = rows(core)
    wanted = [f"{17 - i}000" for i in range(15)]
    got = ["".join(ch for ch in row if ch.isdigit())[-9:-3] for row in table]
    if not all(w in g for w, g in zip(wanted, got)):
        failures.append(f"la tabla fria no es la del cartucho: {got[:3]} ...")
    elif "AAA" not in table[0]:
        failures.append(f"la primera entrada no sale como AAA: {table[0]!r}")
    else:
        print("  arranca con las quince del cartucho, de 17000 a 3000, todas AAA")

    # ...and back out, then a score that belongs on it.
    press_start(core); run(core, 40)
    press_start(core); run(core, 10)   # title -> game select
    press_start(core); run(core, 12)   # -> level settings
    press_start(core); run(core, 30)   # -> play
    to_gameover(core, 50000)
    table = rows(core)
    if "050" not in table[0] or "017000" not in table[1]:
        failures.append(f"un 50000 no entra en cabeza: {table[0]!r} / {table[1]!r}")
    elif "003000" in "".join(table):
        failures.append("la ultima entrada no se cayo de la tabla")
    else:
        print("  un 50000 entra el primero y empuja a las demas una fila abajo")

    # The initials: Right walks the alphabet, A takes the letter.
    def tap(name, times=1):
        for _ in range(times):
            core.set_keys(KEYS[name]); run(core, 3); core.set_keys(); run(core, 6)

    # UP and DOWN walk the alphabet, LEFT and RIGHT pick the letter, B undoes
    # the last change and A takes the name. B with nothing to undo is an
    # alarm, which cannot be read off the tilemap — the core check for that is
    # that it does not change anything.
    tap("UP", 1); tap("RIGHT")     # B
    tap("UP", 2); tap("RIGHT")     # C
    tap("UP", 3)                   # D
    tap("B")                       # ...and take the D back
    if "BC" not in tilemap_text(core, LEADER_FIRST_TY, 0, 30):
        failures.append(f"B no deshizo solo la ultima letra: "
                         f"{tilemap_text(core, LEADER_FIRST_TY, 0, 30)!r}")
    tap("UP", 3); tap("A")
    run(core, 20)
    if " BCD " not in tilemap_text(core, LEADER_FIRST_TY, 0, 30):
        failures.append("las iniciales no se escriben: "
                         f"{tilemap_text(core, LEADER_FIRST_TY, 0, 30)!r}")
    else:
        print("  arriba/abajo la letra, izquierda/derecha el hueco, B deshace, "
               "A acepta")

    # AND IT SURVIVES THE POWER GOING OFF, which is the one thing the NES
    # cartridge wanted and could not have: its magic at $04F7 only carries the
    # table across a RESET. Here it is in the GBA's battery-backed SRAM, under
    # the same four letters, and core.reset() is the console being switched
    # off and on again as far as that memory is concerned.
    sram = bytes(core.memory.u8[0x0E000000 + i] for i in range(8))
    if sram[:4] != b"LOGG":
        failures.append(f"la tabla no se guarda en SRAM: {sram[:4]!r}")
    else:
        core.reset(); run(core, 40)
        start_game(core)
        to_gameover(core, 10)
        if "BCD" not in rows(core)[0]:
            failures.append("la tabla no sobrevive al apagado")
        else:
            print("  y sobrevive a apagar la consola, en la SRAM de la pila")
        if failures:
            return _report(failures)
        print("OK: la tabla de records es la del cartucho, se escribe en ella, "
               "y se guarda.")
        return 0

    # And it survives the next game.
    press_start(core); run(core, 40)
    press_start(core); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 30)
    to_gameover(core, 20)
    if "BCD" not in rows(core)[0]:
        failures.append("la tabla no sobrevive a la siguiente partida")
    else:
        print("  y sigue ahi despues de la siguiente partida")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: la tabla de records es la del cartucho, y se escribe en ella.")
    return 0


def counters_check(rom_path):
    """NO LEADING ZEROS. renderStatistics walks each counter's digits from the
    top and, while it finds a '0', shortens the run and advances the write
    position (main.asm.txt:4067-4082) — the number keeps its place and the
    zeros in front of it are never drawn. The cartridge's own screen reads
    8294 / 30 / 2, not 008294 / 0030 / 02.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    run(core, 20)
    for i in range(4):
        core.memory.u8[base + off["score"] + i] = (8294 >> (8 * i)) & 0xFF
        core.memory.u8[base + off["lines"] + i] = (30 >> (8 * i)) & 0xFF
    core.memory.u8[base + off["level"]] = 2
    run(core, 20)

    failures = []
    for name, ty, want in (("SCORE", 3, "8294"), ("LINES", 6, "30"),
                            ("LEVEL", 9, "2")):
        # columns 2-8 are the counter's own; 0-1 and 9 are the braid,
        # whose tiles happen to fall in the printable range.
        line = tilemap_text(core, ty, 2, 8).strip()
        if line != want:
            failures.append(f"{name} sale como {line!r}, el cartucho lo pinta "
                             f"como {want!r}")
    if failures:
        for f in failures:
            print("FALLA:", f)
        return 1
    print("OK: los contadores no pintan ceros a la izquierda, como el cartucho.")
    return 0


# Where the pause menu's lines land, derived the way gba/port.h derives them:
# a box PMENU_H tall centred on a 20-row screen, with the column inside it.
# The lines do NOT all live on the same background — the heading and the
# question's second line ride the counters' layer two pixels down, the
# even-length ones the offset layer three across — but tilemap_text reads all
# four, so these are just rows.
PMENU_H = 10
PMENU_TY = (SCREEN_H // TILE - PMENU_H) // 2
PM_HEAD = PMENU_TY + 2       # PAUSE
PM_MUSIC = PMENU_TY + 4      # MUSIC
PM_TUNE = PMENU_TY + 5       # ...and the tune's name under it
PM_EXIT = PMENU_TY + 7       # EXIT
PM_ASK = PMENU_TY + 2        # the question's EXIT
PM_SURE = PMENU_TY + 3       # ...and its SURE?
PM_ANSWER = PMENU_TY + 6     # YES, with NO under it
# The box's own columns, which is all a check about the box should read: the
# rest of the row is the HUD, and the braid decodes as stray letters.
# FOURTEEN, not thirteen: an odd width cannot be centred on the board, and the
# box lands on the board. See PMENU_W in gba/port.h.
PMENU_W_T = 14
PMENU_TX = (SCREEN_TW_TILES - PMENU_W_T) // 2
PM_L = PMENU_TX + 1
PM_R = PMENU_TX + PMENU_W_T - 1


def pausemenu_check(rom_path):
    """THE PAUSE MENU, AND THE CHORD THAT IS NOT IN THE GAME.

    Not the cartridge's — its PAUSE is a plaque and nothing else — so what
    this checks is that it behaves: that MUSIC really changes the tune while
    the game is held, that EXIT asks before it does anything, that NO comes
    back, and that YES leaves by the same road a finished game leaves by. And
    that the cheat codes cannot be typed through it, which is the one way it
    could break something that already worked.

    THE CHORD IS RUNG ON THE MENUS, not on the plaque. L+R on GAME SELECT or
    on LEVEL SETTINGS uncovers this menu and the hidden tunes together; in
    play the same chord swaps the HUD and uncovers nothing, so a plaque that
    has not been unlocked stays a plaque however long you hold the shoulders
    on it. Both halves are checked: that the game screen does NOT open it, and
    that the menu screen does.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def row(r):
        """Only the pause box's own columns: see PM_L."""
        return tilemap_text(core, r, PM_L, PM_R)

    # FIRST: a game with the chord never rung. The plaque must stay a plaque.
    start_game(core)
    run(core, 30)
    tap("START")
    if not core.memory.u8[base + off["paused"]]:
        failures.append("START no pausa")
    tap("L", "R")
    if "PAUSE" in row(PM_HEAD) and "EXIT" in row(PM_EXIT):
        failures.append("L+R en la partida abre el menu: el acorde es de los "
                         "menus, y en juego solo cambia el HUD")
    else:
        print("  L+R en la partida no abre nada: el acorde ya no vive aqui")

    # ...now ring it where it lives, and come back into a game.
    core, screen = load(rom_path)
    run(core, 8)
    press_start(core); run(core, 10)    # title -> GAME SELECT
    tap("L", "R")                        # the chord, on the first menu
    press_start(core); run(core, 12)     # -> LEVEL SETTINGS
    press_start(core); run(core, 30)     # -> play
    tap("START")
    if ("PAUSE" not in row(PM_HEAD) or "MUSIC" not in row(PM_MUSIC)
            or "EXIT" not in row(PM_EXIT)):
        failures.append(f"el acorde en GAME SELECT no abre el menu de pausa: "
                         f"{row(PM_HEAD)!r} / {row(PM_MUSIC)!r} / "
                         f"{row(PM_EXIT)!r}")
        print("FALLA:", failures[-1])
        return 1
    print("  L+R en GAME SELECT destapa el menu, y la pausa ya es el menu")

    # AND THE BOX IS CENTRED ON THE BOARD, which an odd width cannot be: the
    # playfield's ten columns run 10-19, so its middle is x=120, and so is the
    # screen's. Measured off the framebuffer rather than off PMENU_TX, because
    # what went wrong before was the arithmetic and not the drawing.
    # Off the box's OWN tiles, not off lit pixels: the HUD panels are lit on
    # this scanline too. $29/$2A/$2B are the game-over plaque's top-left, top
    # and top-right, which is what the box is framed with.
    top = [c for c in range(SCREEN_TW_TILES)
           if (core.memory.u16[SCREENBLOCK_ADDR + (PMENU_TY * 32 + c) * 2] & 0x3FF)
           in (0x29, 0x2A, 0x2B)]
    if not top:
        failures.append("no se encuentra el borde superior de la caja de pausa")
    else:
        x0, x1 = top[0] * TILE, (top[-1] + 1) * TILE
        middle = (x0 + x1) / 2
        if abs(middle - SCREEN_W / 2) > 1:
            failures.append(f"la caja de pausa no esta centrada: va de x={x0} "
                             f"a {x1 - 1}, centro {middle}, y la pantalla "
                             f"{SCREEN_W / 2}")
        else:
            print(f"  la caja va de x={x0} a {x1 - 1}, centro {middle}: "
                   f"centrada en el tablero")

    before = row(PM_TUNE)
    tap("RIGHT")
    if row(PM_TUNE) == before:
        failures.append("DERECHA no cambia la cancion en el menu de pausa")
    else:
        print(f"  la musica se cambia sin salir: {before.strip()!r} -> "
               f"{row(PM_TUNE).strip()!r}")

    # ...AND MUSIC MIX IS AS QUIET HERE AS IT IS ON THE SETTINGS SCREEN.
    # There is no one tune to audition in a rotation, so the selection screen
    # goes silent on it like it does on NO MUSIC; this menu was starting
    # whichever tune the rotation happened to be on, which made the same
    # choice sound like two different things depending on where you made it.
    # And going quiet must not leave the MATCH quiet: nothing else restarts
    # the engine on the way out of a pause, so the silence is spent on the
    # unpause.
    for _ in range(12):
        if "MIX" in row(PM_TUNE):
            break
        tap("RIGHT")
    if "MIX" not in row(PM_TUNE):
        failures.append("el menu de pausa no ofrece MUSIC MIX")
    else:
        loud = 0
        for _ in range(120):
            core.run_frame()
            st = sound_state(core)
            loud |= st["activos"] | st["pulso 1"] | st["pulso 2"] | st["triangulo"]
        if loud:
            failures.append(f"MUSIC MIX suena en el menu de pausa "
                             f"(canales {loud:#06b}); en el de niveles calla")
        else:
            print("  MUSIC MIX no suena aqui, igual que en la pantalla de "
                  "seleccion")
            # The match has to come back with it, though.
            tap("START", settle=30)
            back = 0
            for _ in range(180):
                core.run_frame()
                st = sound_state(core)
                if st["activos"] or st["pulso 1"] or st["pulso 2"] or st["triangulo"]:
                    back += 1
            if not back:
                failures.append("tras elegir MUSIC MIX en la pausa la partida "
                                 "vuelve muda")
            else:
                print(f"  ...y al reanudar la partida suena "
                      f"({back}/180 frames)")
            # Back into the menu for the checks below.
            tap("START", settle=20)
            tap("L", "R", settle=20)

    # THE CURSOR IS AN ARROW, and it has to be ON the line it marks and on no
    # other -- picking the line out by palette read as a colour scheme rather
    # than as a cursor. $3E is the cartridge's own right arrow and the tileset
    # is ASCII-indexed, so it comes back from tilemap_text as '>'.
    if ">" not in row(PM_MUSIC) or ">" in row(PM_EXIT):
        failures.append(f"la flecha no esta en MUSIC: {row(PM_MUSIC)!r} / "
                         f"{row(PM_EXIT)!r}")
    # ...and SELECT moves it, the way it does on the settings screen.
    tap("SELECT")
    if ">" not in row(PM_EXIT) or ">" in row(PM_MUSIC):
        failures.append(f"SELECT no mueve la flecha: {row(PM_MUSIC)!r} / "
                         f"{row(PM_EXIT)!r}")
    else:
        print("  la flecha marca la linea, y SELECT la mueve")

    # START IS THE WAY OUT FROM EVERY LINE OF IT, the EXIT line included: it
    # is the button that put the plaque up. The cursor is sitting on EXIT
    # right now, which is where it used to open the question instead.
    tap("START")
    if core.memory.u8[base + off["paused"]]:
        failures.append("START sobre EXIT no reanuda la partida")
    else:
        # AND IT TAKES THE WHOLE MENU WITH IT. The frame and the lines that
        # centre exactly are on the main background and a repaint covers
        # those; PAUSE, the tune's name and EXIT are on the counter and
        # offset layers, which the static screen never writes in the middle
        # of the board -- so the window vanished and the words stayed.
        # The board's own frame art decodes as the odd stray letter inside
        # these columns, so this looks for the menu's WORDS and its cursor.
        seen = " ".join(row(r) for r in range(PMENU_TY, PMENU_TY + PMENU_H))
        leftover = [w for w in ("PAUSE", "MUSIC", "EXIT", "LOGINSKA", ">")
                    if w in seen]
        if leftover:
            failures.append("START reanuda pero deja texto del menu en "
                             f"pantalla: {leftover} en {seen!r}")
        else:
            print("  START cierra el menu desde cualquier linea, y no deja "
                   "nada escrito")
    tap("START"); tap("L", "R")

    # The cheat codes must not be reachable through it. The level-up code is
    # Up Down Up Down Left Right B B A; typed here it moves the cursor and
    # picks tunes, and the level must not move.
    level = core.memory.u8[base + off["level"]]
    for name in ("UP", "DOWN", "UP", "DOWN", "LEFT", "RIGHT", "B", "B", "A"):
        tap(name)
    if core.memory.u8[base + off["level"]] != level:
        failures.append("el codigo de subir nivel se cuela por el menu de pausa")
    else:
        print("  los codigos de trucos no atraviesan el menu")

    # Walk to EXIT, which must ASK. (The cheat-code sequence above left the
    # game in whatever state its last button put it in, so make sure it is
    # held before driving the menu.)
    if not core.memory.u8[base + off["paused"]]:
        tap("START")
    while "SURE" in row(PM_SURE):
        tap("DOWN"); tap("A")    # back out of a question it may have opened
    while "EXIT" not in row(PM_EXIT):
        tap("START")
    # A is what takes a choice now; START only ever leaves.
    while ">" not in row(PM_EXIT):
        tap("DOWN")
    tap("A")
    if "SURE" not in row(PM_SURE):
        failures.append(f"EXIT no pregunta antes de salir: {row(PM_SURE)!r}")
    else:
        print("  EXIT pregunta antes de nada")
        # NO comes back to the game, still paused, still playing.
        tap("A")
        if not core.memory.u8[base + off["paused"]]:
            failures.append("decir NO al salir dejo la partida sin pausa")
        elif "SURE" in row(PM_SURE):
            failures.append("decir NO no cierra la pregunta")
        else:
            print("  NO vuelve a la partida")
        # ...and YES leaves STRAIGHT to the title: a game you walked out of
        # has not ended, and its score has no business on the board.
        while "SURE" not in row(PM_SURE):
            while ">" not in row(PM_EXIT):
                tap("DOWN")
            tap("A")
        tap("LEFT"); tap("A"); run(core, 60)
        if "HIGH SCORES" in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
            failures.append("salir a la fuerza pasa por la tabla de records")
        elif "EXIT" in row(PM_EXIT) or "PAUSE" in row(PM_HEAD):
            failures.append("decir SI no sale de la partida")
        else:
            print("  SI sale al titulo, sin pasar por la tabla")

    # ONCE FOUND, STILL FOUND. The chord is a thing you discover, not a thing
    # you should have to remember to do at the start of every game — so the
    # next game's PAUSE is the menu, with no chord. And the HUD you picked is
    # still the HUD you picked.
    press_start(core); run(core, 10)    # title -> game select
    press_start(core); run(core, 12)    # -> level settings
    press_start(core); run(core, 30)    # -> play
    tap("START")
    if "PAUSE" not in row(PM_HEAD) or "EXIT" not in row(PM_EXIT):
        failures.append("el menu de pausa se pierde al empezar otra partida")
    else:
        print("  y en la siguiente partida la pausa ya es el menu, sin acorde")
    tap("START")

    # ...and so does the HUD. The banner's letters are art, not ASCII, so this
    # reads the tilemap directly: the right column carries them in HUD Banner
    # and is blank there in HUD Stats.
    def hud():
        row10 = [core.memory.u16[SCREENBLOCK_ADDR + ((10 * 32 + c) * 2)] & 0x3FF
                 for c in range(24, 28)]
        return "BANNER" if any(row10) else "STATS"

    if hud() != "BANNER":
        failures.append(f"la partida no abre en HUD Banner sino en {hud()}")
    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 20)
    if hud() != "STATS":
        failures.append("SELECT en juego no cambia el HUD")
    else:
        # quit out and come back
        tap("START")
        while ">" not in row(PM_EXIT):
            tap("DOWN")
        tap("A"); tap("LEFT"); tap("A"); run(core, 60)
        press_start(core); run(core, 10)
        press_start(core); run(core, 12)
        press_start(core); run(core, 30)
        if hud() != "STATS":
            failures.append("el HUD elegido se pierde al empezar otra partida")
        else:
            print("  y el HUD que elegiste sigue siendo el tuyo")

    failures += pause_menu_visible(rom_path)
    failures += pause_resume_music(rom_path)

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el menu de pausa cambia la musica, sale preguntando, y se queda.")
    return 0


def pause_menu_visible(rom_path):
    """EL MAPA DE TILES NO ES LO QUE SE VE, Y AQUI ESO IMPORTA.

    Todo lo que este fichero comprueba del menu de pausa lo lee del mapa de
    tiles, y el mapa mintio: durante un rato el menu estaba escrito entero y
    en pantalla le faltaban el titulo PAUSE, la linea MUSIC y el borde de
    arriba de la caja. La causa no era el dibujo sino el RELOJ -- partir
    gba/main.c en cinco ficheros le quito al compilador el inline entre ellos,
    el frame crecio un 5% y el dibujado empezo a pasarse del blanco vertical.
    Pasarse no se ve como lentitud: se ve como que falta la PARTE DE ARRIBA de
    lo ultimo que se dibuja, porque el haz ya ha pasado por esas lineas cuando
    se escriben.

    Asi que esto mira PIXELES. La caja se dibuja la ultima de todo el frame,
    de modo que es el canario: si su fila de arriba y su titulo llegan a la
    pantalla, el dibujado cabe en el blanco.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=16):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    run(core, 40)
    press_start(core); run(core, 30)
    tap("L", "R"); run(core, 20)            # el acorde, en el menu
    press_start(core); run(core, 24)
    press_start(core); run(core, 90)
    tap("START"); run(core, 60)
    rows = pixels(core if False else screen)

    def lit(ty, tx0, tx1):
        return sum(1 for y in range(ty * TILE, (ty + 1) * TILE)
                   for x in range(tx0 * TILE, tx1 * TILE)
                   if rows[y][x] != (0, 0, 0))

    # Las filas de la caja, de arriba abajo: el borde, PAUSE, MUSIC, el nombre
    # de la cancion, EXIT y el borde de abajo. Ninguna puede salir vacia.
    want = ((PMENU_TY, "el borde de arriba de la caja"),
            (PMENU_TY + 2, "el titulo PAUSE"),
            (PMENU_TY + 4, "la linea MUSIC"),
            (PMENU_TY + 5, "el nombre de la cancion"),
            (PMENU_TY + 7, "la linea EXIT"),
            (PMENU_TY + PMENU_H - 1, "el borde de abajo de la caja"))
    for ty, what in want:
        n = lit(ty, PMENU_TX + 2, PMENU_TX + PMENU_W_T - 2)
        if n < 20:
            failures.append(f"{what} no llega a la pantalla ({n} pixeles "
                             f"encendidos en la fila {ty}): el dibujado se "
                             f"esta pasando del blanco vertical")
        else:
            print(f"  {what}: {n} pixeles en pantalla")
    del core, screen
    return failures


def pause_resume_music(rom_path):
    """UNA PAUSA SUSPENDE LA CANCION; NO LA REBOBINA.

    Las canciones del cartucho ya lo hacian solas: pausar manda MUSIC_SUSPEND
    y reanudar MUSIC_RESUME, y el motor vuelve donde estaba. Las dos entradas a
    mano tienen su propio secuenciador, y la salida de la pausa llamaba a
    handtune_start, que devuelve las dos voces al primer compas: Korobeiniki y
    Katiuska empezaban de nuevo tras cada pausa y tras cada visita a la linea
    MUSIC del menu, que es justo lo que se reporto.

    No se compara frame contra frame -- el retardo al reanudar es variable y
    eso derrota cualquier comparacion sin alinear. Se graba la melodia UNA vez
    desde una partida limpia como lista de notas, y se le pregunta al caso con
    pausa por que entrada de esa lista sigue: continuar es la nota siguiente a
    la ultima oida, reiniciar es la entrada cero.
    """
    failures = []
    for want in ("KOROBEINIKI", "KATIUSKA"):
        got = tune_notes(rom_path, want, pause_at=None, frames=1400)
        if got is None:
            failures.append(f"no se puede elegir {want} en LEVEL SETTINGS")
            continue
        melody = got
        heard, after = tune_notes(rom_path, want, pause_at=600, frames=400)
        n = len(heard)
        if heard != melody[:n]:
            failures.append(f"{want}: lo oido antes de pausar no sigue a la "
                             f"melodia de referencia")
            continue
        # La nota que sonaba al caer la pausa se sostiene a traves de ella, asi
        # que una continuacion puede retomar una entrada a un lado u otro de
        # donde se quedo la cuenta.
        at = [i for i in range(len(melody) - 5) if melody[i:i + 6] == after[:6]]
        if not at:
            failures.append(f"{want}: al reanudar suena algo que no esta en la "
                             f"melodia: {after[:6]}")
        elif at == [0]:
            failures.append(f"{want}: al reanudar la cancion EMPIEZA DE NUEVO")
        elif not any(abs(i - n) <= 2 for i in at):
            failures.append(f"{want}: al reanudar salta a la nota {at}, y se "
                             f"habia quedado en la {n}")
        else:
            print(f"  {want} se reanuda en la nota {min(at, key=lambda i: abs(i - n))} "
                   f"de {len(melody)}, donde la dejo la pausa")
    return failures


def tune_notes(rom_path, want, pause_at, frames):
    """Las notas que suenan, una entrada por nota y no por frame.

    Con pause_at a None devuelve la lista entera de una partida sin tocar.
    Con un numero de frames devuelve (lo oido antes de pausar, lo oido tras
    reanudar), con la pausa echada en ese frame.
    """
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def notes(n):
        out, last = [], None
        for _ in range(n):
            core.run_frame()
            p = core.memory.u16[REG_SOUND1CNT_X] & 0x7FF
            if p and p != last:
                out.append(p)
            last = p
        return out

    run(core, 40)
    tap("START"); run(core, 20)
    tap("L", "R"); run(core, 20)         # destapa las canciones extra
    tap("START"); run(core, 20)
    tap("SELECT"); tap("SELECT")          # el cursor sobre MUSIC
    for _ in range(MUSIC_COUNT_MAX):
        if want in tilemap_text(core, MUSIC_ROW):
            break
        tap("RIGHT")
    if want not in tilemap_text(core, MUSIC_ROW):
        return None
    tap("START"); run(core, 30)
    if pause_at is None:
        return notes(frames)
    heard = notes(pause_at)
    tap("START", settle=30)               # pausa
    run(core, 120)                        # un rato sobre el cartel
    tap("START", settle=30)               # reanuda
    return heard, notes(frames)


def quit_audio_check(rom_path):
    """SALIR POR EL MENU DE PAUSA NO PUEDE DEJAR LA MAQUINA MUDA.

    Pausar manda el MUSIC_SUSPEND del cartucho, que no es un silencio de la
    musica sino una MORDAZA sobre el motor entero: tambien calla los efectos, y
    lo unico que la levanta es MUSIC_RESUME. Toda salida normal de la pausa
    pasa por pauseOrUnpause y lo manda; la del menu secreto desmonta la partida
    por debajo del cartel y no pasaba por ahi, asi que el motor se quedaba
    amordazado para el resto de la sesion. Sonaba exactamente asi: ni las
    piezas al caer, ni la musiquita de game over, ni el blip de los menus, ni
    el tema al volver al titulo, hasta apagar la consola.

    Asi que esto no mira una pantalla: recorre el camino del jugador -- una
    partida en WITH COMPUTER, pausa, L+R, EXIT, SI -- y cuenta frames con algun
    canal sonando en cada sitio al que lleva.
    """
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def heard(frames):
        n = 0
        for _ in range(frames):
            core.run_frame()
            if sound_state(core)["activos"]:
                n += 1
        return n

    run(core, 8)
    tap("START"); run(core, 10)            # titulo -> GAME SELECT
    tap("L", "R")                           # el acorde vive aqui ahora
    for _ in range(4):                      # -> WITH COMPUTER
        tap("DOWN")
    tap("START"); run(core, 12)             # -> LEVEL SETTINGS
    for _ in range(2):                      # el cursor hasta MUSIC
        tap("DOWN")
    tap("RIGHT")                            # NO MUSIC -> LOGINSKA
    tap("START"); run(core, 30)             # -> a jugar

    playing = heard(120)
    tap("START"); run(core, 10)             # pausa, que ya es el menu
    tap("DOWN")                             # MUSIC -> EXIT
    tap("A")                                # -> la pregunta
    tap("RIGHT")                            # NO -> YES
    tap("A"); run(core, 40)                 # fuera

    title = heard(300)
    tap("START"); run(core, 30)             # -> GAME SELECT
    core.set_keys(KEYS["DOWN"])
    blip = heard(4)
    core.set_keys()
    blip += heard(20)
    tap("START"); run(core, 20)             # -> LEVEL SETTINGS
    for _ in range(2):
        tap("DOWN")
    tap("RIGHT")
    tap("START"); run(core, 30)             # -> a jugar otra vez
    again = heard(180)

    print(f"  jugando antes de salir:   {playing:3d}/120 frames con sonido")
    print(f"  el titulo al volver:      {title:3d}/300")
    print(f"  el blip del cursor:       {blip:3d}/24")
    print(f"  la siguiente partida:     {again:3d}/180")

    failures = []
    if playing < 20:
        failures.append("no habia sonido antes de salir; la medida no prueba nada")
    if title < 60:
        failures.append("el titulo vuelve mudo despues de salir por el menu de pausa")
    if blip < 2:
        failures.append("los menus pierden su blip despues de salir por el menu de pausa")
    if again < 40:
        failures.append("la siguiente partida es muda despues de salir por el menu de pausa")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: salir por el menu de pausa deja el motor de sonido como estaba.")
    return 0


def loans_check(rom_path):
    """LOS BANCOS QUE UNA SKIN TIENE PRESTADOS TIENEN QUE DEVOLVERSE.

    Tres paletas de una skin son PRESTAMOS de las cuatro del titulo -- la del
    logo de los menus, la de las tiradas del histograma y la del cartel de
    GAME OVER -- porque nunca hay un titulo y un tablero en pantalla a la vez.
    Eso solo funciona si cada lado las recupera al entrar, y durante un tiempo
    ninguno de los dos lo hacia:

      * el titulo instalaba sus cuatro bancos solo al CAMBIAR de skin, asi que
        volver de los menus lo dibujaba con lo que los menus habian dejado --
        el marco gris de la pantalla con licencia de Nintendo volvia con el
        borde de arriba y el de la derecha azules;
      * y apply_skin se saltaba el trabajo entero cuando la skin ya estaba
        cargada, asi que una vez arreglado lo anterior, la segunda visita a
        los menus dibujaba el logo con los colores del titulo.

    Asi que esto no mira una pantalla: da la vuelta entera -- titulo, menus,
    titulo, menus, partida, titulo -- y compara los dieciseis colores de los
    cuatro bancos del titulo con los que tenia la primera vez.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=16):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def title_banks():
        return [core.memory.u16[PALETTE_ADDR + ((TITLE_PAL_BASE + b) * 16 + i) * 2]
                for b in range(4) for i in range(4)]

    run(core, 40)
    tap("L", "R"); run(core, 24)          # una skin puesta
    first = title_banks()
    if not any(first):
        print("SALTADO: esta ROM no trae skins")
        return 0

    tap("START"); run(core, 30)            # -> GAME SELECT
    menu_first = title_banks()
    tap("B"); run(core, 60)                # -> titulo
    if title_banks() != first:
        failures.append("el titulo no recupera sus bancos al volver de los menus")
    else:
        print("  el titulo recupera sus cuatro bancos al volver de los menus")

    tap("START"); run(core, 30)            # -> GAME SELECT otra vez
    if title_banks() != menu_first:
        failures.append("la segunda visita a los menus no repone los prestamos: "
                         "apply_skin se los salta por tener la skin ya cargada")
    else:
        print("  ...y los menus reponen los suyos en cada visita")

    # ...y lo mismo pasando por una partida, que pide prestados otros dos.
    # EL ACORDE, AQUI: el del titulo cambia la skin y no destapa nada mas, asi
    # que sin este el cartel de pausa es un cartel y no hay por donde salir.
    tap("L", "R"); run(core, 20)
    press_start(core); run(core, 24)
    press_start(core); run(core, 60)       # -> jugando
    run(core, 60)
    tap("START"); run(core, 20)            # pausa, que dibuja el cartel
    tap("L", "R"); run(core, 20)           # ...y el acorde lo vuelve menu
    # EXIT, y SI. START aqui seria reanudar, no salir.
    for _ in range(3):
        if ">" in tilemap_text(core, PM_EXIT, PM_L, PM_R):
            break
        tap("DOWN")
    tap("A")                                # abre la pregunta
    tap("LEFT")                             # SI
    tap("A"); run(core, 120)
    if title_banks() != first:
        failures.append("el titulo no recupera sus bancos al volver de una partida")
    else:
        print("  ...y tambien al volver de una partida")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: los bancos prestados vuelven a su sitio en los dos sentidos.")
    return 0


def onscreen_check(rom_path):
    """LO QUE ESTA ESCRITO TIENE QUE LLEGAR A LA PANTALLA.

    Todas las comprobaciones de este fichero leen el MAPA DE TILES, y el mapa
    puede decir la verdad mientras la pantalla miente. Paso: al partir
    gba/main.c en cinco ficheros el compilador perdio el inline entre ellos,
    el dibujado de un frame crecio un cinco por ciento y empezo a pasarse del
    blanco vertical -- y pasarse no se ve como lentitud, se ve como que falta
    la PARTE DE ARRIBA de lo ultimo que se dibuja, porque el haz ya ha pasado
    por esas lineas cuando se escriben. Faltaban el titulo del menu de pausa y
    el borde de arriba de su caja, con el mapa entero y correcto.

    Asi que esto cruza las dos cosas en las pantallas que importan: para cada
    fila de tiles suma la TINTA que el mapa promete -- mirando el arte de cada
    tile en las cuatro capas -- y la compara con los pixeles encendidos de esa
    fila. Una fila que promete tinta y no enciende nada es una fila que no
    llego a tiempo.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=16):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    # El arte de cada tile, una vez: cuantos pixeles suyos no son el color 0.
    def tile_ink(cache, tile):
        if tile in cache:
            return cache[tile]
        n = 0
        for i in range(16):
            v = core.memory.u16[0x06000000 + tile * 32 + i * 2]
            for s in range(0, 16, 4):
                if (v >> s) & 0xF:
                    n += 1
        cache[tile] = n
        return n

    def audit(name):
        cache = {}
        rows = pixels(screen)
        # CELDA A CELDA, no fila a fila: una fila con las paredes de una caja
        # a los lados enciende pixeles aunque su contenido no llegue, y eso es
        # justo lo que hay que cazar.
        bad = []
        for ty in range(SCREEN_H // TILE):
            for tx in range(SCREEN_TW_TILES):
                promised = 0
                for sb in (28, 29, 30, 31):
                    base = 0x06000000 + sb * 0x800
                    t = core.memory.u16[base + ((ty * 32 + tx) * 2)] & 0x3FF
                    if t:
                        promised = max(promised, tile_ink(cache, t))
                if promised < ONSCREEN_MIN_INK:
                    continue
                # Las capas van tres pixeles a un lado y dos al otro, asi que
                # se mira la celda y su vecindad inmediata.
                lit = sum(1 for y in range(ty * TILE, (ty + 1) * TILE + 2)
                          for x in range(tx * TILE - 3, (tx + 1) * TILE + 3)
                          if 0 <= y < SCREEN_H and 0 <= x < SCREEN_W
                          and rows[y][x] != (0, 0, 0))
                if lit == 0:
                    bad.append((ty, tx))
        if bad:
            failures.append(f"{name}: {len(bad)} celdas con tiles escritos no "
                             f"encienden un solo pixel, la primera en "
                             f"{bad[0]}")
        else:
            print(f"  {name}: todo lo escrito esta en pantalla")

    run(core, 40)
    audit("el titulo")
    press_start(core); run(core, 30)
    tap("L", "R"); run(core, 20)
    audit("GAME SELECT")
    press_start(core); run(core, 30)
    audit("LEVEL SETTINGS")
    press_start(core); run(core, 90)
    core.set_keys(KEYS["DOWN"]); run(core, 200); core.set_keys(); run(core, 30)
    audit("una partida")
    tap("START"); run(core, 40)
    audit("el menu de pausa")
    tap("START"); run(core, 20)
    # ...y el game over, que dibuja su propia placa encima de todo.
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is not None:
        addr = base + off["field"]
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(1, TENGEN_PF_WIDTH - 1):
                core.memory.u8[addr + row * TENGEN_PF_WIDTH + col] = (
                    0 if col == 5 else CELL_BLOCK)
        run(core, 240)
        audit("el game over")
    del core, screen

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: lo que el mapa promete, la pantalla lo dibuja.")
    return 0


def effects_check(rom_path):
    """LOS DOS RUIDOS DE UN MENU, QUE NO SON LOS MISMOS EN LAS CUATRO ROMS.

    Medido en los volcados de verdad, apuntando cada escritura a $4000-$4015
    mientras se movia el cursor y mientras cambiaba la pantalla:

      cursor    release      pulso 2, periodo $4B7 (unos 93Hz) con el barrido
                             del chip doblandolo hacia abajo once frames
                A, B y C     pulso 2, periodo $021 (unos 3.3kHz), sin barrido,
                             y se acaba en ocho
      pantalla  release      pulso 1, una nota barrida
                proto_a      nada en absoluto
                B y C        pulso 1, un trino que sube y que barren a mano,
                             reescribiendo el periodo cada frame

    El del release lo toca el motor del cartucho, que va dentro de esta ROM.
    Los de los prototipos no: se capturan de sus volcados como los registros
    que escriben y se reproducen por la misma conversion (nes_audio_effect).

    Lo que se comprueba aqui es lo que llega al chip: que el tic del cursor
    con skin esta CINCO OCTAVAS por encima del que suena sin ella, que la
    pantalla suena distinta, que un prototipo que contesta con silencio
    contesta con silencio, y que ninguno de los dos se queda sonando.
    """
    failures = []
    R1X, R1H = 0x04000064, 0x04000062
    R2H, R2L = 0x0400006C, 0x04000068

    def listen(skin, button):
        """(frecuencias oidas, volumenes frame a frame) de los dos pulsos."""
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        _ = screen

        def tap(*names, hold=4, settle=12):
            core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
            core.set_keys(); run(core, settle)

        run(core, 40)
        if skin:
            tap("L", "R"); run(core, 20)
            for _ in range(skin - 1):
                tap("R"); run(core, 20)
        press_start(core); run(core, 30)      # -> GAME SELECT
        core.set_keys(KEYS[button]); run(core, 4); core.set_keys()
        f1, f2, v1, v2 = set(), set(), [], []
        for _ in range(EFFECT_WATCH_FRAMES):
            core.run_frame()
            v1.append(core.memory.u16[R1H] >> 12)
            v2.append(core.memory.u16[R2L] >> 12)
            if v1[-1]:
                f1.add(core.memory.u16[R1X] & 0x7FF)
            if v2[-1]:
                f2.add(core.memory.u16[R2H] & 0x7FF)
        del core, screen
        return f1, f2, v1, v2

    # EL TIC DEL CURSOR. El del release es grave y el de los prototipos agudo,
    # y en la GBA una frecuencia mas alta es un registro MAS GRANDE (R = 2048 -
    # 131072/f), asi que el numero del prototipo tiene que ser mayor.
    _f1, rel_f2, _v1, rel_v2 = listen(0, "DOWN")
    if not rel_f2:
        failures.append("sin skin el cursor no hace ruido")
    _f1, pro_f2, _v1, pro_v2 = listen(1, "DOWN")
    if not pro_f2:
        failures.append("con skin el cursor no hace ruido")
    if rel_f2 and pro_f2:
        rel, pro = max(rel_f2), max(pro_f2)
        if pro <= rel:
            failures.append(f"el tic del cursor con skin ({pro}) no es mas agudo "
                             f"que el del cartucho ({rel})")
        else:
            print(f"  el tic del cursor sube de {rel} a {pro}: el del prototipo "
                   f"esta cinco octavas por encima")
    # ...y NINGUNO se queda sonando: los dos acaban en silencio.
    for name, vols in (("release", rel_v2), ("prototipo", pro_v2)):
        if vols and vols[-1]:
            failures.append(f"el tic del cursor del {name} se queda sonando "
                             f"(volumen {vols[-1]:X} al final de "
                             f"{EFFECT_WATCH_FRAMES} frames)")
        else:
            print(f"  ...y el del {name} se apaga solo")

    # EL CAMBIO DE PANTALLA. proto_a contesta con silencio y los otros dos con
    # un trino que sube; el release, con una nota barrida.
    rel_f1, _f2, rel_v1, _v2 = listen(0, "START")
    a_f1, _f2, a_v1, _v2 = listen(1, "START")
    b_f1, _f2, b_v1, _v2 = listen(2, "START")
    if not rel_f1:
        failures.append("sin skin cambiar de pantalla no hace ruido")
    else:
        print(f"  el cambio de pantalla del cartucho suena en {sorted(rel_f1)}")
    if any(a_v1):
        failures.append(f"proto_a deberia cambiar de pantalla EN SILENCIO y "
                         f"suena (volumenes {a_v1})")
    else:
        print("  proto_a cambia de pantalla sin decir nada, como su volcado")
    if len(b_f1) < 3:
        failures.append(f"el trino de proto_b no barre: solo {sorted(b_f1)}")
    elif b_f1 == rel_f1:
        failures.append("el cambio de pantalla de proto_b suena igual que el "
                         "del cartucho")
    else:
        print(f"  ...y proto_b con un trino de {min(b_f1)} a {max(b_f1)}, "
               f"{len(b_f1)} pasos barridos a mano")
    # ...Y NINGUNO SE QUEDA SONANDO. El de proto_c gastaba justo el ultimo
    # frame de la ventana de captura, asi que su apagado se quedaba fuera y el
    # trino sonaba para siempre; la ventana es mas ancha y el extractor se
    # niega ahora a capturar un efecto que no ha terminado dentro de ella.
    c_f1, _f2, c_v1, _v2 = listen(3, "START")
    for name, vols in (("proto_b", b_v1), ("proto_c", c_v1)):
        if vols and vols[-1]:
            failures.append(f"el trino de {name} se queda sonando (volumen "
                             f"{vols[-1]:X} tras {EFFECT_WATCH_FRAMES} frames)")
        else:
            print(f"  ...y el trino de {name} se apaga solo")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada construccion hace en sus menus el ruido que hace la suya.")
    return 0


def stats_check(rom_path):
    """EL HISTOGRAMA Y EL CARTEL, QUE CADA CONSTRUCCION DIBUJA A SU MANERA.

    El release comparte UNA barra de ocho pasos entre las siete columnas y
    pone debajo una tira de iconos para decir cual es cual. Un prototipo no
    tiene tira: le da a cada pieza su propia tirada de ocho, pintada con el
    patron de bloque de esa pieza, y el patron es la etiqueta. Asi que bajo
    skin las dos filas de la tira son dos filas mas de barra, las siete
    columnas tienen que dibujarse con tiles DISTINTOS entre si, y ninguna
    puede quedar vacia teniendo cuenta.

    Y el cartel de GAME OVER: rojo en el release, azul en los tres
    prototipos. El marco viaja como arte en las ranuras del release, asi que
    lo que se comprueba aqui es que cambia -- y que NEXT no cambia con el,
    que es lo que paso al tomar prestado el banco 3 entero: los prototipos
    tienen su NEXT tan rojo como el release.
    """
    failures = []
    # La capa del histograma es la suya propia (SCREENBLOCK_HISTOGRAM en
    # gba/port.h), asi que no hace falta saber donde cae la caja: se barre
    # entera y lo que haya escrito ES el histograma.
    hist = 0x06000000 + STATS_SCREENBLOCK * 0x800
    # Cuentas bien distintas, ninguna nula, una por pieza.
    counts = (37, 5, 61, 12, 84, 29, 50)
    seen = {}
    for skin in (0, 1):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        _ = screen

        def tap(*names, hold=4, settle=12):
            core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
            core.set_keys(); run(core, settle)

        run(core, 40)
        if skin:
            tap("L", "R"); run(core, 20)
        press_start(core); run(core, 24)
        press_start(core); run(core, 24)
        press_start(core); run(core, 60)
        tap("SELECT"); run(core, 30)          # al HUD de estadisticas
        off = game_offsets(rom_path)
        base, why = game_state_address(rom_path)
        if base is None:
            print(f"SALTADO: {why}")
            return 0
        # piece_stats es uint8_t[8] y la entrada 0 es TT_NONE.
        for i, n in enumerate(counts):
            core.memory.u8[base + off["stats"] + 1 + i] = n
        run(core, 30)

        used = {}
        for ty in range(32):
            for tx in range(32):
                e = core.memory.u16[hist + ((ty * 32 + tx) * 2)]
                if e & 0x3FF:
                    used.setdefault(tx, []).append((ty, e))
        cols = [sorted(v) for _k, v in sorted(used.items())]
        if len(cols) != STATS_COLUMNS:
            failures.append(f"{'release' if not skin else 'prototipo'}: el "
                             f"histograma ocupa {len(cols)} columnas, no "
                             f"{STATS_COLUMNS}")
            del core, screen
            continue
        name = "release" if not skin else "prototipo"
        # Las alturas siguen a las cuentas: mas piezas, barra mas alta.
        order = sorted(range(len(counts)), key=lambda i: counts[i])
        heights = [len(c) - (STATS_ICON_ROWS if not skin else 0) for c in cols]
        if any(heights[a] > heights[b] for a, b in zip(order, order[1:])):
            failures.append(f"{name}: las alturas {heights} no siguen al orden "
                             f"de las cuentas {counts}")
        else:
            print(f"  {name}: siete barras, alturas {heights} para {counts}")
        seen[skin] = cols
        # El cartel: se entierra el tablero y se mira de que color sale.
        addr = base + off["field"]
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(1, TENGEN_PF_WIDTH - 1):
                core.memory.u8[addr + row * TENGEN_PF_WIDTH + col] = (
                    0 if col == 5 else CELL_BLOCK)
        run(core, 240)
        # El cartel se busca por sus propios tiles: las cuatro esquinas de la
        # caja son $29 $2B $3A $3C en las dos construcciones -- lo que cambia
        # es el dibujo y el banco, no el numero.
        plaque = [core.memory.u16[SCREENBLOCK_ADDR + (i * 2)]
                  for i in range(32 * 20)
                  if (core.memory.u16[SCREENBLOCK_ADDR + (i * 2)] & 0x3FF)
                  in PLAQUE_CORNERS]
        if len(plaque) < len(PLAQUE_CORNERS):
            failures.append(f"{name}: no sale el cartel de GAME OVER")
        else:
            seen[("plaque", skin)] = plaque
            print(f"  {name}: el cartel de GAME OVER esta puesto")
        # ...y la CABECERA sigue en su banco. NEXT, SCORE, LINES, LEVEL y HIGH
        # no son ASCII sino arte propio, a partir de HUD_LABEL_TILE_BASE, asi
        # que se buscan por numero de tile y se anotan sus bancos.
        banks = {core.memory.u16[SCREENBLOCK_ADDR + (i * 2)] >> 12
                 for i in range(32 * 20)
                 if HUD_LABEL_TILE_BASE <= (core.memory.u16[SCREENBLOCK_ADDR +
                                                            (i * 2)] & 0x3FF)
                 < HUD_LABEL_TILE_END}
        if not banks:
            failures.append(f"{name}: no se encuentran las etiquetas del HUD")
        seen[("labels", skin)] = banks
        del core, screen

    if 0 in seen and 1 in seen and not failures:
        # LAS DOS FILAS DE LA TIRA DE ICONOS SON BARRA BAJO SKIN. En el
        # release las dos de abajo de cada columna son el icono, asi que la
        # barra es lo que queda; en un prototipo no hay icono y todo es barra.
        # Las dos llegan igual de abajo -- al suelo de la caja -- y es la barra
        # la que crece.
        rel_floor = max(ty for c in seen[0] for ty, _e in c)
        pro_floor = max(ty for c in seen[1] for ty, _e in c)
        if rel_floor != pro_floor:
            failures.append(f"el histograma no llega igual de abajo con skin "
                             f"({pro_floor}) que sin ella ({rel_floor})")
        rel = max(len(c) for c in seen[0]) - STATS_ICON_ROWS
        pro = max(len(c) for c in seen[1])
        if pro <= rel:
            failures.append(f"bajo skin la barra mas alta mide {pro} filas y sin "
                             f"skin {rel}: la tira de iconos no ha dejado su sitio")
        else:
            print(f"  la barra mas alta pasa de {rel} filas a {pro}: las dos de "
                   "la tira de iconos son barra en el prototipo")
        # ...Y CADA COLUMNA CON SU PROPIA TIRADA bajo skin, que es la
        # diferencia entera: los tiles de una columna no aparecen en ninguna
        # otra. Sin skin es justo al reves -- las siete comparten una sola
        # tirada de ocho pasos, asi que todos sus tiles caben en una ventana
        # de ocho.
        pro_sets = [{e & 0x3FF for _ty, e in c} for c in seen[1]]
        shared = [(i, j) for i in range(len(pro_sets))
                  for j in range(i + 1, len(pro_sets))
                  if pro_sets[i] & pro_sets[j]]
        if shared:
            failures.append(f"bajo skin las columnas {shared} comparten tiles de "
                             f"barra: no llevan el patron de su pieza")
        else:
            print(f"  ...y cada columna con el patron de su pieza: "
                   f"{len(pro_sets)} tiradas sin un tile en comun")
        rel_bars = {e & 0x3FF for c in seen[0] for _ty, e in c[:-STATS_ICON_ROWS]}
        if max(rel_bars) - min(rel_bars) >= STATS_RUN_STEPS:
            failures.append(f"sin skin las columnas deberian compartir una sola "
                             f"tirada y usan {sorted(hex(t) for t in rel_bars)}")

    if ("plaque", 0) in seen and ("plaque", 1) in seen:
        rel = {e >> 12 for e in seen[("plaque", 0)] if e & 0x3FF}
        pro = {e >> 12 for e in seen[("plaque", 1)] if e & 0x3FF}
        if rel == pro:
            failures.append(f"el cartel de GAME OVER usa el mismo banco con y "
                             f"sin skin ({sorted(rel)}): no cambia de color")
        else:
            print(f"  el cartel cambia de paleta con la skin: banco "
                   f"{sorted(rel)} -> {sorted(pro)}")
    if ("labels", 0) in seen and ("labels", 1) in seen:
        if seen[("labels", 0)] != seen[("labels", 1)]:
            failures.append(f"la cabecera cambia de banco con la skin "
                             f"({sorted(seen[('labels', 0)])} -> "
                             f"{sorted(seen[('labels', 1)])}): el cartel se ha "
                             f"llevado NEXT y los contadores con el")
        else:
            print(f"  ...y la cabecera se queda en el banco "
                   f"{sorted(seen[('labels', 0)])}: el cartel no la arrastra")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el histograma y el cartel son los de cada construccion.")
    return 0


def gameover_check(rom_path):
    """THE WAY OUT. Every mode has to end, and end where the player left.

    This is the check the twenty-one before it did not do: they all started
    games and none of them ever lost one. What a lost game has to do is

      * stop — in coop that means BOTH players, because there is one board and
        the cartridge kills both flags at once (main.asm.txt:83D4-83DD), and
        against the computer it means the computer too;
      * stay stopped — no piece of anybody's moves after the plaque is up;
      * let go — Start goes back to the title from the mode's own screen; and
      * take the HUD swap with it: SELECT is a thing you do to a game in play.

    It buries the board by hand rather than stacking pieces for ten minutes:
    every row solid but one column, so nothing can clear and the next piece
    tops out where it stands. That is the same `game_active` path the player
    walked into, reached in a second instead of a quarter of an hour.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH

    def player(core, slot, key):
        return core.memory.u8[base + off[key] + slot * off["stride"]]

    def board_cells(core, which):
        addr = base + off["field"] + which * PF
        return sum(1 for i in range(PF) if core.memory.u8[addr + i] == CELL_BLOCK)

    def bury(core, which, coop):
        """Solid everywhere but one column, so no row can ever complete."""
        addr = base + off["field"] + which * PF
        gap = 5
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(TENGEN_PF_WIDTH):
                edge = not coop and col in (0, TENGEN_PF_WIDTH - 1)
                value = CELL_WALL if edge else (0 if col == gap else CELL_BLOCK)
                core.memory.u8[addr + row * TENGEN_PF_WIDTH + col] = value

    def enter(entry):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        run(core, 8)
        press_start(core)               # title -> GAME SELECT
        run(core, 10)
        for _ in range(entry):
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)   # -> LEVEL SETTINGS
        press_start(core); run(core, 30)   # -> play
        return core, screen

    def title_face(core):
        """Three rows of the title's own tilemap, which no other screen has."""
        return tuple(tilemap_text(core, r) for r in (4, 6, 8))

    reference, _ref_screen = load(rom_path)
    run(reference, 40)
    face = title_face(reference)

    for name, entry, coop in (("1 PLAYER", 0, False),
                               ("WITH COMPUTER", 4, True),
                               ("VERSUS COMPUTER", 3, False)):
        core, screen = enter(entry)
        bury(core, 0, coop)
        run(core, 240)

        if player(core, 0, "active"):
            failures.append(f"{name}: el tablero del jugador no muere aun enterrado")
            continue
        if coop and player(core, 1, "active"):
            failures.append(f"{name}: el jugador 1 murio y el 2 sigue vivo "
                             "sobre el mismo tablero")
            continue

        # Nothing may move behind the plaque — not the computer, not anybody.
        # UNDER THE PLAQUE'S OWN CLOCK, though: it now leaves for the high
        # scores after 498 frames on its own (GAMEOVER_HOLD_FRAMES, and the
        # cartridge's own number), so this has to say its piece before then.
        # 240 + 150 + the chord below is comfortably inside it whatever frame
        # the board actually died on.
        before = (board_cells(core, 0), board_cells(core, 1))
        run(core, 150)
        after = (board_cells(core, 0), board_cells(core, 1))
        if after != before:
            failures.append(f"{name}: despues del game over se siguio jugando "
                             f"({before} -> {after})")
            continue

        # SELECT is for a game in play. Read the far right column, which is
        # the box the banner would take over.
        hud = tuple(tilemap_text(core, r, 22, 30) for r in range(4, 12))
        core.set_keys(KEYS["SELECT"]); run(core, 6)
        core.set_keys(); run(core, 12)
        if tuple(tilemap_text(core, r, 22, 30) for r in range(4, 12)) != hud:
            failures.append(f"{name}: SELECT todavia cambia el HUD despues del game over")
            continue

        # The cartridge's road out of a game runs through its HIGH SCORES
        # page and only then back to the title (main.asm.txt:2643-2675).
        press_start(core); run(core, 40)
        if "HIGH SCORES" not in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
            failures.append(f"{name}: START no lleva a la tabla de records "
                             "tras el game over")
            continue
        press_start(core); run(core, 40)
        if title_face(core) != face:
            failures.append(f"{name}: no se vuelve al titulo desde la tabla")
            continue
        print(f"  {name}: el tablero muere, todo se para, y START lleva a la "
               "tabla y al titulo")

    # ...AND NEITHER PAGE NEEDS A BUTTON. The cartridge's game over is a
    # countdown, not a prompt: $F9 goes into player1FallTimer as well as into
    # gameState, one decrement every other frame takes 498 to reach zero, and
    # the high scores then underflow the same byte to 255 and hold for 1020
    # at one decrement every fourth. Measured on the cartridge by burying its
    # field and watching gameState: $F9 at frame 48, $F8 at 546, the title at
    # 1571. This port waited for a button on the first and let go of the
    # second after 300 frames.
    #
    # Counted in blocks of 30 frames, so the tolerance below is a block and a
    # half either way rather than a promise about a single frame.
    core, _screen = enter(0)
    bury(core, 0, False)
    dead_at = None
    to_table = to_title = None
    for tick in range(120):                   # 3600 frames, sixty seconds
        run(core, 30)
        if dead_at is None:
            if not player(core, 0, "active"):
                dead_at = tick * 30
            continue
        if to_table is None:
            if "HIGH SCORES" in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
                to_table = tick * 30 - dead_at
            continue
        if title_face(core) == face:
            to_title = tick * 30 - dead_at - to_table
            break

    for what, got, want in (("el game over", to_table, 498),
                             ("la tabla", to_title, 1020)):
        if got is None:
            failures.append(f"{what} no se va solo, hay que pulsar algo")
        elif abs(got - want) > 45:
            failures.append(f"{what} dura {got} frames y el cartucho {want}")
        else:
            print(f"  {what} se va solo tras {got} frames "
                  f"(el cartucho, {want})")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada modo termina, se para del todo, y suelta al jugador.")
    return 0


def falling_piece_check(rom_path):
    """THE PARTNER'S PIECE HAS TO BE ON THE SCREEN WHILE IT FALLS.

    Coop is one board with two pieces coming down it, and the renderer used to
    draw only the one belonging to the player it was showing. On a shared
    board that is the difference between watching somebody play and watching
    pieces appear on the floor out of nowhere — which is exactly what WITH
    COMPUTER looked like.

    Counted rather than eyeballed: the tiles drawn over the board minus the
    cells the playfield buffer actually holds are the falling pieces, and two
    pieces are eight cells.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    run(core, 8)
    press_start(core); run(core, 10)
    for _ in range(4):              # GAME SELECT -> WITH COMPUTER
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 30)

    best = 0
    for _ in range(120):
        run(core, 4)
        drawn = 0
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(TENGEN_PF_WIDTH):
                tile = core.memory.u16[SCREENBLOCK_ADDR +
                                        ((row * 32) + COOP_FIELD_TX + col) * 2] & 0x3FF
                cell = core.memory.u8[base + off["field"] +
                                       row * TENGEN_PF_WIDTH + col]
                if tile and not cell:
                    drawn += 1
        best = max(best, drawn)
        if best >= 8:
            break

    if best < 8:
        print(f"FALLA: solo {best} celdas cayendo sobre el tablero compartido; "
               "una de las dos piezas no se dibuja")
        return 1
    print(f"  {best} celdas cayendo a la vez — las dos piezas de un tablero coop")

    # ...AND IT HAS TO BE WATCHABLE. Drawn is not the same as seen: a piece
    # that waits above the field and then crosses the board in twenty frames
    # is on screen for a tenth of its life, which is what "no veo la caida de
    # sus piezas" was even after both pieces were being drawn. So this
    # measures the fraction of frames the computer's piece spends INSIDE the
    # field, off the piece's own row rather than off the tilemap.
    y_addr = base + off["y"] + off["stride"]
    inside = 0
    frames = 1200
    for _ in range(frames):
        run(core, 1)
        y = core.memory.u8[y_addr]
        if y > 127:
            y -= 256
        if y >= TENGEN_ROM_ROW_ORIGIN:
            inside += 1
    share = inside * 100 // frames
    if share < 60:
        print(f"FALLA: la pieza del ordenador solo esta dentro del campo el "
               f"{share}% del tiempo: no se le ve caer")
        return 1
    print(f"  las dos piezas se dibujan, y la del ordenador esta a la vista "
           f"el {share}% del tiempo")

    # ...AND THEY ARE SOLID TO EACH OTHER. The playfield buffer holds only
    # settled blocks, so without checkCoopCollision (main.asm.txt:1827-1924)
    # the two pieces of a shared board walk straight through each other --
    # which they did. Measured off the SCREEN rather than off the rule: every
    # frame, the cells each falling piece covers, and no cell may be in both.
    #
    # The port draws them in two palette banks so the partner's piece can be
    # told apart, but a cell is a cell: this reads the two pieces' own
    # positions out of the game state and intersects them, which is the same
    # question checkCoopCollision answers and a completely different route to
    # it.
    def piece_cells(slot):
        b = base + slot * off["stride"]
        piece = core.memory.u8[b + off["current"]]
        if not piece:
            return set()
        y = core.memory.u8[b + off["y"]]
        if y > 127:
            y -= 256
        x = core.memory.u8[b + off["x"]]
        if x > 127:
            x -= 256
        orientation = core.memory.u8[b + off["orientation"]] & 3
        bits = ORIENTATION_BITS[piece][orientation]
        return {(y + r, x + c)
                for r in range(4) for c in range(4)
                if bits & (0x8000 >> (r * 4 + c))}

    overlaps = 0
    ghost = None
    for _ in range(2400):
        run(core, 1)
        both = piece_cells(0) & piece_cells(1)
        if both:
            overlaps += 1
            if ghost is None:
                ghost = sorted(both)
    if overlaps:
        print(f"FALLA: las dos piezas del tablero compartido se atraviesan: "
               f"{overlaps} frames con celdas en comun, la primera en {ghost}")
        return 1
    print(f"OK: las dos piezas se dibujan, se ven caer, y no se atraviesan.")
    return 0


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


def credits_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []
    run(core, 8)
    press_start(core)               # title -> game select
    run(core, 20)

    # Long enough for every one of them to come round twice over.
    seen = []
    for _ in range(1600):
        pair = (tilemap_text(core, CREDIT_TY).strip(),
                tilemap_text(core, CREDIT_TY + 1).strip())
        if not seen or pair != seen[-1]:
            seen.append(pair)
        core.run_frame()

    def shown(role, name):
        return any(role in a and name in b for a, b in seen)

    missing = [f"{role} {name}" for role, name in CREDITS if not shown(role, name)]
    if missing:
        failures.append("no aparecen estos creditos del cartucho: "
                         + "; ".join(missing))
    else:
        print(f"  los seis creditos del cartucho salen, uno a uno "
              f"({len(seen)} cambios en 1600 frames)")

    # The invented line is gone: the cartridge spells him PAZHITNOV, on its
    # own level screen, and the port used to print a PAJITNOV of its own.
    if any("PAJITNOV" in a or "PAJITNOV" in b for a, b in seen):
        failures.append("sigue saliendo PAJITNOV, que no es como lo escribe el "
                         "cartucho")

    # ...and nothing of it touches the frame. The bottom braid starts at
    # y=145; the credit's second line rides the lifted layer to clear it.
    rows = pixels(screen)
    braid = None
    for y in range(120, 160):
        lit = sum(1 for x in range(24, 216) if tuple(rows[y][x]) != (0, 0, 0))
        if lit > 180:
            braid = y
            break
    if braid is None:
        failures.append("no encuentro el borde inferior del marco")
    else:
        ink = [y for y in range(120, braid)
               if any(tuple(rows[y][x]) != (0, 0, 0) for x in range(24, 216))]
        if ink and ink[-1] >= braid - 1:
            failures.append(f"el credito toca la greca: tinta hasta y={ink[-1]}, "
                             f"greca en y={braid}")
        else:
            print(f"  y la ultima linea deja aire sobre la greca "
                  f"(tinta hasta y={ink[-1] if ink else '-'}, greca en y={braid})")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: los creditos del cartucho estan, con sus palabras.")
    return 0


# ---------------------------------------------------------------------------
# TETRIS TENGEN XE, which rides the same L+R as the tunes and the pause menu.
#
# The mod is levels 18 and 19 and nothing else — see TENGEN_MAX_LEVEL_XE in
# src/tengen_core.h, where its ten IPS records are accounted for one by one.
# So there are three things to check and the first is the one that matters:
#
#   * before the chord the LEVEL field still stops at 9, which is the
#     cartridge's own menu range;
#   * after it, the field runs 0-19 and wraps there;
#   * and a game started on 19 really starts on 19, which is the flag having
#     reached the core rather than just the menu.
# ---------------------------------------------------------------------------
def xe_check(rom_path):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    def tap(*names, settle=8):
        core.set_keys(*[KEYS[n] for n in names])
        run(core, 4)
        core.set_keys()
        run(core, settle)

    def level_field():
        text = tilemap_text(core, LEVEL_ROW)
        digits = "".join(c for c in text.split("LEVEL", 1)[-1] if c.isdigit())
        return digits

    run(core, 8)
    press_start(core); run(core, 10)
    press_start(core); run(core, 10)          # LEVEL SETTINGS, cursor on LEVEL

    seen = set()
    for _ in range(24):
        seen.add(level_field())
        tap("RIGHT")
    if seen != {str(n) for n in range(10)}:
        failures.append(f"antes del acorde el nivel ofrece {sorted(seen)}, no 0-9")
    else:
        print("  antes del acorde: el nivel llega a 9, como en el cartucho")

    # The chord leaves the cursor on MUSIC — that is what it does on this
    # screen, so the tune it has just uncovered is the one under the arrow.
    # One more DOWN wraps back round to LEVEL.
    tap("L", "R", settle=10)
    tap("DOWN")
    seen = set()
    for _ in range(40):
        seen.add(level_field())
        tap("RIGHT")
    if seen != {str(n) for n in range(20)}:
        failures.append(f"tras el acorde el nivel ofrece {len(seen)} valores, no 20: "
                         f"{sorted(seen, key=int)}")
    else:
        print("  tras el acorde: 0-19, que es todo lo que anade Tetris Tengen XE")

    # ...and it has to be the GAME's level, not just the menu's.
    for _ in range(40):
        if level_field() == "19":
            break
        tap("RIGHT")
    press_start(core); run(core, 30)
    off = game_offsets(rom_path)
    level = core.memory.u8[base + off["level"]]
    if level != 19:
        failures.append(f"la partida empezo en el nivel {level}, no en el 19")
    else:
        print("  ...y la partida arranca de verdad en el nivel 19")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el acorde abre los niveles 18 y 19, y solo eso.")
    return 0


def menu_check(rom_path):
    """LEVEL SETTINGS: three fields, and nothing touching the braid.

    The frame's bottom run starts at y=145, so a line on tile row 17 ends one
    pixel short of it — which is what "START TO PLAY pisa la greca" was. The
    last row with air under it is 16, and this measures that rather than
    trusting a constant.
    """
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    run(core, 8)
    press_start(core)               # title -> game select
    press_start(core)               # -> LEVEL SETTINGS
    run(core, 20)

    failures = []
    for row, want in ((LEVEL_ROW, "LEVEL"), (HANDICAP_ROW, "HANDICAP"),
                       (MUSIC_ROW, "MUSIC"), (16, "PRESS START TO PLAY")):
        if want not in tilemap_text(core, row):
            failures.append(f"la fila {row} no dice {want}: "
                             f"{tilemap_text(core, row)!r}")

    # Where the braid's bottom run actually begins, found by looking for the
    # first scanline the frame fills right across the interior.
    rows = pixels(screen)
    braid_y = None
    for y in range(120, 160):
        if all(rows[y][x] != (0, 0, 0) for x in range(24, 216)):
            braid_y = y
            break
    if braid_y is None:
        failures.append("no se encuentra la greca de abajo")
    else:
        last = max((y for y in range(100, braid_y)
                    if any(rows[y][x] != (0, 0, 0) for x in range(24, 216))),
                   default=None)
        if last is None:
            failures.append("no hay texto en la mitad de abajo de la pantalla")
        elif braid_y - last < 4:
            failures.append(f"el texto llega a y={last} y la greca empieza en "
                             f"y={braid_y}: se tocan")
        else:
            print(f"  la ultima linea acaba en y={last}, la greca empieza en "
                   f"y={braid_y}: {braid_y - last - 1} pixeles de aire")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: LEVEL SETTINGS trae sus tres campos y no pisa la greca.")
    return 0


def braid_check(rom_path):
    core, screen = load(rom_path)
    start_game(core)
    run(core, 20)

    def frame_columns(x0, x1):
        px = pixels(screen)
        return [tuple(px[y][x] for x in range(x0, x1)) for y in range(160)]

    LEFT_FRAME = (64, 80)     # the board's left rope: the left panel's own side
    RIGHT_FRAME = (160, 176)  # ...and its right, which is the right panel's

    box = frame_columns(*RIGHT_FRAME)
    box_left = frame_columns(*LEFT_FRAME)

    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 12)
    banner = frame_columns(*RIGHT_FRAME)

    failures = []
    # The banner takes the whole column, so its top and bottom rows are rope
    # where the box has its lid and floor. Everything BETWEEN them is the same
    # vertical run in both modes and has to match pixel for pixel.
    body = range(16, 144)
    diff = sum(1 for y in body if box[y] != banner[y])
    if diff:
        failures.append(
            f"la greca del campo cambia entre caja y banner: {diff} filas distintas")
    else:
        print("  la greca junto al campo es identica con caja y con banner")

    # And the two panels frame the board from opposite sides, so their runs
    # must be MIRRORS of each other, not copies — that is what makes the pair
    # read as two boxes rather than two copies of the same edge.
    same = sum(1 for y in body if box_left[y] == box[y])
    if same > len(list(body)) // 2:
        failures.append(
            "las dos grecas del campo son iguales; deberian ser espejo la una de la otra")
    else:
        print("  las dos grecas son espejo la una de la otra, como en el cartucho")

    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 12)
    back = frame_columns(*RIGHT_FRAME)
    if any(box[y] != back[y] for y in body):
        failures.append("al volver del banner la greca no queda como estaba")
    else:
        print("  al volver del banner la caja queda como estaba")

    # ...AND THEY MUST BE THE CARTRIDGE'S OWN WALLS, THIS WAY ROUND. Mirrors
    # of each other is not enough: swap the pair and they are still mirrors,
    # and that is exactly the state this port shipped in for a long time --
    # the fret of both walls pointing OUT at the HUD instead of in at the
    # board. The rope's fret is chiral, so which tile goes on which side is
    # the whole of it.
    #
    # The numbers are the cartridge's, read off its own nametable: the 1P
    # screen walls its playfield with $6A $6B at columns 0-1 and $73 $74 at
    # 12-13, and the coop screen hangs the same two off the header's rule with
    # $95 $96 / $99 $9A and $97 $98 / $9B $9C. The port uploads the background
    # tiles at their own indices, so these are the map entries as well.
    WALL_L, WALL_R = (0x6A, 0x6B), (0x73, 0x74)
    HANG_L = ((0x95, 0x96), (0x99, 0x9A))
    HANG_R = ((0x97, 0x98), (0x9B, 0x9C))

    def row_tiles(ty, x0, n):
        return tuple(core.memory.u16[SCREENBLOCK_ADDR + (ty * 32 + x0 + i) * 2]
                      & 0x3FF for i in range(n))

    for name, tx, want in (("izquierda", 8, WALL_L), ("derecha", 20, WALL_R)):
        got = row_tiles(10, tx, 2)
        if got != want:
            failures.append(
                f"la pared {name} del campo es {[hex(t) for t in got]} y el "
                f"cartucho pone {[hex(t) for t in want]}")
        else:
            print(f"  la pared {name} del campo es la del cartucho, "
                  f"${want[0]:02X} ${want[1]:02X}")
    for name, tx, want in (("izquierdo", 8, HANG_L), ("derecho", 20, HANG_R)):
        got = (row_tiles(0, tx, 2), row_tiles(1, tx, 2))
        if got != want:
            failures.append(
                f"el codo del panel {name} no es el del cartucho: {got}")
    if not failures:
        print("  y los dos codos son los de la regla de cabecera del cartucho")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: la greca conserva su sentido en los dos modos.")
    return 0


def leaving_title_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    run(core, 40)
    if not oam_visible(core, 0, 64):
        failures.append("la pantalla de titulo no dibuja ningun sprite")

    press_start(core)               # title -> game select
    run(core, 10)
    left = oam_visible(core, 0, 64)
    if left:
        failures.append(f"quedan {len(left)} sprites del titulo sobre GAME SELECT")
    else:
        print("  al salir del titulo no queda ni un sprite suyo por la pantalla")

    # The engine's request ring: $0200-$0207 with its indices at $0208/$0209
    # (setMusicOrSoundEffect, main.asm.txt:CFB1). Reading it says exactly what
    # the port asked the cartridge to play, which is better than guessing from
    # the sound registers.
    ram, why = game_state_address(rom_path, "g_nes_ram")
    if ram is None:
        print(f"  (sin comprobar la musica: {why})")
    else:
        # setMusicOrSoundEffect stores AT the incremented write index
        # ($0209), so that slot holds the last thing asked for and the one
        # before it the one before that. $0208 is the READ index and says how
        # far the engine has got, which is a different question.
        # The requests in the ring, newest last. $01 and $02 are SUSPEND and
        # RESUME — transport, not tracks — and $0E and up are sound effects
        # (constants.asm.txt:37-61); both are stepped over when the question
        # is "which tune was asked for".
        def recent_tracks(n):
            write = core.memory.u8[ram + 0x209]
            out = []
            for back in range(8):
                v = core.memory.u8[ram + 0x200 + (write - back) % 8]
                if 0x02 < v < 0x0E:
                    out.append(v)
                    if len(out) == n:
                        break
            return tuple(reversed(out))

        def last_two():
            pair = recent_tracks(2)
            return pair if len(pair) == 2 else (0, 0)

        # A screen that is already playing the right thing queues nothing,
        # which is correct and has to read as correct.
        def last_music():
            got = recent_tracks(1)
            return got[0] if got else 0

        press_start(core)           # game select -> LEVEL, the first setup page
        run(core, 12)
        # Arriving at the selection screen settles the music: silence, then
        # whatever the cursor shows.
        recent = last_two()
        if recent[0] != 0x08:
            failures.append(
                f"al entrar en la seleccion no se manda MUSIC_SILENCE (se mando ${recent[0]:02X})")
        elif recent[1] == 0x08:
            failures.append("se manda el silencio pero no la cancion detras")
        else:
            print(f"  al entrar en la seleccion: silencio y despues ${recent[1]:02X}")

        # ...and the tune is picked with LEFT/RIGHT once the cursor is on the
        # MUSIC row, two rows down.
        for _ in range(2):
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 10)
        moved = last_two()
        if moved[0] != 0x08 or moved[1] == 0x08:
            failures.append("mover el cursor no toca la cancion nueva")
        else:
            print(f"  mover el cursor toca la cancion: silencio y despues ${moved[1]:02X}")

        # WHICH SLOTS ARE HELD, which is the question underneath all of the
        # others. The engine keeps eleven voice slots at $0292 and the top
        # bits of each are the song's PRIORITY CLASS: 7 for the four in-game
        # tunes, 8 for the title theme and the game-over tune, 29 and 62 for
        # the effects. MUSIC_SILENCE only frees class 7, and a class-7 tune
        # can never evict a class-8 one — so "is the theme still there" is not
        # a question about volume, it is a question about who holds the slots,
        # and asking it that way is what finally cornered this.
        def music_classes():
            return sorted({core.memory.u8[ram + 0x292 + y] >> 2
                            for y in range(11)
                            if core.memory.u8[ram + 0x292 + y]})

        # AND IT HAS TO STOP COMING BACK OUT WITH YOU — which is a question
        # about the SOUND, not about the request, and asking only about the
        # request is how this passed for so long while the theme went on
        # playing. So this LISTENS for two seconds and then asks who holds the
        # voice slots, which is the question the volume could not answer.
        core.set_keys(KEYS["B"]); run(core, 4); core.set_keys(); run(core, 14)
        heard = 0
        for _ in range(120):
            core.run_frame()
            st = sound_state(core)
            heard |= st["activos"] | st["pulso 1"] | st["pulso 2"]
            heard |= st["triangulo"] | st["ruido"]
        klass = music_classes()
        if heard or klass:
            failures.append(
                "la musica del titulo sigue en GAME SELECT "
                f"(canales {heard:#06b}, clases {klass})")
        else:
            print("  al volver a GAME SELECT se calla, y suelta sus voces")

        core.set_keys(KEYS["B"]); run(core, 4); core.set_keys(); run(core, 14)
        if last_music() != 0x09:
            failures.append(
                f"en el titulo no esta sonando su tema (${last_music():02X})")
        else:
            # ...and FROM THE TOP. The silence has to be the request before
            # it, because that is what resets the engine; picking the theme up
            # from wherever a suspend froze it is not coming back to a title.
            pair = last_two()
            if pair != (0x08, 0x09):
                failures.append(
                    "el tema del titulo no se reinicia al volver "
                    f"(se pidio ${pair[0]:02X} y luego ${pair[1]:02X}, "
                    "deberia ser $08 y $09)")
            else:
                print("  y en el titulo sigue siendo el suyo, desde el principio")

        # NO MUSIC HAS TO BE NO MUSIC. musicSelectTable's first entry is the
        # silence, which resets the engine without quieting it, so this is the
        # one menu choice that must leave the engine suspended rather than
        # resumed — and it is where a resumed title theme used to surface,
        # since nothing came after it to take the speaker back.
        to_music_page(core, 12)
        # ...onto a tune that is a tune: NO MUSIC and MUSIC MIX are both quiet
        # on this screen by design, so neither can answer the next question.
        for _ in range(8):
            row = tilemap_text(core, MUSIC_ROW)
            if "NO MUSIC" not in row and "MUSIC MIX" not in row:
                break
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 12)
        # THE TUNE HAS TO BE ALONE. This is the shape the bug actually had:
        # the theme kept its class-8 slots, the chosen tune could not take
        # them, and it played on to its END before the tune was heard.
        klass = music_classes()
        if klass != [7]:
            failures.append(
                f"en LEVEL SELECT las voces estan en las clases {klass}, "
                "no solo en la 7: el tema del titulo sigue ocupandolas")
        else:
            print("  en LEVEL SELECT solo suena la clase 7, la cancion elegida")

        for _ in range(8):
            if "NO MUSIC" in tilemap_text(core, MUSIC_ROW):
                break
            core.set_keys(KEYS["LEFT"]); run(core, 4); core.set_keys(); run(core, 12)
        run(core, 20)
        heard = 0
        for _ in range(120):
            core.run_frame()
            st = sound_state(core)
            heard |= st["activos"] | st["pulso 1"] | st["pulso 2"]
            heard |= st["triangulo"] | st["ruido"]
        if heard:
            failures.append(
                f"con NO MUSIC elegido algo sigue sonando (canales {heard:#06b})")
        else:
            print("  NO MUSIC deja el motor callado, sin tema de titulo debajo")

    # THE BUTTONS THE CARTRIDGE ANSWERS TO. processMenuInput takes SELECT as
    # well as START on the title ($9FA4), and takes SELECT as a cursor MOVE on
    # the menus, the same direction as DOWN ($9FBC, and LA048's carry-set add).
    # Both were missing, which is the whole of "SELECT does not select".
    core, screen = load(rom_path)
    run(core, 40)

    def tap(key, settle=12):
        core.set_keys(key)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    tap(KEYS["SELECT"])
    if "GAME SELECT" not in tilemap_text(core, 8):
        failures.append("SELECT no avanza desde el titulo")
    else:
        print("  SELECT avanza desde el titulo, como START")

    # THE LIST IS ONE BLUE AND THE CURSOR IS THE ARROW. Measured off the
    # cartridge: every entry on its GAME SELECT is (48,50,236), chosen or not,
    # and the only white on the screen is the arrow. So this asserts the
    # colour as well as the movement — the port had the list in white with the
    # choice picked out in the CREDIT's orange, which is two mistakes wearing
    # each other's clothes and neither of them shows up in a text comparison.
    def entry_banks():
        return {core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2] >> 12
                for r in range(GAME_SELECT_TY, GAME_SELECT_TY + GAME_SELECT_ROWS)
                for x in range(6, 26)
                if core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2] & 0x3FF}

    banks = entry_banks()
    if banks != {MENU_TEXT_BANK}:
        failures.append(f"las entradas de GAME SELECT no van todas en el azul "
                        f"del cartucho (bancos {sorted(banks)})")
    else:
        print("  las cinco entradas van en el azul del cartucho, sin resaltado")

    def menu_rows():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2]
                for r in (GAME_SELECT_TY, GAME_SELECT_TY + 1)
                for x in range(4, 26)]

    before = menu_rows()
    tap(KEYS["SELECT"])
    if menu_rows() == before:
        failures.append("SELECT no mueve el cursor en GAME SELECT")
    else:
        print("  SELECT mueve el cursor en GAME SELECT")

    # ...and back onto 1 PLAYER, because SELECT just moved it and every other
    # entry goes to the cable instead of the settings screen. SELECT only ever
    # moves DOWN (LA048's carry-set add), so this walks it round rather than
    # counting the entries — there are three now and there may be five when
    # the COMPUTER player lands.
    def chosen_row():
        """Which GAME SELECT row the cursor ARROW stands beside.

        It used to be "which row is drawn in a palette of its own", and that
        stopped being a question the moment the screen went back to the
        cartridge's colours: there every entry is the same blue and the arrow
        is the whole of the cursor. Asking about the palette instead answered
        "the first row" for every row, which walked this check straight past a
        cursor that was somewhere else entirely and pressed A on 2 PLAYER.
        """
        for row in range(GAME_SELECT_TY, GAME_SELECT_TY + GAME_SELECT_ROWS):
            e = core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + MENU_ARROW_TX) * 2]
            if e & 0x3FF:
                return row
        return None

    for _ in range(GAME_SELECT_ROWS + 1):
        if chosen_row() == GAME_SELECT_TY:
            break
        tap(KEYS["SELECT"])
    if chosen_row() != GAME_SELECT_TY:
        failures.append("no se puede volver a 1 PLAYER con SELECT")
    tap(KEYS["A"])
    if "LEVEL" not in tilemap_text(core, LEVEL_ROW):
        failures.append("A no confirma en GAME SELECT")
    else:
        print("  A confirma, ademas de START")

    # On through the cartridge's three setup screens to the one SELECT is
    # being asked about.
    # SELECT moves the CURSOR down the settings, the way DOWN does ($9FBC).
    # The cursor is an arrow tile a couple of columns left of the labels; read
    # the whole gutter so moving the column does not silently break this.
    def cursor_gutter():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2]
                for r in (8, 11, 14) for x in range(2, 6)]

    before = cursor_gutter()
    tap(KEYS["SELECT"])
    if cursor_gutter() == before:
        failures.append("SELECT no mueve el cursor en LEVEL SETTINGS")
    else:
        print("  SELECT mueve el cursor en LEVEL SETTINGS")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: al salir del titulo no quedan sprites ni musica suyos, y "
          "SELECT y A hacen lo suyo.")
    return 0


def handtunes_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    def tap(key, settle=8):
        core.set_keys(key)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 8)
    to_music_page(core, 8)
    run(core, 8)

    # Four tunes, and no fifth on offer.
    # musicSelectTable has FIVE entries and the first is silence
    # (main.asm.txt:4741, "silence, loginska, bradinsky, karinka, troika").
    seen = set()
    for _ in range(10):
        seen.add(tilemap_text(core, MUSIC_ROW))
        tap(KEYS["RIGHT"])
    for hidden in ("KOROBEINIKI", "KATIUSKA"):
        if any(hidden in row for row in seen):
            failures.append(f"{hidden} se ofrece sin haber metido el codigo")
    if len(seen) != 5:
        failures.append(f"el menu ofrece {len(seen)} canciones, no 5: {sorted(seen)}")
    elif not any("NO MUSIC" in row for row in seen):
        failures.append("falta la primera entrada de musicSelectTable, el silencio")
    else:
        print(f"  antes del codigo: {len(seen)} canciones, con el silencio de la ROM")

    # L+R together, the two buttons a NES pad never had.
    core.set_keys(KEYS["L"], KEYS["R"])
    run(core, 4)
    core.set_keys()
    run(core, 10)
    # The row carries the menu frame's border tiles either side of the name,
    # so this looks for the name IN it, the way the other menu checks do.
    if "KOROBEINIKI" not in tilemap_text(core, MUSIC_ROW):
        failures.append("L+R no descubre KOROBEINIKI ni la deja elegida")
        print(f"       la fila dice {tilemap_text(core, MUSIC_ROW)!r}")
    else:
        print("  L+R descubre KOROBEINIKI y la deja elegida")

    # The code uncovers THREE entries, not one: the two hand-entered tunes and
    # MUSIC MIX, which plays them all in turn and turns over at every level-up.
    unlocked = set()
    for _ in range(16):
        unlocked.add(tilemap_text(core, MUSIC_ROW))
        tap(KEYS["RIGHT"])
    if len(unlocked) != 8:
        failures.append(f"tras el codigo el menu ofrece {len(unlocked)}, no 8")
    else:
        missing = [n for n in ("KATIUSKA", "MUSIC MIX")
                   if not any(n in row for row in unlocked)]
        if missing:
            failures.append(f"el codigo no descubre {', '.join(missing)}")
        else:
            print(f"  tras el codigo: {len(unlocked)} entradas, "
                  "con KATIUSKA y MUSIC MIX")

    # Back onto it, then into a game.
    for _ in range(10):
        if "KOROBEINIKI" in tilemap_text(core, MUSIC_ROW):
            break
        tap(KEYS["RIGHT"])
    press_start(core)
    run(core, 10)

    # It has to make notes, and they have to move. The pulse channels are the
    # two it drives; the frequency register is what a tune changes.
    io = core._native.memory.io

    def reg(addr):
        return io[(addr - 0x04000000) >> 1]

    pitches, volumes = set(), set()
    bass_vol, bass_on, tri_on = set(), 0, 0
    for _ in range(400):
        core.run_frame()
        pitches.add(reg(REG_SOUND1CNT_X) & 0x7FF)
        volumes.add((reg(REG_SOUND1CNT_H) >> 12) & 0xF)
        v2 = (reg(REG_SOUND2CNT_L) >> 12) & 0xF
        if v2:
            bass_vol.add(v2)
            bass_on += 1
        if reg(REG_SOUND3CNT_H) & 0xE000:
            tri_on += 1
    if max(volumes) == 0:
        failures.append("KOROBEINIKI no suena: el pulso 1 queda a volumen cero")
    elif len(pitches) < 6:
        failures.append(f"KOROBEINIKI no cambia de nota: {len(pitches)} tono(s)")
    else:
        print(f"  suena y se mueve: {len(pitches)} tonos distintos en 400 frames")

    # AND NO LOUDER THAN THE FOUR THE CARTRIDGE PLAYS. Measured off these same
    # registers while the ROM's engine played its own: pulse 1 sits at 5 (7 for
    # BRADINSKY), pulse 2 at 3 (5 for TROIKA), and the triangle is going under
    # all four of them for half the frames or more. The hand-entered pair used
    # to run at 11 and 8 with the second sounding 97% of the time and nothing
    # on the triangle at all — twice the amplitude on the lead, nearly three
    # times on a second square that never stopped, which is what "saturated"
    # was.
    loud = max(volumes)
    if loud > 7:
        failures.append(f"KOROBEINIKI lleva el pulso 1 a {loud}; el cartucho "
                         "no pasa de 7")
    elif bass_vol and max(bass_vol) > 5:
        failures.append(f"...y el pulso 2 a {max(bass_vol)}; el cartucho no "
                         "pasa de 5")
    elif bass_on > 400 * 7 // 10:
        failures.append(f"el pulso 2 suena en el {100 * bass_on // 400}% de los "
                         "frames; en el cartucho no llega al 50")
    elif tri_on < 400 // 4:
        failures.append(f"el triangulo solo suena el {100 * tri_on // 400}%: el "
                         "cartucho lleva ahi el bajo de sus cuatro canciones")
    else:
        print(f"  y se mantiene en el nivel del cartucho: pulso 1 a {loud}, "
              f"pulso 2 a {max(bass_vol)} el {100 * bass_on // 400}% del "
              f"tiempo, triangulo el {100 * tri_on // 400}%")

    # PAUSE has to reach it too.
    tap(KEYS["START"], settle=10)
    worst = 0
    for _ in range(120):
        core.run_frame()
        for k, v in sound_state(core).items():
            worst = max(worst, v)
    if worst:
        failures.append(f"en pausa la cancion de mas sigue sonando ({worst})")
    else:
        print("  PAUSE la silencia igual que a las del cartucho")

    # ...and the cartridge's engine is still there underneath: unpause and drop
    # a piece, which plays SOUND_DROP through the ROM's own engine.
    # Watched over the whole drop, not sampled at the end: a sound effect is a
    # few frames long and asking once, afterwards, mostly asks too late.
    tap(KEYS["START"], settle=10)
    core.set_keys(KEYS["DOWN"])
    engine = 0
    for _ in range(300):
        core.run_frame()
        engine = max(engine, sound_state(core)["activos"])
    core.set_keys()
    if not engine:
        failures.append("el motor del cartucho no sigue vivo bajo la cancion de mas")
    else:
        print("  el motor del cartucho sigue sonando debajo (los efectos son suyos)")

    # THE SECOND HAND-ENTERED TUNE, on its own console. Katyusha is a
    # different score and a different tempo, so "it plays" is not enough: the
    # set of pitches it reaches has to be its own. Two tunes sharing one
    # sequencer is exactly how a bad table index looks like nothing at all.
    other, other_screen = load(rom_path)    # `other_screen` must stay alive
    _ = other_screen
    run(other, 8)
    to_music_page(other, 8)
    run(other, 8)
    other.set_keys(KEYS["L"], KEYS["R"]); run(other, 4)
    other.set_keys(); run(other, 10)
    for _ in range(12):
        if "KATIUSKA" in tilemap_text(other, MUSIC_ROW):
            break
        other.set_keys(KEYS["RIGHT"]); run(other, 4)
        other.set_keys(); run(other, 8)
    else:
        failures.append("no pude dejar KATIUSKA elegida en el menu")
    press_start(other)
    run(other, 10)
    other_io = other._native.memory.io
    kat_pitches, kat_volumes = set(), set()
    for _ in range(400):
        other.run_frame()
        kat_pitches.add(other_io[(REG_SOUND1CNT_X - 0x04000000) >> 1] & 0x7FF)
        kat_volumes.add((other_io[(REG_SOUND1CNT_H - 0x04000000) >> 1] >> 12) & 0xF)
    if max(kat_volumes) == 0:
        failures.append("KATIUSKA no suena: el pulso 1 queda a volumen cero")
    elif len(kat_pitches) < 6:
        failures.append(f"KATIUSKA no cambia de nota: {len(kat_pitches)} tono(s)")
    elif kat_pitches == pitches:
        failures.append("KATIUSKA toca exactamente los tonos de KOROBEINIKI")
    else:
        print(f"  KATIUSKA es otra cancion: {len(kat_pitches)} tonos, "
              f"{len(kat_pitches ^ pitches)} distintos de los de KOROBEINIKI")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: las dos canciones de mas estan escondidas, suenan, "
          "y no pisan al cartucho.")
    return 0


def link_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    def tap(name, settle=6):
        core.set_keys(KEYS[name])
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 8)
    tap("START")                      # title -> game select
    if "GAME SELECT" not in tilemap_text(core, 8):
        failures.append("no aparece GAME SELECT tras el titulo")
    # The cartridge's five, on consecutive rows the way it lists them
    # (gameSelectArrowPpuAddrs, $A0AB).
    for i, want in enumerate(("1 PLAYER", "2 PLAYER", "COOPERATIVE",
                               "VERSUS COMPUTER", "WITH COMPUTER")):
        if want not in tilemap_text(core, GAME_SELECT_TY + i):
            failures.append(f"GAME SELECT no ofrece {want}")

    # The credits sit under whatever the last entry is, at the foot of the
    # frame, so they move when an entry is added. Which one is up depends on
    # the frame count, so this only asks that SOMETHING is there — the six of
    # them are credits_check's business.
    if not (tilemap_text(core, CREDIT_TY).strip()
            and tilemap_text(core, CREDIT_TY + 1).strip()):
        failures.append("faltan los creditos al pie de GAME SELECT")

    tap("DOWN")                       # 1 PLAYER -> 2 PLAYER
    # 2 PLAYER goes STRAIGHT to the cable now: on a link game only one of the
    # two players picks the level and the tune, and neither console knows which
    # one that is until the cable has said so, so the choosing happens after
    # the connecting and only on the master.
    tap("START")                      # game select -> the cable

    if "LINK CABLE" not in tilemap_text(core, 8):
        failures.append("2 PLAYER no lleva a la pantalla de cable link")

    # Nothing is plugged in, so the lobby must be spinning without a partner.
    # If any serial wait lacked a timeout the emulator would stop advancing
    # here; comparing two frames a hundred apart is how that shows up.
    before = pixels(screen)
    run(core, 100)
    if pixels(screen) != before:
        failures.append("la pantalla de espera no es estable")

    # And it gives up rather than waiting forever.
    run(core, LINK_TIMEOUT_FRAMES + 60)
    msg = tilemap_text(core, LINK_MSG_ROW)
    if "NO CABLE" not in msg:
        failures.append(f"el lobby no se rinde sin cable (fila {LINK_MSG_ROW}: {msg!r})")
    else:
        print(f"  sin cable: '{msg}' tras {LINK_TIMEOUT_FRAMES} frames")

    tap("B")                          # back out of the link screen
    if "GAME SELECT" not in tilemap_text(core, 8):
        failures.append("B no vuelve del cable link al menu")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el modo 2 jugadores llega al cable, no se cuelga sin el, y se sale.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--png")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--lineclear", action="store_true",
                     help="watch the line-clear animation, for 1 row and for 4")
    ap.add_argument("--pause", action="store_true",
                     help="check Start pauses and the cheat codes respond")
    ap.add_argument("--audio", action="store_true",
                     help="check the emulated sound engine against its golden recording")
    ap.add_argument("--pause-audio", action="store_true",
                     help="check that pausing actually silences the sound")
    ap.add_argument("--link", action="store_true",
                     help="check the 2-player front end with no cable attached")
    ap.add_argument("--title", action="store_true",
                     help="check the cathedral overlay and the fireworks")
    ap.add_argument("--skin", action="store_true",
                     help="check the L/R title-skin easter egg")
    ap.add_argument("--credits", action="store_true",
                     help="check the cartridge's six front-end credit lines")
    ap.add_argument("--xe", action="store_true",
                     help="check the Tetris Tengen XE level range behind the chord")
    ap.add_argument("--handtunes", "--korobeiniki", action="store_true",
                     help="check the two hidden hand-entered tunes")
    ap.add_argument("--leave-title", action="store_true",
                     help="check the title leaves no sprites or music behind")
    ap.add_argument("--braid", action="store_true",
                     help="check the braid keeps its weave in both HUD modes")
    ap.add_argument("--handicap", action="store_true",
                     help="check the starting handicap reaches the playfield")
    ap.add_argument("--panel", action="store_true",
                     help="check the four counters share one headroom")
    ap.add_argument("--next-palette", action="store_true",
                     help="check NEXT wears the next piece's colours")
    ap.add_argument("--menu", action="store_true",
                     help="check LEVEL SETTINGS fits inside its frame")
    ap.add_argument("--computer", action="store_true",
                     help="check the COMPUTER player plays VERSUS and WITH")
    ap.add_argument("--demo", action="store_true",
                     help="check the title starts playing by itself")
    ap.add_argument("--loans", action="store_true",
                    help="check the palette banks a skin borrows come back")
    ap.add_argument("--onscreen", action="store_true",
                    help="check what the tile map promises reaches the screen")
    ap.add_argument("--effects", action="store_true",
                    help="check each build's menu noises are its own")
    ap.add_argument("--stats", action="store_true",
                    help="check the histogram and the GAME OVER plaque follow "
                         "the skin")
    ap.add_argument("--gameover", action="store_true",
                     help="check every mode ends and lets go of the player")
    ap.add_argument("--counters", action="store_true",
                     help="check the counters blank their leading zeros")
    ap.add_argument("--pausemenu", action="store_true",
                     help="check the L+R pause menu")
    ap.add_argument("--quit-audio", action="store_true",
                     help="check quitting from the pause menu leaves the sound alive")
    ap.add_argument("--leaderboard", action="store_true",
                     help="check the HIGH SCORES table and its initials")
    ap.add_argument("--points", action="store_true",
                     help="check the drop-point sprites beside the piece")
    ap.add_argument("--falling", action="store_true",
                     help="check both pieces of a coop board are drawn")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest(args.rom))
    if args.lineclear:
        sys.exit(lineclear_check(args.rom, 1) or lineclear_check(args.rom, 4))
    if args.pause:
        sys.exit(pause_check(args.rom))
    if args.audio:
        sys.exit(audio_check(args.rom))
    if args.pause_audio:
        sys.exit(pause_audio_check(args.rom))
    if args.link:
        sys.exit(link_check(args.rom))
    if args.title:
        sys.exit(title_check(args.rom))
    if args.skin:
        sys.exit(skin_check(args.rom))
    if args.credits:
        sys.exit(credits_check(args.rom))
    if args.xe:
        sys.exit(xe_check(args.rom))
    if args.handtunes:
        sys.exit(handtunes_check(args.rom))
    if args.leave_title:
        sys.exit(leaving_title_check(args.rom))
    if args.braid:
        sys.exit(braid_check(args.rom))
    if args.handicap:
        sys.exit(handicap_check(args.rom))
    if args.panel:
        sys.exit(panel_check(args.rom))
    if args.next_palette:
        sys.exit(next_palette_check(args.rom))
    if args.menu:
        sys.exit(menu_check(args.rom))
    if args.computer:
        sys.exit(computer_check(args.rom))
    if args.demo:
        sys.exit(demo_check(args.rom))
    if args.gameover:
        sys.exit(gameover_check(args.rom))
    if args.falling:
        sys.exit(falling_piece_check(args.rom))
    if args.points:
        sys.exit(points_check(args.rom))
    if args.leaderboard:
        sys.exit(leaderboard_check(args.rom))
    if args.quit_audio:
        sys.exit(quit_audio_check(args.rom))
    if args.loans:
        sys.exit(loans_check(args.rom))
    if args.onscreen:
        sys.exit(onscreen_check(args.rom))
    if args.effects:
        sys.exit(effects_check(args.rom))
    if args.stats:
        sys.exit(stats_check(args.rom))
    if args.pausemenu:
        sys.exit(pausemenu_check(args.rom))
    if args.counters:
        sys.exit(counters_check(args.rom))

    core, screen = load(args.rom)
    start_game(core)
    run(core, args.frames)
    describe(pixels(screen))
    if args.png:
        with open(args.png, "wb") as fh:
            screen.save_png(fh)
        print(f"guardado {args.png}")


if __name__ == "__main__":
    main()
