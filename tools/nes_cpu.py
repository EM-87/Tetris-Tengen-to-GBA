#!/usr/bin/env python3
"""
nes_cpu.py — a small 6502 interpreter, here for exactly one job.

The port's music comes from running the CARTRIDGE'S OWN sound engine rather
than from a reimplementation of it. Tengen's engine is a compact but dense
piece of 6502 with vibrato, portamento, per-channel envelopes and a
sound-effect priority system spread over a thousand-odd lines of
`reference/disasm/main.asm.txt`, most of it working through unlabelled RAM.
Transcribing that by hand would mean guessing, and guessing is the one thing
this project doesn't do (see ../CLAUDE.md).

So instead: execute the ROM's `updateAudio` here, once per frame, and record
what it writes to the APU. What comes out is not an interpretation of the
music — it IS the music, bit for bit, and `tools/extract_music.py` turns it
into a compact register log the GBA replays.

The engine only ever touches RAM and $4000-$4017 (its own comment structure
makes that plain, and `--strict` here proves it by trapping anything else),
which is what makes this narrow emulator enough.

Only the documented NMOS opcodes are implemented; an undocumented one raises
rather than guessing. Decimal mode is implemented because the ROM's score
code uses ASCII digits, not BCD, and never sets D — but if something ever
does, this will do the right thing instead of silently drifting.
"""

# Flag bits
C, Z, I, D, B, U, V, N = 1, 2, 4, 8, 16, 32, 64, 128


class Bus:
    """RAM + PRG ROM + a log of APU writes. No PPU: the audio engine has no
    business touching one, and --strict turns any attempt into an error."""

    def __init__(self, prg: bytes, strict=True):
        assert len(prg) in (16384, 32768), f"unexpected PRG size {len(prg)}"
        self.prg = prg
        self.ram = bytearray(0x800)
        self.apu = bytearray(0x18)      # $4000-$4017, last value written
        self.writes = []                # (addr, value) since the last drain
        self.strict = strict
        self.stray = []

    def read(self, addr):
        addr &= 0xFFFF
        if addr < 0x2000:
            return self.ram[addr & 0x7FF]
        if 0x4000 <= addr <= 0x4017:
            return self.apu[addr - 0x4000]
        if addr >= 0x8000:
            return self.prg[(addr - 0x8000) % len(self.prg)]
        if self.strict:
            self.stray.append(("read", addr))
        return 0

    def write(self, addr, value):
        addr &= 0xFFFF
        value &= 0xFF
        if addr < 0x2000:
            self.ram[addr & 0x7FF] = value
            return
        if 0x4000 <= addr <= 0x4017:
            self.apu[addr - 0x4000] = value
            self.writes.append((addr, value))
            return
        if self.strict:
            self.stray.append(("write", addr))

    def drain(self):
        out = self.writes
        self.writes = []
        return out


class CPU:
    def __init__(self, bus: Bus):
        self.bus = bus
        self.a = self.x = self.y = 0
        self.sp = 0xFD
        self.pc = 0
        self.p = U | I
        self.cycles = 0

    # -- flag helpers -----------------------------------------------------
    def _set(self, mask, on):
        self.p = (self.p | mask) if on else (self.p & ~mask & 0xFF)

    def _zn(self, value):
        value &= 0xFF
        self._set(Z, value == 0)
        self._set(N, value & 0x80)
        return value

    # -- stack ------------------------------------------------------------
    def push(self, value):
        self.bus.write(0x100 + self.sp, value & 0xFF)
        self.sp = (self.sp - 1) & 0xFF

    def pop(self):
        self.sp = (self.sp + 1) & 0xFF
        return self.bus.read(0x100 + self.sp)

    # -- fetch ------------------------------------------------------------
    def fetch(self):
        value = self.bus.read(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        return value

    def fetch16(self):
        low = self.fetch()
        return low | (self.fetch() << 8)

    # -- addressing modes: each returns an effective address --------------
    def a_zp(self):   return self.fetch()
    def a_zpx(self):  return (self.fetch() + self.x) & 0xFF
    def a_zpy(self):  return (self.fetch() + self.y) & 0xFF
    def a_abs(self):  return self.fetch16()
    def a_abx(self):  return (self.fetch16() + self.x) & 0xFFFF
    def a_aby(self):  return (self.fetch16() + self.y) & 0xFFFF

    def a_indx(self):
        base = (self.fetch() + self.x) & 0xFF
        return self.bus.read(base) | (self.bus.read((base + 1) & 0xFF) << 8)

    def a_indy(self):
        base = self.fetch()
        addr = self.bus.read(base) | (self.bus.read((base + 1) & 0xFF) << 8)
        return (addr + self.y) & 0xFFFF

    def a_ind(self):
        """JMP ($nnnn), including the NMOS page-wrap bug — the ROM may rely
        on it, and 'fixing' it here would be a divergence."""
        ptr = self.fetch16()
        low = self.bus.read(ptr)
        high = self.bus.read((ptr & 0xFF00) | ((ptr + 1) & 0xFF))
        return low | (high << 8)

    # -- operations -------------------------------------------------------
    def _adc(self, value):
        if self.p & D:
            lo = (self.a & 0x0F) + (value & 0x0F) + (1 if self.p & C else 0)
            hi = (self.a >> 4) + (value >> 4)
            if lo > 9:
                lo += 6
                hi += 1
            self._set(Z, ((self.a + value + (1 if self.p & C else 0)) & 0xFF) == 0)
            self._set(N, (hi << 4) & 0x80)
            self._set(V, False)
            if hi > 9:
                hi += 6
            self._set(C, hi > 15)
            self.a = ((hi << 4) | (lo & 0x0F)) & 0xFF
            return
        total = self.a + value + (1 if self.p & C else 0)
        self._set(C, total > 0xFF)
        self._set(V, (~(self.a ^ value) & (self.a ^ total) & 0x80) != 0)
        self.a = self._zn(total)

    def _sbc(self, value):
        if self.p & D:
            borrow = 0 if self.p & C else 1
            lo = (self.a & 0x0F) - (value & 0x0F) - borrow
            hi = (self.a >> 4) - (value >> 4)
            if lo & 0x10:
                lo -= 6
                hi -= 1
            if hi & 0x10:
                hi -= 6
            total = self.a - value - borrow
            self._set(C, total >= 0)
            self._set(V, ((self.a ^ value) & (self.a ^ total) & 0x80) != 0)
            self._zn(total & 0xFF)
            self.a = ((hi << 4) | (lo & 0x0F)) & 0xFF
            return
        self._adc(value ^ 0xFF)

    def _cmp(self, reg, value):
        diff = (reg - value) & 0x1FF
        self._set(C, reg >= value)
        self._zn(diff & 0xFF)

    def _branch(self, taken):
        offset = self.fetch()
        if offset & 0x80:
            offset -= 0x100
        if taken:
            self.pc = (self.pc + offset) & 0xFFFF
            self.cycles += 1

    def _asl(self, value):
        self._set(C, value & 0x80)
        return self._zn((value << 1) & 0xFF)

    def _lsr(self, value):
        self._set(C, value & 0x01)
        return self._zn(value >> 1)

    def _rol(self, value):
        carry = 1 if self.p & C else 0
        self._set(C, value & 0x80)
        return self._zn(((value << 1) | carry) & 0xFF)

    def _ror(self, value):
        carry = 0x80 if self.p & C else 0
        self._set(C, value & 0x01)
        return self._zn((value >> 1) | carry)

    def _bit(self, value):
        self._set(Z, (self.a & value) == 0)
        self._set(N, value & 0x80)
        self._set(V, value & 0x40)

    # -- the interpreter --------------------------------------------------
    def step(self):
        op = self.fetch()
        self.cycles += 2  # approximate; nothing here depends on exact timing
        b, r, w = self.bus.read, None, self.bus.write

        # Loads / stores
        if   op == 0xA9: self.a = self._zn(self.fetch())
        elif op == 0xA5: self.a = self._zn(b(self.a_zp()))
        elif op == 0xB5: self.a = self._zn(b(self.a_zpx()))
        elif op == 0xAD: self.a = self._zn(b(self.a_abs()))
        elif op == 0xBD: self.a = self._zn(b(self.a_abx()))
        elif op == 0xB9: self.a = self._zn(b(self.a_aby()))
        elif op == 0xA1: self.a = self._zn(b(self.a_indx()))
        elif op == 0xB1: self.a = self._zn(b(self.a_indy()))
        elif op == 0xA2: self.x = self._zn(self.fetch())
        elif op == 0xA6: self.x = self._zn(b(self.a_zp()))
        elif op == 0xB6: self.x = self._zn(b(self.a_zpy()))
        elif op == 0xAE: self.x = self._zn(b(self.a_abs()))
        elif op == 0xBE: self.x = self._zn(b(self.a_aby()))
        elif op == 0xA0: self.y = self._zn(self.fetch())
        elif op == 0xA4: self.y = self._zn(b(self.a_zp()))
        elif op == 0xB4: self.y = self._zn(b(self.a_zpx()))
        elif op == 0xAC: self.y = self._zn(b(self.a_abs()))
        elif op == 0xBC: self.y = self._zn(b(self.a_abx()))
        elif op == 0x85: w(self.a_zp(), self.a)
        elif op == 0x95: w(self.a_zpx(), self.a)
        elif op == 0x8D: w(self.a_abs(), self.a)
        elif op == 0x9D: w(self.a_abx(), self.a)
        elif op == 0x99: w(self.a_aby(), self.a)
        elif op == 0x81: w(self.a_indx(), self.a)
        elif op == 0x91: w(self.a_indy(), self.a)
        elif op == 0x86: w(self.a_zp(), self.x)
        elif op == 0x96: w(self.a_zpy(), self.x)
        elif op == 0x8E: w(self.a_abs(), self.x)
        elif op == 0x84: w(self.a_zp(), self.y)
        elif op == 0x94: w(self.a_zpx(), self.y)
        elif op == 0x8C: w(self.a_abs(), self.y)

        # Transfers
        elif op == 0xAA: self.x = self._zn(self.a)
        elif op == 0xA8: self.y = self._zn(self.a)
        elif op == 0x8A: self.a = self._zn(self.x)
        elif op == 0x98: self.a = self._zn(self.y)
        elif op == 0xBA: self.x = self._zn(self.sp)
        elif op == 0x9A: self.sp = self.x

        # Stack
        elif op == 0x48: self.push(self.a)
        elif op == 0x68: self.a = self._zn(self.pop())
        elif op == 0x08: self.push(self.p | B | U)
        elif op == 0x28: self.p = (self.pop() | U) & ~B & 0xFF

        # Logic
        elif op == 0x29: self.a = self._zn(self.a & self.fetch())
        elif op == 0x25: self.a = self._zn(self.a & b(self.a_zp()))
        elif op == 0x35: self.a = self._zn(self.a & b(self.a_zpx()))
        elif op == 0x2D: self.a = self._zn(self.a & b(self.a_abs()))
        elif op == 0x3D: self.a = self._zn(self.a & b(self.a_abx()))
        elif op == 0x39: self.a = self._zn(self.a & b(self.a_aby()))
        elif op == 0x21: self.a = self._zn(self.a & b(self.a_indx()))
        elif op == 0x31: self.a = self._zn(self.a & b(self.a_indy()))
        elif op == 0x09: self.a = self._zn(self.a | self.fetch())
        elif op == 0x05: self.a = self._zn(self.a | b(self.a_zp()))
        elif op == 0x15: self.a = self._zn(self.a | b(self.a_zpx()))
        elif op == 0x0D: self.a = self._zn(self.a | b(self.a_abs()))
        elif op == 0x1D: self.a = self._zn(self.a | b(self.a_abx()))
        elif op == 0x19: self.a = self._zn(self.a | b(self.a_aby()))
        elif op == 0x01: self.a = self._zn(self.a | b(self.a_indx()))
        elif op == 0x11: self.a = self._zn(self.a | b(self.a_indy()))
        elif op == 0x49: self.a = self._zn(self.a ^ self.fetch())
        elif op == 0x45: self.a = self._zn(self.a ^ b(self.a_zp()))
        elif op == 0x55: self.a = self._zn(self.a ^ b(self.a_zpx()))
        elif op == 0x4D: self.a = self._zn(self.a ^ b(self.a_abs()))
        elif op == 0x5D: self.a = self._zn(self.a ^ b(self.a_abx()))
        elif op == 0x59: self.a = self._zn(self.a ^ b(self.a_aby()))
        elif op == 0x41: self.a = self._zn(self.a ^ b(self.a_indx()))
        elif op == 0x51: self.a = self._zn(self.a ^ b(self.a_indy()))
        elif op == 0x24: self._bit(b(self.a_zp()))
        elif op == 0x2C: self._bit(b(self.a_abs()))

        # Arithmetic
        elif op == 0x69: self._adc(self.fetch())
        elif op == 0x65: self._adc(b(self.a_zp()))
        elif op == 0x75: self._adc(b(self.a_zpx()))
        elif op == 0x6D: self._adc(b(self.a_abs()))
        elif op == 0x7D: self._adc(b(self.a_abx()))
        elif op == 0x79: self._adc(b(self.a_aby()))
        elif op == 0x61: self._adc(b(self.a_indx()))
        elif op == 0x71: self._adc(b(self.a_indy()))
        elif op == 0xE9: self._sbc(self.fetch())
        elif op == 0xE5: self._sbc(b(self.a_zp()))
        elif op == 0xF5: self._sbc(b(self.a_zpx()))
        elif op == 0xED: self._sbc(b(self.a_abs()))
        elif op == 0xFD: self._sbc(b(self.a_abx()))
        elif op == 0xF9: self._sbc(b(self.a_aby()))
        elif op == 0xE1: self._sbc(b(self.a_indx()))
        elif op == 0xF1: self._sbc(b(self.a_indy()))
        elif op == 0xC9: self._cmp(self.a, self.fetch())
        elif op == 0xC5: self._cmp(self.a, b(self.a_zp()))
        elif op == 0xD5: self._cmp(self.a, b(self.a_zpx()))
        elif op == 0xCD: self._cmp(self.a, b(self.a_abs()))
        elif op == 0xDD: self._cmp(self.a, b(self.a_abx()))
        elif op == 0xD9: self._cmp(self.a, b(self.a_aby()))
        elif op == 0xC1: self._cmp(self.a, b(self.a_indx()))
        elif op == 0xD1: self._cmp(self.a, b(self.a_indy()))
        elif op == 0xE0: self._cmp(self.x, self.fetch())
        elif op == 0xE4: self._cmp(self.x, b(self.a_zp()))
        elif op == 0xEC: self._cmp(self.x, b(self.a_abs()))
        elif op == 0xC0: self._cmp(self.y, self.fetch())
        elif op == 0xC4: self._cmp(self.y, b(self.a_zp()))
        elif op == 0xCC: self._cmp(self.y, b(self.a_abs()))

        # Increment / decrement
        elif op == 0xE6: a = self.a_zp();  w(a, self._zn(b(a) + 1))
        elif op == 0xF6: a = self.a_zpx(); w(a, self._zn(b(a) + 1))
        elif op == 0xEE: a = self.a_abs(); w(a, self._zn(b(a) + 1))
        elif op == 0xFE: a = self.a_abx(); w(a, self._zn(b(a) + 1))
        elif op == 0xC6: a = self.a_zp();  w(a, self._zn(b(a) - 1))
        elif op == 0xD6: a = self.a_zpx(); w(a, self._zn(b(a) - 1))
        elif op == 0xCE: a = self.a_abs(); w(a, self._zn(b(a) - 1))
        elif op == 0xDE: a = self.a_abx(); w(a, self._zn(b(a) - 1))
        elif op == 0xE8: self.x = self._zn(self.x + 1)
        elif op == 0xC8: self.y = self._zn(self.y + 1)
        elif op == 0xCA: self.x = self._zn(self.x - 1)
        elif op == 0x88: self.y = self._zn(self.y - 1)

        # Shifts
        elif op == 0x0A: self.a = self._asl(self.a)
        elif op == 0x06: a = self.a_zp();  w(a, self._asl(b(a)))
        elif op == 0x16: a = self.a_zpx(); w(a, self._asl(b(a)))
        elif op == 0x0E: a = self.a_abs(); w(a, self._asl(b(a)))
        elif op == 0x1E: a = self.a_abx(); w(a, self._asl(b(a)))
        elif op == 0x4A: self.a = self._lsr(self.a)
        elif op == 0x46: a = self.a_zp();  w(a, self._lsr(b(a)))
        elif op == 0x56: a = self.a_zpx(); w(a, self._lsr(b(a)))
        elif op == 0x4E: a = self.a_abs(); w(a, self._lsr(b(a)))
        elif op == 0x5E: a = self.a_abx(); w(a, self._lsr(b(a)))
        elif op == 0x2A: self.a = self._rol(self.a)
        elif op == 0x26: a = self.a_zp();  w(a, self._rol(b(a)))
        elif op == 0x36: a = self.a_zpx(); w(a, self._rol(b(a)))
        elif op == 0x2E: a = self.a_abs(); w(a, self._rol(b(a)))
        elif op == 0x3E: a = self.a_abx(); w(a, self._rol(b(a)))
        elif op == 0x6A: self.a = self._ror(self.a)
        elif op == 0x66: a = self.a_zp();  w(a, self._ror(b(a)))
        elif op == 0x76: a = self.a_zpx(); w(a, self._ror(b(a)))
        elif op == 0x6E: a = self.a_abs(); w(a, self._ror(b(a)))
        elif op == 0x7E: a = self.a_abx(); w(a, self._ror(b(a)))

        # Jumps and calls
        elif op == 0x4C: self.pc = self.a_abs()
        elif op == 0x6C: self.pc = self.a_ind()
        elif op == 0x20:
            target = self.fetch16()
            ret = (self.pc - 1) & 0xFFFF
            self.push(ret >> 8)
            self.push(ret & 0xFF)
            self.pc = target
        elif op == 0x60:
            low = self.pop()
            self.pc = ((low | (self.pop() << 8)) + 1) & 0xFFFF
        elif op == 0x40:
            self.p = (self.pop() | U) & ~B & 0xFF
            low = self.pop()
            self.pc = low | (self.pop() << 8)

        # Branches
        elif op == 0x10: self._branch(not (self.p & N))
        elif op == 0x30: self._branch(bool(self.p & N))
        elif op == 0x50: self._branch(not (self.p & V))
        elif op == 0x70: self._branch(bool(self.p & V))
        elif op == 0x90: self._branch(not (self.p & C))
        elif op == 0xB0: self._branch(bool(self.p & C))
        elif op == 0xD0: self._branch(not (self.p & Z))
        elif op == 0xF0: self._branch(bool(self.p & Z))

        # Flags and nop
        elif op == 0x18: self._set(C, False)
        elif op == 0x38: self._set(C, True)
        elif op == 0x58: self._set(I, False)
        elif op == 0x78: self._set(I, True)
        elif op == 0xB8: self._set(V, False)
        elif op == 0xD8: self._set(D, False)
        elif op == 0xF8: self._set(D, True)
        elif op == 0xEA: pass
        elif op == 0x00:
            raise RuntimeError(f"BRK at ${(self.pc - 1) & 0xFFFF:04X}")
        else:
            raise RuntimeError(f"undocumented opcode ${op:02X} at "
                               f"${(self.pc - 1) & 0xFFFF:04X}")

    # -- driving ----------------------------------------------------------
    SENTINEL = 0xFFF0  # a return address that can never be real code here

    def call(self, addr, a=0, x=0, y=0, max_steps=2_000_000):
        """Runs a subroutine to its RTS, the way a JSR from nowhere would.

        The return address is a sentinel outside ROM; when PC lands there the
        routine has returned. `max_steps` is a hang guard, not a timing model.
        """
        self.a, self.x, self.y = a & 0xFF, x & 0xFF, y & 0xFF
        ret = self.SENTINEL - 1
        self.push(ret >> 8)
        self.push(ret & 0xFF)
        self.pc = addr
        for _ in range(max_steps):
            if self.pc == self.SENTINEL:
                return
            self.step()
        raise RuntimeError(f"subroutine at ${addr:04X} did not return")


def load_prg(nes_path):
    """PRG ROM of an iNES file, as the 6502 sees it at $8000."""
    data = open(nes_path, "rb").read()
    if data[:4] != b"NES\x1a":
        raise ValueError("not an iNES file")
    prg_banks = data[4]
    prg = data[16:16 + prg_banks * 16384]
    if prg_banks == 1:
        prg = prg + prg       # 16KB carts mirror into $C000
    return prg
