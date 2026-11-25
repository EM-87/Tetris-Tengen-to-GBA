#!/usr/bin/env python3
"""
run_link.py — two ROMs, one cable, checked frame by frame.

The point of a two-player port is the thing that is hardest to test: two
consoles simulating the same game from nothing but each other's buttons. The
lockstep rules are already tested on the host (tests/test_tengen.c runs two
TengenLinks against each other), and the handshake with them. What none of
that can reach is the REAL article: the built ROM, on ARM, driving GBA serial
hardware through an interrupt, with all the timing that implies.

So this runs two mGBA cores and puts a cable between them.

mGBA lets a program supply its own serial driver, so the "cable" here is a
faithful little model of GBA multiplayer mode:

  - the master's SI line reads 0 and the slave's reads 1, which is how each
    console works out who it is;
  - when the master sets SIOCNT's start bit, both consoles' send registers
    are copied into all four of both consoles' receive slots, exactly as the
    hardware does — every console sees every console's word, its own
    included;
  - the empty slots read $FFFF, the same as a real cable with two consoles
    on it;
  - and both consoles get a serial interrupt.

The two cores are then run a frame each, alternately, which is what a master
and a slave actually do. Note what that does NOT model: the two consoles here
share one clock, so this says nothing about the drift between two real
crystals — that is what the interrupt and the transfer queue in gba/link.c are
for, and it is argued there rather than tested here. What this does test is
everything else: the handshake, the interrupt path, the frame counters, the
order the two players are stepped in, and the agreement between the two
machines.

The check itself is the one that matters and the one a person cannot do by
looking: after every frame, the whole TengenGame in one console's memory must
equal the one in the other's, byte for byte. Two boards, two piece sequences,
two scores, two of everything — and if the cable, the interrupt, the frame
counters or the order of the two tengen_step calls were wrong in any way, the
two would drift apart and this would say on which frame.

Requires: pip install pygba  (plus the mGBA shared library)
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romcheck.harness  # noqa: E402,F401 - skips the splash; see there
from romcheck.harness import skip, strict_verdict  # noqa: E402

try:
    import mgba.core
    import mgba.gba
    import mgba.image
    import mgba.log
    from mgba import lib
except ImportError as exc:  # pragma: no cover - environment problem
    sys.exit(f"mGBA python bindings unavailable ({exc}).\n"
             "Install with: pip install pygba && apt-get install libmgba0.10")

SCREEN_W, SCREEN_H = 240, 160

# The coop board: twelve storage columns, drawn from the coop screen's own
# origin (SCREEN_COOP_FIELD_TX in the generated header).
TENGEN_PF_WIDTH = 12
TENGEN_PF_HEIGHT = 20
COOP_FIELD_TX = 9
CELL_WALL = 15          # the sentinel the ROM keeps in the wall columns
LEADER_INITIALS = 3     # three letters a name, as the cartridge's table has

# I/O registers, as halfword indices into struct GBA's io[] array.
IO_SIOMULTI0 = 0x120 >> 1
IO_SIOMULTI1 = 0x122 >> 1
IO_SIOMULTI2 = 0x124 >> 1
IO_SIOMULTI3 = 0x126 >> 1
IO_SIOCNT = 0x128 >> 1
IO_SIOMLT_SEND = 0x12A >> 1

REG_SIOCNT = 0x128
REG_RCNT = 0x134
SIO_SI = 0x0004
SIO_SD = 0x0008
SIO_ERR = 0x0040
SIO_START = 0x0080
SIO_ID_SHIFT = 4
SIO_MODE_SHIFT = 12
SIO_MODE_MULTI = 2

IRQ_SIO = 7        # enum GBAIRQ
ABSENT = 0xFFFF    # what a slot with no console in it reads
LINK_LOST_FRAMES_PY = 120   # LINK_LOST_FRAMES in gba/port.h
LINK_GIVEUP_FRAMES_PY = 600  # LINK_GIVEUP_FRAMES in gba/port.h
IRQ_STEPS = 1500            # instructions for a serial interrupt to run

KEYS = {"A": 0, "B": 1, "SELECT": 2, "START": 3,
        "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7,
        # The GBA's own two, which the port uses for the handicap and the HUD.
        "R": 8, "L": 9}


class CableEnd(mgba.gba.GBASIODriver):
    """One console's end of the cable.

    mGBA calls writeRegister for every write the game makes to a serial
    register, and stores what this returns — which is how the SI bit and the
    start/busy bit come to read the way real hardware would.
    """

    def __init__(self, cable, master):
        super(CableEnd, self).__init__()
        self.cable = cable
        self.master = master
        # In multiplayer mode, as far as the other end can tell. A console
        # that has not written SIOCNT yet — still on GAME SELECT — is not.
        self.ready = False
        # This console's SO pin is HIGH: in multiplayer mode at rest, or in
        # normal mode with SIOCNT bit 3 ("SO during inactivity") set. At
        # power-on (SIOCNT 0) it is LOW, so a slave whose master has not come
        # to the cable yet reads SI low.
        self.so_high = False
        # THE ERROR IS STICKY: a transfer that went wrong leaves the port
        # failing every transfer after it until the program writes SIOCNT
        # again without the start bit (takes the port down and up). That is
        # the pessimistic reading of the hardware, and it is the one the first
        # real cable agreed with: two consoles that never heard each other.
        self.error = False
        # MUTE: transfers go on, but this console's slot comes back $FFFF on
        # both ends and nobody's error bit says so — what two real SPs did on
        # the second session of an evening (one good transfer, then "B 999",
        # "R 0" on both). Only this console starting its port over ends it.
        self.mute = False
        cable.ends.append(self)

    def writeRegister(self, address, value):
        # A PORT TAKEN TO GENERAL PURPOSE LETS GO OF ITS LINES, and the other
        # console hears the flutter: it goes MUTE until it restarts its own
        # port. Inferred from two real SPs that ended up doing nothing but
        # empty transfers and restarts on both sides ("A 999 R 999", each
        # reading itself a slave at the interrupt, the master's own slot
        # empty) after going in and out of the cable: every restart then
        # went through general purpose and silenced the other console.
        # mGBA hands this driver only the write that brings RCNT back to the
        # serial modes, so the tell is ANY RCNT write while this port is
        # already in multiplayer mode: the program only does that to go
        # through general purpose and back.
        if address == REG_RCNT:
            if self.ready:
                for end in self.cable.ends:
                    if end is not self and self.cable.plugged:
                        end.mute = True
            return value
        if address != REG_SIOCNT:
            return value
        self.ready = ((value >> SIO_MODE_SHIFT) & 3) == SIO_MODE_MULTI
        self.so_high = self.ready or (
            ((value >> SIO_MODE_SHIFT) & 3) == 0 and bool(value & 0x0008))
        if not (value & SIO_START):
            self.error = False            # the port taken down and up again
            self.mute = False
        # ...BUT NOT THE ERROR BIT. That is the last transfer's verdict and
        # only the next transfer rewrites it: a real SP kept SIOCNT at $6049
        # through a thousand resets. So it carries over from what the
        # console last read, whatever the program writes.
        io = (self.cable.master if self.master else self.cable.slave)._native.sio
        value = (value & ~SIO_ERR) | (io.siocnt & SIO_ERR)
        # SI says which end of the cable this console is plugged into; SD says
        # EVERY console is in multiplayer mode, so it changes on both when
        # either one switches; the id is 0 for the master and 1 for the slave.
        # THE SLAVE'S SI IS THE MASTER'S SO: high only while the master's
        # port is in multiplayer mode and driving it. Otherwise it floats low
        # and the slave reads itself as a master — which is how a real slave
        # came up on LEVEL SETTINGS once its master had left the cable.
        value &= ~(SIO_SI | SIO_SD | (3 << SIO_ID_SHIFT))
        if not self.master:
            value |= 1 << SIO_ID_SHIFT
            if self.cable.parent_drives_so():
                value |= SIO_SI
            value &= ~SIO_START          # read-only on a slave
        sd = self.cable.all_ready()
        if sd:
            value |= SIO_SD
        self.cable.set_sd(sd, self)

        if self.master and (value & SIO_START) and self.cable.slow:
            # A TRANSFER TAKES TIME: busy on both, and the slave's SI low —
            # the master's SO passing it the turn — until the end of the
            # frame (Cable.settle). A slave that reads SI meanwhile sees the
            # master's token, not the cable.
            self.cable.pending = True
            # The words go at the START, as the hardware shifts them out.
            self.cable.latched = (
                self.cable.master._native.memory.io[IO_SIOMLT_SEND],
                self.cable.slave._native.memory.io[IO_SIOMLT_SEND])
            self.cable._set_bit(self.cable.slave, SIO_START, True)
            self.cable._set_bit(self.cable.slave, SIO_SI, False)
            return value
        if self.master and (value & SIO_START):
            if self.cable.transfer():
                value &= ~SIO_ERR
            else:
                value |= SIO_ERR
            value &= ~SIO_START      # the transfer is over by the time we return
        return value

    # The bindings in some builds call the snake_case name instead.
    def write_register(self, address, value):
        return self.writeRegister(address, value)


def _is_move(word):
    """A match's word (tengen_link_is_match_word): mark 110, $C000-$DFFF."""
    return (word & 0xE000) == 0xC000


class Cable:
    def __init__(self, master, slave):
        self.master = master
        self.slave = slave
        self.transfers = 0
        self.failed = 0
        self.plugged = True
        self.ends = []
        # Good-transfer counts at which the slave goes mute (see CableEnd.mute).
        self.mute_slave_at = set()
        # ...and at which ONE transfer fails with the error bit, the way a
        # cheap cable drops a bit: nothing sticky, the next one is fine.
        self.glitch_at = set()
        # SD STILL DOWN WHEN THE INTERRUPT LOOKS: every transfer leaves SD low
        # on both consoles until the end of the frame (settle). The reading
        # of two real SPs entering the cable together — every transfer thrown
        # away ("G 0 B 999 R 999") while SIOCNT at leisure showed SD high.
        self.sd_lags = False
        self.lagging = False
        # SLOW TRANSFERS: see the note in CableEnd.writeRegister.
        self.slow = False
        self.pending = False
        self.latched = None
        # A SLAVE LATE WITH ITS WORD: at these good-transfer counts the
        # slave's slot carries the word of the transfer before — what a
        # console sends when the transfer starts before its program has
        # loaded the next one (an interrupt held off a frame; the
        # Single-Pak copy, which polls, on a heavy frame).
        self.stale_slave_at = set()
        self.slave_sent = None
        # A SLAVE LATE INTO THE MATCH: for this many transfers from the first
        # move the master sends, the slave's slot still carries the last
        # word of the lobby's it sent — a console whose program is a few
        # frames behind the other's into the match (a heavy frame, a save),
        # its register still holding what it had loaded before.
        self.late_start = 0
        self.slave_lobby_word = None
        # WHAT WENT OUT, for the checks that read the wire itself: with a
        # list here, every transfer the master starts is put on it as
        # (frame, speed bits of the master's SIOCNT, its word, the answer or
        # $FFFF, SD as it was). `frames` counts settle().
        self.wire = None
        self.frames = 0

    def all_ready(self):
        # A console in its BIOS is in multiplayer mode only part of the time
        # (FakeMultibootBios.sd): SD is low for the rest.
        bios = getattr(self, "fake_bios", None)
        if bios is not None and self.plugged and not getattr(bios, "sd", lambda: True)():
            return False
        return all(end.ready for end in self.ends) or not self.plugged

    def parent_drives_so(self):
        return (self.plugged and bool(self.ends) and self.ends[0].so_high
                and not self.pending)

    @staticmethod
    def _set_bit(core, bit, on):
        # Both copies: mGBA answers a read of SIOCNT from its SIO state
        # (sio.siocnt), and keeps io[] beside it.
        io = core._native.memory.io
        io[IO_SIOCNT] = (io[IO_SIOCNT] | bit) if on else (io[IO_SIOCNT] & ~bit)
        sio = core._native.sio
        sio.siocnt = (sio.siocnt | bit) if on else (sio.siocnt & ~bit)

    def set_sd(self, sd, writer):
        """SD is one wire: when it moves, every console reads it moved. And
        the slave's SI follows the master's port (parent_drives_so)."""
        for end, core in zip(self.ends, (self.master, self.slave)):
            if not end.master:
                self._set_bit(core, SIO_SI, self.parent_drives_so())
            if end is writer:
                continue
            self._set_bit(core, SIO_SD, sd)

    def settle(self):
        """The end of a frame: a slow transfer in flight lands, and SD, if a
        transfer left it down (sd_lags), is back where the consoles' modes
        put it."""
        if self.pending:
            self.pending = False
            ok = self.transfer()
            for core in (self.master, self.slave):
                self._set_bit(core, SIO_START, False)
            self._set_bit(self.master, SIO_ERR, not ok)
            self._set_bit(self.slave, SIO_SI, self.parent_drives_so())
            # ...AND THE INTERRUPT IS TAKEN AT ONCE, as on hardware, where it
            # is microseconds after the transfer and long before the next.
            # Left to the next run_frame, the master would start its next
            # transfer before the slave had loaded its answer.
            for core in (self.master, self.slave):
                for _ in range(IRQ_STEPS):
                    core.step()
        if self.lagging:
            self.lagging = False
            for core in (self.master, self.slave):
                self._set_bit(core, SIO_SD, self.all_ready())
        self.frames += 1
        # A frame of the stand-in BIOS's own (its modes turn by the frame),
        # and SD as it leaves it on the master's end.
        bios = getattr(self, "fake_bios", None)
        if bios is not None and hasattr(bios, "frame"):
            bios.frame()
            self._set_bit(self.master, SIO_SD, self.all_ready())

    def transfer(self):
        """True for a transfer that carried both words."""
        if not self.plugged:
            # An unplugged cable is not a cable that says nothing: the master
            # still starts its transfer and still gets an interrupt, and reads
            # $FFFF back from the slot where nobody is. Modelling that, rather
            # than simply going quiet, is what makes the check below mean
            # something.
            mio = self.master._native.memory.io
            for slot in (IO_SIOMULTI0, IO_SIOMULTI1, IO_SIOMULTI2, IO_SIOMULTI3):
                mio[slot] = ABSENT
            mio[IO_SIOCNT] &= ~SIO_START
            lib.GBARaiseIRQ(self.master._native, IRQ_SIO, 0)
            return True
        mio = self.master._native.memory.io
        master_end = self.ends[0]
        # STARTED INTO A CONSOLE THAT WAS NOT READY, or on a port still in
        # error: nobody's word goes anywhere, the master's slots read $FFFF,
        # its error bit goes up and stays up (see CableEnd.error).
        glitch = self.transfers in self.glitch_at
        if glitch:
            self.glitch_at.discard(self.transfers)
        if not self.all_ready() or master_end.error or glitch:
            if self.wire is not None:
                self.wire.append((self.frames, mio[IO_SIOCNT] & 3,
                                  mio[IO_SIOMLT_SEND], ABSENT, self.all_ready()))
            master_end.error = not glitch
            for slot in (IO_SIOMULTI0, IO_SIOMULTI1, IO_SIOMULTI2, IO_SIOMULTI3):
                mio[slot] = ABSENT
            mio[IO_SIOCNT] = (mio[IO_SIOCNT] & ~SIO_START) | SIO_ERR
            lib.GBARaiseIRQ(self.master._native, IRQ_SIO, 0)
            self.failed += 1
            return False
        sio = self.slave._native.memory.io
        m = mio[IO_SIOMLT_SEND]
        s = sio[IO_SIOMLT_SEND]
        if self.latched is not None:
            m, s = self.latched
            self.latched = None
        # A CONSOLE WITH NO CARTRIDGE, in its BIOS: its answers are the
        # handshake's, not whatever the slave core has in its register (see
        # FakeMultibootBios).
        if getattr(self, "fake_bios", None) is not None:
            # ...at the speed the master's port is set to (SIOCNT bits 0-1).
            s = self.fake_bios.answer(m, mio[IO_SIOCNT] & 3)
        elif (mio[IO_SIOCNT] & 3) != (sio[IO_SIOCNT] & 3):
            # TWO SPEEDS ON ONE CABLE: neither console makes a word of what
            # the other sent. The probe for a console in its BIOS runs at
            # 115200 and the game at 38400 (gba/link.c), so a port left at
            # the probe's speed — at rest, its beacon going out — would be a
            # console the other end cannot hear at all.
            m = s = ABSENT
        if self.wire is not None:
            self.wire.append((self.frames, mio[IO_SIOCNT] & 3, m, s, True))
        if self.late_start and _is_move(m) and self.slave_lobby_word is not None:
            self.late_start -= 1
            s = self.slave_lobby_word
        elif s != ABSENT and not _is_move(s):
            self.slave_lobby_word = s
        if self.transfers in self.stale_slave_at and self.slave_sent is not None:
            self.stale_slave_at.discard(self.transfers)
            s = self.slave_sent
        else:
            self.slave_sent = s
        if self.transfers in self.mute_slave_at:
            self.mute_slave_at.discard(self.transfers)
            self.ends[1].mute = True
        if self.ends[0].mute:
            m = ABSENT
        if self.ends[1].mute:
            s = ABSENT
        if m == ABSENT or s == ABSENT:
            self.failed += 1

        for io in (mio, sio):
            io[IO_SIOMULTI0] = m
            io[IO_SIOMULTI1] = s
            io[IO_SIOMULTI2] = ABSENT
            io[IO_SIOMULTI3] = ABSENT
            io[IO_SIOCNT] &= ~SIO_START

        if self.sd_lags:
            self.lagging = True
            for core in (self.master, self.slave):
                self._set_bit(core, SIO_SD, False)
        lib.GBARaiseIRQ(self.master._native, IRQ_SIO, 0)
        lib.GBARaiseIRQ(self.slave._native, IRQ_SIO, 0)
        if s != ABSENT and m != ABSENT:
            self.transfers += 1
        return True


# Every pair of cores a check makes is kept here until the process ends.
# Freeing one — `del`, or the collector — frees a core out from under its
# cable and segfaults (see the note at the bottom of this file).
_KEEP = []


def symbol(rom_path, name):
    """(address, size) of a symbol in the built ROM, out of the ELF.

    Link-time optimisation renames some of them to `name.lto_priv.N`, and
    which ones moves with the code around them: g_wl_present was renamed so,
    and three wireless checks went quiet (SALTADO) instead of failing. Both
    spellings count; two of the second kind would be two variables, and that
    is an error rather than a guess."""
    elf = os.path.splitext(rom_path)[0] + ".elf"
    if not os.path.exists(elf):
        return None, f"no encuentro {elf} (hace falta para leer el estado)"
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    try:
        out = subprocess.check_output([nm, "-S", elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        return None, f"no pude ejecutar {nm}: {exc}"
    renamed = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        if parts[3] == name:
            return (int(parts[0], 16), int(parts[1], 16)), None
        if parts[3].startswith(name + ".lto_priv."):
            renamed.append((int(parts[0], 16), int(parts[1], 16)))
    if len(renamed) == 1:
        return renamed[0], None
    if renamed:
        return None, f"el ELF tiene {len(renamed)} {name}.lto_priv: cual?"
    return None, f"el ELF no exporta {name}"


def read_bytes(core, addr, count):
    m = core.memory.u8
    return bytes(m[addr + i] for i in range(count))


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: run_link.py build/tengen.gba")
    rom = sys.argv[1]

    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    session_addr, session_size = sym
    # TengenLink is the TengenGame followed by this console's own slot, its
    # frame counter and the desync flag — the four bytes that are SUPPOSED to
    # differ between the two consoles. Everything before them is the game, and
    # the game is what must match.
    game_size = session_size - 4

    mgba.log.silence()
    cores, screens = [], []
    for _ in range(2):
        core = mgba.core.load_path(rom)
        if core is None:
            sys.exit(f"mGBA could not load {rom}")
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)   # must stay alive; see run_rom.py load()
        core.reset()
        cores.append(core)
        screens.append(screen)

    master, slave = cores
    cable = Cable(master, slave)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)

    failures = []

    def both(frames, keys=None):
        """One frame on each console, alternately, master first — which is the
        order they really run in, the master starting each transfer and the
        slave answering it."""
        for _ in range(frames):
            for i, core in enumerate(cores):
                core.set_keys(*(keys[i] if keys else []))
                core.run_frame()

    # THE CHOOSING COMES AFTER THE CONNECTING NOW. Both players walk to the
    # cable together — title, GAME SELECT, 2 PLAYER — and there the handshake
    # parks at its greeting: whichever console the cable made master goes on to
    # the level screen, the other stays put with a dancing cossack. So only
    # the master's START starts the match.
    def tap(name, who=None):
        keys = [[KEYS[name]] if who in (None, i) else [] for i in range(2)]
        both(4, keys)
        both(6, [[], []])

    both(8)
    tap("START")     # title -> game select
    tap("DOWN")      # 1 PLAYER -> 2 PLAYER
    tap("START")     # -> the cable

    # Long enough for the greeting to land and the master to reach the menu.
    both(40)
    if cable.transfers == 0:
        failures.append("no hubo ni una transferencia mientras se buscaban")

    # Only the master presses START; the slave must not be able to start it.
    tap("START", who=1)
    both(20)
    if read_bytes(master, session_addr, game_size) != bytes(game_size):
        failures.append("el esclavo pudo arrancar la partida el solo")

    # TWO HANDICAPS, AND SELECT PICKS WHICH. Only here: 2 PLAYER is the one
    # mode with two on the cartridge's handicap screen (against the computer
    # there is one cursor, and both boards take it). Up and Down already move
    # the cursor and Left and Right the value, so SELECT — "the other one"
    # everywhere else on this page — hops between the two numbers without
    # leaving the line, and the one it is on is written in the cursor's
    # white.
    import run_rom

    def hrow():
        return run_rom.tilemap_text(master, run_rom.HANDICAP_ROW)

    def white_digits():
        text = hrow()
        banks = [(master.memory.u16[run_rom.SCREENBLOCK_ADDR +
                                    (run_rom.HANDICAP_ROW * 32 + c) * 2] >> 12) & 0xF
                 for c in range(30)]
        return "".join(ch for ch, b in zip(text, banks)
                       if ch.isdigit() and b == run_rom.MENU_ARROW_BANK)

    tap("DOWN", who=0)                    # the cursor onto HANDICAP
    steps = []
    if "HANDICAP 0 0" not in hrow():
        failures.append(f"en 2 PLAYER la fila no trae dos numeros: {hrow()!r}")
    else:
        tap("RIGHT", who=0)
        steps.append(("DERECHA sube el primero", white_digits() == "3"))
        tap("SELECT", who=0)
        steps.append(("SELECT pasa al segundo",
                      white_digits() == "0" and ">" in hrow()))
        tap("RIGHT", who=0); tap("RIGHT", who=0)
        steps.append(("DERECHA sube el segundo", white_digits() == "6"))
        tap("SELECT", who=0)
        steps.append(("SELECT vuelve al primero", white_digits() == "3"))
        if "HANDICAP 3 6" not in hrow() or not all(lit for _, lit in steps):
            failures.append(f"SELECT y DERECHA no llevan cada handicap por su "
                            f"lado: {hrow()!r}, pasos {steps}")
        else:
            print("  en 2 PLAYER SELECT pasa de un handicap al otro sin salir "
                  "de la linea: 3 filas para uno, 6 para el otro")

    # The cable put the master on LEVEL SETTINGS already; one START from
    # there releases the handshake and both consoles go.
    tap("START", who=0)
    both(10)

    # The handshake is five stages of two transfers; give it far more than
    # that and then check it did not just time out.
    both(90)
    if cable.transfers == 0:
        failures.append("no hubo ni una transferencia por el cable")

    m_state = read_bytes(master, session_addr, game_size)
    s_state = read_bytes(slave, session_addr, game_size)
    if m_state == bytes(game_size):
        failures.append("la partida no arranco: el estado sigue en cero")
    if m_state != s_state:
        failures.append("las dos consolas empezaron con partidas distintas")

    # ...and each board is buried under its own: 3 rows and 6. Garbage is
    # the $F sentinel inside the walls.
    PF_W, PF_H = 12, 20
    buried = []
    for board in (0, 1):
        cells = m_state[board * PF_W * PF_H:(board + 1) * PF_W * PF_H]
        buried.append(sum(1 for r in range(PF_H)
                          if any(cells[r * PF_W + c] == 15 for c in range(1, 11))))
    if buried != [3, 6]:
        failures.append(f"los handicaps no llegan a los tableros: filas de "
                        f"basura {buried}, deberian ser [3, 6]")
    else:
        print("  y cada tablero sale enterrado bajo el suyo, igual en las dos "
              "consolas")

    # Each console must know which player it is, and they must not agree.
    view, why = symbol(rom, "g_view")
    if view is not None:
        m_view = master.memory.u8[view[0]]
        s_view = slave.memory.u8[view[0]]
        if (m_view, s_view) != (0, 1):
            failures.append(f"los slots salieron mal: maestro={m_view} esclavo={s_view}")
        else:
            print("  el maestro juega de jugador 1, el esclavo de jugador 2")

    if failures:
        for f in failures:
            print(f"FALLA: {f}")
        return 1

    print(f"  handshake completo en {cable.transfers} transferencias")

    # Now play. The two consoles are given DIFFERENT buttons, which is the
    # whole point: each sends its own and receives the other's, and both must
    # end up simulating the same two boards.
    script = [
        (60, ["LEFT"], ["RIGHT"]),
        (40, ["DOWN"], []),
        (40, ["A"], ["B"]),
        (60, ["RIGHT"], ["LEFT"]),
        (60, [], ["DOWN"]),
        (100, ["DOWN"], ["DOWN"]),
    ]
    frame = 0
    diverged = None
    for count, p1, p2 in script:
        for _ in range(count):
            for i, core in enumerate(cores):
                core.set_keys(*[KEYS[k] for k in (p1 if i == 0 else p2)])
                core.run_frame()
            frame += 1
            if diverged is None:
                a = read_bytes(master, session_addr, game_size)
                b = read_bytes(slave, session_addr, game_size)
                if a != b:
                    diverged = frame

    if diverged is not None:
        failures.append(f"las dos consolas divergieron en el frame {diverged}")

    # And the match must have actually been played, or the comparison above
    # proved nothing: both boards moved, and neither console gave up on the
    # cable.
    final = read_bytes(master, session_addr, session_size)
    link_frame = final[game_size + 1]
    if link_frame == 0:
        failures.append("el contador de frames del enlace no avanzo")
    if final[game_size + 2]:
        failures.append("el enlace se declaro desincronizado")
    if read_bytes(slave, session_addr, session_size)[game_size + 2]:
        failures.append("el esclavo se declaro desincronizado")

    # And the two boards must have been played DIFFERENTLY, or "identical on
    # both consoles" would be a much weaker claim than it sounds: two consoles
    # ignoring the cable and each running the same solo game would also match.
    # The two players were given opposite buttons, so their stacks must differ.
    PF = 20 * 12                      # TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH
    board1, board2 = final[:PF], final[PF:PF * 2]
    if board1 == board2:
        failures.append("los dos campos son identicos: no se jugaron por separado")
    else:
        settled = sum(1 for i in range(PF * 2)
                       if final[i] not in (0, 15))   # 15 is the wall sentinel
        print(f"  {settled} celdas asentadas entre los dos campos, y distintas")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1

    print(f"  {frame} frames jugados, {cable.transfers} transferencias, "
           f"estado identico en las dos consolas byte a byte")

    # Finally: pull the cable out mid-match. Neither console may freeze, both
    # must notice — and both must WAIT: a knocked plug is not the end of a
    # match any more (link_wait in gba/match.c). Plugged back in, the match
    # goes on from where it stopped, still the same game on both.
    waiting, why = symbol(rom, "g_link_waiting")
    lost, why2 = symbol(rom, "g_link_lost")
    if waiting is None or lost is None:
        print(f"  (sin comprobar el tiron del cable: {why or why2})")
    else:
        def link_frame(core):
            return core.memory.u8[session_addr + game_size + 1]
        cable.plugged = False
        before = cable.transfers
        both(200, [[], []])
        if cable.transfers != before:
            failures.append("siguio habiendo transferencias con el cable fuera")
        for name, core in (("maestro", master), ("esclavo", slave)):
            if not core.memory.u8[waiting[0]]:
                failures.append(f"el {name} no se entero de que el cable se fue")
            if core.memory.u8[lost[0]]:
                failures.append(f"el {name} dio la partida por perdida en vez "
                                f"de esperar al cable")
        # ...and the ROM is still running, not wedged in a serial wait: its
        # own frame counter has to have moved.
        if master.frame_counter == 0:
            failures.append("el maestro dejo de correr frames")
        frozen = link_frame(master)
        cable.plugged = True
        both(120, [[KEYS["DOWN"]], [KEYS["DOWN"]]])
        m = read_bytes(master, session_addr, game_size)
        sl = read_bytes(slave, session_addr, game_size)
        if any(core.memory.u8[waiting[0]] for core in (master, slave)):
            failures.append("con el cable de vuelta la partida sigue esperando")
        elif (link_frame(master) - frozen) % 256 < 60:
            failures.append(f"con el cable de vuelta la partida no avanza "
                            f"(frame {frozen} -> {link_frame(master)})")
        elif m != sl:
            failures.append("tras volver el cable las dos consolas ya no "
                            "juegan la misma partida")
        for f in failures:
            print(f"FALLA: {f}")
        if failures:
            return 1
        print("  cable fuera: las dos consolas esperan; de vuelta, la partida "
              "sigue igual en las dos")

    print("OK: dos GBA por cable link juegan la misma partida sin desviarse.")
    return coop_check(rom)


def coop_check(rom):
    """COOPERATIVE: two consoles, ONE twelve-wide board between them.

    The shared field is what makes this its own check rather than a flag on
    the one above: 2P can pass while coop is broken, because 2P never writes
    two players into one playfield. What it looks for is the field the port
    actually draws — twelve columns of it, at the coop screen's own origin —
    and pieces from both players settling in it.
    """
    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    session_addr, session_size = sym
    game_size = session_size - 4

    mgba.log.silence()
    cores, screens = [], []
    for _ in range(2):
        core = mgba.core.load_path(rom)
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
        core.reset()
        cores.append(core)
        screens.append(screen)
    master, slave = cores
    cable = Cable(master, slave)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)

    def both(frames, keys=None):
        for _ in range(frames):
            for i, core in enumerate(cores):
                core.set_keys(*(keys[i] if keys else []))
                core.run_frame()

    def tap(name, who=None):
        both(4, [[KEYS[name]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    failures = []
    both(8)
    tap("START")                      # title -> game select
    tap("DOWN"); tap("DOWN")          # 1 PLAYER -> 2 PLAYER -> COOPERATIVE
    tap("START")                      # -> the cable
    both(50)
    tap("START", who=0)               # the master releases the lobby
    both(60)

    if read_bytes(master, session_addr, game_size) == bytes(game_size):
        failures.append("la partida cooperativa no arranco")

    # Play it: opposite buttons, so the two are not doing the same thing.
    script = [(60, ["LEFT"], ["RIGHT"]), (50, ["DOWN"], ["DOWN"]),
              (40, ["A"], ["B"]), (60, ["RIGHT"], ["LEFT"]),
              (120, ["DOWN"], ["DOWN"])]
    diverged = None
    frame = 0
    for count, p1, p2 in script:
        for _ in range(count):
            for i, core in enumerate(cores):
                core.set_keys(*[KEYS[k] for k in (p1 if i == 0 else p2)])
                core.run_frame()
            frame += 1
            if diverged is None and (read_bytes(master, session_addr, game_size)
                                     != read_bytes(slave, session_addr, game_size)):
                diverged = frame
    if diverged is not None:
        failures.append(f"las dos consolas divergieron en el frame {diverged}")

    # ONE board: everything settled is in field[0], and field[1] is untouched.
    PF = 20 * 12
    final = read_bytes(master, session_addr, session_size)
    shared = sum(1 for b in final[:PF] if b)
    other = sum(1 for b in final[PF:PF * 2] if b)
    if other:
        failures.append(f"el segundo campo tiene {other} celdas: coop deberia "
                         "jugarse entero en el primero")
    if shared < 8:
        failures.append(f"solo {shared} celdas asentadas: no se jugo")

    # ...and it is TWELVE wide ON SCREEN, which is the half of coop the core
    # tests cannot see. The two outermost storage columns are the ones that
    # only exist here — elsewhere they hold the wall sentinel — so a block
    # planted in each has to come out as ink at the coop layout's own origin
    # and eleven columns along from it.
    import run_rom
    off = run_rom.game_offsets(rom)
    field = session_addr + off["field"]
    row = 18
    for col in (0, TENGEN_PF_WIDTH - 1):
        master.memory.u8[field + row * TENGEN_PF_WIDTH + col] = 0x0F
    both(3, [[], []])
    px = run_rom.pixels(screens[0])
    y = row * 8 + 4
    for col, tx in ((0, COOP_FIELD_TX), (TENGEN_PF_WIDTH - 1,
                                          COOP_FIELD_TX + TENGEN_PF_WIDTH - 1)):
        if all(px[y][tx * 8 + x] == (0, 0, 0) for x in range(1, 7)):
            failures.append(f"la columna {col} del campo no se dibuja en la "
                             f"columna {tx} de la pantalla: coop no esta "
                             "usando sus doce columnas")
    # AND THE GUEST SEES IT TOO. The guest views slot 1, and drawing the
    # field by view showed it field[1] — the one coop never writes — so the
    # partner's console had an empty board with two pieces falling through
    # it. Every cell settled in the shared field has to be ink on the
    # guest's screen as well. (`final` was read before the two cells above
    # were planted on the master alone.)
    gpx = run_rom.pixels(screens[1])
    missing = 0
    for r in range(20):
        for c in range(TENGEN_PF_WIDTH):
            if not final[r * TENGEN_PF_WIDTH + c]:
                continue
            if gpx[r * 8 + 4][(COOP_FIELD_TX + c) * 8 + 4] == (0, 0, 0):
                missing += 1
    if missing:
        failures.append(f"el invitado no ve {missing} de las {shared} celdas "
                         "asentadas: dibuja el campo equivocado")
    if not failures:
        print(f"  campo compartido: {shared} celdas, doce columnas desde la "
               f"columna {COOP_FIELD_TX}, el segundo campo vacio, y el "
               "invitado ve las mismas celdas")

    # ...AND EACH PANEL STANDS ON ITS PLAYER'S SIDE OF THE BOARD. The coop
    # HUD is one panel per player, NEXT, score and lines, and player 1's
    # pieces come in on the left and player 2's on the right — so player 1's
    # panel is on the left and player 2's on the right on BOTH consoles. It
    # used to be "yours on the left", which on the second console put each
    # preview over the other player's half of the board; two players
    # swapping consoles kept losing track of which piece was theirs.
    # Distinct scores are planted on both cores (identically, so the
    # simulation does not diverge) and each screen is read back.
    LEFT, RIGHT = (0, 7), (30 - 7, 30)   # the two coop panels, COOP_PANEL_W wide
    # BELOW THE TABLE'S LAST ENTRY (3000) on purpose: a qualifying score
    # would send the game-over check below into typing initials instead of
    # back to the title.
    for who, score in ((0, 2345), (1, 1678)):
        for core in cores:
            at = session_addr + off["score"] + who * off["stride"]
            for k in range(4):
                core.memory.u8[at + k] = (score >> (8 * k)) & 0xFF
    both(40, [[], []])

    def panel(core, cols):
        rows = [run_rom.tilemap_text(core, ty, *cols) for ty in range(9, 14)]
        return " ".join(r for r in rows if r)

    mine = (panel(master, LEFT), panel(master, RIGHT))
    theirs = (panel(slave, LEFT), panel(slave, RIGHT))
    if "2345" not in mine[0] or "1678" not in mine[1]:
        failures.append(f"el anfitrion no lleva al jugador 1 a la izquierda: {mine!r}")
    elif "2345" not in theirs[0] or "1678" not in theirs[1]:
        failures.append(f"el invitado no lleva al jugador 1 a la izquierda, "
                        f"donde caen sus piezas: {theirs!r}")
    else:
        print("  y las dos consolas llevan el panel del jugador 1 a la "
               "izquierda y el del 2 a la derecha, donde caen sus piezas")

    # THE OTHER COOP HUD IS NOT FOR THE CABLE. It hides the partner's board,
    # which against the computer is a difficulty setting and against a person
    # is just less game, so SELECT must do nothing here.
    before = (panel(master, RIGHT), panel(slave, RIGHT))
    both(4, [[KEYS["SELECT"]], [KEYS["SELECT"]]])
    both(40, [[], []])
    if (panel(master, RIGHT), panel(slave, RIGHT)) != before:
        failures.append("SELECT cambia el HUD en un cooperativo por cable")
    else:
        print("  y SELECT no esconde el panel del companero por cable")

    # AND IT HAS TO END. Topping out a coop game used to leave the partner's
    # `game_active` standing, so neither console ever agreed the match was
    # over: the board sat there and Start did nothing. Buried by hand — solid
    # but for one column, so no row can complete — and buried IDENTICALLY on
    # both cores, which is the only way to touch memory without breaking the
    # lockstep the rest of this check just proved.
    def face(core):
        return tuple(run_rom.tilemap_text(core, r) for r in (4, 6, 8))

    reference = mgba.core.load_path(rom)
    ref_screen = mgba.image.Image(SCREEN_W, SCREEN_H)
    reference.set_video_buffer(ref_screen)
    reference.reset()
    for _ in range(40):
        reference.run_frame()
    title = face(reference)

    for core in cores:
        for r in range(TENGEN_PF_HEIGHT):
            for c in range(TENGEN_PF_WIDTH):
                core.memory.u8[field + r * TENGEN_PF_WIDTH + c] = (
                    0 if c == 5 else 0x01)
    both(300, [[], []])

    active = [read_bytes(core, session_addr + off["active"] + slot * off["stride"], 1)[0]
              for core in cores for slot in (0, 1)]
    if any(active):
        failures.append(f"tras el game over cooperativo siguen vivos {active}: "
                         "en coop mueren los dos a la vez")
    else:
        # Out through the HIGH SCORES page, which is the cartridge's own road
        # back to the title (main.asm.txt:2643-2675).
        tap("START")
        both(40, [[], []])
        blind = [i for i, core in enumerate(cores)
                 if "-LINES" not in run_rom.tilemap_text(core, 2, 0, 30)]
        if blind:
            failures.append(f"la(s) consola(s) {blind} no llegan a la tabla de "
                             "records tras el game over cooperativo")
        else:
            tap("START")
            both(40, [[], []])
            stuck = [i for i, core in enumerate(cores) if face(core) != title]
            if stuck:
                failures.append(f"la(s) consola(s) {stuck} no vuelven al titulo "
                                 "desde la tabla")
            else:
                print("  las dos mueren juntas y las dos salen por la tabla de "
                       "records al titulo")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: en cooperativo los dos juegan en un solo campo de doce columnas.")
    return records_check(rom)


def records_check(rom):
    """THE RIVAL'S RECORD, and the one thing lockstep cannot hand over.

    A linked match simulates both boards on both consoles, so each one knows
    the other player's score and lines to the byte — the check above proves
    exactly that, byte for byte. What neither can know is the NAME the person
    at the other end typed, and without it a race ended with two HIGH SCORES
    pages that disagreed about who had been there: each console wrote down
    its own player and nobody else.

    So both consoles put BOTH players on their own table, each types only its
    own three letters, and the letters cross afterwards on the same cable the
    match ran on. What this drives is that whole road: a linked 2P game,
    two different scores, two different names typed on two different
    consoles, and then both pages read back off the tilemap.
    """
    import run_rom

    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    session_addr, session_size = sym
    game_size = session_size - 4
    off = run_rom.game_offsets(rom)

    mgba.log.silence()
    cores, screens = [], []
    for _ in range(2):
        core = mgba.core.load_path(rom)
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
        core.reset()
        cores.append(core)
        screens.append(screen)
    master, slave = cores
    cable = Cable(master, slave)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)

    def both(frames, keys=None):
        for _ in range(frames):
            for i, core in enumerate(cores):
                core.set_keys(*(keys[i] if keys else []))
                core.run_frame()

    def tap(name, who=None):
        both(4, [[KEYS[name]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    failures = []
    both(8)
    tap("START")            # title -> game select
    tap("DOWN")             # 1 PLAYER -> 2 PLAYER
    tap("START")            # -> the cable
    both(50)
    tap("START", who=0)     # the master releases the lobby
    both(60)
    if read_bytes(master, session_addr, game_size) == bytes(game_size):
        print("FALLA: la partida por cable no arranco")
        return 1

    # TWO SCORES THAT BOTH BELONG ON THE TABLE, planted identically on both
    # cores — the only way to touch memory here without breaking the lockstep
    # the checks above just proved. The cold table runs 17000 down to 3000,
    # so both of these go in near the top and neither falls off.
    SCORES = (60000, 45000)
    for who, score in enumerate(SCORES):
        for core in cores:
            at = session_addr + off["score"] + who * off["stride"]
            for k in range(4):
                core.memory.u8[at + k] = (score >> (8 * k)) & 0xFF

    # ...and bury both boards, solid but for one column so no row completes.
    field = session_addr + off["field"]
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH
    for core in cores:
        for board in (0, 1):
            for r in range(TENGEN_PF_HEIGHT):
                for c in range(TENGEN_PF_WIDTH):
                    core.memory.u8[field + board * PF + r * TENGEN_PF_WIDTH + c] = (
                        CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                        else (0 if c == 5 else 0x01))
    both(300, [[], []])

    # WHAT THEY ACTUALLY ENDED ON, not what was planted: the last piece of
    # each board is still worth points as it lands, so the figure on the
    # table is a little above the one written in.
    final = []
    for who in range(2):
        at = session_addr + off["score"] + who * off["stride"]
        final.append(sum(master.memory.u8[at + k] << (8 * k) for k in range(4)))

    tap("START")            # off the plaque, onto the table
    both(40, [[], []])
    blind = [i for i, core in enumerate(cores)
             if "-LINES" not in run_rom.tilemap_text(core, 2, 0, 30)]
    if blind:
        print(f"FALLA: la(s) consola(s) {blind} no llegan a la tabla")
        return 1

    def rows(core):
        return [run_rom.tilemap_text(core, run_rom.LEADER_FIRST_TY + i, 0, 30)
                for i in range(15)]

    # BOTH SCORES, ON BOTH PAGES. This is the half that needed no cable at
    # all — each console had simulated the other's board all along — and the
    # half that was simply never written down.
    for name, core in (("maestro", master), ("esclavo", slave)):
        # Leading zeros blanked, as the cartridge prints them: a score is a
        # word of its own on the page.
        page = " ".join(rows(core)).split()
        for score in final:
            if str(score) not in page:
                failures.append(f"el {name} no anoto {score}: la tabla solo "
                                 "lleva a su propio jugador")
    if not failures:
        print(f"  las dos consolas anotan las dos puntuaciones, {final[0]} y "
               f"{final[1]}")

    # AND NOW THE NAMES. Each console types its own and only its own: the
    # master walks the alphabet twice for a B and the slave four times for a
    # D, so the two names cannot be confused for one another.
    def letters(who, steps):
        for _ in range(LEADER_INITIALS):
            for _ in range(steps):
                both(3, [[KEYS["UP"]] if who == i else [] for i in range(2)])
                both(5, [[], []])
            both(3, [[KEYS["A"]] if who == i else [] for i in range(2)])
            both(6, [[], []])

    letters(0, 1)     # the master types BBB
    letters(1, 3)     # the slave types DDD
    # ...and then the swap, which is a handful of transfers. Give it room.
    both(180, [[], []])

    for name, core, own, rival in (("maestro", master, "BBB", "DDD"),
                                    ("esclavo", slave, "DDD", "BBB")):
        page = rows(core)
        if not any(own in row for row in page):
            failures.append(f"el {name} no tiene su propio nombre {own} en la "
                             f"tabla: {page[0]!r} / {page[1]!r}")
        elif not any(rival in row for row in page):
            failures.append(f"el {name} no recibio el nombre del rival "
                             f"({rival}): {page[0]!r} / {page[1]!r}")
    if not failures:
        print("  y el nombre que tecleo cada jugador cruza el cable al otro")

    # THE ROW IS THE RIVAL'S ROW, not just their letters somewhere: the name
    # has to land beside the score that earned it.
    for name, core, pairs in (
            ("maestro", master, ((final[0], "BBB"), (final[1], "DDD"))),
            ("esclavo", slave, ((final[0], "BBB"), (final[1], "DDD")))):
        for score, who in pairs:
            row = next((r for r in rows(core) if str(score) in r.split()), "")
            if who not in row:
                failures.append(f"en el {name} la fila de {score} no lleva "
                                 f"{who}: {row!r}")
    if not failures:
        print("  y cada nombre va en la fila de la puntuacion que lo gano")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: los dos acaban con la misma pagina de records, con el rival "
           "dentro y con su nombre.")
    return restart_check(rom)


def restart_check(rom):
    """A+B PUTS A DEAD BOARD BACK ON ITS FEET, and the race carries on.

    The cartridge reads the dead player's own pad every frame its board is
    finished — activeGamePlay falls into handleGameOver the moment
    player1GameActive,x reads zero, whether or not the other board is still
    going (main.asm.txt:472-478) — and A and B held together run
    restartVsMode. The port ended the match instead.

    Over a cable this is the one that could quietly break everything: the
    restart is driven by BUTTONS, which cross the wire, so it has to happen
    inside the core on the frame both consoles agree on. If it were done in
    the front end off the local keypad, the two simulations would part
    company on that frame and never meet again. So what this measures, after
    the restart, is the same thing every other check here measures: the whole
    game state, byte for byte, on both consoles.
    """
    import run_rom

    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    session_addr, session_size = sym
    game_size = session_size - 4
    off = run_rom.game_offsets(rom)

    mgba.log.silence()
    cores, screens = [], []
    for _ in range(2):
        core = mgba.core.load_path(rom)
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
        core.reset()
        cores.append(core)
        screens.append(screen)
    master, slave = cores
    cable = Cable(master, slave)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)

    def both(frames, keys=None):
        for _ in range(frames):
            for i, core in enumerate(cores):
                core.set_keys(*(keys[i] if keys else []))
                core.run_frame()

    def tap(name, who=None):
        both(4, [[KEYS[name]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    def hold(names, who, frames):
        both(frames, [[KEYS[n] for n in names] if who == i else []
                      for i in range(2)])
        both(10, [[], []])

    def alive(core, slot):
        return core.memory.u8[session_addr + off["active"] + slot * off["stride"]]

    def score(core, slot):
        at = session_addr + off["score"] + slot * off["stride"]
        return sum(core.memory.u8[at + k] << (8 * k) for k in range(4))

    failures = []
    both(8)
    tap("START"); tap("DOWN"); tap("START")   # title -> 2 PLAYER -> the cable
    both(50)
    tap("START", who=0)
    both(60)
    if read_bytes(master, session_addr, game_size) == bytes(game_size):
        print("FALLA: la partida por cable no arranco")
        return 1

    # Player 2's board alone, buried identically on both cores — the only way
    # to touch memory without breaking the lockstep this check is about.
    field = session_addr + off["field"]
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH
    for core in cores:
        # A score that belongs on the table (the cold one runs 17000 down to
        # 3000), or "the game before the restart is still there" would be
        # checking nothing.
        at = session_addr + off["score"] + off["stride"]
        for k in range(4):
            core.memory.u8[at + k] = (33000 >> (8 * k)) & 0xFF
        for r in range(TENGEN_PF_HEIGHT):
            for c in range(TENGEN_PF_WIDTH):
                core.memory.u8[field + PF + r * TENGEN_PF_WIDTH + c] = (
                    CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                    else (0 if c == 5 else 0x01))
    both(240, [[], []])

    if alive(master, 1):
        print("FALLA: el tablero del jugador 2 no llego a morir")
        return 1
    if not alive(master, 0):
        failures.append("el jugador 1 murio tambien: el otro tablero deberia "
                         "seguir corriendo")

    # A ALONE MUST NOT LEAVE. It used to: START, A and B all took a dead board
    # to the table, so the chord could never be held down long enough to mean
    # anything. Now, while the other board is still going, the way out is
    # START and A and B belong to the restart.
    hold(["A"], 1, 20)
    if "-LINES" in run_rom.tilemap_text(slave, 2, 0, 30):
        failures.append("A solo se lleva a la tabla: el acorde no puede existir")
    elif alive(slave, 1):
        failures.append("A solo reinicio el tablero")
    else:
        print("  con el otro tablero vivo, A solo ni sale ni reinicia")

    dead_score = score(master, 1)
    hold(["A", "B"], 1, 20)
    if not alive(master, 1):
        failures.append("A+B no levanto el tablero muerto")
    elif score(master, 1) != 0:
        failures.append(f"el tablero reiniciado conserva {score(master, 1)} "
                         "puntos: deberia empezar de cero")
    elif not alive(master, 0):
        failures.append("el reinicio se llevo por delante al otro jugador")
    else:
        settled = sum(1 for i in range(PF)
                       if master.memory.u8[field + PF + i] not in (0, CELL_WALL))
        if settled:
            failures.append(f"el tablero reiniciado conserva {settled} celdas")
        else:
            print(f"  A+B levanta el tablero: {dead_score} puntos a cero y el "
                   "campo limpio, sin tocar al rival")

    # AND THE TWO CONSOLAS SIGUEN SIENDO LA MISMA PARTIDA. This is the claim
    # that matters: the restart is driven by buttons that crossed the wire, so
    # it has to have happened on the same frame on both machines.
    both(120, [[], []])
    if read_bytes(master, session_addr, game_size) != read_bytes(slave, session_addr, game_size):
        failures.append("las dos consolas divergieron al reiniciar un tablero")
    elif read_bytes(master, session_addr, session_size)[game_size + 2]:
        failures.append("el enlace se declaro desincronizado tras el reinicio")
    else:
        print("  y las dos consolas siguen byte a byte en la misma partida")

    # ...AND THE GAME IT JUST FINISHED IS ON THE BOARD. The cartridge writes a
    # game down as it ends (L81DD from the top-out itself, main.asm.txt:600),
    # which is what stops a restart throwing the last one away. Bury both and
    # read the page: two rows for player 2's two games, and one for player 1.
    for core in cores:
        for board in (0, 1):
            for r in range(TENGEN_PF_HEIGHT):
                for c in range(TENGEN_PF_WIDTH):
                    core.memory.u8[field + board * PF + r * TENGEN_PF_WIDTH + c] = (
                        CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                        else (0 if c == 5 else 0x01))
    # Player 2's second game has to make the table too, or there is only one
    # row of theirs to name: plant a score on it on both consoles, the same
    # on each so the two stay the same game.
    # (Player 1's too, so the master is on the page to see the name arrive.)
    for core in cores:
        for slot, planted in ((0, 20000), (1, 50000)):
            at = session_addr + off["score"] + slot * off["stride"]
            for k in range(4):
                core.memory.u8[at + k] = (planted >> (8 * k)) & 0xFF
    both(300, [[], []])
    last_score = score(master, 1)      # player 2's second game
    tap("START")
    both(40, [[], []])
    page = " ".join(run_rom.tilemap_text(slave, run_rom.LEADER_FIRST_TY + i, 0, 30)
                     for i in range(15)).split()
    if "-LINES" not in run_rom.tilemap_text(slave, 2, 0, 30):
        failures.append("no se llega a la tabla tras el segundo game over")
    elif str(dead_score) not in page:
        failures.append(f"la partida que acabo antes del reinicio ({dead_score}) "
                         "no esta en la tabla: el reinicio se la comio")
    else:
        print(f"  y la partida anterior al reinicio ({dead_score}) sigue "
               "anotada: cada partida es una fila")

    # ...AND THE NAME THAT CROSSES THE CABLE GOES ON BOTH OF THEM. Player 2
    # has two rows on the master's page, one per game; the slave types its
    # name once, and the master must show it on each, not just the newest.
    for _ in range(LEADER_INITIALS):
        both(3, [[KEYS["A"]], []]); both(6, [[], []])
    # The slave has one row per game to type, in the order they were played,
    # and the name only crosses once both are done. The second row comes up
    # with the name just typed in the first (g_last_name), so A takes it.
    for _ in range(LEADER_INITIALS):
        for _ in range(3):
            both(3, [[], [KEYS["UP"]]]); both(5, [[], []])
        both(3, [[], [KEYS["A"]]]); both(6, [[], []])
    for _ in range(LEADER_INITIALS):
        both(3, [[], [KEYS["A"]]]); both(6, [[], []])
    both(150, [[], []])
    mrows = [run_rom.tilemap_text(master, run_rom.LEADER_FIRST_TY + i, 0, 30)
             for i in range(15)]
    for sc in (dead_score, last_score):
        row = next((r for r in mrows if str(sc) in r.split()), "")
        if "DDD" not in row:
            failures.append(f"en el maestro la fila del rival de {sc} no lleva "
                            f"su nombre DDD: {row!r}")
    if not any("fila del rival" in f for f in failures):
        print("  y el nombre del rival llega a sus dos filas en la pantalla "
              "del maestro")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: A+B levanta un tablero muerto sin parar la carrera ni romper "
           "el enlace.")
    return walkout_check(rom)


def _bury(cores, session_addr, off, slot):
    """A board filled to the top but for one column, the same on every core
    given: its next piece does not fit."""
    field = session_addr + off["field"]
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH
    for core in cores:
        for r in range(TENGEN_PF_HEIGHT):
            for c in range(TENGEN_PF_WIDTH):
                core.memory.u8[field + slot * PF + r * TENGEN_PF_WIDTH + c] = (
                    CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                    else (0 if c == 5 else 0x01))


def _to_a_match(rom, coop=False, chord=True, tune=True):
    """Two cartridges from power-on into a linked match: the chord rung on
    both (so the match is under it, the master's), a tune chosen. Returns
    (cores, cable, both, tap), or None if the match did not start."""
    linked = symbol(rom, "g_linked")[0]
    cores, cable, both, tap = _pair(rom)
    both(8)
    tap("START")
    if chord:
        both(4, [[KEYS["L"], KEYS["R"]]] * 2)
        both(10, [[], []])
    tap("DOWN")
    if coop:
        tap("DOWN")
    tap("START")
    both(60)
    if tune:
        tap("DOWN", who=0); tap("DOWN", who=0); tap("RIGHT", who=0)
    tap("START", who=0)
    both(90)
    if [c.memory.u8[linked[0]] for c in cores] != [1, 1]:
        return None
    return cores, cable, both, tap


def walkout_check(rom):
    """START ON A DEAD BOARD LEAVES THE RACE, AND THE OTHER CONSOLE PLAYS ON.

    Two cartridges, a real cable: one player lost a board, pressed START to
    play again, and was on the title — and the other console froze with its
    HUD still answering. The leaver's cable had been put away off its own
    keypad; at rest it still answers transfers (with 0, and a master on its
    menus sends a beacon of them), which the console left behind took for a
    partner slow to play, for ever. The same START had crossed as a button
    and paused the other player's game where it did arrive.

    Now the START is taken off the wire on the step both consoles have it:
    the leaver goes to the table, the other plays on alone from the next
    step, unpaused, its tune going, RIVAL LEFT in the rival's cell for a
    moment. Either end leaving; and afterwards the two find each other on
    the cable again. And what the dead board's pad does is said under its
    plaque, under the chord: A AND B: AGAIN, START: LEAVE.

    Also here: a board put back on its feet with A+B has its tune back (its
    console went quiet for the game-over tune and stayed so).
    """
    import run_rom
    names = ("g_session", "g_linked", "g_link_alone", "g_link_waiting",
             "g_link_lost")
    syms = {n: symbol(rom, n) for n in names}
    missing = [why for s, why in syms.values() if s is None]
    if missing:
        skip(missing[0])
        return 0
    at = {n: s[0][0] for n, s in syms.items()}
    session_addr, session_size = syms["g_session"][0]
    game_size = session_size - 4
    off = run_rom.game_offsets(rom)

    def flag(core, name):
        return core.memory.u8[at[name]]

    def alive(core, slot):
        return core.memory.u8[session_addr + off["active"] + slot * off["stride"]]

    def step_of(core):
        return core.memory.u8[session_addr + game_size + 1]

    failures = []
    for leaver in (0, 1):
        who = "la maestra" if leaver == 0 else "la esclava"
        other = "la esclava" if leaver == 0 else "la maestra"
        made = _to_a_match(rom)
        if made is None:
            print("FALLA: la carrera por cable no arranco")
            return 1
        cores, cable, both, tap = made
        gone, stay = cores[leaver], cores[1 - leaver]

        def loudest(core, frames):
            top = 0
            for _ in range(frames):
                both(1)
                top = max(top, run_rom.sound_state(core)["activos"])
            return top

        _bury(cores, session_addr, off, leaver)
        both(240)
        if alive(gone, leaver) or not alive(gone, 1 - leaver):
            print("FALLA: no queda un tablero muerto y el otro vivo "
                  f"({alive(gone, 0)}{alive(gone, 1)})")
            return 1
        # What the pad does, under the plaque of the dead board and nowhere
        # on the other console.
        hint = " ".join(run_rom.tilemap_text(gone, r) for r in range(8, 16))
        there = " ".join(run_rom.tilemap_text(stay, r) for r in range(8, 16))
        if "A AND B:" not in hint or "AGAIN" not in hint or \
                "START:" not in hint or "LEAVE" not in hint:
            failures.append(f"el tablero muerto de {who} no dice que hacen A+B "
                            f"y START: {hint.split()}")
        if "AGAIN" in there or "LEAVE" in there:
            failures.append(f"la pista del tablero muerto sale tambien en {other}")

        # UNDER A PAUSE, START IS THE WAY BACK INTO THE GAME — the dead
        # board's as much as the other's: nobody leaves on it.
        tap("START", who=1 - leaver)
        paused = [c.memory.u8[session_addr + off["paused"]] for c in cores]
        tap("START", who=leaver)
        both(10)
        after = ([flag(c, "g_linked") for c in cores],
                 [flag(c, "g_link_alone") for c in cores],
                 [c.memory.u8[session_addr + off["paused"]] for c in cores])
        if paused != [1, 1]:
            failures.append(f"START de {other} no pone la pausa en las dos: {paused}")
        elif after != ([1, 1], [0, 0], [0, 0]):
            failures.append(f"en pausa, el START de {who} (tablero muerto) no "
                            "es volver al juego: enlazadas, solas, en pausa = "
                            f"{after}")

        tap("START", who=leaver)
        both(20)
        page = run_rom.tilemap_text(gone, 2, 0, 30)
        if flag(gone, "g_linked") or "-LINES" not in page:
            failures.append(f"START en el tablero muerto no lleva a {who} a su tabla "
                            f"(enlazada {flag(gone, 'g_linked')}, {page.strip()!r})")
        state = (flag(stay, "g_linked"), flag(stay, "g_link_alone"),
                 flag(stay, "g_link_waiting"), flag(stay, "g_link_lost"),
                 stay.memory.u8[session_addr + off["paused"]],
                 alive(stay, 1 - leaver))
        if state != (1, 1, 0, 0, 0, 1):
            failures.append(f"al irse {who}, {other} no sigue sola y sin pausa: "
                            f"enlazada, sola, esperando, perdida, pausa, viva = {state}")
        note = " ".join(run_rom.tilemap_text(stay, r) for r in range(1, 9))
        if "RIVAL" not in note or "LEFT" not in note:
            failures.append(f"{other} no dice RIVAL LEFT: {note.split()}")
        # THE CABLE PUT AWAY IS NOBODY ON IT (link_connected): the count of
        # frames since the last transfer is only kept up while something
        # steps the cable, and it stood where the session left it —
        # "connected" until the power switch, which is what the cartridge's
        # flash waits on before it writes its journal afresh (cartsave.c).
        starved = symbol(rom, "g_starved")[0]
        if starved is None:
            failures.append("el ELF no tiene g_starved")
        else:
            heard = [c.memory.u16[starved[0]] for c in cores]
            if min(heard) <= 30:        # CONNECTED_GRACE, gba/link.c
                failures.append(f"guardado el cable, el juego sigue contando a "
                                f"la otra consola como presente ({heard} frames "
                                "sin oirla, y no pasan de ahi)")
        # It PLAYS: a minute's worth of steps in a second, and its own pad
        # moving its own piece.
        # ...at the match's own pace: a step a frame, no more.
        before = step_of(stay)
        both(60)
        moved = (step_of(stay) - before) % 256
        if not 55 <= moved <= 61:
            failures.append(f"{other} no sigue a su paso al irse {who}: "
                            f"{moved} pasos en 60 frames")
        y_at = session_addr + off["y"] + (1 - leaver) * off["stride"]
        x_at = session_addr + off["x"] + (1 - leaver) * off["stride"]
        x0 = stay.memory.u8[x_at]
        both(3, [[KEYS["LEFT"]] if i != leaver else [] for i in range(2)])
        both(6, [[], []])
        if stay.memory.u8[x_at] == x0:
            # Against the wall already: the other way.
            both(3, [[KEYS["RIGHT"]] if i != leaver else [] for i in range(2)])
            both(6, [[], []])
        if stay.memory.u8[x_at] == x0:
            failures.append(f"sola, {other} no mueve su pieza con su cruceta")
        if not loudest(stay, 60):
            failures.append(f"sola, {other} se queda sin su musica")
        both(200)
        note = " ".join(run_rom.tilemap_text(stay, r) for r in range(1, 9))
        if "RIVAL" in note:
            failures.append(f"RIVAL LEFT se queda puesto en {other}")
        _ = y_at

        # ...and to its own end: the table, with nobody to swap a name with,
        # and the title.
        _bury([stay], session_addr, off, 1 - leaver)
        both(300)
        tap("START", who=1 - leaver)
        both(30)
        if "-LINES" not in run_rom.tilemap_text(stay, 2, 0, 30):
            failures.append(f"sola, la partida de {other} no acaba en su tabla")
        # ...with its port at rest, 0 to send: nobody to swap a name with,
        # and a swap begun anyway would answer the other console's next
        # lobby with its own words.
        both(30)
        sending = stay._native.memory.io[IO_SIOMLT_SEND]
        if sending:
            failures.append(f"acabada sola su partida, {other} deja ${sending:04X} "
                            "en el puerto: ha empezado un intercambio de nombres "
                            "con nadie")
        tap("START", who=1 - leaver)
        tap("START", who=leaver)
        both(30)
        if any(flag(c, "g_linked") for c in cores):
            failures.append("tras la carrera alguna consola sigue enlazada")
        # THE CABLE IS WHOLE: both back to it, and a new race starts.
        tap("START"); tap("START")
        both(60)
        tap("START", who=0)
        both(90)
        if [flag(c, "g_linked") for c in cores] != [1, 1] or not _same_match(rom, cores):
            failures.append(f"tras irse {who} de una carrera, las dos consolas "
                            "no vuelven a encontrarse por el cable")
        elif not any(f for f in failures):
            print(f"  {who} sale con START de su tablero muerto: a su tabla; "
                  f"{other} sigue sola, sin pausa y con musica, y luego se "
                  "reencuentran")

    # WITHOUT THE CHORD THE SCREEN IS THE CARTRIDGE'S, and the cartridge
    # says nothing under a dead board: no hint. START leaves all the same.
    made = _to_a_match(rom, chord=False)
    if made is None:
        print("FALLA: la carrera por cable (sin el acorde) no arranco")
        return 1
    cores, cable, both, tap = made
    _bury(cores, session_addr, off, 1)
    both(240)
    hint = " ".join(run_rom.tilemap_text(cores[1], r) for r in range(0, 20))
    if alive(cores[1], 1) or not alive(cores[1], 0):
        failures.append("sin el acorde: no queda un tablero muerto y el otro vivo")
    elif "AGAIN" in hint or "LEAVE" in hint or "A AND B" in hint:
        failures.append("sin el acorde, el tablero muerto lleva la pista de "
                        f"A+B y START: {hint.split()}")
    else:
        tap("START", who=1)
        both(20)
        state = (flag(cores[1], "g_linked"), flag(cores[0], "g_linked"),
                 flag(cores[0], "g_link_alone"), alive(cores[0], 0))
        if state != (0, 1, 1, 1):
            failures.append("sin el acorde, START en el tablero muerto no saca "
                            "a su consola y deja a la otra jugando sola: "
                            f"{state}")
        else:
            print("  sin el acorde: ninguna pista bajo el tablero muerto, y "
                  "START sale igual")

    # A BOARD BACK ON ITS FEET HAS ITS TUNE BACK.
    made = _to_a_match(rom)
    if made is None:
        print("FALLA: la carrera por cable no arranco")
        return 1
    cores, cable, both, tap = made

    def loudest(core, frames):
        top = 0
        for _ in range(frames):
            both(1)
            top = max(top, run_rom.sound_state(core)["activos"])
        return top

    _bury(cores, session_addr, off, 1)
    both(420)                       # dead, and its game-over tune played out
    quiet = loudest(cores[1], 60)
    both(20, [[], [KEYS["A"], KEYS["B"]]])
    both(10, [[], []])
    back = loudest(cores[1], 60)
    if alive(cores[1], 1) != 1:
        failures.append("A+B no levanta el tablero muerto (con musica elegida)")
    elif quiet:
        failures.append(f"el tablero muerto sigue con musica ({quiet})")
    elif not back:
        failures.append("el tablero que A+B levanta sigue en silencio: su "
                        "consola no recupera la musica de la partida")
    else:
        print("  y el tablero que A+B levanta recupera su musica")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: salir de una carrera con START no deja a la otra consola "
          "colgada: sigue jugando sola.")
    return gone_check(rom)


def gone_check(rom):
    """A CONSOLE THAT IS THERE AND NO LONGER IN THE MATCH.

    Started over under a match — the soft reset, the power switch — a
    console of ours answers the cable from its menus: 0, not a move. The
    one still in the match used to take every such transfer for "the
    partner is there" and wait for a move for ever: no LINK ISSUES, no
    CABLE LOST, no way out. Now: a race goes on alone (its board is its
    own), and a shared board, which cannot, ends as a cable gone does —
    the CABLE LOST window at once, START to the title.
    """
    import run_rom
    names = ("g_session", "g_linked", "g_link_alone", "g_link_lost")
    syms = {n: symbol(rom, n) for n in names}
    missing = [why for s, why in syms.values() if s is None]
    if missing:
        skip(missing[0])
        return 0
    at = {n: s[0][0] for n, s in syms.items()}
    session_addr, session_size = syms["g_session"][0]
    game_size = session_size - 4
    off = run_rom.game_offsets(rom)
    RESET = [KEYS[k] for k in ("A", "B", "START", "SELECT")]
    failures = []
    for coop in (False, True):
        for quitter in (0, 1):
            mode = "cooperativa" if coop else "carrera"
            who = "la maestra" if quitter == 0 else "la esclava"
            made = _to_a_match(rom, coop=coop)
            if made is None:
                print(f"FALLA: la {mode} por cable no arranco")
                return 1
            cores, cable, both, tap = made
            stay = cores[1 - quitter]
            both(8, [RESET if i == quitter else [] for i in range(2)])
            # However long the other takes to come up again: until this one
            # has made up its mind, and not for ever.
            for _ in range(LINK_GIVEUP_FRAMES_PY // 10):
                both(10, [[], []])
                if stay.memory.u8[at["g_link_alone"]] or stay.memory.u8[at["g_link_lost"]]:
                    break
            both(20, [[], []])
            flags = (stay.memory.u8[at["g_linked"]], stay.memory.u8[at["g_link_alone"]],
                     stay.memory.u8[at["g_link_lost"]])
            if not coop:
                # The reset's START crossed as a button first: paused, and
                # START again plays on.
                if flags != (1, 1, 0):
                    failures.append(f"{mode}: se reinicia {who} y la otra no "
                                    f"sigue sola (enlazada, sola, perdida = {flags})")
                    continue
                if stay.memory.u8[session_addr + off["paused"]]:
                    tap("START", who=1 - quitter)
                before = stay.memory.u8[session_addr + game_size + 1]
                both(60)
                moved = (stay.memory.u8[session_addr + game_size + 1] - before) % 256
                if not 55 <= moved <= 61:
                    failures.append(f"{mode}: reiniciada {who}, la otra no "
                                    f"sigue a su paso ({moved} pasos en 60 frames)")
                else:
                    print(f"  {mode}: {who} se reinicia a media partida y la "
                          "otra sigue sola")
            else:
                heads = run_rom.tilemap_text(stay, run_rom.PM_HEAD)
                if flags != (1, 0, 1) or "CABLE LOST" not in heads:
                    failures.append(f"{mode}: se reinicia {who} y la otra no da "
                                    f"el cable por perdido (enlazada, sola, "
                                    f"perdida = {flags}; {heads.strip()!r})")
                    continue
                tap("START", who=1 - quitter)
                both(30)
                page = " ".join(run_rom.tilemap_text(stay, r) for r in range(0, 20))
                if stay.memory.u8[at["g_linked"]] or "SCORES" in page or "-LINES" in page:
                    failures.append(f"{mode}: con la otra reiniciada, START no "
                                    "saca al titulo")
                else:
                    print(f"  {mode}: {who} se reinicia a media partida y la "
                          "otra lo da por perdido: CABLE LOST, y START al titulo")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: una consola que se reinicia en mitad de una partida por cable "
          "no deja colgada a la otra.")
    return coop_end_check(rom)


def coop_end_check(rom):
    """A SHARED BOARD'S END IS BOTH CONSOLES'.

    Two cartridges in COOPERATIVE: at the game over the tune went on
    playing on one of them — under the plaque, the table, to the title —
    and that one never played the game-over tune. A coop top-out takes both
    players (top_out, tengen_core.c) and is reported on the player whose
    piece did not fit; each console announced only its own player's.
    """
    import run_rom
    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(why)
        return 0
    session_addr = sym[0]
    off = run_rom.game_offsets(rom)
    failures = []
    for tune in (True, False):
        made = _to_a_match(rom, coop=True, tune=tune)
        if made is None:
            print("FALLA: la cooperativa por cable no arranco")
            return 1
        cores, cable, both, tap = made

        def loudest(frames):
            top = [0, 0]
            for _ in range(frames):
                both(1)
                for i, c in enumerate(cores):
                    top[i] = max(top[i], run_rom.sound_state(c)["activos"])
            return top

        _bury(cores, session_addr, off, 0)
        over = loudest(200)             # the top-out, and its tune
        both(160)                       # ...played out
        after = loudest(90)             # the plaque still up
        dead = [c.memory.u8[session_addr + off["active"] + s * off["stride"]]
                for c in cores for s in (0, 1)]
        what = "con musica" if tune else "sin musica"
        if any(dead):
            print(f"FALLA: el tablero compartido no llego a morir ({dead})")
            return 1
        if 0 in over:
            failures.append(f"cooperativa {what}: el game over no suena en las "
                            f"dos consolas (maestra {over[0]}, esclava {over[1]})")
        elif any(after):
            failures.append(f"cooperativa {what}: tras el game over sigue sonando "
                            f"(maestra {after[0]}, esclava {after[1]})")
        else:
            print(f"  cooperativa {what}: el game over suena en las dos y "
                  "despues callan las dos")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el final de un tablero compartido se oye igual en las dos consolas.")
    return skin_check(rom)


def skin_check(rom):
    """THE PROTOTYPES AS A SKIN OVER THE CABLE.

    The master wears a prototype chosen on its own title, the slave is left
    in the release, and the match that follows has to be the master's skin on
    BOTH screens, played by the release's RULES on both, and still the same
    game byte for byte. What makes that hard is not the paint: a skinned board
    stores piece ids where the release stores joined-block tiles
    (piece_id_cells), so the two consoles have to agree on the cell format or
    lockstep is over on the first piece. The lobby's SKIN stage settles it by
    the art's fingerprint, and both reach the same answer.

    And the other way round: with no skin on the master's title, neither
    console wears one, whatever the slave's title says.
    """
    import run_rom

    sym, why = symbol(rom, "g_session")
    title_sym, why2 = symbol(rom, "g_title_skin")
    if sym is None or title_sym is None:
        skip(f"{why or why2}")
        return 0
    session_addr, session_size = sym
    game_size = session_size - 4
    off = run_rom.game_offsets(rom)
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH

    def match(master_chord, slave_chord):
        mgba.log.silence()
        cores, screens = [], []
        for _ in range(2):
            core = mgba.core.load_path(rom)
            screen = mgba.image.Image(SCREEN_W, SCREEN_H)
            core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
            core.reset()
            cores.append(core)
            screens.append(screen)
        cable = Cable(*cores)
        ends = [CableEnd(cable, True), CableEnd(cable, False)]
        for core, end in zip(cores, ends):
            core.attach_sio(end, lib.SIO_MULTI)

        def both(frames, keys=None):
            for _ in range(frames):
                for i, core in enumerate(cores):
                    core.set_keys(*(keys[i] if keys else []))
                    core.run_frame()

        def tap(name, who=None):
            both(4, [[KEYS[name]] if who in (None, i) else [] for i in range(2)])
            both(10, [[], []])

        both(60)
        # THE TITLE'S CHORD, which is the game's, on whichever console is
        # asked to wear a skin: it puts the first prototype on.
        chord = [[KEYS["L"], KEYS["R"]] if want else []
                 for want in (master_chord, slave_chord)]
        both(4, chord)
        both(30, [[], []])
        titles = [core.memory.u8[title_sym[0]] for core in cores]
        tap("START"); tap("DOWN"); tap("START")   # -> 2 PLAYER -> the cable
        both(50)
        tap("START", who=0)
        both(60)
        # Play: both hold Down for a while so pieces lock on both boards.
        both(400, [[KEYS["DOWN"]], [KEYS["DOWN"]]])
        both(20, [[], []])
        return cores, screens, titles

    failures = []

    def flags(core):
        return (core.memory.u8[session_addr + off["piece_ids"]],
                core.memory.u8[session_addr + off["proto_rules"]])

    def settled(core):
        field = session_addr + off["field"]
        return [core.memory.u8[field + i] for i in range(2 * PF)
                if core.memory.u8[field + i] not in (0, CELL_WALL)]

    def frame_colour(screen):
        # The left box's braid, where a skin puts its fret: a pixel of the
        # rope's inner run.
        px = run_rom.pixels(screen)
        cols = [px[y][x] for y in range(40, 120) for x in range(64, 80)]
        cols = [c for c in cols if sum(c) > 60]
        n = max(len(cols), 1)
        r, g, b = (sum(c[k] for c in cols) // n for k in range(3))
        # The release's braid is blue through and through; a prototype's fret
        # is a dark green-grey with red in it (about 84, 80, 80 measured).
        return "release" if b > r + 40 and b > g + 20 else "prototipo"

    cores, screens, titles = match(True, False)
    # Skipped only if the BUILD has none: a build with prototypes whose title
    # did not put one on is a failure, not a skip (this check once skipped
    # silently when the title stopped answering without the game's chord).
    proto_h = os.path.join(os.path.dirname(rom), "..", "gba", "screen_proto.h")
    has_protos = (os.path.exists(proto_h) and
                  "#define SCREEN_PROTO_AVAILABLE 1" in open(proto_h).read())
    if not titles[0] and has_protos:
        print("FALLA: la ROM trae prototipos y el titulo del maestro no se puso "
              "ninguno")
        return 1
    if not titles[0]:
        skip("esta ROM no trae prototipos (make assets PROTO=...)")
        return 0
    if titles[1]:
        failures.append("el acorde del maestro cambio el titulo del esclavo")
    fm, fs = flags(cores[0]), flags(cores[1])
    if fm != (1, 0) or fs != (1, 0):
        failures.append(f"con skin en el maestro: piece_id_cells/proto_rules "
                        f"{fm} y {fs}, deberian ser (1, 0) en las dos")
    else:
        print(f"  el maestro lleva la skin {titles[0]} y el esclavo el release: "
              "las dos consolas juegan con celdas de pieza y reglas del release")
    if read_bytes(cores[0], session_addr, game_size) != \
            read_bytes(cores[1], session_addr, game_size):
        failures.append("con skin por cable las dos consolas divergieron")
    else:
        cells = settled(cores[0])
        if not cells:
            failures.append("ninguna pieza llego a asentarse")
        elif max(cells) > 7:
            failures.append(f"hay celdas que no son ids de pieza: "
                            f"{sorted(set(cells))}")
        else:
            print(f"  {len(cells)} celdas asentadas, todas ids de pieza, "
                  "identicas byte a byte en las dos consolas")
    colours = [frame_colour(sc) for sc in screens]
    if colours != ["prototipo", "prototipo"]:
        failures.append(f"el marco no es la greca del prototipo en las dos "
                        f"pantallas: {colours}")
    else:
        print("  y las dos pantallas llevan la greca del prototipo, tambien la "
              "del esclavo, que dejo su titulo en el release")

    # ...and the master's title decides: a skin on the SLAVE's title alone
    # puts none on either board.
    cores, screens, titles = match(False, True)
    fm, fs = flags(cores[0]), flags(cores[1])
    if fm != (0, 0) or fs != (0, 0):
        failures.append(f"con skin solo en el esclavo: {fm} y {fs}, deberian "
                        "ser (0, 0)")
    elif [frame_colour(sc) for sc in screens] != ["release", "release"]:
        failures.append("con skin solo en el esclavo algun tablero no es el "
                        "del release")
    else:
        print("  y una skin solo en el titulo del esclavo no viste a nadie: "
              "manda el maestro")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: los prototipos van por cable como skin, con las reglas del "
          "release, y las dos consolas siguen siendo la misma partida.")
    return pause_check(rom)


def pause_check(rom):
    """THE PAUSE MENU OVER THE CABLE, THE SAME ON BOTH CONSOLES.

    It is the MASTER's chord that decides whether a linked match has one (it
    crosses in the lobby), and it is driven by both players' presses as the
    transfers carry them, so both consoles show the same menu and do the
    same thing with it (link_match_begin, gba/match.c). Here: both found the
    chord; the master pauses and the tune stops on both; the menu is up on
    both, the same width; the SLAVE changes the tune and it changes on both;
    START resumes with the tune on both; paused again, EXIT and YES take
    both consoles out of the match together. And a match whose master never
    found the chord has the plaque, on both, whatever the slave found.
    """
    sym, why = symbol(rom, "g_session")
    names = ["g_pmenu_drawn_w", "g_music", "g_linked"]
    syms = {n: symbol(rom, n)[0] for n in names}
    if sym is None or None in syms.values():
        skip(f"{why or 'faltan simbolos del menu de pausa'}")
        return 0
    import run_rom
    off = run_rom.game_offsets(rom)
    failures = []

    def start(chord):
        cores, cable, both, tap = _pair(rom)
        both(8)
        tap("START")                                   # title -> game select
        both(4, [[KEYS["L"], KEYS["R"]] if c else [] for c in chord])
        both(10, [[], []])
        tap("DOWN"); tap("START")                      # 2 PLAYER -> the cable
        both(50)
        # A tune, so there is something for the pause to silence.
        tap("DOWN", who=0); tap("DOWN", who=0); tap("RIGHT", who=0)
        tap("START", who=0)
        both(80)
        return cores, both, tap

    def read(cores, name, size=1):
        a = syms[name][0]
        return [c.memory.u8[a] if size == 1 else c.memory.u16[a] for c in cores]

    cores, both, tap = start([True, True])

    def loudest(frames):
        top = 0
        for _ in range(frames):
            both(1)
            top = max([top] + [run_rom.sound_state(c)["activos"] for c in cores])
        return top

    playing = loudest(30)
    tap("START", who=0)                                # the master pauses
    both(20)
    paused = [c.memory.u8[sym[0] + off["paused"]] for c in cores]
    silent = loudest(40)
    widths = read(cores, "g_pmenu_drawn_w")
    tune = read(cores, "g_music")
    # The SLAVE picks a tune: MUSIC, the next one down, A.
    tap("A", who=1); tap("DOWN", who=1); tap("A", who=1)
    both(10)
    tune2 = read(cores, "g_music")
    tap("START", who=0)                                # ...and resume
    both(10)
    back = loudest(60)
    if paused != [1, 1]:
        failures.append(f"la pausa no llego a las dos consolas: {paused}")
    elif not playing or silent:
        failures.append(f"la pausa no calla la musica (antes {playing}, en "
                        f"pausa {silent} canales)")
    elif widths[0] == 0 or widths[0] != widths[1]:
        failures.append(f"el menu de pausa no esta igual en las dos: anchos "
                        f"{widths}")
    elif tune2[0] != tune2[1] or tune2 == tune:
        failures.append(f"la cancion elegida por el esclavo no cambio en las "
                        f"dos: {tune} -> {tune2}")
    elif not back:
        failures.append("al quitar la pausa la musica no vuelve")
    else:
        print(f"  menu en las dos (ancho {widths[0]}), el esclavo cambia la "
              f"cancion en las dos ({tune[0]} -> {tune2[0]}), y vuelve")

    # EXIT, YES: both out, on the same transfer.
    tap("START", who=0)
    both(10)
    tap("UP", who=0)                                   # MUSIC -> EXIT, round
    tap("A", who=0)                                    # EXIT? ...
    tap("UP", who=1)                                   # NO -> YES, from the slave
    #                                                    (no SAVE over a cable, and
    #                                                    the arrow does not wrap)
    tap("A", who=0)
    both(30)
    if read(cores, "g_linked") != [0, 0]:
        failures.append(f"EXIT no saca a las dos consolas de la partida: "
                        f"g_linked {read(cores, 'g_linked')}")
    else:
        print("  y EXIT las saca a las dos a la vez")

    # Only the slave found the chord: the plaque, on both — and the board
    # behind it on both, not the chord's wall of bricks (T_BRICK, $0F) on the
    # slave's alone, which is what this console's chord drew there.
    cores, both, tap = start([False, True])
    tap("START", who=0)
    both(20)
    widths = read(cores, "g_pmenu_drawn_w")
    bricks = [sum(1 for r in range(20) for col in range(10, 20)
                  if c.memory.u16[0x0600E000 + (r * 32 + col) * 2] & 0x3FF == 0x0F)
              for c in cores]
    if widths != [0, 0]:
        failures.append(f"sin el acorde del maestro hubo menu: anchos {widths}")
    elif bricks != [0, 0]:
        failures.append(f"sin el acorde del maestro, ladrillos en la pausa: "
                        f"{bricks}")
    else:
        print("  sin el acorde del maestro, la placa en las dos, y el tablero "
              "detras sin ladrillos")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el menu de pausa va por cable y hace lo mismo en las dos consolas.")
    if (menu_list_check(rom) or levelup_music_check(rom) or
            coop_levelup_music_check(rom) or options_check(rom) or
            coop_ghosts_check(rom) or tempo_check(rom, start)):
        return 1
    return late_check(rom)


def tempo_check(rom, start):
    """EL TEMPO POR LA PILA ES DEL ACORDE DE LA PARTIDA, no del de cada consola.

    Bajo el acorde la musica corre un cuarto mas desde doce filas y la mitad
    desde quince (stack_tempo); sin el, el tempo del cartucho por alta que
    este la pila. Por cable el acorde de la partida es el del maestro: con
    solo el del esclavo, ninguna de las dos acelera; con solo el del
    maestro, las dos. Se lee g_tempo_quarters, lo que cada frame deja para
    el siguiente audio_frame, con dieciseis filas puestas en los dos
    tableros de las dos consolas (las mismas, para no romper el paso)."""
    from romcheck.harness import CELL_BLOCK, CELL_WALL, PF_W
    tempo, why = symbol(rom, "g_tempo_quarters")
    sym, why2 = symbol(rom, "g_session")
    if tempo is None or sym is None:
        skip(f"{why or why2}")
        return 0
    import run_rom
    off = run_rom.game_offsets(rom)
    PF = 20 * PF_W
    failures = []
    for chord, want, name in (([False, True], [0, 0], "solo el del esclavo"),
                              ([True, False], [2, 2], "solo el del maestro")):
        cores, both, tap = start(chord)
        for core in cores:
            for board in (0, 1):
                at = sym[0] + off["field"] + board * PF
                for row in range(4, 20):
                    for col in range(PF_W):
                        cell = (CELL_WALL if col in (0, PF_W - 1) else
                                0 if col == 5 else CELL_BLOCK)
                        core.memory.u8[at + row * PF_W + col] = cell
        both(6)
        got = [c.memory.u8[tempo[0]] for c in cores]
        if got != want:
            failures.append(f"con {name}, el tempo con la pila a dieciseis "
                            f"filas es {got} cuartos, no {want}")
        else:
            print(f"  con {name}, pila de dieciseis filas: {got[0]} cuartos "
                  f"de mas en las dos")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el tempo por la pila sigue al acorde de la partida en las dos.")
    return 0


def options_check(rom):
    """LAS OPCIONES POR CABLE: LAS REGLAS SON DEL MAESTRO, LO DEMAS DE CADA UNO.

    Las tres reglas de OPTIONS -- HARD DROP, FAST DOWN, FAST SHIFT -- son de
    la partida: cruzan en el lobby desde las del maestro (bits 8-10 de
    HANDICAP) y las dos consolas juegan con ellas, para los dos jugadores; las
    del esclavo no cuentan. Desde el menu compartido, cualquiera de los dos
    cambia una regla en las dos consolas, solo para esa partida (la bateria
    del maestro no cambia). GHOST y SWAP A-B son de cada jugador: la sombra en
    el tablero de quien la tiene, y A y B girados en la palabra que manda su
    consola (g_pad_swap) mientras se juega, nunca en pausa. Y la partida sigue
    siendo la misma en las dos.
    """
    from romcheck.harness import check_layout
    sym, why = symbol(rom, "g_session")
    syms = {n: symbol(rom, n)[0] for n in ("g_options", "g_pad_swap")}
    if sym is None or None in syms.values():
        skip(f"{why or 'faltan simbolos de las opciones'}")
        return 0
    import run_rom
    off = run_rom.game_offsets(rom)
    ghost0 = check_layout(rom)["ghost"]
    failures = []

    def check_byte(v):
        return 0xC3 ^ ((v * 7) & 0xFF)

    cores, cable, both, tap = _pair(rom)
    # The master: HARD DROP, GHOST and HOLD. The slave: FAST DOWN (a rule,
    # which must lose) and SWAP A-B. Straight into each battery before boot.
    for core, v in zip(cores, (0x29, 0x12)):
        core.memory.u8[0x0E000404] = v
        core.memory.u8[0x0E000405] = check_byte(v)
    both(8)
    tap("START")
    both(4, [[KEYS["L"], KEYS["R"]]] * 2)
    both(10, [[], []])
    tap("DOWN"); tap("START")                          # 2 PLAYER -> the cable
    both(50)
    tap("START", who=0)                                # the master starts it
    both(80)

    def game(name):
        return [c.memory.u8[sym[0] + off[name]] for c in cores]

    def ghosts(core):
        return sum(1 for r in range(20) for c in range(32)
                   if ghost0 <= (core.memory.u16[0x0600E000 + (r * 32 + c) * 2]
                                 & 0x3FF) < ghost0 + 8)   # T_GHOST: 4 + 4

    def mine(name):
        return [c.memory.u8[syms[name][0]] for c in cores]

    if (game("hard_drop"), game("fast_soft"), game("fast_shift"), game("hold")) != \
            ([1, 1], [0, 0], [0, 0], [3, 3]):
        failures.append(f"las reglas no son las del maestro en las dos: "
                        f"hard_drop {game('hard_drop')}, fast_soft "
                        f"{game('fast_soft')}, fast_shift {game('fast_shift')}, "
                        f"hold {game('hold')}")
    else:
        print("  las reglas de la partida son las del maestro (HARD DROP y HOLD), "
              "en las dos; el FAST DOWN del esclavo no cuenta")
    # HOLD over the cable: the slave's SELECT puts its piece aside on both.
    held_at = sym[0] + off["held"] + off["stride"]
    tap("SELECT", who=1)
    both(10)
    held = [c.memory.u8[held_at] for c in cores]
    if held[0] == 0 or held[0] != held[1] or not _same_match(rom, cores):
        failures.append(f"el HOLD del esclavo no es el mismo en las dos: {held}")
    else:
        print(f"  el esclavo guarda su pieza con SELECT, y las dos lo ven ({held[0]})")
    # ...and with it the cable's one HUD becomes two: L+R goes to STATS,
    # where HOLD's cell is (hud_set).
    hud_sym, _ = symbol(rom, "g_hud")
    if hud_sym is not None:
        before = cores[1].memory.u8[hud_sym[0]]
        both(4, [[], [KEYS["L"], KEYS["R"]]])
        both(20, [[], []])
        after = cores[1].memory.u8[hud_sym[0]]
        if before == after:
            failures.append(f"por cable con HOLD, L+R no cambia de HUD ({before})")
        else:
            print(f"  por cable con HOLD, L+R cambia de HUD ({before} -> {after})")
        hud_versus = before
    g = [ghosts(c) for c in cores]
    if g[0] != 4 or g[1] != 0:
        failures.append(f"la sombra no es de quien la tiene: celdas {g}")
    else:
        print("  la sombra en el tablero del maestro, que la tiene, y no en el "
              "del esclavo")
    if mine("g_pad_swap") != [0, 1]:
        failures.append(f"SWAP A-B no gira la palabra del esclavo: g_pad_swap "
                        f"{mine('g_pad_swap')}")
    else:
        print("  el esclavo manda A y B girados; el maestro no")

    # The shared menu: the SLAVE opens OPTIONS and throws FAST DOWN (a rule:
    # both consoles, this match only) and its own GHOST (its own console).
    tap("START", who=1)
    both(10)
    paused_swap = mine("g_pad_swap")
    tap("DOWN", who=1); tap("A", who=1)                # OPTIONS
    tap("DOWN", who=1); tap("A", who=1)                # FAST DOWN
    tap("DOWN", who=1); tap("DOWN", who=1); tap("A", who=1)   # GHOST
    # ...and HOLD off, a rule, from the slave on HUD STATS: the cable's set
    # of HUDs loses STATS, and the slave's goes back to its first.
    tap("DOWN", who=1); tap("DOWN", who=1); tap("A", who=1)   # HOLD
    tap("START", who=0)
    both(30)
    if hud_sym is not None and (game("hold") != [0, 0] or
                                cores[1].memory.u8[hud_sym[0]] != hud_versus):
        failures.append(f"HOLD apagado por cable no vuelve al HUD de la carrera: "
                        f"hold {game('hold')}, HUD {cores[1].memory.u8[hud_sym[0]]}")
    elif hud_sym is not None:
        print("  HOLD apagado desde el menu compartido: en las dos, y el esclavo "
              "vuelve de STATS al HUD de la carrera")
    if paused_swap != [0, 0]:
        failures.append(f"en pausa la palabra sigue girada: {paused_swap}")
    if game("fast_soft") != [3, 3]:
        failures.append(f"FAST DOWN desde el menu compartido no cambia en las "
                        f"dos: fast_soft {game('fast_soft')}")
    elif mine("g_options") != [0x29, 0x1A]:
        failures.append(f"las opciones de cada uno no son las suyas: "
                        f"g_options {[hex(v) for v in mine('g_options')]}")
    elif cores[0].memory.u8[0x0E000404] != 0x29:
        failures.append("una regla cambiada por cable se guarda en la bateria "
                        "del maestro")
    elif [ghosts(c) for c in cores] != [4, 4]:
        failures.append(f"el GHOST del esclavo no sale en su tablero: "
                        f"{[ghosts(c) for c in cores]}")
    elif not _same_match(rom, cores):
        failures.append("tras las opciones las dos consolas ya no juegan la "
                        "misma partida")
    else:
        print("  desde el menu compartido el esclavo pone FAST DOWN en las dos "
              "(sin guardarlo) y su GHOST en la suya; la partida sigue igual")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: por cable las reglas son del maestro y de los dos; la sombra y "
          "el giro, de cada uno.")
    return 0


def coop_ghosts_check(rom):
    """COOPERATIVO POR CABLE CON GHOST EN LAS DOS: CADA UNA LA SUYA ENTERA.

    Un tablero, dos piezas y dos consolas que lo dibujan: cada una pinta la
    sombra de su jugador de linea entera (T_GHOST) y la del companero a
    trazos (los cuatro tiles siguientes), asi que lo que una llama la suya la
    otra lo llama la del companero, en las mismas celdas. Con las dos piezas
    sobre el mismo hueco las sombras encajan una sobre otra -- la de la
    pieza de abajo en el suelo -- en vez de pedir las mismas celdas, y las
    dos consolas estan de acuerdo en cuales; y la partida sigue siendo la
    misma en las dos (la sombra no es del juego).

    Y DE PASO, LAS PILAS (stack_paint, en el arnes): por cable el intercambio
    corre en la interrupcion del puerto serie, con su pila de 256 bytes, y
    la del usuario lleva encima el lobby, la partida y el menu compartido.
    Pintadas al arrancar y miradas al final, con las dos manos ocupadas
    entre medias: a la del usuario le tiene que sobrar 1KB, y a la de la
    interrupcion la mitad.
    """
    from romcheck.harness import (check_layout, stack_bounds, stack_depth,
                                  stack_paint)
    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    import run_rom
    off = run_rom.game_offsets(rom)
    ghost0 = check_layout(rom)["ghost"]
    failures = []

    def check_byte(v):
        return 0xC3 ^ ((v * 7) & 0xFF)

    cores, cable, both, tap = _pair(rom)
    for core in cores:                                  # GHOST, on both
        core.memory.u8[0x0E000404] = 0x08
        core.memory.u8[0x0E000405] = check_byte(0x08)
    both(8)
    bounds = stack_bounds(rom)
    if bounds is not None:
        for core in cores:
            stack_paint(core, bounds)
    tap("START")
    both(4, [[KEYS["L"], KEYS["R"]]] * 2)
    both(10, [[], []])
    tap("DOWN"); tap("DOWN"); tap("START")              # COOPERATIVE -> the cable
    both(50)
    tap("START", who=0)
    both(100)

    def place(slot, piece, o, x, y):
        for core in cores:
            b = sym[0] + slot * off["stride"]
            core.memory.u8[b + off["current"]] = piece
            core.memory.u8[b + off["orientation"]] = o
            core.memory.u8[b + off["x"]] = x
            core.memory.u8[b + off["y"]] = y

    def ghosts(core):
        """The two sets' cells: [this console's player's, the partner's]."""
        got = [set(), set()]
        for r in range(20):
            for c in range(32):
                t = core.memory.u16[0x0600E000 + (r * 32 + c) * 2] & 0x3FF
                if ghost0 <= t < ghost0 + 8:
                    got[(t - ghost0) // 4].add((r, c))
        return got

    def ink(core, which):
        """How many pixels of ink the set's four tiles hold."""
        return sum(1 for t in range(ghost0 + 4 * which, ghost0 + 4 * which + 4)
                   for b in read_bytes(core, 0x06000000 + t * 32, 32)
                   for nib in (b & 15, b >> 4) if nib)

    # Two O's over the same two columns: player 1's high, player 2's low.
    place(0, 3, 0, 5, 6)
    place(1, 3, 0, 5, 12)
    both(2)
    seen = [ghosts(c) for c in cores]
    master, slave = seen
    rows = [[sorted({r for r, _ in cells}) for cells in g] for g in seen]
    if [len(cells) for g in seen for cells in g] != [4, 4, 4, 4]:
        failures.append(f"con las dos piezas sobre el mismo hueco no salen las "
                        f"dos sombras enteras en las dos consolas: "
                        f"{[[len(c) for c in g] for g in seen]} celdas")
    elif master[0] != slave[1] or master[1] != slave[0]:
        failures.append(f"las dos consolas no ponen las sombras en las mismas "
                        f"celdas: filas {rows}")
    elif rows[0][0][-1] + 1 != rows[0][1][0]:
        failures.append(f"la sombra de la pieza de arriba no descansa sobre la "
                        f"de la de abajo: filas {rows[0]}")
    else:
        print(f"  dos piezas sobre el mismo hueco: las sombras encajan (filas "
              f"{rows[0][0]} sobre {rows[0][1]}) y las dos consolas las ponen "
              f"en las mismas celdas, cada una la suya en sus tiles")
    # ...and whose is whose: the same O in both hands, so the two outlines
    # are the same outline — whole in this console's set, broken in the
    # partner's, on both consoles.
    inks = [[ink(c, w) for w in (0, 1)] for c in cores]
    if any(mine <= 0 or theirs <= 0 or theirs * 3 > mine * 2 for mine, theirs in inks):
        failures.append(f"la sombra del companero no va a trazos en las dos "
                        f"consolas: pixeles [propia, companero] {inks}")
    elif inks[0] != inks[1]:
        failures.append(f"con la misma pieza en las dos manos las dos consolas "
                        f"no dibujan los mismos contornos: {inks}")
    else:
        print(f"  la propia entera y la del companero a trazos, en las dos "
              f"({inks[0][0]} y {inks[0][1]} pixeles)")
    if not _same_match(rom, cores):
        failures.append("con las sombras las dos consolas ya no juegan la misma "
                        "partida")
    # Busy hands on both pads, and the shared menu and its windows; then how
    # deep the stacks went, the serial interrupt's among them.
    import random
    rng = random.Random(11)
    hands = [[], [], ["LEFT"], ["RIGHT"], ["A"], ["B"], ["DOWN"], ["UP"]]
    keys = [[], []]
    for f in range(900):
        if f % 4 == 0:
            keys = [[KEYS[k] for k in rng.choice(hands)] for _ in range(2)]
        both(1, keys)
    both(4, [[], []])
    tap("START", who=1); both(20)
    tap("A", who=1); both(20); tap("B", who=1)
    tap("DOWN", who=1); tap("A", who=1); both(20); tap("B", who=1)
    tap("START", who=1); both(60)
    if bounds is None:
        skip("el ELF no exporta __bss_end, __sp_usr y __sp_irq")
    else:
        room, irq_room = bounds[1] - bounds[0], bounds[2] - bounds[1]
        deep = [stack_depth(core, bounds) for core in cores]
        if any(u > room - 1024 or q > irq_room // 2 or u < 256 for u, q in deep):
            failures.append(f"las pilas por cable: [usuario, interrupcion] {deep} "
                            f"de {room} y {irq_room} bytes")
        elif not _same_match(rom, cores):
            failures.append("tras las manos ocupadas y el menu compartido las "
                            "dos consolas ya no juegan la misma partida")
        else:
            print(f"  las pilas por cable, con las dos manos ocupadas y el menu "
                  f"compartido: {deep[0][0]} y {deep[1][0]} de {room} bytes, y "
                  f"{deep[0][1]} y {deep[1][1]} de {irq_room} las de la "
                  f"interrupcion")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: en cooperativo por cable cada consola ve entera su sombra y a "
          "trazos la del companero, encajadas y en las mismas celdas.")
    return 0


def menu_list_check(rom):
    """LA LISTA DE CANCIONES DEL MENU ES LA DEL MAESTRO, EN LAS DOS.

    El menu existe porque el MAESTRO hizo el acorde; si la lista salia del
    acorde de cada consola, un esclavo sin el pasaba por cinco canciones y
    el maestro por ocho, y cada una acababa tocando una distinta ("en una
    Korobeiniki y en la otra Troika"). Aqui el esclavo no hizo el acorde, y
    cambia la cancion dos veces desde el menu.
    """
    sym, _ = symbol(rom, "g_music")
    if sym is None:
        skip("falta g_music")
        return 0
    cores, cable, both, tap = _pair(rom)
    both(8)
    tap("START")
    both(4, [[KEYS["L"], KEYS["R"]], []])          # solo el maestro
    both(10, [[], []])
    tap("DOWN"); tap("START")
    both(50)
    # KOROBEINIKI, the first of the uncovered tunes: past the slave's list.
    tap("DOWN", who=0); tap("DOWN", who=0)
    for _ in range(5):
        tap("RIGHT", who=0)
    tap("START", who=0)
    both(80)
    tap("START", who=0)                            # pausa
    both(20)
    seen = []
    for _ in range(2):
        # el ESCLAVO cambia: MUSIC, la siguiente, A. La ventana abre en
        # la que suena, asi que va a KATIUSKA y luego a MUSIC MIX, las dos
        # fuera de la lista de cinco que tendria sin el acorde.
        tap("A", who=1); tap("DOWN", who=1); tap("A", who=1)
        both(10)
        seen.append([c.memory.u8[sym[0]] for c in cores])
    if any(a != b for a, b in seen):
        print(f"FALLA: cada consola en una cancion distinta: {seen}")
        return 1
    print(f"  sin el acorde en el esclavo, las dos van a la par: {seen}")
    return 0


def levelup_music_check(rom):
    """TRAS SUBIR DE NIVEL POR CABLE, LA MUSICA VUELVE.

    Solo, la devuelve el baile de los cosacos al terminar; por cable no hay
    baile, y la consola del que subia de nivel se quedaba en silencio el
    resto de la partida si la cancion era de las anadidas (Korobeiniki,
    Katiuska): las del cartucho las retoma su propio motor, esas no. Aqui el
    maestro sube de nivel con Korobeiniki, y cuando la fanfarria termina
    tiene que volver a sonar.
    """
    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    import run_rom
    from romcheck.harness import fill_rows
    off = run_rom.game_offsets(rom)
    base = sym[0]
    cores, cable, both, tap = _pair(rom)
    both(8)
    tap("START")
    both(4, [[KEYS["L"], KEYS["R"]], []])
    both(10, [[], []])
    tap("DOWN"); tap("START")
    both(50)
    tap("DOWN", who=0); tap("DOWN", who=0)
    for _ in range(5):
        tap("RIGHT", who=0)                         # KOROBEINIKI
    tap("START", who=0)
    both(80)
    for core in cores:                              # lo mismo en las dos
        core.memory.u8[base + off["lines"]] = 28
        core.memory.u8[base + off["lines"] + 1] = 0
        fill_rows(core, base + off["field"], (19, 18))
    level = cores[0].memory.u8[base + off["level"]]
    for _ in range(80):
        both(8, [[KEYS["DOWN"]], []])
        both(2, [[], []])
        if cores[0].memory.u8[base + off["level"]] != level:
            break
    else:
        print("FALLA: el maestro no llego a subir de nivel")
        return 1
    both(144 + 60)                                  # la fanfarria, y algo mas
    top = 0
    for _ in range(40):
        both(1)
        top = max(top, run_rom.sound_state(cores[0])["activos"])
    if not top:
        print("FALLA: tras subir de nivel por cable la musica no vuelve")
        return 1
    print(f"  tras la fanfarria la cancion vuelve ({top} canales)")
    return 0


def coop_levelup_music_check(rom):
    """EN COOP LA SUBIDA DE NIVEL ES DE LAS DOS CONSOLAS, Y LA MUSICA TAMBIEN.

    El nivel de coop es uno, y el cartucho tiene un solo motor de sonido para
    los dos jugadores: suba quien suba, suena la fanfarria. Aqui cada consola
    la tocaba solo para su jugador, asi que la del que no habia hecho la
    linea seguia con su cancion mientras la otra paraba Korobeiniki o Katiuska
    y la volvia a empezar, o pasaba MUSIC MIX a la siguiente: desde ahi cada
    consola tocaba una musica distinta ("la musica se desacompaso"). Aqui, con
    MUSIC MIX, sube de nivel el jugador del ESCLAVO, y las dos tienen que
    pasar a la misma cancion.
    """
    sym, why = symbol(rom, "g_session")
    mix, why2 = symbol(rom, "g_mix_step")
    if sym is None or mix is None:
        skip(f"{why or why2}")
        return 0
    import run_rom
    from romcheck.harness import fill_rows
    off = run_rom.game_offsets(rom)
    base = sym[0]
    cores, cable, both, tap = _pair(rom)
    both(8)
    tap("START")
    both(4, [[KEYS["L"], KEYS["R"]], []])           # el acorde del maestro
    both(10, [[], []])
    tap("DOWN"); tap("DOWN"); tap("START")          # COOPERATIVE
    both(50)
    tap("DOWN", who=0); tap("DOWN", who=0)
    for _ in range(7):
        tap("RIGHT", who=0)                         # MUSIC MIX
    tap("START", who=0)
    both(80)
    before = [c.memory.u8[mix[0]] for c in cores]
    for core in cores:                              # lo mismo en las dos
        for p in range(2):
            at = base + off["lines"] + p * off["stride"]
            core.memory.u32[at] = 29
        fill_rows(core, base + off["field"], (19,))
    level = cores[0].memory.u8[base + off["level"]]
    for _ in range(80):                             # solo el esclavo baja
        both(8, [[], [KEYS["DOWN"]]])
        both(2, [[], []])
        if cores[0].memory.u8[base + off["level"]] != level:
            break
    else:
        print("FALLA: el jugador del esclavo no llego a subir de nivel")
        return 1
    both(30)
    after = [c.memory.u8[mix[0]] for c in cores]
    if after[0] != after[1]:
        print(f"FALLA: tras subir de nivel el esclavo, MUSIC MIX va por la "
              f"cancion {after[0]} en el maestro y {after[1]} en el esclavo")
        return 1
    if after == before:
        print(f"FALLA: la subida de nivel no paso MUSIC MIX a la siguiente "
              f"({before} -> {after})")
        return 1
    print(f"  sube el jugador del esclavo y MUSIC MIX pasa de {before[0]} a "
          f"{after[0]} en las dos")
    return 0


def late_check(rom):
    """ONE CONSOLE REACHES THE CABLE FIRST, as one always does.

    Every other check here walks both consoles to the lobby on the same
    frame, which two people with two consoles never do. The one that gets
    there first is in multiplayer mode alone for a while, and if it is the
    master it has been starting transfers into a partner that is not ready:
    on the first real cable that was the end of it, both consoles waiting
    and then NO CABLE FOUND. With the cable model above (SD only once both
    are in multiplayer mode, a transfer into a console that is not ready
    fails and its error stays until the port is started over) this is that
    evening, both ways round: the master first, then the slave.

    AND THE OTHER ONE TAKES ITS TIME: twenty seconds, twice what the lobby
    used to wait before it said NO CABLE FOUND. Somebody opening the cable a
    minute before the other player finds the menu is the normal case; the
    lobby waits for a partner who has never answered for as long as it takes.
    """
    LATE = 1200
    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return 0
    session_addr, session_size = sym
    game_size = session_size - 4
    failures = []
    for first, name in ((0, "el maestro"), (1, "el esclavo")):
        mgba.log.silence()
        cores, screens = [], []
        for _ in range(2):
            core = mgba.core.load_path(rom)
            screen = mgba.image.Image(SCREEN_W, SCREEN_H)
            core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
            core.reset()
            cores.append(core)
            screens.append(screen)
        cable = Cable(*cores)
        ends = [CableEnd(cable, True), CableEnd(cable, False)]
        for core, end in zip(cores, ends):
            core.attach_sio(end, lib.SIO_MULTI)

        def both(frames, keys=None):
            for _ in range(frames):
                for i, core in enumerate(cores):
                    core.set_keys(*(keys[i] if keys else []))
                    core.run_frame()

        def tap(key, who=None):
            both(4, [[KEYS[key]] if who in (None, i) else [] for i in range(2)])
            both(10, [[], []])

        both(8)
        tap("START"); tap("DOWN")             # both on GAME SELECT, 2 PLAYER
        tap("START", who=first)               # one goes to the cable...
        both(LATE)
        tap("START", who=1 - first)           # ...and the other much later
        both(60)
        tap("START", who=0)                   # the master releases the lobby
        both(120)
        m = read_bytes(cores[0], session_addr, game_size)
        sl = read_bytes(cores[1], session_addr, game_size)
        if m == bytes(game_size) or m != sl:
            failures.append(f"con {name} {LATE // 60} s antes en el cable no "
                            f"arranco la partida ({cable.transfers} "
                            f"transferencias buenas, {cable.failed} fallidas)")
        else:
            print(f"  {name} llega {LATE // 60} s antes: se encuentran igual "
                  f"({cable.transfers} buenas, {cable.failed} fallidas)")
        _KEEP.append((cores, screens, cable, ends))
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: da igual que consola llegue antes al cable.")
    return mute_check(rom)


def _disturbed_match(rom, name, mute_at=(), glitch_at=(), stale_at=(),
                     late_start=0):
    """The match mute_check, glitch_check and stale_check play; returns
    failures."""
    sym, why = symbol(rom, "g_session")
    if sym is None:
        skip(f"{why}")
        return None
    session_addr, session_size = sym
    game_size = session_size - 4

    mgba.log.silence()
    cores, screens = [], []
    for _ in range(2):
        core = mgba.core.load_path(rom)
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
        core.reset()
        cores.append(core)
        screens.append(screen)
    cable = Cable(*cores)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)
    cable.mute_slave_at = set(mute_at)
    cable.glitch_at = set(glitch_at)
    cable.stale_slave_at = set(stale_at)
    cable.late_start = late_start

    def both(frames, keys=None):
        for _ in range(frames):
            for i, core in enumerate(cores):
                core.set_keys(*(keys[i] if keys else []))
                core.run_frame()

    def tap(key, who=None):
        both(4, [[KEYS[key]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    both(8)
    tap("START"); tap("DOWN"); tap("START")   # both to the cable
    both(60)
    tap("START", who=0)                       # the master releases the lobby
    both(60)
    started = read_bytes(cores[0], session_addr, game_size) != bytes(game_size)
    # The link's own frame counter, the byte after local_slot (see main()'s
    # game_size): it has to keep counting after the slave's second silence.
    def frame(core):
        return core.memory.u8[session_addr + game_size + 1]
    both(200, [[KEYS["DOWN"]], [KEYS["DOWN"]]])
    mid = frame(cores[0])
    both(100, [[KEYS["DOWN"]], [KEYS["DOWN"]]])
    both(30, [[], []])
    late = frame(cores[0])
    m = read_bytes(cores[0], session_addr, game_size)
    sl = read_bytes(cores[1], session_addr, game_size)
    failures = []
    left = cable.mute_slave_at | cable.glitch_at | cable.stale_slave_at
    if left:
        failures.append(f"la prueba no llego a {name} en {sorted(left)}")
    if cable.late_start:
        failures.append(f"la prueba no llego a {name}: quedan "
                        f"{cable.late_start} transferencias de retraso")
    if not started:
        failures.append(f"al {name} en el lobby la partida no arranco "
                        f"({cable.transfers} buenas, {cable.failed} malas)")
    elif m != sl:
        failures.append(f"tras {name} en plena partida las dos consolas ya "
                        "no juegan la misma")
    elif (late - mid) % 256 < 60:
        failures.append(f"tras {name} la partida se paro: frame {mid} -> "
                        f"{late} en 130 frames")
    else:
        print(f"  {name}, en el lobby y en plena partida: {cable.transfers} "
              f"buenas, {cable.failed} malas, misma partida en las dos")
    _KEEP.append((cores, screens, cable, ends))
    return failures


def mute_check(rom):
    """A CONSOLE THE CABLE STOPS HEARING, AND THE PORT STARTED OVER.

    Two real SPs, second session of the evening: one good transfer, then
    every one after it with the slave's slot empty — no error bit, SD high,
    no reset — until they were switched off. gba/link.c starts the port over
    after LINK_ABSENT_LIMIT such transfers in a row. The cable mutes the
    slave (CableEnd.mute) in the lobby and again in the middle of the match.
    """
    failures = _disturbed_match(rom, "enmudecer al esclavo", mute_at=(1, 150))
    if failures is None:
        return 0
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: si el cable deja de oir a una consola, el puerto se reinicia y "
          "siguen.")
    return glitch_check(rom)


def glitch_check(rom):
    """ONE TRANSFER THAT FAILS, AND THE ERROR BIT THAT OUTLIVES IT.

    The next evening on the same SPs: CABLE LOST in a coop match, and then
    no connecting at all — the master at SIOCNT $6049, the error bit up,
    "R 999": a port started over every frame and not one transfer tried,
    because the pump reset on the error bit instead of transferring, and the
    error bit is only rewritten by a transfer. The cable fails ONE transfer
    with the error bit (Cable.glitch_at), once in the lobby and once in the
    middle of the match, and keeps the bit through resets as the SP did.
    """
    failures = _disturbed_match(rom, "fallar una transferencia",
                                glitch_at=(1, 150))
    if failures is None:
        return 0
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: una transferencia fallida no deja el cable muerto.")
    return stale_check(rom)


def stale_check(rom):
    """A CONSOLE LATE WITH ITS MOVE, ONCE.

    A move is loaded for the next transfer as the last one ends, by the
    serial interrupt (and on the Single-Pak copy by a poll). A console that
    does not get to it in time sends the move before again — and that word,
    queued, was "a word for the wrong frame": desynced, CABLE LOST, the
    match over for one late interrupt. Both consoles have the same two
    words, so both can see that they are not for the same step: neither
    takes them, both send again, and the match has lost one transfer.
    The cable makes the slave late (Cable.stale_slave_at) eight times in
    the match, the last two running.

    AND NONE OF IT IS A CONSOLE THAT HAS LEFT (link_partner_left counts the
    transfers in which this console's move crossed and the other slot held
    no move for this step): a late word is one, so the count has to start
    again with every move that does arrive — eight late words in a match
    are not six in a row — and it does not start at all before the other
    console's first move: a slave a fifth of a second behind the master
    into the match (Cable.late_start, twelve transfers) is a slave that is
    coming.
    """
    failures = _disturbed_match(rom, "llegar tarde el esclavo con su jugada",
                                stale_at=(150, 170, 190, 210, 230, 250, 260, 261))
    if failures is None:
        return 0
    late = _disturbed_match(rom, "entrar tarde el esclavo en la partida",
                            late_start=12)
    failures += late or []
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: una jugada que llega tarde cuesta una transferencia, no la partida.")
    return jitter_check(rom)


# ---------------------------------------------------------------------------
# The evening of the second real test, one check per thing it found.
# ---------------------------------------------------------------------------
def _pair(rom, sd_lags=False, slow=False, slave_rom=None):
    """Two consoles on a cable, and the helpers every check below uses.
    `slave_rom` puts a different image on the slave's end (Single-Pak)."""
    mgba.log.silence()
    cores, screens = [], []
    for path in (rom, slave_rom or rom):
        core = mgba.core.load_path(path)
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)   # must stay alive; see run_rom.load()
        core.reset()
        cores.append(core)
        screens.append(screen)
    cable = Cable(*cores)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)
    cable.sd_lags = sd_lags
    cable.slow = slow
    _KEEP.append((cores, screens, cable, ends))

    def both(frames, keys=None):
        for _ in range(frames):
            for i, core in enumerate(cores):
                core.set_keys(*(keys[i] if keys else []))
                core.run_frame()
            cable.settle()

    def tap(key, who=None):
        both(4, [[KEYS[key]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    return cores, cable, both, tap


def _screen_of(core):
    """Which front-end page a console is on, read off its map."""
    import run_rom
    rows = [run_rom.tilemap_text(core, r) for r in range(8, 17)]
    text = " ".join(rows)
    if "LINK CABLE" in rows[0]:
        return "cable"
    if "HANDICAP" in text:
        return "ajustes"
    if "GAME SELECT" in rows[0]:
        return "menu"
    return "otra"


def _same_match(rom, cores):
    sym, _ = symbol(rom, "g_session")
    size = sym[1] - 4
    m = read_bytes(cores[0], sym[0], size)
    s = read_bytes(cores[1], sym[0], size)
    return m != bytes(size) and m == s


def jitter_check(rom):
    """BOTH CONSOLES INTO THE CABLE AT ONCE, WITH SD LATE.

    Two real SPs entering the cable together never met: "G 0 B 999 R 999" on
    both, every transfer thrown away and the port restarted, while SIOCNT at
    leisure showed neither the error bit nor SD low. The cable here leaves SD
    down until the end of the frame after every transfer (Cable.sd_lags), so
    an interrupt that judges by SD judges every transfer bad. gba/link.c
    judges by the two words instead.
    """
    if symbol(rom, "g_session")[0] is None:
        skip("el ELF no exporta g_session")
        return 0
    cores, cable, both, tap = _pair(rom, sd_lags=True)
    both(8)
    tap("START"); tap("DOWN"); tap("START")    # both to the cable, together
    both(60)
    tap("START", who=0)
    both(80)
    if not _same_match(rom, cores):
        print(f"FALLA: con SD rezagado y las dos a la vez no arranco la "
              f"partida ({cable.transfers} buenas, {cable.failed} malas)")
        return 1
    print(f"  a la vez y con SD rezagado: {cable.transfers} transferencias "
          f"buenas, la misma partida en las dos")
    print("OK: las dos consolas se encuentran aunque SD llegue tarde.")
    return role_check(rom)


def role_check(rom):
    """THE MASTER LEAVES AND THE SLAVE STAYS A SLAVE.

    On two real SPs a slave came up on LEVEL SETTINGS: its master had left
    the cable, the port that left went to general purpose, the slave's SI —
    wired to the master's SO — floated low, and the slave read itself as the
    master of a lobby it had already linked in. The cable here floats the
    slave's SI whenever the master's port is out of multiplayer mode
    (Cable.parent_drives_so). The master reaches LEVEL SETTINGS, backs out
    to GAME SELECT; a second later the slave must be WAITING, not choosing.
    Then the master comes back and they play.
    """
    if symbol(rom, "g_session")[0] is None:
        skip("el ELF no exporta g_session")
        return 0
    cores, cable, both, tap = _pair(rom)
    failures = []
    both(8)
    tap("START"); tap("DOWN"); tap("START")
    both(60)
    where = [_screen_of(c) for c in cores]
    if where != ["ajustes", "cable"]:
        failures.append(f"al encontrarse: maestro en {where[0]}, esclavo en "
                        f"{where[1]}")
    tap("B", who=0)                     # LEVEL SETTINGS -> GAME SELECT
    both(120)
    where = [_screen_of(c) for c in cores]
    if where[1] != "cable":
        failures.append(f"con el maestro fuera, el esclavo esta en {where[1]}")
    import run_rom
    # Its master in a menu says it is there (link_beacon), so the slave
    # waits for its player rather than asking for a cable.
    if "WAITING FOR OTHER PLAYER" not in run_rom.tilemap_text(cores[1], 10):
        failures.append("con el maestro fuera, el esclavo no vuelve a esperar")
    tap("START", who=0)                 # the master comes back
    both(60)
    tap("START", who=0)
    both(80)
    if not _same_match(rom, cores):
        failures.append("de vuelta el maestro, no arranca la misma partida")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("  el maestro se va: el esclavo vuelve a esperar; vuelve: juegan")
    print("OK: la consola esclava no se cree maestra cuando la otra se va.")
    return churn_check(rom)


def churn_check(rom):
    """THE SLAVE COMES AND GOES, AND THE MATCH IS STILL ONE GAME.

    What two players testing the cable do: in, out, in again. The slave
    walks off while the master is on LEVEL SETTINGS (the master goes back to
    waiting, and to choosing when the slave returns), and again after the
    master has chosen, halfway through the handshake (it starts again from
    HELLO). An empty board and then the points was the real version of this
    — a slave that walked in halfway took the rest of the settings and kept
    its own seed.
    """
    if symbol(rom, "g_session")[0] is None:
        skip("el ELF no exporta g_session")
        return 0
    cores, cable, both, tap = _pair(rom)
    failures = []
    both(8)
    tap("START"); tap("DOWN"); tap("START")
    both(60)
    tap("B", who=1)                     # the slave leaves the cable
    both(120)
    if _screen_of(cores[0]) != "cable":
        failures.append(f"sin esclavo el maestro sigue en "
                        f"{_screen_of(cores[0])}")
    tap("START", who=1)                 # ...and comes back
    both(60)
    if _screen_of(cores[0]) != "ajustes":
        failures.append(f"con el esclavo de vuelta el maestro esta en "
                        f"{_screen_of(cores[0])}")
    # The master chooses; the slave walks off on the very next frame, in the
    # middle of the handshake, and comes back.
    both(4, [[KEYS["START"]], [KEYS["B"]]])
    both(10, [[], []])
    both(90)
    tap("START", who=1)
    both(60)
    if _screen_of(cores[0]) == "ajustes":
        tap("START", who=0)
        both(80)
    if not _same_match(rom, cores):
        failures.append(f"tras las idas y venidas no juegan la misma partida "
                        f"(maestro en {_screen_of(cores[0])}, esclavo en "
                        f"{_screen_of(cores[1])})")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("  el esclavo se va y vuelve, antes y en pleno saludo: la misma "
          "partida en las dos")
    print("OK: las consolas entran y salen del cable sin enredarse.")
    return lost_check(rom)


def lost_check(rom):
    """A QUIET CABLE: LINK ISSUES, SILENCE AND A CHIME, AND BACK OR OUT.

    On a real cable the slave went quiet and the master played on. The
    cable comes out of a match with a tune: after its chime, neither console
    has a channel going; back in, the tune comes back on both. Then out for
    good: the CABLE LOST window on both, and START to the title without the
    high scores (which a real test went through, for a match nobody ended).
    """
    waiting, why = symbol(rom, "g_link_waiting")
    lost, why2 = symbol(rom, "g_link_lost")
    if waiting is None or lost is None or symbol(rom, "g_session")[0] is None:
        skip(f"{why or why2}")
        return 0
    import run_rom
    cores, cable, both, tap = _pair(rom)
    both(8)
    tap("START"); tap("DOWN"); tap("START")
    both(60)
    tap("DOWN", who=0); tap("DOWN", who=0); tap("RIGHT", who=0)   # a tune
    tap("START", who=0)
    both(80)

    def loudest(frames):
        top = 0
        for _ in range(frames):
            both(1)
            top = max([top] + [run_rom.sound_state(c)["activos"] for c in cores])
        return top

    failures = []
    playing = loudest(30)
    cable.plugged = False
    both(LINK_LOST_FRAMES_PY + 5)
    chime = loudest(20)
    silent = loudest(60)
    cable.plugged = True
    both(20)
    back = loudest(60)
    if not playing:
        failures.append("la partida por cable no sonaba")
    elif not chime:
        failures.append("al perder el cable no suena ningun aviso")
    elif silent:
        failures.append(f"con el cable perdido siguen sonando {silent} canales")
    elif not back:
        failures.append("con el cable de vuelta la musica no vuelve")
    else:
        print(f"  cable fuera: aviso y silencio en las dos; de vuelta, la "
              f"musica ({playing} -> 0 -> {back} canales)")
    # Out again, and this time for good: after LINK_GIVEUP_FRAMES both
    # consoles put up the CABLE LOST window, and START leaves for the title
    # — not the high scores.
    drawn = symbol(rom, "g_pmenu_drawn_w")[0]
    linked = symbol(rom, "g_linked")[0]
    cable.plugged = False
    both(LINK_GIVEUP_FRAMES_PY + 10)
    boxes = [c.memory.u8[drawn[0]] for c in cores] if drawn else [1, 1]
    gone = [c.memory.u8[lost[0]] for c in cores]
    if gone != [1, 1] or 0 in boxes:
        failures.append(f"tras {LINK_GIVEUP_FRAMES_PY} frames sin cable no "
                        f"sale la ventana de CABLE LOST en las dos (perdido "
                        f"{gone}, ventana {boxes})")
    # ...AND ITS HEADING ON ONE LINE: every letter the raised copy, as PAUSE's
    # are. Half of them were, and CABLE LOST stood at two heights.
    heads = [run_rom.tilemap_text(c, run_rom.PM_HEAD) for c in cores]
    plain = [run_rom.heading_unraised(c, run_rom.PM_HEAD) for c in cores]
    if any("CABLE LOST" not in h for h in heads):
        failures.append(f"la ventana no dice CABLE LOST: {heads}")
    elif any(plain):
        failures.append(f"CABLE LOST a dos alturas: {plain[0] or plain[1]} "
                        f"un pixel por debajo de las demas letras")
    else:
        print("  CABLE LOST, todas sus letras a la misma altura")
    tap("START")
    both(30)
    pages = [" ".join(run_rom.tilemap_text(c, r) for r in range(0, 20))
             for c in cores]
    if linked and any(c.memory.u8[linked[0]] for c in cores):
        failures.append("START no saca de la partida con el cable perdido")
    elif any("HIGH" in p or "SCORES" in p for p in pages):
        failures.append("con el cable perdido se pasa por la tabla de records")
    else:
        print("  diez segundos sin cable: ventana CABLE LOST en las dos, y "
              "START al titulo sin pasar por los records")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: perder el cable calla la musica en las dos y avisa; si vuelve se "
          "sigue, y si no, se sale al titulo.")
    return storm_check(rom)


def storm_check(rom):
    """IN AND OUT OF THE CABLE, HESITATING, AND STILL THEY MEET.

    Two real SPs, going in and out of the lobby a few times, ended up on
    "A 999 R 999" on both: nothing but empty transfers and restarts. The
    cable here mutes a console whenever the other takes its port to general
    purpose (CableEnd.writeRegister), which is what the restarts did; one
    console restarting then silenced the other, which restarted and silenced
    the first. Here: both to the cable, the slave out and back twice, the
    master out and back once; then they have to meet and play.
    """
    if symbol(rom, "g_session")[0] is None:
        skip("el ELF no exporta g_session")
        return 0
    cores, cable, both, tap = _pair(rom)
    both(8)
    tap("START"); tap("DOWN"); tap("START")
    both(40)
    for who, wait in ((1, 30), (1, 90), (0, 45)):
        tap("B", who=who)
        both(wait)
        tap("START", who=who)
        both(wait)
    both(120)
    if _screen_of(cores[0]) == "ajustes":
        tap("START", who=0)
        both(90)
    if not _same_match(rom, cores):
        print(f"FALLA: tras entrar y salir del cable no llegan a jugar "
              f"(maestro en {_screen_of(cores[0])}, esclavo en "
              f"{_screen_of(cores[1])}; {cable.transfers} buenas, "
              f"{cable.failed} malas)")
        return 1
    print(f"  entran y salen tres veces y juegan: {cable.transfers} buenas, "
          f"{cable.failed} malas")
    print("OK: entrar y salir del cable no deja a las consolas sordas.")
    return race_check(rom)


def race_check(rom):
    """THE MASTER FIRST, AND THE SLAVE STAYS A SLAVE.

    On two real SPs the cable failed every time the master reached the lobby
    first, and never the other way round; once, both consoles went on to
    LEVEL SETTINGS. A slave's SI is the master's SO, which the master drives
    LOW for the length of each transfer to pass the slave its turn; a slave
    that read SI while the master was pumping took itself for the master.
    Transfers here take time (Cable.slow: busy, and the slave's SI low, until
    the end of the frame). The master goes to the cable first; the slave a
    second later; it must never reach LEVEL SETTINGS, and they must play.
    Then the same the other way round.
    """
    if symbol(rom, "g_session")[0] is None:
        skip("el ELF no exporta g_session")
        return 0
    failures = []
    for first, name in ((0, "el maestro"), (1, "el esclavo")):
        cores, cable, both, tap = _pair(rom, slow=True)
        both(8)
        tap("START"); tap("DOWN")
        tap("START", who=first)
        both(60)
        tap("START", who=1 - first)
        wrong = 0
        for _ in range(12):
            both(10)
            if _screen_of(cores[1]) == "ajustes":
                wrong += 1
        if wrong:
            failures.append(f"con {name} primero, el esclavo llego a LEVEL "
                            f"SETTINGS")
            continue
        if _screen_of(cores[0]) != "ajustes":
            failures.append(f"con {name} primero, el maestro no llega a "
                            f"LEVEL SETTINGS (esta en {_screen_of(cores[0])}; "
                            f"{cable.transfers} buenas, {cable.failed} malas)")
            continue
        tap("START", who=0)
        both(90)
        sym = symbol(rom, "g_session")[0]
        size = sym[1] - 4
        frame0 = cores[0].memory.u8[sym[0] + size + 1]
        both(60, [[KEYS["DOWN"]], [KEYS["DOWN"]]])
        moved = (cores[0].memory.u8[sym[0] + size + 1] - frame0) % 256
        if not _same_match(rom, cores):
            failures.append(f"con {name} primero no juegan la misma partida")
        elif moved < 30:
            failures.append(f"con {name} primero la partida no avanza "
                            f"({moved} frames en 60)")
        else:
            print(f"  {name} primero, con transferencias lentas: cada uno en "
                  f"su sitio, y juegan")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la consola esclava no se cree maestra mientras el maestro "
          "transfiere.")
    return mode_check(rom)


def mode_check(rom):
    """2 PLAYER ON ONE CONSOLE, COOPERATIVE ON THE OTHER: NO MATCH.

    The lobby was one for both linked modes and the master's choice won, so
    a player who had picked 2 PLAYER was taken into a coop game. Each
    console now says its mode in HELLO (tengen_lobby_mode) and a partner of
    the other mode is no partner. Here the master picks 2 PLAYER and the
    slave COOPERATIVE: after two seconds both are still waiting and neither
    is on LEVEL SETTINGS. The slave goes back and picks 2 PLAYER: they link,
    and the match is a race, not a shared board.
    """
    sym = symbol(rom, "g_session")[0]
    if sym is None:
        skip("el ELF no exporta g_session")
        return 0
    import run_rom
    off = run_rom.game_offsets(rom)
    failures = []
    cores, cable, both, tap = _pair(rom, slow=True)
    both(8)
    tap("START")
    tap("DOWN")                                  # both on 2 PLAYER...
    tap("DOWN", who=1)                           # ...the slave on COOPERATIVE
    tap("START")
    both(120)
    where = [_screen_of(c) for c in cores]
    if where != ["cable", "cable"]:
        failures.append(f"con modos distintos no deberian enlazarse: maestro "
                        f"en {where[0]}, esclavo en {where[1]}")
    tap("B", who=1)                              # back to GAME SELECT
    tap("UP", who=1)                             # COOPERATIVE -> 2 PLAYER
    tap("START", who=1)
    both(90)
    if _screen_of(cores[0]) != "ajustes":
        failures.append(f"con el mismo modo el maestro no llega a LEVEL "
                        f"SETTINGS (esta en {_screen_of(cores[0])})")
    else:
        tap("START", who=0)
        both(90)
        coop = [c.memory.u8[sym[0] + off["coop"]] for c in cores] \
            if "coop" in off else [0, 0]
        if not _same_match(rom, cores):
            failures.append("con el mismo modo no juegan la misma partida")
        elif coop != [0, 0]:
            failures.append(f"eligieron 2 PLAYER y la partida es cooperativa "
                            f"({coop})")
        else:
            print("  2 PLAYER contra COOPERATIVE: las dos esperan; con el "
                  "mismo modo, juegan una carrera")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: solo se enlazan dos consolas que eligieron el mismo modo.")
    return singlepak_check(rom)


def singlepak_check(rom):
    """SINGLE-PAK: THE CARTRIDGE AGAINST THE IMAGE IT SENDS.

    The image a console with no cartridge receives (build/tengen_mb.mb, the
    same program linked for external WRAM) boots straight into the lobby,
    follows whichever game the cartridge's player chose, and plays it in
    lockstep with the cartridge. Here the image is loaded as if the BIOS
    had just received it, for 2 PLAYER and for COOPERATIVE: the master
    reaches LEVEL SETTINGS, starts, and both consoles hold the same game
    byte for byte, before and after a few seconds of play. The sending
    itself is the BIOS's and is not in this emulator; see mb_send_check.
    """
    mb = os.path.splitext(rom)[0] + "_mb.mb"
    if not os.path.exists(mb):
        skip(f"no encuentro {mb}")
        return 0
    msym, why = symbol(rom, "g_session")
    ssym, why2 = symbol(mb, "g_session")
    if msym is None or ssym is None:
        skip(f"{why or why2}")
        return 0
    import run_rom
    off = run_rom.game_offsets(rom)
    size = msym[1] - 4
    failures = []
    for coop in (False, True):
        name = "COOPERATIVE" if coop else "2 PLAYER"
        cores, cable, both, tap = _pair(rom, slow=True, slave_rom=mb)
        both(1000)                  # the start, and its breadcrumbs (MB_STAGE)
        if _screen_of(cores[1]) != "cable":
            failures.append(f"{name}: la imagen no arranca en el lobby "
                            f"({_screen_of(cores[1])})")
            continue
        tap("START", who=0)
        tap("DOWN", who=0)
        if coop:
            tap("DOWN", who=0)
        tap("START", who=0)
        both(90)
        if _screen_of(cores[0]) != "ajustes":
            failures.append(f"{name}: el cartucho no llega a LEVEL SETTINGS "
                            f"({_screen_of(cores[0])})")
            continue
        tap("START", who=0)
        both(90)

        def same():
            m = read_bytes(cores[0], msym[0], size)
            s_ = read_bytes(cores[1], ssym[0], size)
            return m != bytes(size) and m == s_
        if not same():
            failures.append(f"{name}: las dos no juegan la misma partida")
            continue
        got = cores[1].memory.u8[ssym[0] + off["coop"]] if "coop" in off \
            else int(coop)
        if bool(got) != coop:
            failures.append(f"{name}: la imagen juega otro modo ({got})")
            continue
        for k in range(6):
            both(20, [[KEYS["LEFT" if k % 2 else "RIGHT"]], [KEYS["DOWN"]]])
            both(20, [[], []])
        if not same():
            failures.append(f"{name}: jugando, las dos partidas se separan")
        else:
            print(f"  {name}: la imagen sigue al cartucho y juegan la misma "
                  "partida")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    # ...AND STARTED THE WAY A REAL BIOS LEAVES IT: the serial interrupt
    # still on, its vector not this program's, and the cartridge already
    # back in its lobby and talking. On two real SPs the slave froze on the
    # BIOS's logo; here, before crt0 switched interrupts off first thing,
    # the two never linked.
    cores, cable, both, tap = _pair(rom, slow=True, slave_rom=mb)
    both(1000)
    tap("START", who=0); tap("DOWN", who=0); tap("START", who=0)
    both(30)
    slave = cores[1]
    slave.reset()
    io = slave._native.memory.io
    io[0x208 >> 1] = 1                     # IME
    io[0x200 >> 1] = 0x0080                # IE: serial
    io[0x128 >> 1] = 0x6003 | 0x4000       # multiplayer, interrupt on
    slave.memory.u32[0x03007FFC] = 0x03001000
    both(1200)
    if _screen_of(cores[0]) != "ajustes":
        failures.append(f"arrancada como la deja la BIOS, la imagen no enlaza "
                        f"(el cartucho esta en {_screen_of(cores[0])})")
    else:
        print("  y arrancada como la deja la BIOS, con la interrupcion del "
              "cable encendida y el cartucho hablando, enlaza igual")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la imagen de Single-Pak juega contra el cartucho.")
    return (mb_send_check(rom) or records_dump_check(rom) or
            records_carts_check(rom) or
            clone_reset_check(rom) or clone_budget_check(rom) or
            flash_wait_check(rom))


def records_dump_check(rom):
    """LOS RECORDS DE UNA COPIA PASAN AL CARTUCHO AL ENLAZAR.

    Una copia de Single-Pak no tiene memoria de guardado: lo que pone en sus
    tablas se pierde al apagarla. Lo que puso y aun no ha entregado lo
    lleva marcado (g_unsent), y al enlazar con una consola que si guarda
    (TengenRecordSync, entre el GO del lobby y la partida) se lo pasa; la que
    guarda lo mete en su tabla y en su guardado, y las dos dicen RECORDS
    SAVED dos segundos antes de empezar. Aqui la copia trae dos records
    plantados, uno en 1 PLAYER y otro en la tabla de COOPERATIVE (las de los
    modos cruzan con su numero del cable, 8 en adelante: leader_wire_id), y
    despues: estan en las mismas tablas del cartucho con sus letras, en su
    guardado, la copia ya no los tiene pendientes, las dos lo dijeron, y la
    partida empieza igual en las dos.
    """
    mb = os.path.splitext(rom)[0] + "_mb.mb"
    tables_m, why = symbol(rom, "g_tables")
    tables_s, why2 = symbol(mb, "g_tables")
    unsent_s, why3 = symbol(mb, "g_unsent")
    if not os.path.exists(mb) or tables_m is None or tables_s is None or unsent_s is None:
        skip(f"{why or why2 or why3 or 'no encuentro ' + mb}")
        return 0
    import run_rom
    entry = tables_s[1] // unsent_s[1]           # bytes per row
    SCORE, LINES, LETTERS = 123456, 77, (3, 12, 26)
    # The modes' tables follow the skins' (LEADER_TABLES, gba/port.h): four
    # of them, 2 PLAYER first, so COOPERATIVE is the skins' count plus one.
    lay = run_rom.check_layout(rom)
    coop = lay["skin_tables"] + 1
    entries = lay["leader_entries"]
    COOP_SCORE, COOP_LINES, COOP_LETTERS = 65432, 55, (15, 1, 4)
    cores, cable, both, tap = _pair(rom, slow=True, slave_rom=mb)
    both(1000)
    if _screen_of(cores[1]) != "cable":
        print(f"FALLA: la copia no arranca en el lobby ({_screen_of(cores[1])})")
        return 1
    copy = cores[1]
    copy.memory.u32[tables_s[0]] = SCORE           # table 0, row 0
    copy.memory.u16[tables_s[0] + 4] = LINES
    for c, v in enumerate(LETTERS):
        copy.memory.u8[tables_s[0] + 6 + c] = v
    copy.memory.u8[unsent_s[0]] = 1
    at = tables_s[0] + coop * entries * entry      # its table's row 0
    copy.memory.u32[at] = COOP_SCORE
    copy.memory.u16[at + 4] = COOP_LINES
    for c, v in enumerate(COOP_LETTERS):
        copy.memory.u8[at + 6 + c] = v
    copy.memory.u8[unsent_s[0] + coop * entries] = 1
    tap("START", who=0); tap("DOWN", who=0); tap("START", who=0)
    both(90)
    tap("START", who=0)
    said = [False, False]
    for _ in range(240):
        both(1)
        for i, core in enumerate(cores):
            if "SAVED" in run_rom.tilemap_text(core, 11):
                said[i] = True
    failures = []
    row = tables_m[0]
    got = (cores[0].memory.u32[row], cores[0].memory.u16[row + 4],
           tuple(cores[0].memory.u8[row + 6 + c] for c in range(3)))
    if got != (SCORE, LINES, LETTERS):
        failures.append(f"el cartucho no tiene el record de la copia arriba: {got}")
    row = tables_m[0] + coop * entries * entry
    got = (cores[0].memory.u32[row], cores[0].memory.u16[row + 4],
           tuple(cores[0].memory.u8[row + 6 + c] for c in range(3)))
    if got != (COOP_SCORE, COOP_LINES, COOP_LETTERS):
        failures.append(f"la tabla de COOPERATIVE del cartucho no tiene el de la "
                        f"copia arriba: {got}")
    pending = [copy.memory.u8[unsent_s[0] + i] for i in range(unsent_s[1])]
    if any(pending):
        failures.append("la copia sigue teniendo records sin entregar")
    if not all(said):
        failures.append(f"RECORDS SAVED en cartucho/copia: {said}")
    sram = bytes(cores[0].memory.u8[0x0E000000 + i] for i in range(64))
    score_bytes = SCORE.to_bytes(4, "little")
    if score_bytes not in sram:
        failures.append("el record no llego al guardado del cartucho")
    # SAVE_MODE_OFF(1): the modes' tables at their fixed place (kCheckProbe).
    lay = run_rom.check_layout(rom)
    at = 0x0E000000 + lay["save_mode"] + lay["save_table_bytes"]
    sram = bytes(cores[0].memory.u8[at + i] for i in range(9))
    if sram[:4] != COOP_SCORE.to_bytes(4, "little"):
        failures.append(f"el de COOPERATIVE no llego a su sitio del guardado: {sram!r}")
    both(120)
    if not _same_match(rom, cores) and not _same_match_mb(rom, mb, cores):
        failures.append("despues, las dos no juegan la misma partida")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print(f"  la copia entrega {SCORE} / {LINES} lineas, y {COOP_SCORE} en "
          "COOPERATIVE; el cartucho los guarda y las dos dicen RECORDS SAVED")
    print("OK: los records de una copia se guardan en el cartucho al enlazar.")
    return 0


def records_carts_check(rom):
    """TWO CARTRIDGES SHOW EACH OTHER THEIR BEST, IF THEIR PLAYERS SAY SO.

    Each has a save, and until now gave the other nothing: only a
    Single-Pak copy handed its rows on. But a table is its owner's — a
    friend's monstrous scores push the owner's own off the bottom — so
    between two cartridges nothing crosses unasked: as they link
    (TengenRecordSync, between the lobby's GO and the match) each console
    whose tables differ from the other's asks its player, NO under the
    arrow. Here the master has a 1 PLAYER record and the slave a
    COOPERATIVE one:

      - the first time, the master says yes and the slave no: the slave's
        row is on the master's tables and in its save, the master's is NOT
        on the slave's, and both say RECORDS SAVED;
      - the second time, the same evening, nobody is asked and nothing
        crosses (the slave's player said no; the master's has the rows);
      - the slave makes a new record and they link again: the master's
        player, who said yes, is asked about it — a yes is to those rows,
        not to whatever comes after — and the slave's, who said no, is not;
      - switched off and on, they are asked again; now the slave says yes
        and has both rows too;
      - and after that, off and on again, the tables are the same and
        nobody is asked at all.
    """
    tables, why = symbol(rom, "g_tables")
    linked = symbol(rom, "g_linked")[0]
    if tables is None or linked is None:
        skip(f"{why}")
        return 0
    import run_rom
    lay = run_rom.check_layout(rom)
    entries = lay["leader_entries"]
    entry = 12                                  # sizeof(LeaderEntry); see below
    coop = lay["skin_tables"] + 1
    MINE = (123456, 77, (3, 12, 26))            # the master's, 1 PLAYER
    THEIRS = (65432, 55, (15, 1, 4))            # the slave's, COOPERATIVE
    COLD = (17000, 0, (1, 1, 1))
    cores, cable, both, tap = _pair(rom)
    both(300)

    def row(core, table, i=0):
        at = tables[0] + (table * entries + i) * entry
        return (core.memory.u32[at], core.memory.u16[at + 4],
                tuple(core.memory.u8[at + 6 + c] for c in range(3)))

    def plant(core, table, rec):
        at = tables[0] + table * entries * entry
        # The rows under it keep the cold table's order: 17000 goes second.
        for i in range(entries - 1, 0, -1):
            for b in range(entry):
                core.memory.u8[at + i * entry + b] = core.memory.u8[at + (i - 1) * entry + b]
        core.memory.u32[at] = rec[0]
        core.memory.u16[at + 4] = rec[1]
        for c, v in enumerate(rec[2]):
            core.memory.u8[at + 6 + c] = v

    # The size of a row, from the cold table itself: 17000 and then 16000.
    if row(cores[0], 0) != COLD or row(cores[0], 0, 1)[0] != 16000:
        print(f"FALLA: no leo la tabla fria del cartucho ({row(cores[0], 0)}, "
              f"{row(cores[0], 0, 1)})")
        return 1
    plant(cores[0], 0, MINE)
    # Three of the slave's, so that the offer is seen to go down a table and
    # not only across the tops: planted lowest first, each goes in at the top.
    MORE = ((55000, 30, (2, 2, 2)), (60000, 40, (5, 5, 5)))
    for rec in MORE:
        plant(cores[1], coop, rec)
    plant(cores[1], coop, THEIRS)

    def screen(core):
        return " ".join(run_rom.tilemap_text(core, r) for r in range(8, 17))

    def link_up(answers, first):
        """Both to the cable and into a match. `answers`: what each player
        says if asked (True yes, False no). Returns (frames from the
        master's START to the match, who was asked, who said RECORDS
        SAVED and what the line under it read)."""
        asked, said, lines = [False, False], [False, False], ["", ""]
        # Each to the cable's screen from wherever it is — the title after
        # the power switch, the table or the title after a match — and not
        # by counting presses: one START too many here is the master's,
        # and the match has begun before anybody looks.
        for i, core in enumerate(cores):
            for _ in range(8):
                where = _screen_of(core)
                if where == "cable":
                    break
                if where == "menu" and first:
                    tap("DOWN", who=i); tap("DOWN", who=i)  # COOPERATIVE; it stays
                tap("START", who=i)
            else:
                return None, asked, said, lines
        if any(c.memory.u8[linked[0]] for c in cores):
            return None, asked, said, lines
        both(90)
        both(4, [[KEYS["START"]], []])
        took = None
        for f in range(900):
            both(1, [[], []])
            for i, core in enumerate(cores):
                text = screen(core)
                if "ADD THEM HERE?" in text and not asked[i]:
                    asked[i] = True
                    if "> NO" not in text:
                        lines[i] = "la flecha no empieza en NO"
                    if answers[i]:
                        both(3, [[KEYS["RIGHT"]] if k == i else [] for k in range(2)])
                        both(4, [[], []])
                        both(3, [[KEYS["A"]] if k == i else [] for k in range(2)])
                    else:
                        both(3, [[KEYS["B"]] if k == i else [] for k in range(2)])
                if "SAVED" in run_rom.tilemap_text(core, 11):
                    said[i] = True
                    lines[i] = " ".join(run_rom.tilemap_text(core, 13).split()[1:-1])
            if all(c.memory.u8[linked[0]] for c in cores):
                took = f
                break
        return took, asked, said, lines

    def leave_the_match():
        session = symbol(rom, "g_session")[0]
        # Once the match has laid its board out: buried before that, the
        # board is simply cleared again.
        both(60)
        _bury(cores, session[0], run_rom.game_offsets(rom), 0)
        both(300)
        # ...and START until both are out of it (the plaque, the table).
        for _ in range(6):
            if not any(c.memory.u8[linked[0]] for c in cores):
                break
            tap("START")
            both(40)
        return not any(c.memory.u8[linked[0]] for c in cores)

    def power_cycle():
        for core in cores:
            core.reset()
        both(300)

    failures = []

    # 1. The master says yes, the slave says no.
    took, asked, said, lines = link_up([True, False], first=True)
    if took is None:
        print(f"FALLA: dos cartuchos con tablas distintas no llegan a la partida "
              f"(preguntadas {asked}; {screen(cores[0]).split()})")
        return 1
    if asked != [True, True]:
        failures.append(f"con tablas distintas no se pregunta a las dos: {asked}")
    if row(cores[0], coop) != THEIRS or row(cores[0], 0) != MINE:
        failures.append(f"la maestra dijo que si y no tiene el record de la esclava "
                        f"con el suyo: {row(cores[0], coop)}, {row(cores[0], 0)}")
    if (row(cores[0], coop, 1), row(cores[0], coop, 2)) != (MORE[1], MORE[0]):
        failures.append(f"de las tres filas de la esclava la maestra no tiene la "
                        f"segunda y la tercera: {row(cores[0], coop, 1)}, "
                        f"{row(cores[0], coop, 2)}")
    if row(cores[1], 0) != COLD:
        failures.append(f"la esclava dijo que NO y tiene el record de la maestra: "
                        f"{row(cores[1], 0)}")
    if said != [True, True] or lines[0] != "3 FROM THE OTHER GBA" or \
            lines[1] != "3 ON THE OTHER GBA":
        failures.append(f"RECORDS SAVED en maestra/esclava: {said}, {lines}")
    at = 0x0E000000 + lay["save_mode"] + lay["save_table_bytes"]
    if bytes(cores[0].memory.u8[at + i] for i in range(4)) != THEIRS[0].to_bytes(4, "little"):
        failures.append("el record de la esclava no llego al guardado de la maestra")
    if MINE[0].to_bytes(4, "little") in bytes(cores[1].memory.u8[0x0E000000 + i]
                                               for i in range(64)):
        failures.append("el record de la maestra esta en el guardado de la esclava, "
                        "que dijo que no")
    both(60)
    if not _same_match(rom, cores):
        failures.append("tras la pregunta, las dos no juegan la misma partida")
    if not failures:
        print(f"  tablas distintas: se pregunta a las dos (NO bajo la flecha); la "
              f"maestra dice que si y recibe {THEIRS[0]}, la esclava dice que no y "
              f"se queda como estaba; '{lines[0]}' / '{lines[1]}'")

    # 2. The same evening: nobody is asked.
    if not leave_the_match():
        failures.append("las dos consolas no salen de la primera partida")
    took, asked, said, lines = link_up([True, True], first=False)
    if took is None:
        failures.append("la segunda vez no llegan a la partida")
    elif any(asked) or any(said) or row(cores[1], 0) != COLD:
        failures.append(f"la misma tarde se vuelve a preguntar o a cruzar: "
                        f"preguntadas {asked}, avisos {said}, {row(cores[1], 0)}")
    elif took > 60:
        failures.append(f"sin nada que cruzar la partida tarda {took} frames")
    else:
        print(f"  la misma tarde: ni se pregunta ni cruza nada ({took} frames "
              "hasta la partida)")

    # 2b. ...UNLESS THE OTHER HAS SOMETHING NEW, and then only the player
    # who said yes: the answer was to those rows.
    if not leave_the_match():
        failures.append("las dos consolas no salen de la segunda partida")
    NEW = (70000, 60, (7, 7, 7))
    plant(cores[1], coop, NEW)
    took, asked, said, lines = link_up([True, True], first=False)
    if took is None:
        failures.append("con un record nuevo en la esclava no llegan a la partida")
    elif asked != [True, False]:
        failures.append("la esclava tiene un record nuevo: se pregunta a "
                        f"maestra/esclava {asked}, y era solo a la maestra "
                        "(dijo que si a los de antes; la esclava dijo que no)")
    elif row(cores[0], coop) != NEW or row(cores[1], 0) != COLD or \
            lines != ["1 FROM THE OTHER GBA", "1 ON THE OTHER GBA"]:
        failures.append(f"el record nuevo de la esclava: la maestra tiene "
                        f"{row(cores[0], coop)}, la esclava {row(cores[1], 0)}, "
                        f"avisos {lines}")
    else:
        print(f"  la esclava hace un record nuevo: solo se pregunta a la "
              f"maestra, que dijo que si a los de antes; '{lines[0]}'")

    # 3. Switched off and on: asked again, and now the slave says yes.
    if not leave_the_match():
        failures.append("las dos consolas no salen de la tercera partida")
    power_cycle()
    if row(cores[0], coop) != NEW or row(cores[1], coop) != NEW:
        failures.append("tras apagar y encender los records no estan en los guardados")
    took, asked, said, lines = link_up([False, True], first=True)
    if took is None:
        failures.append("tras apagar y encender no llegan a la partida")
    elif asked != [True, True]:
        failures.append(f"tras apagar y encender, con tablas distintas, no se "
                        f"pregunta a las dos: {asked}")
    elif row(cores[1], 0) != MINE:
        failures.append(f"la esclava dijo que si y no tiene el record de la "
                        f"maestra: {row(cores[1], 0)}")
    else:
        print(f"  apagadas y encendidas se pregunta de nuevo; ahora la esclava "
              f"dice que si y recibe {MINE[0]}")

    # 4. The same tables on both: nobody is asked, ever.
    if not leave_the_match():
        failures.append("las dos consolas no salen de la cuarta partida")
    power_cycle()
    took, asked, said, lines = link_up([True, True], first=True)
    if took is None:
        failures.append("con las tablas iguales no llegan a la partida")
    elif any(asked) or any(said):
        failures.append(f"con las mismas tablas en las dos se pregunta o se avisa: "
                        f"{asked}, {said}")
    elif took > 60:
        failures.append(f"con las mismas tablas la partida tarda {took} frames")
    else:
        print(f"  y con las mismas tablas en las dos no se pregunta nada "
              f"({took} frames hasta la partida)")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: dos cartuchos solo se pasan records si sus jugadores dicen que si.")
    return 0


def clone_budget_check(rom):
    """LA COPIA TAMBIEN DIBUJA DENTRO DEL BLANCO VERTICAL, JUGANDO.

    La copia de Single-Pak es el mismo programa corriendo desde la RAM
    externa, que es mas lenta que el cartucho: su dibujado acaba unas quince
    lineas mas tarde (213 de 227 donde el cartucho 198). Es la consola con
    menos margen, y `--vblank` solo mide el cartucho. Aqui el cartucho y la
    copia juegan un cooperativo, cada uno con su GHOST puesto desde el menu
    compartido, girando las piezas -- el frame que rehace un contorno --, y
    se lee en la copia la linea en la que acaba cada dibujado (donde entra
    en audio_frame): dentro del blanco, 160-227, todos. Con la sombra
    calculada al dibujar, como estaba, el cartucho ya acababa en la linea 13
    del frame siguiente y la copia mas alla.

    (Con el menu de pausa abierto la copia si se pasa unas lineas, sin que
    se vea: lo que escribe tarde esta a media pantalla y el barrido aun va
    por arriba. Aqui se mide la partida.)
    """
    mb = os.path.splitext(rom)[0] + "_mb.mb"
    if not os.path.exists(mb):
        skip(f"no encuentro {mb}")
        return 0
    import run_rom
    audio = run_rom.game_state_address(mb, "audio_frame")[0]
    there = [symbol(path, "g_session")[0] for path in (rom, mb)]
    if audio is None or None in there:
        skip("el ELF de la copia no exporta audio_frame o g_session")
        return 0
    audio &= ~1
    off = run_rom.game_offsets(rom)
    cores, cable, both, tap = _pair(rom, slave_rom=mb)
    copy = cores[1]

    def measured(frames, keys=None):
        """`frames` frames on both, the copy an instruction at a time: the
        line its drawing ends on, each frame it draws."""
        ends = []
        for _ in range(frames):
            cores[0].set_keys(*(keys[0] if keys else []))
            cores[0].run_frame()
            copy.set_keys(*(keys[1] if keys else []))
            fc, seen = copy.frame_counter, False
            while copy.frame_counter == fc:
                copy.step()
                if not seen and (copy.cpu.pc & ~1) in (audio, audio + 4):
                    seen = True
                    ends.append(copy.memory.u16[0x04000006])
            cable.settle()
        return ends

    def place(slot, piece, o, x, y):
        for core, g in zip(cores, there):
            b = g[0] + slot * off["stride"]
            core.memory.u8[b + off["current"]] = piece
            core.memory.u8[b + off["orientation"]] = o
            core.memory.u8[b + off["x"]] = x
            core.memory.u8[b + off["y"]] = y

    both(1000)
    tap("START", who=0)
    both(4, [[KEYS["L"], KEYS["R"]]] * 2)
    both(12, [[], []])
    tap("DOWN", who=0); tap("DOWN", who=0)              # COOPERATIVE
    tap("START", who=0)
    both(60)
    tap("START", who=0)
    both(120)
    for who in (0, 1):                                   # GHOST, each its own
        tap("START", who=who); both(10)
        tap("DOWN", who=who); tap("A", who=who)
        for _ in range(3):
            tap("DOWN", who=who)
        tap("A", who=who)
        tap("START", who=who); both(30)
    failures = []
    ghost0 = run_rom.check_layout(rom)["ghost"]
    drawn = sum(1 for r in range(20) for c in range(32)
                if ghost0 <= (copy.memory.u16[0x0600E000 + (r * 32 + c) * 2]
                              & 0x3FF) < ghost0 + 8)
    if drawn != 8:
        failures.append(f"la copia no tiene las dos sombras en su tablero "
                        f"({drawn} celdas): no se mide lo que se queria")
    ends, frames = measured(30), 30
    for piece in (1, 2, 5, 7):
        place(0, piece, 0, 3, 6)
        place(1, piece, 0, 8, 8)
        for _ in range(2):
            ends += measured(3, [[KEYS["A"]], [KEYS["A"]]])
            ends += measured(4)
            frames += 7
    late = [v for v in ends if v < 160]
    if len(ends) < frames:
        failures.append(f"la copia deja {frames - len(ends)} de {frames} frames "
                        f"sin dibujar")
    elif late:
        failures.append(f"la copia acaba {len(late)} de {len(ends)} dibujados "
                        f"fuera del blanco (linea {late[:8]})")
    elif not _same_match_mb(rom, mb, cores):
        failures.append("el cartucho y la copia ya no juegan la misma partida")
    else:
        print(f"  la copia, en cooperativo con las dos sombras y las piezas "
              f"girando: {len(ends)} frames, el ultimo acaba en la linea "
              f"{max(ends)} de 227")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la copia de Single-Pak dibuja cada frame de la partida dentro "
          "del blanco vertical.")
    return 0


def clone_reset_check(rom):
    """UNA COPIA SE REINICIA Y SIGUE VIVA.

    El reinicio del juego (A+B+START+SELECT, soft_reset_check en video.c) no
    pasa por la BIOS: salta al principio del programa (_restart, crt0.s),
    que en la copia de Single-Pak esta en la RAM externa, con todo el juego.
    Una consola sin cartucho que pasara por la BIOS se quedaria esperando
    a que alguien le volviera a mandar el juego. Aqui la copia arranca,
    llega al lobby, se reinicia sola, tiene que volver al lobby con su
    codigo intacto, y despues enlazar con el cartucho y jugar la misma
    partida byte a byte.
    """
    mb = os.path.splitext(rom)[0] + "_mb.mb"
    if not os.path.exists(mb):
        skip(f"no encuentro {mb}")
        return 0
    image = open(mb, "rb").read()
    cores, cable, both, tap = _pair(rom, slow=True, slave_rom=mb)
    both(1000)
    copy = cores[1]
    failures = []
    if _screen_of(copy) != "cable":
        print(f"FALLA: la copia no arranca en el lobby ({_screen_of(copy)})")
        return 1
    # The program's own frame count, which the start of the program clears:
    # proof the reset happened, and not a lobby that never noticed the keys.
    vbl, _ = symbol(mb, "g_vblank_count")
    before = copy.memory.u32[vbl[0]]
    reset = [KEYS["A"], KEYS["B"], KEYS["START"], KEYS["SELECT"]]
    both(6, [[], reset])
    both(400, [[], []])
    after = copy.memory.u32[vbl[0]]
    if not after < before:
        failures.append(f"A+B+START+SELECT no reinicio la copia (cuenta de "
                        f"frames {before} -> {after})")
    # The code itself, the first half of the image: nothing writes there.
    half = len(image) // 2
    now = bytes(copy.memory.u8[0x02000000 + i] for i in range(half))
    if now != image[:half]:
        diff = sum(1 for a, b in zip(now, image[:half]) if a != b)
        failures.append(f"despues del reinicio el codigo de la copia cambio "
                        f"({diff} bytes)")
    if _screen_of(copy) != "cable":
        failures.append(f"la copia no vuelve al lobby tras reiniciarse "
                        f"({_screen_of(copy)})")
    else:
        print("  A+B+START+SELECT en la copia: vuelve al lobby, sin BIOS, "
              "con el juego entero en memoria")
    if not failures:
        tap("START", who=0); tap("DOWN", who=0); tap("START", who=0)
        both(90)
        tap("START", who=0)
        both(360)
        if not _same_match_mb(rom, mb, cores):
            failures.append("despues del reinicio, la copia y el cartucho no "
                            "juegan la misma partida")
        else:
            print("  y despues enlaza con el cartucho y juegan la misma partida")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: una copia de Single-Pak se reinicia sin perderse.")
    return 0


def _same_match_mb(rom, mb, cores):
    m, _ = symbol(rom, "g_session")
    s, _ = symbol(mb, "g_session")
    size = m[1] - 4
    a = read_bytes(cores[0], m[0], size)
    b = read_bytes(cores[1], s[0], size)
    return a != bytes(size) and a == b


class FakeMultibootBios:
    """THE BIOS OF A CONSOLE SWITCHED ON WITH NO CARTRIDGE, as far as the
    cable can tell: the handshake, then the image word by word, decrypted
    the way GBATEK's pseudo-code for SWI $25 encrypts it, and the checksum
    at the end. What arrives is kept, so a check can compare it with what
    was meant to be sent. It says nothing for its first few transfers, like
    a console that is still being switched on, and answers the first
    end-of-image request with "not yet" ($0074), as a slave still checking
    would.

    AND IT LISTENS THE WAY THE BIOS DOES, which is what the first probe for
    it never met (gba/link.c, link_probe; read off PikalaxALT/gba_bios,
    asm/bios.s, sub_2B34 and the handlers from sub_2D70):

      - three modes in turn, a frame at a time: multiplayer at 115200 for
        thirty-one frames, normal for seven, JOY BUS for seven. Out of
        multiplayer mode SD is low (sd) and nothing is heard;
      - a mode's first two frames are setting up: its interrupt is not on,
        and a transfer then carries what its send register holds (0) and
        changes nothing;
      - a word that is not at 115200 arrives with the error bit, and that
        puts it back to the start of those two frames. The master reads
        nothing it can use from it ($FFFF here);
      - its answer to $6200 is for the NEXT transfer: the first reads 0,
        the ones after $7202; each keeps it in multiplayer mode eleven
        frames more, and once the handshake is past $61xx the clock stops;
      - having said $7202, anything but $6200 or $61xx puts it back to the
        start, with 0 to send.

    `strict=False` is the stand-in as it was before any of that was known:
    always listening, in the one mode, deaf to the speed.

    `deaf` is how many frames a word of the wrong speed costs it. Two, as
    read above — and a probe that asked between the lobby's words, one a
    frame, should then have got through every other time; on two real
    consoles it never did. So the real thing is harder on such words than
    that reading (how, is not known), and the checks ask the port to find
    this console with `deaf` at two, twelve and twenty."""

    CRC_XOR, DATA_XOR, SEED_MUL = 0xA517, 0x6465646F, 0x6F646573
    MULTI, NORMAL, JOY = 0, 1, 2
    DWELL = {MULTI: 30, NORMAL: 6, JOY: 6}
    BAUD_115200 = 3

    def __init__(self, strict=True, deaf=2):
        self.deaf = deaf
        self.state = "asleep"
        self.naps = 5
        self.header = []
        self.palette = None
        self.handshake = None
        self.client_data = 0x5A
        self.random = 0x42
        self.done = 0          # images received whole, checksum agreed
        self.received = None
        self.strict = strict
        self.mode = self.MULTI
        self.dwell = self.DWELL[self.MULTI]
        self.setup = 2         # frames until its interrupt is on
        self.held = False      # a handshake under way: the clock is stopped
        self.heard = 0         # $6200s it took, for the checks
        self.restarts = 0      # times a word of the wrong speed set it back
        # ANSWERS THAT GO ASTRAY: for the frames lose[0] <= n < lose[1],
        # counted from its first $7202, it hears as ever and what it
        # answers does not arrive ($FFFF) — a bad stretch on the cable.
        self.lose = (0, 0)
        self.since_found = None

    def sd(self):
        return self.state == "done" or self.mode == self.MULTI

    def _start_over(self, frames=2):
        if self.state != "done":
            self.state = "asleep"
        self.setup = max(self.setup, frames)
        self.held = False

    def frame(self):
        """Once a frame: sub_2B34."""
        if self.since_found is not None:
            self.since_found += 1
        if not self.strict or self.state == "done":
            return
        if self.setup:
            self.setup -= 1
        if self.held:
            return
        self.dwell -= 1
        if self.dwell < 0:
            self.mode = (self.mode + 1) % 3
            self.dwell = self.DWELL[self.mode]
            self._start_over()

    @classmethod
    def crc(cls, crc, data):
        for _ in range(32):
            low = (crc ^ data) & 1
            data >>= 1
            crc >>= 1
            if low:
                crc ^= cls.CRC_XOR
        return crc

    def answer(self, m, baud=BAUD_115200):
        word = self._answer(m, baud)
        if word == 0x7202 and self.since_found is None:
            self.since_found = 0
        if self.since_found is not None and \
                self.lose[0] <= self.since_found < self.lose[1]:
            return ABSENT
        return word

    def _answer(self, m, baud):
        if self.naps:
            self.naps -= 1
            return ABSENT
        st = self.state
        # A CONSOLE THAT HAS THE GAME IS RUNNING IT, not waiting in its BIOS:
        # from the checksum on it answers as the game at rest does (0).
        if st == "done":
            return 0x0000
        if self.strict:
            if self.mode != self.MULTI:
                return ABSENT
            if self.setup:
                # No interrupt yet: nothing is taken — but a word of the
                # wrong speed still starts the wait again when the wait is
                # longer than the BIOS's own two frames (see `deaf`).
                if baud != self.BAUD_115200 and self.deaf > 2:
                    self.setup = self.deaf
                    return ABSENT
                return 0x0000
            if baud != self.BAUD_115200:
                self.restarts += 1
                self._start_over(self.deaf)
                return ABSENT
            if m == 0x6200 and st in ("asleep", "failed"):
                # Its answer is for the next transfer.
                self.state, self.header = "found", []
                self.dwell = 11
                self.heard += 1
                return 0x0000
            if st == "found" and m != 0x6200 and (m & 0xFF00) != 0x6100:
                self._start_over()
                return 0x7202           # what it had loaded; 0 from now
        if m == 0x6200 and st in ("asleep", "found", "failed"):
            self.state, self.header = "found", []
            self.dwell = 11
            self.heard += 1
            return 0x7202
        if m == 0x6102 and st == "found":
            self.state = "header"
            self.held = True
            return 0x7202
        if st == "header":
            left = 0x60 - len(self.header)
            self.header.append(m)
            if len(self.header) == 0x60:
                self.state = "header_done"
            return (left << 8) | 2
        if m == 0x6200 and st == "header_done":
            self.state = "again"
            return 0x0002
        if m == 0x6202 and st == "again":
            self.state = "palette"
            return 0x7202
        if (m & 0xFF00) == 0x6300 and st == "palette":
            self.palette = m & 0xFF
            self.state = "handshake"
            return 0x7300 | self.client_data
        if (m & 0xFF00) == 0x6400 and st == "handshake":
            self.handshake = m & 0xFF
            self.state = "length"
            return 0x7300
        if st == "length":
            self.length = m * 4 + 0x190
            self.words = self.length // 4
            self.i = 0xC0 // 4
            self.low = None
            self.seed = (self.palette | (self.client_data << 8)
                         | 0xFFFF0000)
            self.crc_c = 0xFFF8
            self.image = bytearray(0xC0)
            for k, h in enumerate(self.header):
                self.image[2 * k] = h & 0xFF
                self.image[2 * k + 1] = h >> 8
            self.state = "data"
            self.not_yet = True
            return 0x7300 | self.random
        if st == "data":
            addr = (self.i << 2) & 0xFFFF
            if self.low is None:
                self.low = m
                return addr
            enc = (m << 16) | self.low
            self.low = None
            self.seed = (self.seed * self.SEED_MUL + 1) & 0xFFFFFFFF
            plain = (enc ^ self.DATA_XOR ^ self.seed
                     ^ ((0xFE000000 - (self.i << 2)) & 0xFFFFFFFF))
            self.image += plain.to_bytes(4, "little")
            self.crc_c = self.crc(self.crc_c, plain)
            self.i += 1
            if self.i >= self.words:
                self.state = "end"
            return (addr + 2) & 0xFFFF
        if st == "end" and m == 0x0065:
            if self.not_yet:
                self.not_yet = False
                return 0x0074
            self.state = "final"
            return 0x0075
        if st == "final" and m == 0x0066:
            self.state = "crc"
            return 0x0075
        if st == "crc":
            final = (self.handshake | (self.random << 8)) | 0xFFFF0000
            want = self.crc(self.crc_c & 0xFFFF, final) & 0xFFFF
            if m == want:
                self.done += 1
                self.received = bytes(self.image)
                self.state = "done"
            else:
                self.state = "failed"
            return want
        return 0x0000


def _tile_memory(core):
    """The background's tile memory, charblock 0, a tile at a time: the
    1024 a text background can name, thirty-two bytes each."""
    block = read_bytes(core, 0x06000000, 1024 * 32)
    return [bytes(block[t * 32:t * 32 + 32]) for t in range(1024)]


def _send_to_fake(rom, master_rom, image):
    """One console (master_rom) on the master's end, the stand-in BIOS on
    the other; SELECT on the LINK CABLE screen; the image it takes in --
    and the tiles the sending trod on: every tile that had something in it
    before (the game's, the title's, the ones made at boot) and has
    something else after. The bar draws its two tiles as it goes, and they
    have to be two that were nobody's."""
    import run_rom
    cores, cable, both, tap = _pair(master_rom, slave_rom=rom)
    bios = FakeMultibootBios()
    cable.fake_bios = bios
    both(30)
    before = _tile_memory(cores[0])
    both(970)
    if master_rom != rom:                  # the cartridge: to the lobby first
        tap("START", who=0)
        tap("DOWN", who=0)
        tap("START", who=0)
        both(30)
    rows = " ".join(run_rom.tilemap_text(cores[0], r) for r in range(8, 16))
    # It sends by itself once the cable has found the stand-in (link_peer),
    # so by now it may be sending, or done; SELECT is the way to send again.
    offered = ("THE OTHER GBA IS EMPTY" in rows or "SENDING THE GAME" in rows
               or bios.done > 0)
    bar = "SENDING THE GAME" in rows or bios.done > 0
    if not bios.done and "SENDING THE GAME" not in rows:
        tap("SELECT", who=0)
    for _ in range(120):
        both(30)
        rows = " ".join(run_rom.tilemap_text(cores[0], r) for r in range(8, 16))
        bar = bar or "SENDING THE GAME" in rows
        if bios.done:
            break
    both(120)
    after = _tile_memory(cores[0])
    trodden = [t for t in range(1024)
               if any(before[t]) and before[t] != after[t]]
    drawn = [t for t in range(1024) if not any(before[t]) and any(after[t])]
    # A picture taken before the tiles were there would find nothing trodden
    # whatever was: it has to be of a tile memory with its art in it.
    if sum(1 for t in before if any(t)) < 500:
        trodden = ["la foto de antes del envio no tiene los tiles"]
    return offered, bar, bios, _screen_of(cores[0]), trodden, drawn


def mb_send_check(rom):
    """SINGLE-PAK: THE SENDING, WHOLE, AND THE COPY SENDING ITSELF.

    SELECT on the LINK CABLE screen sends the game to a console with no
    cartridge (link_multiboot_send), all of it in software. Here the other
    end is a stand-in for that console's BIOS that decrypts what it is sent
    the way GBATEK's pseudo-code encrypts it: the image it takes in has to
    be the image, byte for byte, and the two checksums have to agree. Twice:
    from the cartridge, sending the image it carries, and from a console
    running that image, sending itself out of EWRAM. After a send the
    console is back in its lobby.
    """
    mb = os.path.splitext(rom)[0] + "_mb.mb"
    if not os.path.exists(mb):
        skip(f"no encuentro {mb}")
        return 0
    image = open(mb, "rb").read()
    import run_rom
    send_bar = run_rom.check_layout(rom).get("send_bar")
    failures = []
    for name, master in (("el cartucho", rom), ("la copia", mb)):
        # The console on the other end is only there to hold the cable in
        # multiplayer mode (SD high); its answers are the stand-in's.
        offered, bar, bios, where, trodden, drawn = _send_to_fake(mb, master, image)
        got = bios.received
        if not offered:
            failures.append(f"{name}: la pantalla del cable no ofrece enviar")
        elif not bar:
            failures.append(f"{name}: SELECT no pone la pantalla de envio")
        elif not bios.done:
            failures.append(f"{name}: el envio no termina (se quedo en "
                            f"{bios.state})")
        elif bios.palette != 0x93:
            failures.append(f"{name}: paleta {bios.palette:#x}, no 0x93")
        elif got is None or len(got) != len(image) or any(
                got[k] != image[k] for k in range(len(image))
                if k not in (0xC4, 0xC5)):
            diff = next((k for k in range(min(len(got or b""), len(image)))
                         if got[k] != image[k] and k not in (0xC4, 0xC5)),
                        None)
            failures.append(f"{name}: lo recibido no es la imagen "
                            f"({len(got or b'')} de {len(image)} bytes, "
                            f"primera diferencia en {diff})")
        elif where != "cable":
            failures.append(f"{name}: tras enviar no vuelve al lobby ({where})")
        elif trodden:
            # THE BAR'S TWO TILES ARE ITS OWN (T_SEND_BAR). They were 1008
            # and 1009 by hand, which the lit box of OPTIONS and the pause's
            # brick had grown into: after a send the console drew both as
            # the bar's until it was switched off.
            failures.append(f"{name}: el envio pisa tiles que ya eran de otro: "
                            f"{trodden}")
        elif send_bar is not None and not {send_bar, send_bar + 1} <= set(drawn):
            failures.append(f"{name}: la barra del envio no dibuja sus dos "
                            f"tiles, {send_bar} y {send_bar + 1} (nuevos tras "
                            f"enviar: {drawn})")
        else:
            print(f"  {name}: {len(image)} bytes, cifrados, recibidos y "
                  f"descifrados identicos, la suma de comprobacion coincide "
                  f"y vuelve al lobby; la barra dibuja en sus dos tiles "
                  f"({send_bar} y {send_bar + 1}) y no pisa ninguno ajeno")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el envio de Single-Pak llega entero, del cartucho y de la copia.")
    return peer_check(rom)


class DeadEnd:
    """The other end of the cable switched off: every slot it would fill
    reads $FFFF."""
    state = "off"
    done = 0

    def answer(self, m, baud=3):
        return ABSENT


def peer_check(rom):
    """THE LINK CABLE SCREEN SAYS WHAT IS ON THE OTHER END.

    A console with no cartridge (the stand-in BIOS, which the master finds
    by asking it $6200 now and then, link_probe): the screen offers to send
    it the game. Another of ours, still in its menu: wait for its player,
    and no offer. Nothing (the other end switched off): switch it on. And
    while the game goes, the bar is a growing I piece with the figure under
    it, not a row of dashes.
    """
    import run_rom
    failures = []

    def rows(core):
        return " ".join(run_rom.tilemap_text(core, r) for r in range(8, 16))

    cases = (("una GBA sin cartucho", FakeMultibootBios(),
              "THE OTHER GBA IS EMPTY", True),
             ("una GBA sin cartucho, sorda doce frames tras cada palabra del lobby",
              FakeMultibootBios(deaf=12), "THE OTHER GBA IS EMPTY", True),
             ("una GBA sin cartucho, sorda veinte", FakeMultibootBios(deaf=20),
              "THE OTHER GBA IS EMPTY", True),
             ("otra GBA con el juego, en su menu", None,
              "WAITING FOR OTHER PLAYER", False),
             ("la otra GBA apagada", DeadEnd(),
              "SWITCH THE OTHER GBA ON", False))
    for name, other, want, sends in cases:
        cores, cable, both, tap = _pair(rom)
        cable.fake_bios = other
        both(300)
        tap("START", who=0)
        tap("DOWN", who=0)
        tap("START", who=0)
        # A CONSOLE WITH NO CARTRIDGE GETS THE GAME WITHOUT ANYBODY ASKING:
        # the screen says what it found, and then the sending starts by
        # itself (main.c, AUTO_SEND_FRAMES). Nobody presses SELECT here.
        saw = sending = False
        last = ""
        for _ in range(600):
            both(1)
            last = rows(cores[0])
            saw = saw or want in last
            sending = sending or "SENDING THE GAME" in last
            if sending:
                break
        if not saw:
            failures.append(f"{name}: la pantalla no dice {want!r} "
                            f"({' '.join(last.split())!r})")
        elif sending != sends:
            failures.append(f"{name}: {'no ' if sends else ''}empieza a "
                            f"enviar el juego solo")
        else:
            print(f"  {name}: '{want}'"
                  f"{' y empieza a enviar sola' if sends else ''}")
        if sends and sending:
            pct = cells = False
            for _ in range(200):
                both(5)
                row = run_rom.tilemap_text(cores[0], 14)
                # "74%", between the frame's own tiles: how far, not how many
                # kilobytes (the font's '%' is the port's; plant_glyphs).
                pcts = [w[:-1] for w in row.split() if w.endswith("%")]
                if pcts and pcts[0].isdigit() and 0 < int(pcts[0]) < 100:
                    pct = True
                    cells = "-" not in run_rom.tilemap_text(cores[0], 12)
                    break
            if not pct:
                failures.append("durante el envio no sale lo enviado")
            elif not cells:
                failures.append("la barra de envio sigue siendo de guiones")
            else:
                print(f"  enviando: la pieza I crece, "
                      f"'{run_rom.tilemap_text(cores[0], 14).strip()}'")

    # WHAT THE PROBE PUTS ON THE WIRE. The stand-in above was found by a
    # probe no real console ever answered, so "it finds the stand-in" is
    # not the whole of what there is to hold on to: the probe is the send
    # that works, done in the silence it works in, and that is a pattern on
    # the cable (gba/link.c, link_probe; Cable.wire). With somebody in
    # multiplayer mode who never answers: twenty-four frames of the lobby's
    # words, then twenty-four with none of them and an attempt on the
    # eighth, sixteenth and twenty-fourth — four times $6200 at 115200 —
    # and round again.
    def on_the_wire(other, frames, until=None):
        cores, cable, both, tap = _pair(rom)
        cable.fake_bios = other
        both(300)
        tap("START", who=0)
        tap("DOWN", who=0)
        cable.wire = []
        start = cable.frames
        tap("START", who=0)
        screens = []
        for _ in range(frames):
            both(1)
            screens.append(rows(cores[0]))
            if until and until in screens[-1]:
                break
        per = {}
        for f, baud, m, got, sd in cable.wire:
            per.setdefault(f - start, []).append((baud, m, got, sd))
        return per, screens, cable.frames - start - len(screens)

    FAST = FakeMultibootBios.BAUD_115200
    per, _, _ = on_the_wire(DeadEnd(), 340)
    text = ""
    for f in range(max(per) + 1):
        sent = per.get(f, [])
        fast = [t for t in sent if t[0] == FAST]
        if fast and len(fast) != len(sent):
            text += "?"                 # both speeds in one frame
        elif fast:
            text += "P" if [t[1] for t in fast] == [0x6200] * 4 else "p"
        else:
            text += "L" if sent else "."
    cycle = "L" * 24 + ("." * 7 + "P") * 3
    at = text.find(cycle)
    turns = (len(text) - at) // len(cycle) if at >= 0 else 0
    if at < 0 or turns < 4 or text[at:at + turns * len(cycle)] != cycle * turns:
        failures.append("la sonda no hace en el cable lo que dice (24 frames "
                        "de lobby, 24 callada con cuatro $6200 a 115200 en el "
                        f"8, el 16 y el 24): {text}")
    else:
        print(f"  la sonda en el cable, {turns} vueltas: 24 frames de lobby y "
              "24 de silencio con tres intentos de cuatro $6200")

    # ...WITH NOBODY IN MULTIPLAYER MODE (SD low: a console in its BIOS for
    # two of its three modes, a cable half in), it asks nothing: a start
    # there is a transfer with the error bit and no one to hear it.
    class NotListening(DeadEnd):
        def sd(self):
            return False

    per, _, _ = on_the_wire(NotListening(), 200)
    asked = sum(1 for sent in per.values() for t in sent if t[0] == FAST)
    if asked:
        failures.append(f"con SD baja la sonda pregunta igualmente: {asked} "
                        "transferencias a 115200 en 200 frames")
    else:
        print("  con SD baja la sonda no pregunta")

    # ...AND ONCE A CONSOLE HAS ANSWERED, NOT A WORD OF THE LOBBY'S until
    # the game is sent: that console restarts its listening at every one
    # (FakeMultibootBios.restarts; a real one, INFERRED, the same or worse).
    # One $6200 every eighth frame, inside the eleven each keeps it there.
    bios = FakeMultibootBios()
    per, screens, lead = on_the_wire(bios, 600, until="SENDING THE GAME")
    found = min((f for f, sent in per.items() if any(t[2] == 0x7202 for t in sent)),
                default=None)
    sending = lead + len(screens) - 1
    if found is None or "SENDING THE GAME" not in screens[-1]:
        failures.append("la sonda no encuentra a la GBA sin cartucho (para "
                        "mirar que hace despues en el cable)")
    else:
        last = min(sending, found + 44)
        hold = {f: per.get(f, []) for f in range(found + 1, last)}
        slow = sum(1 for sent in hold.values() for t in sent if t[0] != FAST)
        beats = sorted(f - found for f, sent in hold.items() if sent)
        want = [n for n in range(8, last - found, 8)]
        single = all(len(sent) == 1 and sent[0][1] == 0x6200 and sent[0][2] == 0x7202
                     for sent in hold.values() if sent)
        if slow or beats != want or not single or len(want) < 4:
            failures.append("encontrada la GBA sin cartucho, el cable no lleva "
                            "solo un $6200 cada ocho frames hasta el envio: "
                            f"{slow} palabras del lobby, intentos en {beats}")
        else:
            print(f"  encontrada, ni una palabra del lobby hasta el envio: un "
                  f"$6200 cada ocho frames ({len(want)} antes de enviar)")

    # ...AND ONE THAT GOES ASTRAY DOES NOT TAKE THE SCREEN WITH IT
    # (LINK_PROBE_KEEP): two attempts running with their answers lost, and
    # the screen says the same thing every frame until the game goes.
    bios = FakeMultibootBios()
    bios.lose = (4, 20)
    per, screens, _ = on_the_wire(bios, 600, until="SENDING THE GAME")
    seen = [i for i, text in enumerate(screens) if "THE OTHER GBA IS EMPTY" in text]
    lost = sum(1 for sent in per.values() for t in sent
               if t[0] == FAST and t[1] == 0x6200 and t[2] == ABSENT and t[3])
    if not seen or "SENDING THE GAME" not in screens[-1]:
        failures.append("con dos intentos de la sonda perdidos por el camino, "
                        "el juego no llega a enviarse solo")
    elif lost < 8:
        failures.append(f"la prueba no llego a perder dos intentos ({lost} "
                        "respuestas perdidas)")
    elif any("THE OTHER GBA IS EMPTY" not in text
             for text in screens[seen[0]:-1]):
        failures.append("un intento de la sonda perdido y la pantalla deja de "
                        "decir que hay una GBA vacia")
    else:
        print(f"  dos intentos perdidos por el camino ({lost} respuestas) y la "
              "pantalla no se inmuta")

    # B ON THE SENDING SCREEN STOPS IT, AND IT STAYS STOPPED: the screen
    # goes on saying what is there, with SELECT to send, until that console
    # goes away. (Coming back, the cable is started over and the console
    # has to be found again; that used to count as gone, and the send began
    # again by itself a second later.)
    cores, cable, both, tap = _pair(rom)
    bios = FakeMultibootBios()
    cable.fake_bios = bios
    both(300)
    tap("START", who=0)
    tap("DOWN", who=0)
    tap("START", who=0)
    began = False
    for _ in range(300):
        both(1)
        if "SENDING THE GAME" in rows(cores[0]):
            began = True
            break
    both(3, [[KEYS["B"]], []])
    again = False
    seen = ""
    for _ in range(360):
        both(1, [[], []])
        seen = rows(cores[0])
        again = again or "SENDING THE GAME" in seen
    if not began:
        failures.append("para parar el envio con B: no llega a empezar solo")
    elif again or bios.done:
        failures.append("B para el envio y al momento vuelve a empezar solo")
    elif "THE OTHER GBA IS EMPTY" not in seen:
        failures.append(f"parado el envio con B, la pantalla no sigue diciendo "
                        f"que hay una GBA vacia: {' '.join(seen.split())!r}")
    else:
        tap("SELECT", who=0)
        for _ in range(120):
            both(30)
            if bios.done:
                break
        if not bios.done:
            failures.append("parado con B, SELECT no envia el juego")
        else:
            print("  B para el envio y se queda parado; SELECT lo envia")

    # LEAVING THE CABLE'S SCREEN LEAVES THE PORT AT THE GAME'S SPEED. While
    # it looks for a console in its BIOS the master's port is at 115200 for
    # stretches of a third of a second; B at any moment, and from its menus
    # it has to be heard by a lobby on the other end (its beacon, at 38400),
    # or that end asks for a cable that is plugged in.
    cores, cable, both, tap = _pair(rom)
    both(300)
    tap("START", who=0)
    tap("DOWN", who=0)
    tap("START", who=1)
    tap("DOWN", who=1)                      # both on 2 PLAYER, on GAME SELECT
    for wait in (2, 12, 22, 27, 33, 38, 44):
        tap("START", who=0)                 # into the lobby
        both(wait)
        both(3, [[KEYS["B"]], []])          # ...and out, whatever it was doing
        both(20, [[], []])
        tap("START", who=1)                 # the slave into its own
        both(90)
        text = rows(cores[1])
        both(3, [[], [KEYS["B"]]]); both(12, [[], []])     # ...and back out
        if "WAITING FOR OTHER PLAYER" not in text:
            failures.append(f"la maestra sale del cable a los {wait} frames y el "
                            f"otro extremo ya no la oye: {' '.join(text.split())!r}")
            break
    else:
        print("  saliendo del cable en cualquier momento de la sonda, el otro "
              "extremo sigue oyendo a la maestra en sus menus")

    # THE SLAVE'S END OF THE CABLE, ITS PARTNER IN ITS MENUS. With no
    # transfers that end could not tell this from no cable at all; the
    # console in its menus now says it is there (link_beacon), and the
    # screen waits for its player instead of asking for a cable.
    cores, cable, both, tap = _pair(rom)
    both(300)
    tap("START", who=1)
    tap("DOWN", who=1)
    tap("START", who=1)
    both(120)
    text = rows(cores[1])
    if "WAITING FOR OTHER PLAYER" not in text:
        failures.append(f"el extremo esclavo, con la otra en su menu, no "
                        f"espera: {' '.join(text.split())!r}")
    else:
        print("  extremo esclavo, la otra en el titulo: 'WAITING FOR OTHER "
              "PLAYER'")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la pantalla del cable dice que hay al otro lado y que hacer.")
    return 0


def flash_wait_check(rom):
    """THE CARTRIDGE'S FLASH WAITS FOR THE MATCH TO END.

    Two consoles on boards that keep their save in the game's own flash
    chip (gba/cartsave.c; the chip is tools/romcheck/flashchip.py's). A
    save to the chip is a few milliseconds with the interrupts off, and a
    match over the cable is its serial interrupt: so what is saved while
    one runs — the console's figures, at a pause — goes to the save RAM at
    once and to the chip only when the match is over. Here: paused in a
    match, both have something waiting and neither has touched its chip;
    played on and the cable lost for good, START to the title, and it is
    in the chip on the next frame — and still there after the power has
    been off, with no battery.
    """
    from romcheck.flashchip import Cart, WIRE_D0D1
    linked = symbol(rom, "g_linked")[0]
    image_at = symbol(rom, "g_cartsave_image")[0]
    if linked is None or image_at is None or symbol(rom, "g_session")[0] is None:
        skip("el ELF no tiene g_linked o g_cartsave_image")
        return 0
    mgba.log.silence()
    carts = [Cart(rom, chip=dict(wiring=WIRE_D0D1), seed=s) for s in (1, 2)]
    cores = [c.core for c in carts]
    cable = Cable(*cores)
    ends = [CableEnd(cable, True), CableEnd(cable, False)]
    for core, end in zip(cores, ends):
        core.attach_sio(end, lib.SIO_MULTI)
    _KEEP.append((carts, cable, ends))

    def both(frames, keys=None):
        for _ in range(frames):
            for i, cart in enumerate(carts):
                cart.core.set_keys(*(keys[i] if keys else []))
                cart.frame()
            cable.settle()

    def tap(key, who=None):
        both(4, [[KEYS[key]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    def state():
        return [(c.report(), c.chip.programmed, len(c.calls)) for c in carts]

    failures = []
    both(70)                                # the first power-on: a journal each
    tap("START"); tap("DOWN"); tap("START")
    both(60)
    tap("START", who=0)
    both(90)
    if [c.memory.u8[linked[0]] for c in cores] != [1, 1]:
        print("FALLA: las dos consolas no llegan a la partida por cable")
        return 1
    began = state()
    tap("START", who=0)                     # paused: the figures go to the save
    both(20)
    paused = state()
    for i, ((r, words, calls), (_, words0, calls0)) in enumerate(zip(paused, began)):
        who = "la maestra" if i == 0 else "la esclava"
        if not r["dirty"] or not r["waiting"] or r["waits"] != 1:
            failures.append(f"en pausa por cable {who} no tiene nada esperando "
                            f"(pendiente {r['dirty']}, esperando {r['waiting']})")
        if words != words0 or calls != calls0:
            failures.append(f"en pausa por cable {who} escribe en su flash: "
                            f"{words - words0} palabras")
    stat = [bytes(c.memory.u8[image_at[0] + 0x700 + i] for i in range(4)) for c in cores]
    if stat != [b"STAT", b"STAT"]:
        failures.append(f"en pausa por cable no se guardan las cifras: {stat}")
    tap("START", who=0)
    both(20)
    cable.plugged = False
    both(LINK_GIVEUP_FRAMES_PY + 10)
    still = state()
    if any(words != words0 for (_, words, _), (_, words0, _) in zip(still, began)):
        failures.append("con el cable perdido y la partida sin acabar se escribe en la flash")
    tap("START")
    both(30)
    after = state()
    if any(c.memory.u8[linked[0]] for c in cores):
        failures.append("START no saca de la partida con el cable perdido")
    for i, ((r, words, _), (_, words0, _)) in enumerate(zip(after, began)):
        who = "la maestra" if i == 0 else "la esclava"
        if r["dirty"] or r["waiting"] or words == words0:
            failures.append(f"acabada la partida {who} no guarda lo que esperaba "
                            f"(pendiente {r['dirty']}, {words - words0} palabras)")
    for c in carts:
        failures.extend(c.faults + c.chip.faults)
    # ...and it is there with the power gone, on the master's board.
    cart = carts[0]
    saved = bytes(cart.core.memory.u8[image_at[0] + i] for i in range(0x7FE0))
    cart.power_cycle()
    cart.run(90)
    back = bytes(cart.core.memory.u8[image_at[0] + i] for i in range(0x7FE0))
    if cart.report()["boot"] != 3 or back != saved or back[0x700:0x704] != b"STAT":
        failures.append("lo guardado al acabar la partida no vuelve tras apagar sin pila")
    failures.extend(cart.faults + cart.chip.faults)
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("  en pausa en una partida por cable: lo guardado espera en las dos, "
          "y ninguna toca su flash hasta que la partida acaba")
    print("OK: la flash del cartucho espera a que acabe la partida por cable.")
    return 0


if __name__ == "__main__":
    code = strict_verdict(main())
    # TWO CORES CANNOT BE LET GO OF NORMALLY. mgba/gba.py's GBA.__del__ frees
    # the core, and with two of them plus a Python GBASIODriver still attached
    # to both, the interpreter's shutdown collection frees a core out from
    # under the other's cable and segfaults — *after* every check above has
    # already printed its result. It is a teardown bug in the bindings and
    # nothing to do with the ROM, but it still handed `make gba-check` a
    # non-zero status and made the whole gate meaningless.
    #
    # So the process ends before finalization runs. os._exit skips __del__ and
    # atexit both, which is exactly what is wanted here: everything this script
    # produces is on stdout, and the flush below is the only cleanup it owes.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
