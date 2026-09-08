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

--selftest checks the things a broken port would get wrong: that the screen
isn't blank, that the playfield frame is where the resolution mapping says it
should be, and that a piece actually falls.

--lineclear watches the line-clear animation happen, sprite by sprite and
tile by tile. It needs `build/tengen.elf` next to the ROM (for the address of
the game state) and an `arm-none-eabi-nm` to read it with.

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

# The screen layout, in tile columns. These mirror gba/main.c and the reflow
# done by tools/extract_assets.py; if they drift apart, the checks below stop
# meaning anything, so they are asserted against the running ROM rather than
# assumed.
COL_BORDER_L = (0, 2)     # braided border
COL_FIELD = (2, 14)       # 12 columns: wall, 10 playable, wall
COL_BANNER = (14, 18)     # vertical TETRIS banner
COL_PANEL = (18, 28)      # score / lines / level / next / stats
COL_BORDER_R = (28, 30)

FIELD_TILES_W = COL_FIELD[1] - COL_FIELD[0]
FIELD_X0 = COL_FIELD[0] * TILE
FIELD_X1 = COL_FIELD[1] * TILE
WALL_L_X = COL_FIELD[0] * TILE            # left wall column
WALL_R_X = (COL_FIELD[1] - 1) * TILE      # right wall column


def load(rom_path):
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
    for name, bounds in (("borde izq", COL_BORDER_L), ("campo", COL_FIELD),
                         ("banner", COL_BANNER), ("panel", COL_PANEL),
                         ("borde der", COL_BORDER_R)):
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
    """Gets past the title and the level-select screen into a game.

    Two presses, matching the ROM's own shape: a title screen, then a
    selection screen, then play.
    """
    run(core, 8)
    press_start(core)   # title -> level select
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

    # And START must take it to the level-select screen, not straight to play.
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
    for name, bounds in (("borde izquierdo", COL_BORDER_L),
                         ("banner TETRIS", COL_BANNER),
                         ("panel del HUD", COL_PANEL),
                         ("borde derecho", COL_BORDER_R)):
        painted = region(bounds)
        if painted == 0:
            failures.append(f"{name} quedo vacio")
        else:
            print(f"  {name}: {painted} px")

    # The field runs the FULL height of the screen — that is the whole point
    # of the 160px vertical fit, and the first thing a bad window offset
    # breaks. Checked per tile ROW rather than per pixel: the ROM draws its
    # walls with a block graphic that has transparent corners, so the
    # invariant is that no row of the wall column is missing, not that every
    # pixel of it is lit.
    for name, x in (("muro izquierdo", WALL_L_X), ("muro derecho", WALL_R_X)):
        empty_rows = [ty for ty in range(SCREEN_H // TILE)
                      if not any(rows[ty * TILE + dy][x + dx] != (0, 0, 0)
                                 for dy in range(TILE) for dx in range(TILE))]
        if empty_rows:
            failures.append(f"al {name} le faltan filas de tiles: {empty_rows}")

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
# `g_game`, whose address comes out of the ELF — and lets the next piece to
# land trigger the clear. Nothing in the ROM is modified; this is a fixture in
# the harness, the same way a unit test constructs a board.
# ---------------------------------------------------------------------------
PF_W, PF_H = 12, 20            # must match TENGEN_PF_WIDTH / _HEIGHT
CELL_WALL, CELL_BLOCK = 15, 1
SCREENBLOCK_ADDR = 0x0600E000  # screenblock 28, as gba/main.c sets BG0CNT
OAM_ADDR = 0x07000000
SWEEP_TILES = (0x5B, 0x5C, 0x5D, 0x5E, 0x5F)  # main.asm.txt:1274-1338
SWEEP_PAL_BANK = 1
CLEAR_WORDS = {1: "SINGLE", 2: "DOUBLE", 3: "TRIPLE", 4: "TETRIS"}

KEY_DOWN = 7


def game_state_address(rom_path):
    """Address of `g_game`, whose first member is player 1's playfield."""
    elf = os.path.splitext(rom_path)[0] + ".elf"
    if not os.path.exists(elf):
        return None, f"no encuentro {elf} (hace falta para localizar el estado)"
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    try:
        out = subprocess.check_output([nm, elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        return None, f"no pude ejecutar {nm}: {exc}"
    for line in out.splitlines():
        if line.endswith(" g_game"):
            return int(line.split()[0], 16), None
    return None, "el ELF no exporta g_game"


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
    for col in range(PF_W):
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
    if not heads or max(heads) < PF_W - 1:
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--png")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--lineclear", action="store_true",
                     help="watch the line-clear animation, for 1 row and for 4")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest(args.rom))
    if args.lineclear:
        sys.exit(lineclear_check(args.rom, 1) or lineclear_check(args.rom, 4))

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
