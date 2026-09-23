"""The whole cartridge, running, with a controller on it.

nes_cpu.py was built to run PIECES of this ROM — the sound engine, with
`strict` to prove it never leaves RAM and the APU; and `sendNametableToPPU`,
with a minimal PPU, to unpack a screen. This is the same interpreter with the
three things a WHOLE game needs, and nothing more:

  * A CONTROLLER. `pollController` ($A400) strobes $4016 and then reads it
    eight times, taking bit 0 into player 1 and bit 1 into the expansion port.
    A shift register latched on the strobe is all that takes. Bit 0 of the pad
    byte is A, matching BUTTON_A = $01 in constants.asm.txt.

  * A VBLANK FLAG THAT CLEARS. nes_cpu's PPUSTATUS returns $80 for ever so
    that the ROM's "wait for vblank" loops fall through. `enablePPURendering`
    ($A465) waits for the opposite — for the flag to CLEAR — so a flag pinned
    high is a loop the game never leaves, and it never leaves its title
    screen. Here the NMI sets it and the read that sees it clears it, which is
    what the hardware does.

  * AN NMI THE CARTRIDGE ASKED FOR. PPUCTRL bit 7 is the enable and the ROM
    clears it whenever it does not want to be interrupted
    (`waitForNMIAndDisablePPURendering`, $A44A). Firing one anyway runs the
    handler at a moment the code is not ready for, which walks over the PPU
    slot machinery: with that bug the game reached GAMESTATE_PLAYING from its
    own title screen with nothing touching the pad.

What it does NOT model: cycle timing (a frame is a fixed number of
instructions, which is plenty for a main loop written to finish inside one),
sprite-0 hit, and the mapper's CHR banking, which only moves tiles about.
None of them reach the game's own state, and the proof of that is
tools/trace_match.py, where the port matches this thing frame for frame.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nes_cpu

BTN = {"A": 0x01, "B": 0x02, "SELECT": 0x04, "START": 0x08,
       "UP": 0x10, "DOWN": 0x20, "LEFT": 0x40, "RIGHT": 0x80}

# GAMESTATE_*, from constants.asm.txt. gameState lives at $29.
GAMESTATE_PLAYING = 0x00
GAMESTATE_PAUSED = 0x01
GAMESTATE_LEVELUP = 0x03
GAMESTATE_GAMEOVER = 0xF9
GAMESTATE_TITLE = 0xFA


class PadBus(nes_cpu.Bus):
    def __init__(self, prg):
        super().__init__(prg, strict=False, ppu=True)
        self.pad = [0, 0]
        self._shift = [0, 0]
        self._strobe = 0
        self.vblank = False

    def read(self, addr):
        addr = addr & 0xFFFF
        if self.has_ppu and 0x2000 <= addr <= 0x3FFF:
            if 0x2000 + ((addr - 0x2000) & 7) == 0x2002:
                self._latch = 0
                was, self.vblank = self.vblank, False
                return 0x80 if was else 0x00
        if addr in (0x4016, 0x4017):
            i = addr - 0x4016
            bit = self._shift[i] & 1
            # Reads past the eighth return 1 on real hardware; the shift in of
            # a 1 gives that for free and costs nothing here.
            self._shift[i] = (self._shift[i] >> 1) | 0x80
            return bit
        return super().read(addr)

    def write(self, addr, value):
        if (addr & 0xFFFF) == 0x4016:
            was, self._strobe = self._strobe, value & 1
            if self._strobe or was:
                self._shift = list(self.pad)
        super().write(addr, value)


class NesConsole:
    """One console, clocked in frames."""

    # A frame's worth of instructions. The NES runs 29780 CPU cycles a frame
    # and this ROM's main loop finishes well inside one, so a generous
    # division by three is more than it ever needs.
    STEPS_PER_FRAME = 29780 // 3

    def __init__(self, path):
        data = open(path, "rb").read()
        if data[:4] != b"NES\x1a":
            raise ValueError(f"{path} is not an iNES file")
        prg = data[16:16 + data[4] * 16384]
        if data[4] == 1:
            prg = prg + prg          # NROM mirrors its single bank into $C000
        self.bus = PadBus(prg)
        self.cpu = nes_cpu.CPU(self.bus)
        self.cpu.pc = self.bus.read(0xFFFC) | (self.bus.read(0xFFFD) << 8)
        self.cpu.sp = 0xFD
        self.nmi = self.bus.read(0xFFFA) | (self.bus.read(0xFFFB) << 8)
        self.frames = 0

    def frame(self, buttons=0, buttons2=0, hooks=None):
        """One frame: the NMI, then a frame's worth of instructions.

        `hooks`, if given, maps an address to a function called (with this
        console) each time the CPU is about to execute the instruction there.
        That is how a caller follows the GAME's loop rather than the NMI's:
        an iteration of it does not always fit in its frame. One that queues
        more tiles than the PPU slots hold waits for the NMI halfway through
        (enableNMIAndWaitForRendering, $A3DB), finishes in the next frame and
        the next iteration runs straight after it — so read at the NMI such a
        frame is half done, and the pad a frame sets is read by the NEXT
        iteration, not its own.
        """
        self.bus.pad[0] = buttons
        self.bus.pad[1] = buttons2
        cpu = self.cpu
        self.bus.vblank = True
        if self.bus._ctrl & 0x80:
            cpu.push((cpu.pc >> 8) & 0xFF)
            cpu.push(cpu.pc & 0xFF)
            cpu.push(cpu.p | 0x20)
            cpu.pc = self.nmi
        if hooks is None:
            for _ in range(self.STEPS_PER_FRAME):
                cpu.step()
        else:
            for _ in range(self.STEPS_PER_FRAME):
                hook = hooks.get(cpu.pc)
                if hook is not None:
                    hook(self)
                cpu.step()
        self.frames += 1

    def run(self, count, buttons=0, buttons2=0):
        for _ in range(count):
            self.frame(buttons, buttons2)

    def tap(self, name, hold=4, gap=10):
        self.run(hold, BTN[name])
        self.run(gap, 0)

    def ram(self, addr):
        return self.bus.ram[addr & 0x7FF]

    @property
    def state(self):
        return self.ram(0x29)

    def start_game(self, entry=0, handicap=0):
        """Title -> GAME TYPE -> LEVEL -> HANDICAP -> MUSIC -> playing.

        `entry` is how far down GAME SELECT's five to go: 0 is 1 PLAYER and 2
        is COOPERATIVE, in the order gameSelectArrowPpuAddrs lists them.
        `handicap` is player 1's, 0-4, set on its own screen with DOWN — which
        raises it there (UP lowers it, wrapping round from 0).

        Leaves the console ON THE GAME'S OWN FIRST FRAME, which matters: the
        cartridge deals its first piece during that frame, so anything that
        compares timings has to start counting there and not a menu's worth of
        frames later.
        """
        self.run(60)
        self.tap("START")                 # title -> GAME SELECT
        for _ in range(entry):
            self.tap("DOWN")
        for step in range(3):
            self.tap("START")             # level, handicap, music
            if step == 1:                 # ...now on the handicap screen
                for _ in range(handicap):
                    self.tap("DOWN")
        if self.ram(0x4F3) != handicap:   # menuPlayer1Handicap
            raise RuntimeError(f"handicap {self.ram(0x4F3)}, wanted {handicap}")
        self.frame(BTN["START"])
        for _ in range(60):
            if self.state == GAMESTATE_PLAYING:
                return
            self.frame(0)
        raise RuntimeError("the cartridge never reached GAMESTATE_PLAYING")
