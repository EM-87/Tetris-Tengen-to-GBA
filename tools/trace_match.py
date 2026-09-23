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

GAMESTATE_DEMO = 0xFB

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


def field(nes, coop=False, base=0x600):
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
            b = nes.bus.ram[(base + r * 8 + (nib >> 1)) & 0x7FF]
            out.append("%X" % ((b >> 4) if (nib & 1) == 0 else (b & 0xF)))
    return "".join(out)


# One iteration of the game's main loop starts at the `sta` after its wait
# (main.asm.txt:54, before pollController reads the pads) and ends at its
# `jmp mainLoop` (:77).
ITERATION_START = 0x8015
ITERATION_END = 0x804D


def solo_row(nes, f, coop):
    """One iteration of 1 PLAYER or COOPERATIVE, in trace_core.c's shape."""
    # lineClearTimerP1/P2 ($01CE/$01CF). See trace_core.c on why these
    # frames are reported rather than compared.
    if nes.ram(0x1CE) or (coop and nes.ram(0x1CF)):
        return "%d estado 03" % f
    row = "%d p%d o%d y%d x%d n%d t%d" % (
        f, nes.ram(0x64), nes.ram(0x68), nes.ram(0x60), nes.ram(0x62),
        nes.ram(0x66), nes.ram(0x6A))
    if coop:
        row += " q%d o%d y%d x%d n%d t%d" % (
            nes.ram(0x65), nes.ram(0x69), nes.ram(0x61), nes.ram(0x63),
            nes.ram(0x67), nes.ram(0x6B))
    return row + " lvl%d L%d S%d %s" % (
        (10 if nes.ram(0x42C) != 0x30 else 0) + (nes.ram(0x42D) - 0x30),
        digits(nes, 0x424, 4), digits(nes, 0x418, 6), field(nes, coop))


def versus_row(nes, f, shared=False):
    """One frame of VERSUS COMPUTER, both boards, in trace_core.c's shape.

    Player 2's copies of everything sit one byte (or one table) past player
    1's: $61-$6B interleaved, score at $041E, lines at $0428, level at
    $042E, and its playfield at $0700.
    """
    row = "%d" % f
    for i in range(2):
        if not nes.ram(0x4A + i):
            row += " | fin"
            continue
        if nes.ram(0x1CE + i):
            row += " | limpia"
            continue
        lvl = 0x42C + 2 * i
        row += " | p%d o%d y%d x%d n%d t%d lvl%d L%d S%d %s" % (
            nes.ram(0x64 + i), nes.ram(0x68 + i), nes.ram(0x60 + i),
            nes.ram(0x62 + i), nes.ram(0x66 + i), nes.ram(0x6A + i),
            (10 if nes.ram(lvl) != 0x30 else 0) + (nes.ram(lvl + 1) - 0x30),
            digits(nes, 0x424 + 4 * i, 4), digits(nes, 0x418 + 6 * i, 6),
            field(nes, coop=True) if shared
            else field(nes, base=0x600 + 0x100 * i))
    return row + " | T%d,%d" % (nes.ram(0x1CA), nes.ram(0x1CB))


def cartridge_trace(rom, frames, out_path, coop=False, vs=False, with_=False,
                    pad1=None, handicap=0, demo=False, info=None):
    nes = NesConsole(rom)
    if demo:
        # THE ATTRACT DEMO, which the title starts by itself at frameCounterHigh
        # 5 / low $20 (main.asm.txt:4150-4160). Nothing is pressed on the way.
        for _ in range(3000):
            nes.frame(0)
            if nes.state == GAMESTATE_DEMO:
                break
        else:
            raise RuntimeError("the cartridge's demo never started")
    else:
        # GAME SELECT's entries are menuGameMode's values: 1 PLAYER, 2 PLAYER,
        # COOPERATIVE, VERSUS (the computer), WITH (the computer).
        nes.start_game(entry=4 if with_ else 3 if vs else 2 if coop else 0,
                       handicap=handicap)
    computer = vs or with_ or demo

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
    clocks = []

    # The demo is left alone: a button pressed on it ends it.
    # A few entries past `frames`: read per iteration, the cartridge fits one
    # more iteration than frames into its run (its deal overruns into the
    # next frame), and that one must get its button too.
    p1 = (pad1 if pad1 is not None else [0] * (frames + 2) if demo
          else script(frames + 2))
    # Player 2 reads the same generator from a different start, so the two
    # pads are independent without needing a second one. In the computer
    # modes pad 2 is the computer's: compInputForGameplay writes it.
    p2 = script(frames + 2, seed=999983) if coop else [0] * (frames + 2)

    # THE TRACE FOLLOWS THE GAME'S LOOP, NOT THE FRAME: iteration i of
    # mainLoop is handed p1[i] and p2[i] as it starts and read as it ends
    # (NesConsole.frame on why the two differ — the deal itself overruns its
    # frame, in every mode). The deal is iteration 0, as in the port.
    def iteration_start(n):
        i = len(lines) - 1
        n.bus.pad[0] = p1[i] if i < len(p1) else 0
        n.bus.pad[1] = p2[i] if i < len(p2) else 0
        clocks.extend([None] * (i - len(clocks)))
        clocks.append(n.ram(0x32))      # frameCounterLow, as this one reads it

    def iteration_end(n):
        i = len(lines) - 1
        if n.state not in (GAMESTATE_PLAYING, GAMESTATE_DEMO):
            lines.append("%d estado %02X" % (i, n.state))
        elif computer:
            lines.append(versus_row(n, i, shared=with_))
        else:
            lines.append(solo_row(n, i, coop))
    hooks = {ITERATION_START: iteration_start, ITERATION_END: iteration_end}
    if info is not None:
        info["level"] = ((10 if nes.ram(0x42C) != 0x30 else 0)
                         + (nes.ram(0x42D) - 0x30))
    for _ in range(frames):
        nes.frame(0, 0, hooks=hooks)
    with open(out_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return seed, clocks


def computer_match(args):
    """VERSUS and WITH COMPUTER: the script on pad 1, computerMove on pad 2.

    Both sides print both players and compTargetX/Orientation, so a
    difference in the computer's CHOICE shows on the frame it is made, not
    thirty frames later where the piece lands. The computer's cadence runs
    off frameCounterLow; the cartridge's value on the first compared frame
    is handed to the core, which counts on from it.

    Read once per iteration, both deal when 1 PLAYER does (the timers at
    48), so the cartridge's deal line is dropped as it is there.
    """
    mode = "demo" if args.demo else "with" if args.with_ else "vs"
    skip = 1
    info = {}
    rom_path = os.path.join(args.outdir, "trace_cartridge.txt")
    core_path = os.path.join(args.outdir, "trace_core.txt")

    def run_core(seed, clock, extra=()):
        with open(core_path, "w") as fh:
            subprocess.run([args.core, "%04X" % seed,
                            str(args.frames + 1 - skip), mode, str(clock)]
                           + list(extra) + ["h=%d" % args.handicap,
                                            "l=%d" % info.get("level", 0)],
                           stdout=fh, check=True)

    # --pad1: the core plays player 1 with the port's own computer and the
    # cartridge is handed the buttons it pressed. That needs the seed and
    # the clock first, and the boot is deterministic, so a short run of the
    # cartridge gets them before the real one.
    pad1 = None
    if args.pad1:
        seed, clocks = cartridge_trace(args.rom, skip + 2, rom_path,
                                       vs=args.versus, with_=args.with_,
                                       handicap=args.handicap)
        run_core(seed, clocks[skip], ["pad1"])
        with open(core_path) as fh:
            pressed = [int(l.rsplit(" B", 1)[1])
                       for l in fh.read().splitlines()[1:]]
        pad1 = [0] * skip + pressed + [0] * args.frames

    print(f"corriendo el cartucho {args.frames} frames "
          f"(un interprete; tarda ~15s por cada mil)...")
    seed, clocks = cartridge_trace(args.rom, args.frames, rom_path,
                                   vs=args.versus, with_=args.with_,
                                   pad1=pad1, handicap=args.handicap,
                                   demo=args.demo, info=info)
    if args.demo:
        print(f"  la demo del cartucho: semilla ${seed:04X}, nivel "
              f"{info['level']}")
    run_core(seed, clocks[skip], ["pad1"] if args.pad1 else [])

    def body(line):     # the core's pressed-buttons field is not compared
        return line.split(" ", 1)[1].split(" | B")[0]

    def same(rom_line, core_line):
        return body(rom_line) == body(core_line)
    with open(rom_path) as fh:
        rom_lines = fh.read().splitlines()[1 + skip:]
    with open(core_path) as fh:
        core_lines = fh.read().splitlines()[1:]
    for end, line in enumerate(rom_lines):
        if "estado" in line:
            rom_lines = rom_lines[:end]
            break
    n = min(len(rom_lines), len(core_lines))
    for i in range(n):
        if not same(rom_lines[i], core_lines[i]):
            print(f"FALLA: se separan en el frame {rom_lines[i].split()[0]}")
            for name, l in (("cartucho", rom_lines[i]), ("port", core_lines[i])):
                parts = l.split(" | ")
                print(f"  {name}: " + " | ".join(p[:34] for p in parts[1:]))
            return 1
    clears = sum(" | limpia" in l for l in rom_lines[:n])
    print(f"OK: el port y el cartucho juegan la misma partida contra la "
          f"maquina, {n} frames identicos ({clears} de ellos limpiando).")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom", help="an original Tengen Tetris dump")
    ap.add_argument("--frames", type=int, default=3000)
    ap.add_argument("--coop", action="store_true",
                    help="COOPERATIVE: one twelve-wide board, two pads, and "
                         "the collision routine that keeps the pieces apart")
    ap.add_argument("--versus", action="store_true",
                    help="VERSUS COMPUTER: two boards, the script against "
                         "the cartridge's computer")
    ap.add_argument("--with", dest="with_", action="store_true",
                    help="WITH COMPUTER: the same computer on the shared "
                         "twelve-wide board")
    ap.add_argument("--demo", action="store_true",
                    help="the attract demo: the title left alone until it "
                         "starts playing by itself, computerMove on pad 1")
    ap.add_argument("--pad1", action="store_true",
                    help="with --versus/--with: player 1 played by the "
                         "port's computer, its buttons pressed on both")
    ap.add_argument("--handicap", type=int, default=0, choices=range(5),
                    help="player 1 starts under this many steps of garbage, "
                         "three rows a step")
    ap.add_argument("--core", default="build/trace_core",
                    help="the compiled tools/trace_core.c")
    ap.add_argument("--outdir", default="build")
    args = ap.parse_args()
    if args.versus or args.with_ or args.demo:
        return computer_match(args)

    rom_path = os.path.join(args.outdir, "trace_cartridge.txt")
    print(f"corriendo el cartucho {args.frames} frames "
          f"(un interprete; tarda ~15s por cada mil)...")
    seed, _ = cartridge_trace(args.rom, args.frames, rom_path, args.coop,
                              handicap=args.handicap)
    print(f"  savedRNGSeed = ${seed:04X}, escrito {rom_path}")

    core_path = os.path.join(args.outdir, "trace_core.txt")
    with open(core_path, "w") as fh:
        # One more than frames: counted per iteration, the cartridge fits
        # one extra into the frames it ran (its deal overruns into the next).
        subprocess.run([args.core, "%04X" % seed, str(args.frames + 1)]
                        + (["coop"] if args.coop else ["solo"])
                        + ["h=%d" % args.handicap],
                        stdout=fh, check=True)
    print(f"  escrito {core_path}")

    with open(rom_path) as fh:
        rom_lines = fh.read().splitlines()
    with open(core_path) as fh:
        core_lines = fh.read().splitlines()

    # The cartridge's iteration 0 is its deal, which tengen_new_game has
    # already done on the core's side; its trace starts at 1 to match. (In
    # COOPERATIVE as in 1 PLAYER: read per iteration, both deal with the
    # timers at 48. Read at the NMI, coop's looked like 47 and a frame late —
    # its deal overruns the frame, and so did every mode's reading of it.)
    rom_lines = rom_lines[2:]
    core_lines = core_lines[1:]

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
    ended = n < len(core_lines)
    print(f"OK: el port y el cartucho juegan la misma partida, "
          f"{n} frames identicos"
          + (" (hasta que el cartucho sale a su marcador)." if ended else "."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
