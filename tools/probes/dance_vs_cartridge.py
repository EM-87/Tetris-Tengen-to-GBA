"""THE COSSACKS' CHOREOGRAPHY, port against cartridge, frame by frame.

Each dancer follows a little program of its own (LB015, main.asm.txt:6392),
and the port runs that driver in C over the cartridge's own programs, branch
tables and poses — see the choreography note in gba/hud.c. This is the check
that the C says what the 6502 says.

The reference side is the cartridge's own LB015, run on the interpreter in
tools/nes_cpu.py with the dancers seeded by its own L8D8B and the show's
counter set where the interlude sets it. The port side is the built ROM,
driven into a level-up, read back through OAM — the tiles the dancers are
actually drawn with, not an internal the check would have to be told about.

Both are given the SAME number for the dice: the port writes its seed into
the sound engine's RAM at $34 when the show starts, and this reads it back
out and hands it to the reference. The two then walk the same branches —
which is the whole point of the check, because the dice are SHARED between
the dancers and one roll too many moves every one of them.

BOTH CASTS, because they are not the same dancers: a solo screen stands six
in one column and takes programmes 0-5 out of L8E86, and coop stands eight
down two panels and takes 6-13. Half of coop's are mirrored, and a mirrored
dancer's four tiles go into its sprites the other way round (LB0D8), which
the port does by mirroring the sprites' X instead — so the reference is read
back in pose order rather than in sprite order.

    python3 tools/probes/dance_vs_cartridge.py /path/to/tetris.nes [build/tengen.gba]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
os.chdir(os.path.join(HERE, "..", ".."))

import run_rom as R                      # noqa: E402
from nes_cpu import Bus, CPU             # noqa: E402

DANCER_SETUP = 0x8D8B
DANCER_DRIVER = 0xB015
OAM_STAGING = 0x0500 + 0x40     # the dancers' first sprite (L8DE2's ldx #$40)
RAM_SHOW_TIMER, SHOW_TIMER = 0x006A, 0x7C
RAM_PLAY_MODE = 0x002F
RAM_FRAME_LOW = 0x0032
RAM_RNG_SEED = 0x0034
RAM_P1_ACTIVE, RAM_P2_ACTIVE = 0x004A, 0x004B
RAM_CLEAR_COUNTS = 0x006C
FRAMES = 360                    # six seconds of it
CELL_BLOCK = 15                 # any settled cell; the sweep only reads full


def reference_frames(nes_rom, seed, coop):
    data = open(nes_rom, "rb").read()
    prg = data[16:16 + data[4] * 16384]
    if data[4] == 1:
        prg = prg + prg
    bus = Bus(prg)
    cpu = CPU(bus)
    bus.write(RAM_P1_ACTIVE, 1)
    if coop:
        bus.write(RAM_P2_ACTIVE, 1)
        bus.write(RAM_PLAY_MODE, 0x80)   # L8DAF's `bit playMode / bmi`
    for i in range(8):
        bus.write(RAM_CLEAR_COUNTS + i, 9)
    cpu.call(DANCER_SETUP)
    cast = bus.ram[0x36] - (6 if coop else 0)
    bus.write(RAM_SHOW_TIMER, SHOW_TIMER)
    bus.write(RAM_RNG_SEED, seed & 0xFF)
    bus.write(RAM_RNG_SEED + 1, (seed >> 8) & 0xFF)
    out = []
    for f in range(1, FRAMES + 1):
        bus.write(RAM_FRAME_LOW, f & 0xFF)
        cpu.call(DANCER_DRIVER)
        frame = []
        for d in range(cast):
            at = (OAM_STAGING + d * 0x10) & 0x7FF
            t = [bus.ram[(at + 1 + s * 4) & 0x7FF] for s in range(4)]
            if bus.ram[(at + 2) & 0x7FF] & 0x40:   # LB0D8's mirrored order
                t = [t[1], t[0], t[3], t[2]]
            frame.append(tuple(t))
        out.append(frame)
    return cast, out


def port_frames(gba_rom, coop):
    core, screen = R.load(gba_rom)       # `screen` must stay alive; see load()
    _ = screen
    if coop:
        R.run(core, 8)
        R.press_start(core); R.run(core, 20)        # title -> GAME SELECT
        for _ in range(4):                          # ...down to WITH COMPUTER
            core.set_keys(R.KEYS["DOWN"]); R.run(core, 4)
            core.set_keys(); R.run(core, 8)
        R.press_start(core); R.run(core, 12)
        R.press_start(core); R.run(core, 60)
    else:
        R.start_game(core)
        R.run(core, 40)
    base, why = R.game_state_address(gba_rom)
    if base is None:
        raise RuntimeError(why)
    off = R.game_offsets(gba_rom)
    for who in range(2 if coop else 1):
        at = base + off["lines"] + who * off["stride"]
        for i in range(4):
            core.memory.u8[at + i] = (29 >> (8 * i)) & 0xFF
        at = base + off["counts"] + who * off["stride"]
        for i, n in enumerate((0, 0, 0, 9)):   # tetrises: the full cast walks on
            core.memory.u8[at + i] = n
    if coop:
        # All twelve columns are playable on a shared board, so a complete
        # row is a complete row — no walls to leave standing.
        for row in (18, 19):
            for col in range(R.PF_W):
                core.memory.u8[base + off["field"] + row * R.PF_W + col] = CELL_BLOCK
    else:
        R.fill_rows(core, base + off["field"], [18, 19])
    core.set_keys(R.KEYS["DOWN"])
    for _ in range(500):
        core.run_frame()
        if core.memory.u8[base + off["level"]] != 0:
            break
    core.set_keys()

    ram, why = R.game_state_address(gba_rom, "g_nes_ram")
    if ram is None:
        raise RuntimeError(why)
    seed = core.memory.u8[ram + RAM_RNG_SEED] | \
        (core.memory.u8[ram + RAM_RNG_SEED + 1] << 8)

    out = []
    for _ in range(FRAMES):
        core.run_frame()
        out.append([tuple(core.memory.u16[R.OAM_ADDR + (d * 4 + s) * 8 + 4] & 0x3FF
                           for s in range(4)) for d in range(8)])
    return seed, out


def compare(nes_rom, gba_rom, coop):
    name = "COOPERATIVO" if coop else "EN SOLITARIO"
    seed, mine = port_frames(gba_rom, coop)
    cast, theirs = reference_frames(nes_rom, seed, coop)
    mine = [f[:cast] for f in mine]
    print(f"  {name}: semilla comun ${seed:04X}, {cast} bailarines, "
           f"{FRAMES} frames")

    # The port's show starts a frame or two after the level turns over, so the
    # two are lined up on their first differing pose rather than on frame 0.
    best, offset = -1, 0
    for shift in range(0, 16):
        n = sum(1 for i in range(FRAMES - shift) if mine[i + shift] == theirs[i])
        if n > best:
            best, offset = n, shift
    print(f"  alineados con {offset} frames de desfase: "
           f"{best} de {FRAMES - offset} frames identicos")

    if best < FRAMES - offset:
        for i in range(FRAMES - offset):
            if mine[i + offset] != theirs[i]:
                print(f"FALLA: frame {i}: port {mine[i + offset]!r}")
                print(f"                  cartucho {theirs[i]!r}")
                return 1
    poses = len({p for f in theirs for p in f})
    print(f"  OK: los {cast} bailan lo que baila el cartucho, {poses} poses "
           f"distintas entre ellos.")
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    nes_rom = sys.argv[1]
    gba_rom = sys.argv[2] if len(sys.argv) > 2 else "build/tengen.gba"
    bad = 0
    for coop in (False, True):
        bad |= compare(nes_rom, gba_rom, coop)
    return bad


if __name__ == "__main__":
    sys.exit(main())
