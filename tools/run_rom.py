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

--selftest checks the things a broken port would get wrong: that the screen
isn't blank, that the playfield frame is where the resolution mapping says it
should be, and that a piece actually falls.

Requires: pip install pygba  (pulls in the mGBA bindings)
          plus the mGBA shared library, e.g. apt-get install libmgba0.10
"""
import argparse
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


def start_game(core):
    """Gets past the title screen into a game."""
    run(core, 8)
    core.set_keys(KEY_START)
    run(core, 4)
    core.set_keys()
    run(core, 8)


def selftest(rom_path):
    core, screen = load(rom_path)
    failures = []

    # The title screen must come up first and must not be blank.
    run(core, 8)
    title = pixels(screen)
    if all(p == (0, 0, 0) for row in title for p in row):
        failures.append("la pantalla de titulo quedo en negro")

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--png")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest(args.rom))

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
