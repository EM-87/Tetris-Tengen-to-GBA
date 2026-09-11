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

# The screen layout, in tile columns. These mirror gba/main.c and the reflow
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
# blocks painted from the playfield buffer — see the note in gba/main.c.
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

    # The title screen must come up first and must not be blank.
    run(core, 8)
    title = pixels(screen)
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
OAM_ADDR = 0x07000000
SWEEP_TILES = (0x5B, 0x5C, 0x5D, 0x5E, 0x5F)  # main.asm.txt:1274-1338
SWEEP_PAL_BANK = 4   # gba/main.c PAL_OBJ_CLEAR; banks 0-3 are the dancers
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
# what the port keeps rather than the column number. gba/main.c derives these
# the same way.
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
        # L or R swaps the title skin, L+R together the right-hand HUD box.
        "R": 8, "L": 9}
CODE_LEVEL_UP = "UP DOWN UP DOWN LEFT RIGHT B B A".split()
CODE_LONG_BAR = "DOWN DOWN LEFT RIGHT LEFT RIGHT B A".split()

# Offsets into g_session.game, from the structs in src/tengen_core.h. The compiler is
# arm-none-eabi with the EABI's default -fshort-enums, so a TengenTetromino is
# one byte; a mismatch would show up immediately as nonsense readings, which
# the checks below would catch.
# THE ELF KNOWS THE OFFSETS, not this file. They used to be constants here and
# a single new field in TengenGame moved all of them, which showed up as three
# unrelated cheat-code checks failing. gba/main.c exports kGameProbe; this
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
         paused, held, nxt) = (core.memory.u16[addr + i * 2] for i in range(10))
        _GAME_PROBE_CACHE[rom_path] = {
            "field": field, "player": player, "stride": stride,
            "current": player + cur, "y": player + y,
            "level": player + level, "stats": player + stats,
            "paused": paused, "held": player + held, "next": player + nxt,
        }
    return _GAME_PROBE_CACHE[rom_path]


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
# SCREENBLOCK_STATS in gba/main.c.
SCREENBLOCK_OFFSET_ADDR = SCREENBLOCK_ADDR + 0x800


def tilemap_text(core, row, first=0, last=30):
    """The row of the tilemap as text, ACROSS BOTH LAYERS.

    The tileset's letters sit at their ASCII codes (see ascii_tile in
    gba/main.c), so a tile id IS a character. A menu line of odd length is
    drawn on the offset layer instead of the main one — reading only the main
    map would report an empty row and every menu check would quietly stop
    checking anything.
    """
    out = []
    for x in range(first, last):
        off = (row * 32 + x) * 2
        tile = core.memory.u16[SCREENBLOCK_ADDR + off] & 0x3FF
        if not (32 <= tile < 127):
            tile = core.memory.u16[SCREENBLOCK_OFFSET_ADDR + off] & 0x3FF
        out.append(chr(tile) if 32 <= tile < 127 else " ")
    return "".join(out).strip()


# ---------------------------------------------------------------------------
# The title screen's sprites: the cathedral overlay and the fireworks.
#
# Neither is drawn by the port. Both are subroutines of the cartridge run on
# the same 6502 interpreter as the sound engine, filling oamStaging, which
# gba/main.c copies into OAM (see draw_title_sprites). What can be checked
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

    def tap(key, settle=12):
        core.set_keys(key)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 40)
    release = frame_tiles()

    tap(KEYS["L"])
    proto = frame_tiles()
    if proto == release:
        print("  esta ROM se construyo sin prototipo: L y R no tienen skin que poner")
        tap(KEYS["R"])
        if frame_tiles() != release:
            print("FALLA: R cambio algo que L no habia cambiado")
            return 1
        print("OK: sin skin de prototipo, y el titulo no se rompe por pulsar L o R.")
        return 0
    print("  L pone el marco del prototipo, que es de otros tiles")

    # The release's fireworks and cathedral overlay belong to the release
    # picture; on the prototype's they must be gone.
    if oam_visible(core, 0, 64):
        failures.append("los sprites del release siguen encima de la skin")
    else:
        print("  los sprites del release (catedral y fuegos) se retiran con ella")

    tap(KEYS["R"])
    if frame_tiles() != release:
        failures.append("R no devuelve la pantalla del release")
    else:
        print("  R vuelve al titulo del release")

    # ...and the choice must survive leaving the title and coming back.
    tap(KEYS["L"])
    for name in ("START", "B"):
        core.set_keys(KEYS[name]); run(core, 4); core.set_keys(); run(core, 10)
    if frame_tiles() != proto:
        failures.append("la skin se pierde al salir del titulo y volver")
    else:
        print("  la skin se mantiene al salir del titulo y volver")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: L y R cambian entre el titulo del release y el del prototipo.")
    return 0


# ---------------------------------------------------------------------------
# The fifth tune.
#
# Korobeiniki is not on this cartridge — Tengen's four are Loginska, Bradinsky,
# Karinka and Troika — so it is entered by hand in gba/korobeiniki.c and hidden
# behind L+R on the selection screen. That makes it the one piece of content in
# the port that was not extracted from the ROM, and the one piece of sound that
# does not come out of the cartridge's own engine, so it is worth checking
# rather than assuming:
#
#   * the menu offers four tunes until the code is entered, and five after;
#   * choosing it actually produces notes, and DIFFERENT notes over time (a
#     stuck channel would still read as "sounding");
#   * PAUSE silences it, which needs its own stop because MUSIC_SUSPEND only
#     reaches the cartridge's engine;
#   * and the cartridge's engine is still running underneath, because the
#     sound effects are still meant to be the ROM's.
# ---------------------------------------------------------------------------
# GAME SELECT's entries start at row 10, two rows apart. The port offers the
# cartridge's first three; the last two want the COMPUTER player.
GAME_SELECT_ROWS = 3
MENU_DIM_BANK = 11              # PAL_MENU_BASE + 3, the menu's plain white

MUSIC_ROW = 14                  # the MUSIC row of LEVEL SETTINGS
LEVEL_ROW = 8                   # ...and the LEVEL one above it
HANDICAP_ROW = 11               # value AND, in one player, what it buries


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
# once the other way, and each time the tell was L+R — the banner redraws the
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
        for _ in range(steps):
            core.set_keys(KEYS["L"]); run(core, 4); core.set_keys(); run(core, 8)
        # One line now: "HANDICAP   n   r ROWS". The word BURIES was what got
        # dropped to make the count fit beside the value.
        row = tilemap_text(core, HANDICAP_ROW)
        if "HANDICAP" not in row:
            failures.append(f"la fila {HANDICAP_ROW} no es la del handicap: {row!r}")
        if f"{steps}  {expect_rows} ROWS" not in row:
            failures.append(f"handicap {steps}: la fila dice {row!r}, "
                             f"esperaba el valor y {expect_rows} ROWS")
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

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el handicap entierra tres filas por paso y deja paso en todas.")
    return 0


def panel_check(rom_path):
    """The four counters must have the SAME headroom.

    SCORE used to have one pixel where LINES, LEVEL and HIGH had three, and
    the counters cannot simply be moved: they share BG0 with a playfield whose
    160 pixels are the whole screen. They ride their own background now,
    scrolled two pixels down (SCREENBLOCK_PANEL in gba/main.c), which is a
    thing that shows up as pixels or not at all — so this counts them.
    """
    core, screen = load(rom_path)
    start_game(core)
    run(core, 40)
    rows = pixels(screen)

    # The left box's interior, in pixels: eight tiles from column 2.
    def ink(y):
        return sum(1 for p in rows[y][16:80] if p != (0, 0, 0))

    floor = min(ink(y) for y in range(14, 116))
    solid = [y for y in range(14, 116) if ink(y) >= 60]   # braid run, or a rule

    # Each counter's headroom is the run of empty scanlines between whatever
    # is above it — the braid for SCORE, a rule for the rest — and its label.
    gaps = []
    for y in range(14, 112):
        if ink(y) < 60 and ink(y - 1) >= 60:
            n = 0
            while ink(y + n) <= floor:
                n += 1
            gaps.append((y, n))

    failures = []
    if len(gaps) != 4:
        failures.append(f"se esperaban 4 contadores, se ven {len(gaps)}: {gaps}")
    elif len({n for _, n in gaps}) != 1:
        failures.append("los contadores no tienen el mismo hueco por arriba: "
                         + ", ".join(f"y={y} -> {n}px" for y, n in gaps))
    else:
        print(f"  los cuatro contadores abren con {gaps[0][1]} pixeles por arriba "
               f"(en y={', '.join(str(y) for y, _ in gaps)})")

    if not solid:
        failures.append("no se ve ni una regla en el panel")

    # ...and the RIGHT box's histogram must clear the braid under it. The
    # icons fill their two tiles to the last pixel, so on the tile grid alone
    # they end one line short of the frame; the histogram has a background of
    # its own precisely so it can be lifted clear (STATS_LIFT_PX).
    core.set_keys(KEYS["L"], KEYS["R"])
    run(core, 5)
    core.set_keys()
    run(core, 40)
    rows = pixels(screen)
    braid = next((y for y in range(140, 160)
                  if all(rows[y][x] != (0, 0, 0) for x in range(176, 240))), None)
    if braid is None:
        failures.append("no se encuentra la greca de abajo del cajon derecho")
    else:
        last = max((y for y in range(100, braid)
                    if any(rows[y][x] != (0, 0, 0) for x in range(176, 240))),
                   default=None)
        if last is None:
            failures.append("el cajon derecho no dibuja las estadisticas")
        elif braid - last < 3:
            failures.append(f"las estadisticas llegan a y={last} y la greca "
                             f"empieza en y={braid}: se tocan")
        else:
            print(f"  las estadisticas acaban en y={last}, la greca empieza en "
                   f"y={braid}: {braid - last - 1} pixeles de aire")

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

    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4); core.set_keys(); run(core, 12)
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

    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4); core.set_keys(); run(core, 12)
    back = frame_columns(*RIGHT_FRAME)
    if any(box[y] != back[y] for y in body):
        failures.append("al volver del banner la greca no queda como estaba")
    else:
        print("  al volver del banner la caja queda como estaba")

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

    # The cursor is a PALETTE change, not a text one — the chosen entry is
    # drawn in the menu's orange instead of its white — so this compares the
    # whole map entries, palette bits and all, rather than the text.
    def menu_rows():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2]
                for r in (10, 12) for x in range(4, 26)]

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
        """Which GAME SELECT row is drawn in the highlight palette."""
        for row in range(10, 10 + GAME_SELECT_ROWS * 2, 2):
            banks = {core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + x) * 2] >> 12
                     for x in range(4, 26)
                     if core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + x) * 2] & 0x3FF}
            if banks and banks != {MENU_DIM_BANK}:
                return row
        return None

    for _ in range(GAME_SELECT_ROWS + 1):
        if chosen_row() == 10:
            break
        tap(KEYS["SELECT"])
    if chosen_row() != 10:
        failures.append("no se puede volver a 1 PLAYER con SELECT")
    tap(KEYS["A"])
    if "LEVEL" not in tilemap_text(core, 8):
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


def korobeiniki_check(rom_path):
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
    if any("KOROBEINIKI" in row for row in seen):
        failures.append("la cancion escondida se ofrece sin haber metido el codigo")
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

    # The code uncovers TWO entries, not one: the fifth tune and MUSIC MIX,
    # which plays the five in turn and turns over at every level-up.
    unlocked = set()
    for _ in range(14):
        unlocked.add(tilemap_text(core, MUSIC_ROW))
        tap(KEYS["RIGHT"])
    if len(unlocked) != 7:
        failures.append(f"tras el codigo el menu ofrece {len(unlocked)}, no 7")
    elif not any("MUSIC MIX" in row for row in unlocked):
        failures.append("el codigo no descubre MUSIC MIX")
    else:
        print(f"  tras el codigo: {len(unlocked)} entradas, con MUSIC MIX")

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
    for _ in range(400):
        core.run_frame()
        pitches.add(reg(REG_SOUND1CNT_X) & 0x7FF)
        volumes.add((reg(REG_SOUND1CNT_H) >> 12) & 0xF)
    if max(volumes) == 0:
        failures.append("KOROBEINIKI no suena: el pulso 1 queda a volumen cero")
    elif len(pitches) < 6:
        failures.append(f"KOROBEINIKI no cambia de nota: {len(pitches)} tono(s)")
    else:
        print(f"  suena y se mueve: {len(pitches)} tonos distintos en 400 frames")

    # PAUSE has to reach it too.
    tap(KEYS["START"], settle=10)
    worst = 0
    for _ in range(120):
        core.run_frame()
        for k, v in sound_state(core).items():
            worst = max(worst, v)
    if worst:
        failures.append(f"en pausa la quinta cancion sigue sonando ({worst})")
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
        failures.append("el motor del cartucho no sigue vivo bajo la quinta cancion")
    else:
        print("  el motor del cartucho sigue sonando debajo (los efectos son suyos)")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la quinta cancion esta escondida, suena, y no pisa al cartucho.")
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
    if "2 PLAYER" not in tilemap_text(core, 12):
        failures.append("GAME SELECT no ofrece 2 PLAYER")
    if "COOPERATIVE" not in tilemap_text(core, 14):
        failures.append("GAME SELECT no ofrece COOPERATIVE")

    # The credit sits under whatever the last entry is, at the foot of the
    # frame, so it moves when an entry is added.
    if "PAJITNOV" not in tilemap_text(core, 17):
        failures.append("falta el credito a Pajitnov en GAME SELECT")

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
    ap.add_argument("--korobeiniki", action="store_true",
                     help="check the hidden fifth tune")
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
    if args.korobeiniki:
        sys.exit(korobeiniki_check(args.rom))
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
