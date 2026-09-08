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

# Must match gba/main.c's FIELD_ORIGIN_TX / TENGEN_PF_WIDTH.
FIELD_TILES_W = 12
FIELD_ORIGIN_TX = (30 - FIELD_TILES_W) // 2
FIELD_X0 = FIELD_ORIGIN_TX * TILE
FIELD_X1 = FIELD_X0 + FIELD_TILES_W * TILE


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

    # Column occupancy profile, to see where the field actually landed.
    cols = [sum(1 for y in range(SCREEN_H) if rows[y][x] != (0, 0, 0))
            for x in range(SCREEN_W)]
    lit = [x for x, c in enumerate(cols) if c > 0]
    if lit:
        print(f"columnas con contenido: {min(lit)}..{max(lit)} "
              f"(esperado {FIELD_X0}..{FIELD_X1 - 1})")
    else:
        print("columnas con contenido: ninguna (pantalla vacia)")
    return cols


def selftest(rom_path):
    core, screen = load(rom_path)
    failures = []

    run(core, 8)
    rows = pixels(screen)
    cols = describe(rows)

    if all(p == (0, 0, 0) for row in rows for p in row):
        failures.append("la pantalla quedo completamente negra")

    lit = [x for x, c in enumerate(cols) if c > 0]
    if lit:
        if min(lit) != FIELD_X0 or max(lit) != FIELD_X1 - 1:
            failures.append(
                f"el campo no cae donde dice el mapeo de resolucion: "
                f"ocupa {min(lit)}..{max(lit)}, esperado {FIELD_X0}..{FIELD_X1 - 1}")
        # The frame runs the full height of the screen: that is the whole
        # point of the 160px mapping.
        wall_col = cols[FIELD_X0]
        if wall_col != SCREEN_H:
            failures.append(
                f"la columna de muro izquierda no ocupa todo el alto "
                f"({wall_col}/{SCREEN_H} px)")

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
    run(core, args.frames)
    describe(pixels(screen))
    if args.png:
        with open(args.png, "wb") as fh:
            screen.save_png(fh)
        print(f"guardado {args.png}")


if __name__ == "__main__":
    main()
