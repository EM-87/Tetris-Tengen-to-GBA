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
# 8 | 2 | 10 | 2 | 8 — a box, the board's braid, the ten playable columns,
# the braid again, and a box of the same width. Symmetric by construction.
COL_BOX_L = (0, 8)        # score / lines / level
COL_FRAME_L = (8, 10)     # the board's left frame, which is its left wall
COL_FIELD = (10, 20)      # the ten playable columns — dead centre
COL_FRAME_R = (20, 22)    # the board's right frame
COL_BOX_R = (22, 30)      # next piece, and the piece histogram

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


def start_game(core):
    """Gets past the title and the selection screens into a solo game.

    Three presses, matching the ROM's own shape: a title screen, then GAME
    SELECT (whose first entry is 1 PLAYER, so START takes it), then the
    level/music screen, then play.
    """
    run(core, 8)
    press_start(core)   # title -> game select
    press_start(core)   # game select (1 PLAYER) -> level select
    press_start(core)   # level select -> play
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
    """Plant complete rows straight into the playfield."""
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
    fill_rows(core, base, rows)
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
        "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7}
CODE_LEVEL_UP = "UP DOWN UP DOWN LEFT RIGHT B B A".split()
CODE_LONG_BAR = "DOWN DOWN LEFT RIGHT LEFT RIGHT B A".split()

# Offsets into g_session.game, from the structs in src/tengen_core.h. The compiler is
# arm-none-eabi with the EABI's default -fshort-enums, so a TengenTetromino is
# one byte; a mismatch would show up immediately as nonsense readings, which
# the checks below would catch.
OFF_CURRENT = 488
OFF_Y = 492
OFF_LEVEL = 504


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
AUDIO_ALIGN_SEARCH = 30   # frames of ROM start-up to look through for the match

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
    start_game(core)

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

    seen = []
    for _ in range(len(golden) + AUDIO_ALIGN_SEARCH):
        core.run_frame()
        seen.append(bytes(iwram[apu_off:apu_off + APU_REGS]))

    failures = []
    if iwram[fault_off]:
        failures.append("el interprete 6502 se detuvo por un opcode que no conoce")

    # The ROM sets up for a few frames before the first updateAudio, so find
    # where the two line up rather than assuming they start together.
    start, matched = 0, 0
    for offset in range(AUDIO_ALIGN_SEARCH):
        n = 0
        while n < len(golden) and offset + n < len(seen) and seen[offset + n] == golden[n]:
            n += 1
        if n > matched:
            matched, start = n, offset

    print(f"  motor de sonido alineado en el frame {start}; "
          f"{matched} de {len(golden)} frames identicos al de referencia")
    if matched < len(golden):
        failures.append(f"el APU emulado se desvia en el frame {matched}: "
                        f"ROM {seen[start + matched].hex(' ')} "
                        f"vs referencia {golden[matched].hex(' ')}")

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
        piece_y = game_base + 480 + 8 + 4      # player[0].piece.y
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


def tilemap_text(core, row, first=0, last=30):
    """The row of the tilemap as text. The tileset's letters sit at their
    ASCII codes (see ascii_tile in gba/main.c), so a tile id IS a character."""
    out = []
    for x in range(first, last):
        tile = core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + x) * 2] & 0x3FF
        out.append(chr(tile) if 32 <= tile < 127 else " ")
    return "".join(out).strip()


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
    if "2 PLAYER" not in tilemap_text(core, 13):
        failures.append("GAME SELECT no ofrece 2 PLAYER")

    tap("DOWN")                       # 1 PLAYER -> 2 PLAYER
    tap("START")                      # game select -> level select
    tap("START")                      # level select -> link screen

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
    if "LEVEL SELECT" not in tilemap_text(core, 8):
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
