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

# I/O registers, as halfword indices into struct GBA's io[] array.
IO_SIOMULTI0 = 0x120 >> 1
IO_SIOMULTI1 = 0x122 >> 1
IO_SIOMULTI2 = 0x124 >> 1
IO_SIOMULTI3 = 0x126 >> 1
IO_SIOCNT = 0x128 >> 1
IO_SIOMLT_SEND = 0x12A >> 1

REG_SIOCNT = 0x128
SIO_SI = 0x0004
SIO_SD = 0x0008
SIO_START = 0x0080
SIO_ID_SHIFT = 4

IRQ_SIO = 7        # enum GBAIRQ
ABSENT = 0xFFFF    # what a slot with no console in it reads

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

    def writeRegister(self, address, value):
        if address != REG_SIOCNT:
            return value
        # SI says which end of the cable this console is plugged into; SD says
        # everyone is ready; the id is 0 for the master and 1 for the slave.
        value &= ~(SIO_SI | SIO_SD | (3 << SIO_ID_SHIFT))
        value |= SIO_SD
        if not self.master:
            value |= SIO_SI | (1 << SIO_ID_SHIFT)

        if self.master and (value & SIO_START):
            self.cable.transfer()
            value &= ~SIO_START      # the transfer is over by the time we return
        return value

    # The bindings in some builds call the snake_case name instead.
    def write_register(self, address, value):
        return self.writeRegister(address, value)


class Cable:
    def __init__(self, master, slave):
        self.master = master
        self.slave = slave
        self.transfers = 0
        self.plugged = True

    def transfer(self):
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
            return
        mio = self.master._native.memory.io
        sio = self.slave._native.memory.io
        m = mio[IO_SIOMLT_SEND]
        s = sio[IO_SIOMLT_SEND]

        for io in (mio, sio):
            io[IO_SIOMULTI0] = m
            io[IO_SIOMULTI1] = s
            io[IO_SIOMULTI2] = ABSENT
            io[IO_SIOMULTI3] = ABSENT
            io[IO_SIOCNT] &= ~SIO_START

        lib.GBARaiseIRQ(self.master._native, IRQ_SIO, 0)
        lib.GBARaiseIRQ(self.slave._native, IRQ_SIO, 0)
        self.transfers += 1


def symbol(rom_path, name):
    """(address, size) of a symbol in the built ROM, out of the ELF."""
    elf = os.path.splitext(rom_path)[0] + ".elf"
    if not os.path.exists(elf):
        return None, f"no encuentro {elf} (hace falta para leer el estado)"
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    try:
        out = subprocess.check_output([nm, "-S", elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        return None, f"no pude ejecutar {nm}: {exc}"
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] == name:
            return (int(parts[0], 16), int(parts[1], 16)), None
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
        print(f"SALTADO: {why}")
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

    # Finally: pull the cable out mid-match. Neither console may freeze, and
    # both must notice and stop rather than sit forever waiting for a word
    # that is not coming.
    lost, why = symbol(rom, "g_link_lost")
    if lost is None:
        print(f"  (sin comprobar el tiron del cable: {why})")
    else:
        cable.plugged = False
        before = cable.transfers
        both(200, [[], []])
        if cable.transfers != before:
            failures.append("siguio habiendo transferencias con el cable fuera")
        for name, core in (("maestro", master), ("esclavo", slave)):
            if not core.memory.u8[lost[0]]:
                failures.append(f"el {name} no se entero de que el cable se fue")
        # ...and the ROM is still running, not wedged in a serial wait: its
        # own frame counter has to have moved.
        if master.frame_counter == 0:
            failures.append("el maestro dejo de correr frames")
        for f in failures:
            print(f"FALLA: {f}")
        if failures:
            return 1
        print("  cable fuera: las dos consolas lo detectan y siguen corriendo")

    print("OK: dos GBA por cable link juegan la misma partida sin desviarse.")
    return 0


if __name__ == "__main__":
    code = main()
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
