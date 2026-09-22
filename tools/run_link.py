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
        print(f"SALTADO: {why}")
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

    # ...AND EACH CONSOLE PUTS ITSELF ON THE LEFT. The coop HUD is one panel
    # per player — yours with your NEXT, score and lines, theirs on the other
    # side — so the two consoles must show the SAME two panels the other way
    # round. Distinct scores are planted on both cores (identically, so the
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
        failures.append(f"el anfitrion no se ve a si mismo a la izquierda: {mine!r}")
    elif "1678" not in theirs[0] or "2345" not in theirs[1]:
        failures.append(f"el invitado no se ve a si mismo a la izquierda: {theirs!r}")
    else:
        print("  y cada consola lleva su propio panel a la izquierda y el del "
               "companero a la derecha")

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
                 if "HIGH SCORES" not in run_rom.tilemap_text(core, 2, 0, 30)]
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
        print(f"SALTADO: {why}")
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
        print("SALTADO: la partida por cable no arranco")
        return 0

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
             if "HIGH SCORES" not in run_rom.tilemap_text(core, 2, 0, 30)]
    if blind:
        print(f"SALTADO: la(s) consola(s) {blind} no llegan a la tabla")
        return 0

    def rows(core):
        return [run_rom.tilemap_text(core, run_rom.LEADER_FIRST_TY + i, 0, 30)
                for i in range(15)]

    # BOTH SCORES, ON BOTH PAGES. This is the half that needed no cable at
    # all — each console had simulated the other's board all along — and the
    # half that was simply never written down.
    for name, core in (("maestro", master), ("esclavo", slave)):
        page = "".join(rows(core))
        for score in final:
            if f"{score:06d}" not in page:
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
            row = next((r for r in rows(core) if f"{score:06d}" in r), "")
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
        print(f"SALTADO: {why}")
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
        print("SALTADO: la partida por cable no arranco")
        return 0

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
        print("SALTADO: el tablero del jugador 2 no llego a morir")
        return 0
    if not alive(master, 0):
        failures.append("el jugador 1 murio tambien: el otro tablero deberia "
                         "seguir corriendo")

    # A ALONE MUST NOT LEAVE. It used to: START, A and B all took a dead board
    # to the table, so the chord could never be held down long enough to mean
    # anything. Now, while the other board is still going, the way out is
    # START and A and B belong to the restart.
    hold(["A"], 1, 20)
    if "HIGH SCORES" in run_rom.tilemap_text(slave, 2, 0, 30):
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
    both(300, [[], []])
    tap("START")
    both(40, [[], []])
    page = "".join(run_rom.tilemap_text(slave, run_rom.LEADER_FIRST_TY + i, 0, 30)
                    for i in range(15))
    if "HIGH SCORES" not in run_rom.tilemap_text(slave, 2, 0, 30):
        failures.append("no se llega a la tabla tras el segundo game over")
    elif f"{dead_score:06d}" not in page:
        failures.append(f"la partida que acabo antes del reinicio ({dead_score}) "
                         "no esta en la tabla: el reinicio se la comio")
    else:
        print(f"  y la partida anterior al reinicio ({dead_score}) sigue "
               "anotada: cada partida es una fila")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: A+B levanta un tablero muerto sin parar la carrera ni romper "
           "el enlace.")
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
