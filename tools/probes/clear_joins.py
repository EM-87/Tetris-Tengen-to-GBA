"""THE JOINS A CLEARED ROW BREAKS, port against cartridge, side by side.

The fourteen block graphics each draw the separator for their own top and
left edges and none where the cell is joined to a piece-mate, and which of
the fourteen a cell gets is settled when its piece LOCKS. A row going away
leaves lies behind it — the cell above still claims a mate below, the cell
below still claims one above — and the cartridge rewrites both through
L8A85's table (main.asm.txt:1589-1624) so it does not.

`make trace` would catch a port that got this wrong, and does not, for a
plain reason: its button script is pseudo-random and never completes a row
in three thousand frames. So this plants the row instead. Each case below
puts the same field into an original dump and into the built ROM, lets a
piece land to trigger the clear in both, and compares the cell the clear was
supposed to rewrite.

    python3 tools/probes/clear_joins.py /path/to/tetris.nes [build/tengen.gba]

About a minute; the cartridge side is an interpreter.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
os.chdir(os.path.join(HERE, "..", ".."))

import run_rom as R                              # noqa: E402
from nes_console import NesConsole, BTN          # noqa: E402

# The cartridge's playfield: eight bytes a row, sixteen nibbles, rows 6..25,
# with the ten playable ones at nibbles 3..12. The port stores twelve columns
# a row with the walls in the outer two, so its row r is the cartridge's
# 6 + r and its column c is the cartridge's nibble 2 + c.
NES_PF = 0x600
NES_ROW0, NES_NIB0 = 6, 3
CUT, COL = 17, 3            # port coordinates: the row that clears, and a
                            # column well left of where pieces spawn

# (what sits above the cut, what sits below it, what it is called)
CASES = (
    (0x01, 0x06, "$01 sobre $06: el de abajo pierde su union hacia arriba"),
    (None, 0x06, "nada encima, $06 debajo: la pierde igual"),
    (0x01, 0x05, "$05 debajo: pierde arriba y conserva abajo"),
    (0x01, 0x03, "$03 debajo: no dice unirse hacia arriba, no se toca"),
    (0x04, 0x06, "$04 encima: pierde su union hacia abajo"),
)


def cartridge(rom, above, below):
    def cell(nes, r, n):
        byte = nes.bus.ram[(NES_PF + r * 8 + (n >> 1)) & 0x7FF]
        return (byte >> (4 if (n & 1) == 0 else 0)) & 0xF

    def put(nes, r, n, v):
        a = (NES_PF + r * 8 + (n >> 1)) & 0x7FF
        b = nes.bus.ram[a]
        nes.bus.ram[a] = ((b & 0x0F) | (v << 4)) if (n & 1) == 0 else ((b & 0xF0) | v)

    nes = NesConsole(rom)
    nes.start_game()
    nes.run(30)
    row, nib = NES_ROW0 + CUT, NES_NIB0 + COL - 1
    for n in range(3, 13):
        put(nes, row, n, 0x02)
    if above is not None:
        put(nes, row - 1, nib, above)
    put(nes, row + 1, nib, below)
    for f in range(400):
        nes.frame(BTN["DOWN"] if f < 300 else 0)
        if f > 120 and cell(nes, row, nib) != 0x02:
            break
    nes.run(40)
    return cell(nes, row, nib), cell(nes, row + 1, nib)


def port(rom, above, below):
    core, screen = R.load(rom)      # `screen` must stay alive; see load()
    _ = screen
    R.start_game(core)
    R.run(core, 40)
    base, why = R.game_state_address(rom)
    if base is None:
        raise RuntimeError(why)
    off = R.game_offsets(rom)
    field = base + off["field"]
    W = R.TENGEN_PF_WIDTH

    def cell(r, c):
        return core.memory.u8[field + r * W + c]

    for c in range(1, W - 1):
        core.memory.u8[field + CUT * W + c] = 0x02
    if above is not None:
        core.memory.u8[field + (CUT - 1) * W + COL] = above
    core.memory.u8[field + (CUT + 1) * W + COL] = below
    core.set_keys(R.KEYS["DOWN"])
    for f in range(400):
        core.run_frame()
        if f > 60 and cell(CUT, COL) != 0x02:
            break
    core.set_keys()
    R.run(core, 40)
    return cell(CUT, COL), cell(CUT + 1, COL)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    nes_rom = sys.argv[1]
    gba_rom = sys.argv[2] if len(sys.argv) > 2 else "build/tengen.gba"
    failures = []
    for above, below, label in CASES:
        want = cartridge(nes_rom, above, below)
        got = port(gba_rom, above, below)
        mark = "" if want == got else "   <-- NO COINCIDE"
        print(f"  {label}")
        print(f"      cartucho ${want[0]:02X} ${want[1]:02X}   "
               f"port ${got[0]:02X} ${got[1]:02X}{mark}")
        if want != got:
            failures.append(label)
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el port rompe las mismas uniones que el cartucho al limpiar "
           "una fila.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
