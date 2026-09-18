"""THE PORT'S TUNES AGAINST THE CARTRIDGE'S, in play, note by note.

`make gba-check --audio` compares the port's sound engine with a golden
recording, but the golden is the reference interpreter playing the TITLE
theme on a fresh engine, and it is four hundred frames long. It cannot see
what the cartridge does to the engine before a tune starts, and it never
hears a tune of the game itself. This does: it boots an original dump in
tools/nes_console.py, walks its menus into a 1 PLAYER game with a tune
chosen, and reads the APU register file out of the cartridge every frame;
then it does the same to the port, through mGBA, and lines up the two as
runs of held notes on the pulse 2 and triangle channels — a run is one
register state and how many frames it lasted. Every run has to agree, state
and length, except the first (the port starts its tune a few frames earlier
on the way in) and the last (cut by the window).

Pulse 1 is left out on purpose: the cartridge's sound effects live there,
and the two games' pieces do not land on the same frame.

THIS IS WHAT FOUND THE MISSING ENGINE RESET: LOGINSKA on the port skipped
the repeat of its first strain, because the cartridge queues track id $00
on its first frame after power-on and the port never had — the engine's
free list of voice slots was empty, and the tune played minus one voice.
See NES_AUDIO_RESET in gba/nes_audio.h.

    python3 tools/probes/tune_vs_cartridge.py /path/to/tetris.nes [build/tengen.gba] [FRAMES]

About half a minute a tune; the cartridge side is an interpreter.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
os.chdir(os.path.join(HERE, "..", ".."))

import run_rom as R                                   # noqa: E402
from nes_console import NesConsole, BTN, GAMESTATE_PLAYING   # noqa: E402

# musicSelectTable's order past NO MUSIC, on both machines.
TUNES = ("LOGINSKA", "BRADINSKY", "KARINKA", "TROIKA")
CHANNELS = (("pulso 2", 4, 8), ("triangulo", 8, 12))


def cartridge(rom, tune, frames):
    """The cartridge's APU register file per frame, in play with `tune`."""
    nes = NesConsole(rom)
    nes.run(60)
    for _ in range(4):
        nes.tap("START")          # title, GAME SELECT, LEVEL, HANDICAP -> MUSIC
    for _ in range(tune):
        nes.tap("DOWN")           # the music page: NO MUSIC, then the four
    nes.frame(BTN["START"])
    for _ in range(60):
        if nes.state == GAMESTATE_PLAYING:
            break
        nes.frame(0)
    else:
        raise RuntimeError("the cartridge never reached GAMESTATE_PLAYING")
    out = []
    for _ in range(frames):
        nes.frame(0)
        out.append(bytes(nes.bus.apu[:0x18]))
    return out


def port(gba, tune, frames):
    core, screen = R.load(gba)        # `screen` must stay alive; see load()
    base, why = R.game_state_address(gba, "g_cpu")
    if base is None:
        raise RuntimeError(why)
    probe, why = R.nes6502_probe(gba, core)
    if probe is None:
        raise RuntimeError(why)
    apu_off = base + probe[0] - 0x03000000
    iwram = core.memory.iwram
    R.start_game(core, tune=tune)
    out = []
    for _ in range(frames):
        core.run_frame()
        out.append(bytes(iwram[apu_off:apu_off + 0x18]))
    return out


def runs(seq, lo, hi):
    out = []
    for s in seq:
        k = s[lo:hi]
        if out and out[-1][0] == k:
            out[-1][1] += 1
        else:
            out.append([k, 1])
    return out


MAX_LEAD = 6      # notes the port may be into the tune before the count starts


def compare(name, a, b):
    """Every run of both, state and length, from the cartridge's second run.

    The first run of each is cut short by where the count started, and the
    two counts do not start on the same frame of the tune: the cartridge's
    starts on its first frame of play, the port's a few frames later, once
    the harness has let go of START. So the port's run that matches the
    cartridge's second is looked for among its first few, and the rest are
    compared from there; the last run of each is cut by the window and free.
    """
    failures = []
    # Either may be the one further into the tune when its count starts —
    # the port usually is, by the frames the harness holds START — so the
    # match of three runs is looked for both ways round.
    pairs = [(1, j) for j in range(1, MAX_LEAD + 1)] + \
            [(i, 1) for i in range(2, MAX_LEAD + 1)]
    start = next(((i, j) for i, j in pairs if a[i:i + 3] == b[j:j + 3]), None)
    if start is None:
        return [f"{name}: el port no toca las notas del cartucho "
                f"({', '.join(k.hex() + ' x' + str(n) for k, n in a[1:4])} ...)"], 0
    ia, ib = start
    n = min(len(a) - ia, len(b) - ib) - 1
    for i in range(n):
        (ka, na), (kb, nb) = a[ia + i], b[ib + i]
        if ka != kb or na != nb:
            failures.append(f"{name}: nota {ia + i}: cartucho {ka.hex()} x{na}, "
                            f"port {kb.hex()} x{nb}")
            if len(failures) >= 3:
                break
    return failures, n


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    rom = sys.argv[1]
    gba = sys.argv[2] if len(sys.argv) > 2 else "build/tengen.gba"
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
    failures = []
    for tune, name in enumerate(TUNES, start=1):
        a, b = cartridge(rom, tune, frames), port(gba, tune, frames)
        for chan, lo, hi in CHANNELS:
            bad, n = compare(f"{name} {chan}", runs(a, lo, hi), runs(b, lo, hi))
            print(f"  {name:<10} {chan:<10} {n - 2} notas comparadas"
                  + ("" if not bad else f", {len(bad)} discrepancias"))
            failures += bad
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print(f"OK: las cuatro melodias del cartucho suenan nota por nota como en el "
          f"original durante {frames} frames de partida.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
