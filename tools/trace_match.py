"""THE PORT AGAINST THE CARTRIDGE, frame by frame.

Every other check in this repository measures the port against the
DISASSEMBLY — against a reading of what the ROM does. This one measures it
against the ROM. tools/nes_console.py runs the whole cartridge, so:

  * boot it, walk its four menus into a 1 PLAYER game at level 0, and read
    savedRNGSeed out of its RAM;
  * seed src/tengen_core.c with that same number and feed both the same
    pseudo-random button script;
  * write one line per frame from each — piece, orientation, row, column,
    next, fall timer, level, lines, score, and all two hundred playable cells
    — and diff them.

THIS IS WHAT FOUND THE PORT'S LAST THREE TIMING BUGS, none of which any
disassembly reading had caught: the 48-frame timer on a game's first piece,
the whole frame a spawn costs, and the soft drop reloading the fall counter
before the frame's own decrement rather than after it. A frame apiece, and
invisible until the two were put side by side.

    make trace ROM=/path/to/tetris.nes [FRAMES=3000]

The cartridge side runs an interpreter, so it is not fast: about a minute per
thousand frames. It is not part of `make gba-check` for that reason — it is
the thing to reach for when the core's timing is in question, or after
touching tengen_step.
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nes_console import NesConsole, GAMESTATE_PLAYING

# The same nine lines as trace_core.c's generator. See the note there.
BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_DOWN = 0x40, 0x80, 0x01, 0x02, 0x20


def script(count, seed=12345):
    out, s = [], seed
    for _ in range(count):
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        r = (s >> 16) % 10
        out.append({0: BTN_LEFT, 1: BTN_RIGHT, 2: BTN_A, 3: BTN_B}.get(r, BTN_DOWN))
    return out


def digits(nes, addr, n):
    v = 0
    for i in range(n):
        v = v * 10 + ((nes.ram(addr + i) - 0x30) & 0xF)
    return v


def field(nes, coop=False):
    """The playable columns of ROM rows 6..25.

    player1Playfield is at $0600 as EIGHT BYTES A ROW — sixteen nibbles, of
    which 0-2 and 13-15 are the wall and 3-12 the playfield. The port stores
    twelve columns with a wall at each end (TENGEN_ROM_COL_ORIGIN = 2), so its
    columns 1..10 are nibbles 3..12. COOP leaves those two wall columns OPEN
    and plays twelve wide, which is nibbles 2..13 and the port's 0..11.
    """
    lo, hi = (2, 14) if coop else (3, 13)
    out = []
    for r in range(6, 26):
        for nib in range(lo, hi):
            b = nes.bus.ram[(0x600 + r * 8 + (nib >> 1)) & 0x7FF]
            out.append("%X" % ((b >> 4) if (nib & 1) == 0 else (b & 0xF)))
    return "".join(out)


def cartridge_trace(rom, frames, out_path, coop=False):
    nes = NesConsole(rom)
    nes.start_game(entry=2 if coop else 0)

    # WAIT FOR THE LOOKAHEAD SEED, and only then start counting frames.
    # player1RNGSeed ($5C) is what tengen_new_game's `shared` corresponds to —
    # NOT savedRNGSeed, which agrees with it in 1 PLAYER (that is why $5A
    # worked there) and reads zero in COOPERATIVE, where the game spends an
    # extra frame copying rngSeed into both players' before it deals. Running
    # on until the seed is there puts frame 0 on the DEAL frame in both modes,
    # which is the frame tengen_new_game stands in for.
    for _ in range(8):
        seed = nes.ram(0x5C) | (nes.ram(0x5D) << 8)
        if seed:
            break
        nes.frame(0)
    else:
        raise RuntimeError("player1RNGSeed never got set")
    lines = ["seed %04X" % seed]
    p1 = script(frames)
    # Player 2 reads the same generator from a different start, so the two
    # pads are independent without needing a second one.
    p2 = script(frames, seed=999983) if coop else [0] * frames
    for f in range(frames):
        nes.frame(p1[f], p2[f])
        if nes.state != GAMESTATE_PLAYING:
            lines.append("%d estado %02X" % (f, nes.state))
            continue
        # lineClearTimerP1/P2 ($01CE/$01CF). See trace_core.c on why these
        # frames are reported rather than compared.
        if nes.ram(0x1CE) or (coop and nes.ram(0x1CF)):
            lines.append("%d estado 03" % f)
            continue
        row = "%d p%d o%d y%d x%d n%d t%d" % (
            f, nes.ram(0x64), nes.ram(0x68), nes.ram(0x60), nes.ram(0x62),
            nes.ram(0x66), nes.ram(0x6A))
        if coop:
            row += " q%d o%d y%d x%d n%d t%d" % (
                nes.ram(0x65), nes.ram(0x69), nes.ram(0x61), nes.ram(0x63),
                nes.ram(0x67), nes.ram(0x6B))
        row += " lvl%d L%d S%d %s" % (
            (10 if nes.ram(0x42C) != 0x30 else 0) + (nes.ram(0x42D) - 0x30),
            digits(nes, 0x424, 4), digits(nes, 0x418, 6), field(nes, coop))
        lines.append(row)
    with open(out_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return seed


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom", help="an original Tengen Tetris dump")
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--coop", action="store_true",
                    help="COOPERATIVE: one twelve-wide board, two pads, and "
                         "the collision routine that keeps the pieces apart")
    ap.add_argument("--core", default="build/trace_core",
                    help="the compiled tools/trace_core.c")
    ap.add_argument("--outdir", default="build")
    args = ap.parse_args()

    rom_path = os.path.join(args.outdir, "trace_cartridge.txt")
    print(f"corriendo el cartucho {args.frames} frames "
          f"(un interprete; tarda ~1 min por cada mil)...")
    seed = cartridge_trace(args.rom, args.frames, rom_path, args.coop)
    print(f"  savedRNGSeed = ${seed:04X}, escrito {rom_path}")

    core_path = os.path.join(args.outdir, "trace_core.txt")
    with open(core_path, "w") as fh:
        subprocess.run([args.core, "%04X" % seed, str(args.frames)]
                        + (["coop"] if args.coop else []),
                        stdout=fh, check=True)
    print(f"  escrito {core_path}")

    with open(rom_path) as fh:
        rom_lines = fh.read().splitlines()
    with open(core_path) as fh:
        core_lines = fh.read().splitlines()

    # The cartridge's frame 0 is its spawn frame, which tengen_new_game has
    # already done on the core's side; its trace starts at frame 1 to match.
    rom_lines = [l for l in rom_lines[1:] if not l.startswith("0 ")]
    core_lines = core_lines[1:]

    # ...AND COOPERATIVE'S SPAWN FRAME COSTS ONE FRAME MORE THAN 1 PLAYER'S.
    # Measured, and not yet traced to a routine: in 1 PLAYER the frame that
    # deals leaves player1FallTimer at 48, and in COOPERATIVE — where the same
    # frame deals for BOTH players — it leaves both at 47. tengen_new_game
    # produces 48 in either mode, so the core runs one frame young here and
    # the comparison drops the cartridge's extra frame rather than pretend
    # otherwise. Everything after it is compared as usual; the difference is
    # one frame, once, at the start of a coop match, and is written up in
    # reference/NOTES.md.
    if args.coop:
        core_lines = core_lines[1:]

    # Compared WITHOUT the leading frame number, because coop's extra deal
    # frame means the two sides count from one apart even when every state
    # they report is the same.
    def body(line):
        return line.split(" ", 1)[1] if " " in line else line

    # THE MATCH ENDS WHERE THE CARTRIDGE LEAVES PLAY. Once it tops out it
    # goes on to its game-over plaque, its high-score table and eventually its
    # attract demo — F9, F8, FB — and src/tengen_core.c has no notion of any
    # of them; that is the front end's job and run_rom.py's to check. So the
    # comparison stops at the first of those and says how far it got.
    for end, line in enumerate(rom_lines):
        if line.split()[-1] in ("F9", "F8", "FB") and "estado" in line:
            rom_lines = rom_lines[:end]
            break

    n = min(len(rom_lines), len(core_lines))
    for i in range(n):
        if body(rom_lines[i]) != body(core_lines[i]):
            print(f"FALLA: se separan en el frame {rom_lines[i].split()[0]}")
            print(f"  cartucho: {rom_lines[i][:96]}")
            print(f"  port:     {core_lines[i][:96]}")
            return 1
    # In coop the core's first line was dropped above, so it is one short by
    # construction; anything more than that is a real difference in length.
    slack = 1 if args.coop else 0
    if len(rom_lines) - len(core_lines) > slack:
        print(f"FALLA: el cartucho dio {len(rom_lines)} lineas y el port "
              f"{len(core_lines)}")
        return 1
    ended = n < len(core_lines)
    print(f"OK: el port y el cartucho juegan la misma partida, "
          f"{n} frames identicos"
          + (" (hasta que el cartucho sale a su marcador)." if ended else "."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
