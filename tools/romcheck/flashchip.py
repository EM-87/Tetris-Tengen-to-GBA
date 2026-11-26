"""
A flash chip for the emulator's cartridge.

mGBA's cartridge is a ROM: a write to it goes nowhere and nothing answers.
The boards the port keeps its save on in flash (gba/cartsave.c) are not —
the game is in a NOR flash chip that takes commands, and while it is taking
one it is not a ROM at all. Nothing of that can be seen in a stock emulator,
and none of it can be tried on the real thing from here. So this stands a
chip in: the port's own routines, the ones that will run on the board, are
run one instruction at a time against a model that behaves as the
datasheets say such a chip does and as the board's wiring makes it look.

HOW. The routines that talk to the chip live in external WRAM (.ramcode).
The first instruction of each is replaced, in the ROM's copy of them, by a
branch to itself, so a console that calls one spins there with the frames
going by; `Cart.run` sees that, puts the instruction back, and steps the
routine to its return. Each store it makes to the cartridge is decoded
before it runs and handed to the chip, and whatever the chip would then
answer is written into the emulator's ROM memory for the routine's next
read to find: its query table, its identity, its status while it is busy,
the erased sector, the programmed word.

WHAT THE MODEL HOLDS THE PORT TO, beyond getting the commands right:

  * nothing runs from the cartridge while the chip is not a ROM, and no
    interrupt is taken (`Cart.faults`): on the board that is a crash;
  * bits are only ever cleared by programming, a word that would need one
    set fails as the chip fails it, and only whole sectors are erased;
  * nothing is ever programmed or erased below `guard`, where the program
    is;
  * the power can go at any instruction (`Cart.cut_after`), leaving the
    word half programmed or the sector half erased.

...and what it can be made to do, for the checks that need a board that
is not well: sectors that refuse (`deaf`), that say they erased and did not
quite (`weak`), single words that will not take (`deaf_words`) or that read
back right and then lose a bit (`fickle`), a word that never finishes
(`slow_words`), a chip deaf to the unlock sequence (`unlock_bits`), a reset
that takes its time (`reset_cycles`, always), and a cartridge with no save
RAM beside the chip at all (`Cart(no_ram=True)`).

It is a model. A chip that does something no datasheet says is not in it.
"""
import os
import random
import subprocess

from mgba import ffi

from .harness import game_state_address, load

ROM = 0x08000000
ROM_SPAN = 0x02000000
SRAM = 0x0E000000
SRAM_BYTES = 0x8000

WIRE_STRAIGHT, WIRE_D0D1, WIRE_D0D1_D6D7 = 0, 1, 2

TRAP = 0xE7FE           # Thumb: b .
CPU_HZ = 16777216


def wire(wiring, value):
    """A byte through a board's data lines, either way. Written out again
    here ON PURPOSE, as harness.ORIENTATION_BITS is: a check that asked
    src/tengen_cfi.c how the lines cross would be marking its own work."""
    v = value & 0xFF
    if wiring in (WIRE_D0D1, WIRE_D0D1_D6D7):
        v = (v & ~0x03) | ((v & 1) << 1) | ((v >> 1) & 1)
    if wiring == WIRE_D0D1_D6D7:
        v = (v & ~0xC0) | ((v & 0x40) << 1) | ((v >> 1) & 0x40)
    return v


def query_table(command_set, size_log2, regions, boot=5):
    """The CFI table of a chip, a byte a word, shaped as the datasheets
    print it (JESD68.01): 128 words, the vendor's own part at 0x40."""
    t = bytearray(0x80)
    t[0x10:0x13] = b"QRY"
    t[0x13], t[0x14] = command_set & 0xFF, command_set >> 8
    t[0x15] = 0x40
    t[0x1B], t[0x1C] = 0x27, 0x36
    t[0x1F], t[0x20], t[0x21], t[0x22] = 0x06, 0x06, 0x09, 0x13
    t[0x23], t[0x24], t[0x25], t[0x26] = 0x03, 0x05, 0x03, 0x02
    t[0x27] = size_log2
    t[0x28] = 0x02
    t[0x2A] = 0x06
    t[0x2C] = len(regions)
    for i, (blocks, size) in enumerate(regions):
        at = 0x2D + 4 * i
        t[at], t[at + 1] = (blocks - 1) & 0xFF, (blocks - 1) >> 8
        t[at + 2], t[at + 3] = (size // 256) & 0xFF, (size // 256) >> 8
    t[0x40:0x45] = b"PRI13"
    t[0x4F] = boot
    return t


class FlashChip:
    """A NOR flash that takes AMD's commands (or, `intel`, one that takes
    Intel's and is only ever asked what it is), behind a board's wiring.
    `mem` is the emulator's ROM memory, which is what it stores into."""

    READ, QUERY, IDENT, PROGRAM, ERASE, STUCK = range(6)
    WINDOW = 0x100          # bytes of the chip that show what mode it is in

    def __init__(self, mem, wiring=WIRE_D0D1, command_set=2, size_log2=24,
                 regions=((128, 0x20000),), maker=0x0001, device=0x227E,
                 boot=5, program_cycles=1000, erase_cycles=300000,
                 guard=0x100000, deaf=(), weak=(), fickle=(), slow_words=0,
                 unlock_bits=0x7FF, reset_cycles=400, seed=1):
        self.mem = mem
        self.wiring = wiring
        self.command_set = command_set
        self.intel = command_set in (1, 3, 0x200)
        self.size = 1 << size_log2
        self.regions = regions
        self.table = query_table(command_set, size_log2, regions, boot)
        self.maker, self.device = maker, device
        self.program_cycles = program_cycles
        self.erase_cycles = erase_cycles
        self.guard = guard
        self.deaf = set(deaf)           # sectors that will not erase or take a word
        self.weak = set(weak)           # ...and ones that say they erased and did not quite
        self.fickle = set(fickle)       # words that read back right once and then lose a bit
        self.deaf_words = set()         # single words that will not take
        self.program_log = []           # every word programmed, in order: its address
        self.slow_words = slow_words    # a word that takes this many cycles more, once
        # How much of the address the chip looks at for its unlock writes:
        # eleven bits on these chips (word 0x555), fifteen on some older
        # ones (0x5555), which the cartridge's sequences do not reach.
        self.unlock_at = (0x5555 & unlock_bits, 0x2AAA & unlock_bits)
        self.unlock_bits = unlock_bits
        # A reset is not instant: the chip goes on showing what it was
        # showing for some microseconds (the datasheets' "reset to read").
        self.reset_cycles = reset_cycles
        self.leaving = None             # when the reset under way is over
        self.faded = None
        self.told_while_stuck = 0       # writes to a chip that had given up, bar the reset
        self.doomed = 0                 # words asked to set a bit that was 0
        self.pointless = 0              # ...and to become what they already were
        self.rng = random.Random(seed)
        self.mode = self.READ
        self.unlock = 0
        self.armed = None
        self.shown = {}                 # what the chip's mode has painted over, to put back
        self.toggle = 0
        self.faults = []
        self.strays = []                # writes that mean nothing to the chip
        self.erased = []                # sector starts, in the order erased
        self.programmed = 0             # words
        self.program_span = [None, None]
        self.queries = 0
        self.idents = 0

    # -- what is stored ---------------------------------------------------
    def word(self, addr):
        return self.mem[addr][0] | (self.mem[addr + 1][0] << 8)

    def put(self, addr, value):
        self.mem[addr:addr + 2] = bytes((value & 0xFF, value >> 8))

    def sector(self, addr):
        base = 0
        for blocks, size in self.regions:
            if addr < base + blocks * size:
                return base + (addr - base) // size * size, size
            base += blocks * size
        return None, 0

    # -- what it shows while it is not a ROM ------------------------------
    def _show(self, addr, data):
        if addr not in self.shown:
            self.shown[addr] = bytes(self.mem[addr:addr + len(data)])
        self.mem[addr:addr + len(data)] = data

    def _hide(self):
        for addr, data in self.shown.items():
            self.mem[addr:addr + len(data)] = data
        self.shown = {}

    def _words(self, values):
        out = bytearray()
        for v in values:
            out += bytes((wire(self.wiring, v & 0xFF), (v >> 8) & 0xFF))
        return bytes(out)

    def _status(self):
        if self.mode == self.PROGRAM:
            st = (~self.pd & 0x80) | (0x40 if self.toggle else 0)
        elif self.mode == self.ERASE:
            st = 0x08 | (0x44 if self.toggle else 0)
        else:                           # STUCK: the time limit bit, for good
            st = self.stuck_status | (0x40 if self.toggle else 0)
        return st

    def _show_status(self):
        st = self._words([self._status()])
        self._show(0, st * (self.WINDOW // 2))
        self._show(self.target, st)

    def _enter(self, mode):
        self._hide()
        self.mode = mode
        if mode == self.QUERY:
            self.queries += 1
            words = [self.maker, self.device] + [0] * 14 + list(self.table[0x10:])
            self._show(0, self._words(words))
        elif mode == self.IDENT:
            # Its identity where an identity is, and no more: what the rest
            # of the chip reads as in this mode no datasheet promises, and
            # here it goes on reading as what is stored.
            self.idents += 1
            self._show(0, self._words([self.maker, self.device]))

    def _reset(self, now=None):
        self.unlock = 0
        self.armed = None
        if now is not None and self.mode != self.READ and self.reset_cycles:
            if self.leaving is None:
                self.leaving = now + self.reset_cycles
            return
        self._hide()
        self.mode = self.READ
        self.leaving = None
        if self.faded is not None:
            # A cell that was barely programmed: right while the chip was
            # still warm from it, a bit back to 1 afterwards.
            self.put(self.faded, self.word(self.faded) | 0x0004)
            self.fickle.discard(self.faded)
            self.faded = None

    # -- the bus -----------------------------------------------------------
    def write(self, addr, value, now):
        """A 16-bit write to byte `addr` of the chip."""
        addr &= self.size - 1 & ~1
        word = addr >> 1
        cmd = wire(self.wiring, value)
        if self.mode in (self.PROGRAM, self.ERASE) or self.leaving is not None:
            return                      # busy: deaf to everything
        if self.intel:
            self._intel(addr, cmd, value, now)
            return
        if self.armed == "program":
            self.armed = None
            self._begin_program(addr, value & 0xFFFF, now)
            return
        if cmd == 0xF0:
            self._reset(now)
            return
        if self.mode == self.STUCK:
            self.told_while_stuck += 1  # only a reset is heard
            return
        low = word & self.unlock_bits
        first, second = self.unlock_at
        if self.unlock == 0:
            if cmd == 0xAA and low == first:
                self.unlock = 1
            elif cmd == 0x98 and (word & 0xFF) == 0x55:
                self._enter(self.QUERY)
            else:
                self.strays.append((addr, value))
            return
        if self.unlock == 1:
            if cmd == 0x55 and low == second:
                self.unlock = 2
            else:
                self.unlock = 0
                self.strays.append((addr, value))
            return
        self.unlock = 0
        if self.armed == "erase":
            self.armed = None
            if cmd == 0x30:
                self._begin_erase(addr, now)
            elif cmd == 0x10:
                self.faults.append("the whole chip was told to erase")
            else:
                self.strays.append((addr, value))
            return
        if low == first and cmd == 0x90:
            self._enter(self.IDENT)
        elif low == first and cmd == 0xA0:
            self.armed = "program"
        elif low == first and cmd == 0x80:
            self.armed = "erase"
        else:
            self.strays.append((addr, value))

    def _intel(self, addr, cmd, value, now):
        if cmd == 0xFF:
            self._reset(now)
        elif cmd == 0x98:
            self._enter(self.QUERY)
        elif cmd == 0x90:
            self._enter(self.IDENT)
        else:
            self.strays.append((addr, value))
            # What on such a chip sets a program or an erase up.
            if cmd in (0x10, 0x40, 0x20, 0x60, 0xD0, 0xE8, 0xEA):
                self.faults.append(f"an Intel chip was sent {cmd:#04x}")

    def _begin_program(self, addr, value, now):
        if addr < self.guard:
            self.faults.append(f"a word of the program was programmed ({addr:#x})")
        self.target, self.pd = addr, value
        self.old = self.word(addr)
        self.programmed += 1
        self.program_log.append(addr)
        if self.old & value != value:
            self.doomed += 1
        if self.old == value:
            self.pointless += 1
        lo, hi = self.program_span
        self.program_span = [addr if lo is None else min(lo, addr),
                             addr + 2 if hi is None else max(hi, addr + 2)]
        self.busy_until = now + self.program_cycles + self.slow_words
        self.slow_words = 0
        self.mode = self.PROGRAM
        self._show_status()

    def _begin_erase(self, addr, now):
        base, size = self.sector(addr)
        if base is None:
            self.faults.append(f"an erase past the chip ({addr:#x})")
            return
        if base < self.guard:
            self.faults.append(f"a sector of the program was erased ({base:#x})")
        self.target, self.sector_base, self.sector_size = addr, base, size
        self.erased.append(base)
        self.busy_until = now + self.erase_cycles
        self.mode = self.ERASE
        self._show_status()

    def tick(self, now):
        """Time passing: a busy chip gets on with it."""
        if self.leaving is not None:
            if now >= self.leaving:
                self._reset()
            return
        if self.mode == self.PROGRAM:
            if now < self.busy_until:
                self.toggle ^= 1
                self._show_status()
                return
            base, _ = self.sector(self.target)
            took = self.old & self.pd
            self._hide()
            if took != self.pd or base in self.deaf or self.target in self.deaf_words:
                # A 0 that was asked to be a 1, or a sector or a cell that
                # is gone: the chip's own time limit, and it says so until
                # reset.
                if base not in self.deaf and self.target not in self.deaf_words:
                    self.put(self.target, took)
                self.mode = self.STUCK
                self.stuck_status = (~self.pd & 0x80) | 0x20
                self._show_status()
            else:
                self.put(self.target, took)
                self.mode = self.READ
                if self.target in self.fickle:
                    self.faded = self.target
        elif self.mode == self.ERASE:
            if now < self.busy_until:
                self.toggle ^= 1
                self._show_status()
                return
            self._hide()
            if self.sector_base in self.deaf:
                self.mode = self.STUCK
                self.stuck_status = 0x08 | 0x20
                self._show_status()
            else:
                self.mem[self.sector_base:self.sector_base + self.sector_size] = \
                    b"\xff" * self.sector_size
                if self.sector_base in self.weak:
                    self.mem[self.sector_base + 0x1001] = b"\x7f"
                self.mode = self.READ

    def power_off(self):
        """The power goes. A word being programmed has some of its bits; a
        sector being erased has some of its."""
        if self.mode == self.PROGRAM:
            self._hide()
            keep = self.rng.getrandbits(16)
            self.put(self.target, self.old & (self.pd | keep))
        elif self.mode == self.ERASE:
            self._hide()
            base, size = self.sector_base, self.sector_size
            was = bytes(self.mem[base:base + size])
            noise = self.rng.randbytes(size)
            self.mem[base:base + size] = bytes(a | b for a, b in zip(was, noise))
        else:
            self._hide()
        self.mode = self.READ
        self.unlock = 0
        self.armed = None
        self.leaving = None


class PowerCut(Exception):
    """The power went while a RAM routine was running (Cart.cut_after)."""


class Cart:
    """A console with a cartridge of one of three kinds: a flash chip
    (`chip`: the arguments of FlashChip), a ROM that is really RAM and
    keeps what is written to it (`rom_is_ram`), or the emulator's own, a
    ROM that ignores writes (neither). `battery`: the save RAM keeps its
    contents over a power cycle."""

    ROUTINES = ("cartsave_ram_query", "cartsave_ram_ident",
                "cartsave_ram_erase", "cartsave_ram_program")

    # What a board with no save RAM at all is made to forget, every frame:
    # everything the game keeps there (its tables and settings, its paused
    # and saved games) and the mark.
    NO_RAM_SPANS = ((0x0000, 0x0800), (0x4000, 0x6000), (0x7FE0, 0x8000))

    def __init__(self, rom_path, chip=None, rom_is_ram=False, battery=False,
                 no_ram=False, seed=1):
        self.rom_path = rom_path
        self.rng = random.Random(seed)
        self.battery = battery
        # No save RAM chip on the board: what is written there is gone by
        # the next frame, and what is read is whatever the bus was doing.
        self.no_ram = no_ram
        self.rom_is_ram = rom_is_ram
        self.faults = []
        self.calls = []                 # (routine, steps, cycles)
        self.stores = []                # (byte offset, value) the routines wrote
        self.cut_after = None           # steps inside the RAM routines until the power goes
        self.steps = 0
        # A routine still running after this many instructions is given
        # up on (the longest honest one, a sector's erase at the model's
        # pace, is a hundredth of it).
        self.step_limit = 8_000_000
        # Whether the interrupts were on the last time the console was
        # seen outside the RAM routines: what they must be again after one.
        self.ime_outside = 0
        self.core, self.screen = load(rom_path)
        native = self.core._native
        m8 = self.core.memory.u8

        sym = lambda name: game_state_address(rom_path, name)[0]
        self.ram_start, self.ram_end = sym("__ramcode_start"), sym("__ramcode_end")
        lma = sym("__ramcode_lma")
        entries = {name: sym(name) for name in self.ROUTINES}
        if None in (self.ram_start, self.ram_end, lma) or None in entries.values():
            raise RuntimeError("the ELF has no RAM routines (.ramcode)")
        self.report_at = (sym("g_cartsave"), sym("kCartSaveProbe"))
        if None in self.report_at:
            raise RuntimeError("the ELF has no g_cartsave or kCartSaveProbe")

        # One byte patched makes the emulator's ROM its own copy, 32MB of
        # it, 0xFF past the image as an erased chip is; from then on it
        # can be written straight through the pointer.
        size = 1 << chip.get("size_log2", 24) if chip else native.memory.romSize
        m8.raw_write(ROM + size - 1, 0xFF if size > native.memory.romSize
                     else m8[ROM + size - 1])
        self.mem = ffi.buffer(ffi.cast("uint8_t *", native.memory.rom), ROM_SPAN)
        # ...and 0xFF it is MADE, whatever the emulator put there. A file
        # of a megabyte exactly is, to mGBA, a Classic NES cartridge — a
        # mask ROM its board shows four times over — and three more copies
        # of this program follow the first, boot logo and all, a megabyte
        # in: where the journal goes, and where a chip flashed with that
        # file has whatever it had before. (A file that is no power of two
        # is followed by zeros, which no erased chip is either.)
        if chip and size > native.pristineRomSize:
            written = native.pristineRomSize
            self.mem[written:size] = b"\xff" * (size - written)
        self.image = bytes(self.mem[0:lma - ROM + (self.ram_end - self.ram_start)])
        self.chip = FlashChip(self.mem, seed=seed, **chip) if chip else None

        self.traps = {}
        for name, entry in entries.items():
            at = lma - ROM + (entry - self.ram_start)
            original = self.mem[at][0] | (self.mem[at + 1][0] << 8)
            self.mem[at:at + 2] = bytes((TRAP & 0xFF, TRAP >> 8))
            self.traps[entry] = (name, original)
        if not battery:
            self.scramble_ram()

    # -- the console -------------------------------------------------------
    def now(self):
        native = self.core._native
        return (native.timing.masterCycles + native.cpu.cycles) & 0xFFFFFFFF

    def scramble_ram(self):
        """Save RAM as it is with no battery behind it: anything. Through
        the bus, a byte at a time: the emulator has no save memory at all
        until something touches it."""
        m8 = self.core.memory.u8
        for i, b in enumerate(self.rng.randbytes(SRAM_BYTES)):
            m8[SRAM + i] = b

    def ram(self, offset, count):
        m8 = self.core.memory.u8
        return bytes(m8[SRAM + offset + i] for i in range(count))

    def power_cycle(self):
        """Off and on. The chip keeps what it had; the save RAM does only
        with a battery."""
        if self.chip:
            self.chip.power_off()
        self.core.reset()
        if not self.battery:
            self.scramble_ram()

    def keys(self, *names):
        from .harness import KEYS
        self.core.set_keys(*[KEYS[k] for k in names])

    def frame(self):
        """One frame of the console, and any RAM routine it ends up
        waiting at the door of."""
        self.core.run_frame()
        if self._trapped() is None:
            self.ime_outside = self.core._native.memory.io[0x208 >> 1] & 1
        self.serve()
        if self.no_ram:
            m8 = self.core.memory.u8
            for lo, hi in self.NO_RAM_SPANS:
                for i, b in enumerate(self.rng.randbytes(hi - lo)):
                    m8[SRAM + lo + i] = b

    def run(self, frames, *names):
        if names:
            self.keys(*names)
        for _ in range(frames):
            self.frame()

    def tap(self, name, hold=4, settle=12):
        self.run(hold, name)
        self.core.set_keys()
        self.run(settle)

    # -- the RAM routines --------------------------------------------------
    def _trapped(self):
        cpu = self.core._native.cpu
        if not cpu.cpsr.t:
            return None
        at = (cpu.gprs[15] - 2) & 0xFFFFFFFF
        return at if at in self.traps else None

    def serve(self):
        """Runs any RAM routine the console is waiting at the door of."""
        while True:
            entry = self._trapped()
            if entry is None:
                return
            self._call(entry)

    def _bus_write(self, ea, value, width, name):
        offset = (ea - ROM) & (ROM_SPAN - 1)
        if width != 2:
            self.faults.append(f"{name}: a store to the cartridge that is not 16 bits")
            return
        self.stores.append((offset, value))
        if self.chip:
            self.chip.write(offset, value, self.now())
        elif self.rom_is_ram:
            self.mem[offset:offset + 2] = bytes((value & 0xFF, value >> 8))

    def _store(self, insn, name):
        """The store a Thumb instruction is about to make, if it is one to
        the cartridge."""
        g = self.core._native.cpu.gprs
        rd, rb = insn & 7, (insn >> 3) & 7
        imm, ro = (insn >> 6) & 0x1F, (insn >> 6) & 7
        top = insn & 0xF800
        if top == 0x8000:
            ea, width = g[rb] + imm * 2, 2
        elif top == 0x6000:
            ea, width = g[rb] + imm * 4, 4
        elif top == 0x7000:
            ea, width = g[rb] + imm, 1
        elif insn & 0xFE00 == 0x5200:
            ea, width = g[rb] + g[ro], 2
        elif insn & 0xFE00 == 0x5000:
            ea, width = g[rb] + g[ro], 4
        elif insn & 0xFE00 == 0x5400:
            ea, width = g[rb] + g[ro], 1
        else:
            return
        ea &= 0xFFFFFFFF
        if 0x08000000 <= ea < 0x0E000000:
            self._bus_write(ea, g[rd] & 0xFFFF, width, name)

    def _call(self, entry):
        name, original = self.traps[entry]
        core = self.core
        cpu = core._native.cpu
        m16 = core.memory.u16
        back = cpu.gprs[14] & 0xFFFFFFFE
        if core._native.memory.io[0x208 >> 1] & 1:
            self.faults.append(f"{name}: called with the interrupts on")
        m16[entry] = original
        core.step()                     # the trap's last turn: it was already fetched
        began = self.now()
        steps, strayed = 0, False
        try:
            while True:
                thumb = cpu.cpsr.t
                at = (cpu.gprs[15] - (2 if thumb else 4)) & 0xFFFFFFFF
                if at == back:
                    break
                if not self.ram_start <= at < self.ram_end:
                    if not strayed:
                        mode = self.chip.mode if self.chip else None
                        self.faults.append(f"{name}: ran outside the RAM routines, "
                                           f"at {at:#x} (chip mode {mode})")
                    strayed = True
                elif thumb:
                    self._store(m16[at], name)
                else:
                    self.faults.append(f"{name}: ARM code in the RAM routines")
                core.step()
                steps += 1
                self.steps += 1
                if self.chip:
                    self.chip.tick(self.now())
                if self.cut_after is not None and self.steps >= self.cut_after:
                    self.cut_after = None
                    raise PowerCut()
                if steps > self.step_limit:
                    # Still in there. Its door stays open — the routine is
                    # in the middle of itself — and the console goes on
                    # spinning in it at the emulator's own pace.
                    self.faults.append(f"{name}: never came back")
                    return
        finally:
            if steps <= self.step_limit:
                m16[entry] = TRAP
        if self.chip and self.chip.mode != FlashChip.READ:
            self.faults.append(f"{name}: back in the cartridge with the chip not a ROM "
                               f"(mode {self.chip.mode})")
        self.calls.append((name, steps, (self.now() - began) & 0xFFFFFFFF))
        # ...and the interrupts as they were, within the few instructions
        # it takes the caller to put them back.
        io = core._native.memory.io
        for _ in range(24):
            if io[0x208 >> 1] & 1 == self.ime_outside or self._trapped() is not None:
                break
            core.step()
        else:
            self.faults.append(f"{name}: the interrupts are not put back as they were")

    # -- what the cartridge made of it --------------------------------------
    FIELDS = ("probe", "why", "boot", "flash", "wiring", "ram_kept", "dead", "error",
              "command_set", "id", "magic", "probe_word",
              "device_bytes", "block_bytes", "half_bytes", "gen", "used", "seq",
              "commits", "fresh", "erases", "refused", "waits", "afresh", "off")
    WIDTH = {"probe": 1, "why": 1, "boot": 1, "flash": 1, "wiring": 1, "ram_kept": 1,
             "dead": 1, "error": 1, "command_set": 2, "id": (2, 2), "magic": (2, 3),
             "probe_word": (2, 3), "device_bytes": 4, "block_bytes": 4, "half_bytes": 4,
             "gen": 4, "used": 4, "seq": 4, "commits": 2, "fresh": 2, "erases": 2,
             "refused": 2, "waits": 2, "afresh": 1, "off": 1}

    def report(self):
        """gba/cartsave.h's CartSaveReport, read out of the console."""
        mem = self.core.memory
        base, probe = self.report_at
        words = [mem.u16[probe + 2 * i] for i in range(2 + len(self.FIELDS) + 3)]
        at = base + words[0]
        read = {1: mem.u8, 2: mem.u16, 4: mem.u32}
        out = {}
        for name, off in zip(self.FIELDS, words[2:]):
            w = self.WIDTH[name]
            if isinstance(w, tuple):
                out[name] = [read[w[0]][at + off + i * w[0]] for i in range(w[1])]
            else:
                out[name] = read[w][at + off]
        out["dirty"] = mem.u8[base + words[-3]]
        out["waiting"] = mem.u8[base + words[-2]]
        out["erase_patience_at"] = base + words[-1]
        return out

    def set_erase_patience(self, looks):
        """How many times the cartridge will look at a sector it is erasing
        before giving it up. A dozen seconds' worth on the console, which is
        forty million instructions stepped one at a time here: a check that
        has to see it run out says so."""
        self.core.memory.u32[self.report()["erase_patience_at"]] = looks


def ramcode_faults(rom_path):
    """The RAM routines taken apart: nothing in them may leave the section
    they are copied to RAM as, since what they would reach is the chip they
    are talking to. Returns what is wrong, as a list."""
    elf = os.path.splitext(rom_path)[0] + ".elf"
    objdump = os.environ.get("OBJDUMP", "arm-none-eabi-objdump")
    try:
        out = subprocess.check_output([objdump, "-d", "-j", ".ramcode", elf],
                                      stderr=subprocess.DEVNULL).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        return [f"no pude desensamblar .ramcode: {exc}"]
    start = game_state_address(rom_path, "__ramcode_start")[0]
    end = game_state_address(rom_path, "__ramcode_end")[0]
    if start is None or end is None or end <= start:
        return ["el ELF no tiene rutinas en RAM (.ramcode)"]
    faults, seen = [], 0
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[0].strip().endswith(":"):
            continue
        seen += 1
        mnemonic = parts[2].strip()
        operands = parts[3].strip() if len(parts) > 3 else ""
        where = parts[0].strip()
        if mnemonic == ".word":
            value = int(operands.split()[0], 16)
            if 0x08000000 <= value < 0x0E000000:
                faults.append(f"{where} una constante que apunta al cartucho: {value:#x}")
        elif mnemonic in ("bl", "blx") or mnemonic.startswith(("swi", "svc")):
            faults.append(f"{where} {mnemonic} {operands}: una llamada desde la RAM")
        elif mnemonic.startswith("b") and mnemonic not in ("bx", "bics", "bic", "bkpt"):
            try:
                target = int(operands.split()[0], 16)
            except ValueError:
                faults.append(f"{where} {mnemonic} {operands}: un salto que no se leer")
                continue
            if not start <= target < end:
                faults.append(f"{where} {mnemonic} a {target:#x}: fuera de la seccion")
    for name in Cart.ROUTINES:
        addr = game_state_address(rom_path, name)[0]
        if addr is None or not start <= addr < end:
            faults.append(f"{name} no esta en .ramcode")
    if seen < 100:
        faults.append(f"solo {seen} instrucciones en .ramcode")
    return faults
