#!/usr/bin/env python3
"""
run_wireless.py — two ROMs, two Wireless Adapters, one stretch of air.

gba/wireless.c drives the GBA Wireless Adapter (AGB-015) from what GBATEK
and gba-link-connection (afska; docs/wireless_adapter.md, LinkRawWireless)
say about it. Nobody here has two adapters to try it on, so this is that
documentation written as a model, and the game checked against it:

  - FakeAdapter is one adapter on one console's port. It answers the
    NINTENDO login (each answer the GBA's halfword and the inverse of the
    one before), the command framing (9966LLCCh, LL parameters each
    answered 80000000h, then the request that brings back 9966RRAAh and RR
    words), and after every command transfer the "ready" handshake on SO
    and SI.
  - Air is what is between two of them: a room hosted with its broadcast,
    found by a search, joined, closed; and data, the host's delivered to
    the client on SendData, the client's sent only when the host sends —
    the adapter's own rule. One packet waits on each side; a newer one
    replaces it.

What it does NOT model is the radio: no latency beyond a frame, nothing
lost. And it is a reading of the documentation, so a check passing here is
INFERRED until two real adapters agree.

The checks: a plain GBA (no adapter) does not think it has one; two with
adapters find each other with nobody choosing who hosts, play 2 PLAYER
with the two games identical byte for byte, and play COOPERATIVE the same.

Requires: pip install pygba  (plus the mGBA shared library)
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romcheck.harness  # noqa: E402,F401 - skips the splash; see there
from run_link import (KEYS, SCREEN_W, SCREEN_H, symbol, read_bytes,  # noqa: E402
                      _KEEP)

import mgba.core  # noqa: E402
import mgba.gba  # noqa: E402
import mgba.image  # noqa: E402
import mgba.log  # noqa: E402
from mgba import lib  # noqa: E402

REG_SIOCNT = 0x128
IO_SIODATA_LO = 0x120 >> 1
IO_SIODATA_HI = 0x122 >> 1
SIO_SI = 0x0004
SIO_SO = 0x0008
SIO_START = 0x0080
SIO_IRQ = 0x4000
IRQ_SIO = 7

DATA_REQUEST = 0x80000000
LOGIN_LAST = 0x8001


class Air:
    """The rooms and the packets between adapters."""

    def __init__(self):
        self.adapters = []
        self.delivered = 0          # data packets that reached the other side

    def rooms(self, asker):
        return [a for a in self.adapters
                if a is not asker and a.hosting and a.room_open]


class FakeAdapter(mgba.gba.GBASIODriver):
    def __init__(self, air, core, ident):
        super(FakeAdapter, self).__init__()
        self.air = air
        self.core = core
        self.ident = ident
        air.adapters.append(self)
        self.state = "login"
        self.prev_lo = 0
        self.si = False             # the adapter's SI to the GBA
        self.acking = False         # a command transfer awaits its handshake
        self.logins = 0
        self.commands = []          # every command id, in order
        self.reset_link()

    def reset_link(self):
        self.broadcast = [0] * 6
        self.hosting = False
        self.room_open = False
        self.clients = []           # adapters connected to this host
        self.host = None            # the host this one is (being) joined to
        self.connected = False
        self.inbox = None           # (bytes, words) waiting to be received
        self.outbox = None          # a client's packet, waiting for the host

    def drop(self):
        """Bye, or a new login: whoever was joined to this one is not."""
        for c in self.clients:
            c.host = None
            c.connected = False
        if self.host is not None and self in self.host.clients:
            self.host.clients.remove(self)
        self.reset_link()

    # ------------------------------------------------------------------ #
    # The commands
    # ------------------------------------------------------------------ #

    def run(self, cmd, params):
        self.commands.append(cmd)
        if cmd in (0x10, 0x17):                      # Hello, Setup
            return []
        if cmd == 0x16:                              # Broadcast
            self.broadcast = (list(params) + [0] * 6)[:6]
            return []
        if cmd == 0x19:                              # StartHost
            self.hosting = True
            self.room_open = True
            self.clients = []
            return []
        if cmd in (0x1A, 0x1B):                      # PollConnections, EndHost
            if cmd == 0x1B:
                self.room_open = False
            return [c.ident | (i << 16) for i, c in enumerate(self.clients)]
        if cmd in (0x1C, 0x1D, 0x1E):                # BroadcastRead
            if cmd == 0x1C:
                return []
            out = []
            for h in self.air.rooms(self):
                slot = len(h.clients) if len(h.clients) < 1 else 0xFF
                out.append(h.ident | (slot << 16))
                out.extend(h.broadcast)
            return out
        if cmd == 0x1F:                              # Connect
            hid = params[0] & 0xFFFF if params else 0
            self.host = next((h for h in self.air.rooms(self)
                              if h.ident == hid), None)
            self.connected = False
            return []
        if cmd == 0x20:                              # IsConnectionComplete
            h = self.host
            if h is None or not h.hosting:
                return [0x01000000 | (1 << 16)]      # gone: no slot
            if self not in h.clients:
                if not h.room_open or len(h.clients) >= 1:
                    return [0x01000000 | (1 << 16)]
                h.clients.append(self)
                return [0x01000000]                  # still connecting
            return [self.ident]                      # slot 0
        if cmd == 0x21:                              # FinishConnection
            if self.host is None or self not in self.host.clients:
                return None
            self.connected = True
            return [self.ident]
        if cmd == 0x24:                              # SendData
            if not params:
                return []
            header, words = params[0], list(params[1:])
            if self.hosting:
                n = header & 0x7F
                for c in self.clients:
                    if c.connected:
                        c.inbox = (n, words)
                        self.air.delivered += 1
                # ...and what each client had waiting comes back with it.
                for c in self.clients:
                    if c.connected and c.outbox is not None:
                        self.inbox = c.outbox
                        c.outbox = None
                        self.air.delivered += 1
            elif self.connected:
                n = (header >> 8) & 0x1F
                self.outbox = (n, words)
            return []
        if cmd == 0x26:                              # ReceiveData
            if self.inbox is None:
                return []
            n, words = self.inbox
            self.inbox = None
            header = (n << 8) if self.hosting else n
            return [header] + words
        if cmd == 0x3D:                              # Bye
            self.drop()
            return []
        return None                                  # not modelled: error

    # ------------------------------------------------------------------ #
    # The wire
    # ------------------------------------------------------------------ #

    def exchange(self, out):
        """One 32-bit transfer: what the GBA sent, what it gets back."""
        lo, hi = out & 0xFFFF, out >> 16
        if self.state != "login" and hi == 0x9966 and \
                self.state in ("idle",):
            self.cmd = out & 0xFF
            self.want = (out >> 8) & 0xFF
            self.params = []
            self.state = "params" if self.want else "request"
            return DATA_REQUEST
        if self.state == "params":
            self.params.append(out)
            if len(self.params) == self.want:
                self.state = "request"
            return DATA_REQUEST
        if self.state == "request" and out == DATA_REQUEST:
            reply = self.run(self.cmd, self.params)
            if reply is None:
                self.reply = [0]
                self.state = "reply"
                return 0x996601EE
            self.reply = reply
            self.state = "reply" if reply else "idle"
            return 0x99660000 | (len(reply) << 8) | ((self.cmd + 0x80) & 0xFF)
        if self.state == "reply":
            w = self.reply.pop(0)
            if not self.reply:
                self.state = "idle"
            return w
        # The login, or one starting over (the reset before it is general
        # purpose, which the emulator does not hand a driver).
        if lo == 0x494E and self.state != "login":
            self.state = "login"
            self.prev_lo = 0
        if self.state == "login":
            ans = (lo << 16) | (~self.prev_lo & 0xFFFF)
            self.prev_lo = lo
            if lo == LOGIN_LAST:
                self.state = "idle"
                self.logins += 1
                self.drop()
            return ans
        return 0

    def _set_si(self, on):
        self.si = on
        io = self.core._native.memory.io
        sio = self.core._native.sio
        if on:
            io[REG_SIOCNT >> 1] |= SIO_SI
            sio.siocnt |= SIO_SI
        else:
            io[REG_SIOCNT >> 1] &= ~SIO_SI
            sio.siocnt &= ~SIO_SI

    def writeRegister(self, address, value):
        if address != REG_SIOCNT:
            return value
        if (value & 0x3000) != 0x1000:              # not 32-bit normal
            return value & ~SIO_SI
        so_high = bool(value & SIO_SO)
        if (value & SIO_START) and (value & 1):     # internal clock: go
            io = self.core._native.memory.io
            out = io[IO_SIODATA_LO] | (io[IO_SIODATA_HI] << 16)
            was_login = self.state == "login"
            ans = self.exchange(out) & 0xFFFFFFFF
            io[IO_SIODATA_LO] = ans & 0xFFFF
            io[IO_SIODATA_HI] = ans >> 16
            value &= ~SIO_START
            if value & SIO_IRQ:
                lib.GBARaiseIRQ(self.core._native, IRQ_SIO, 0)
            # After a command's transfer, "busy" until the GBA has done
            # the handshake: SI high, then low once SO has gone high.
            if not was_login or self.state != "login":
                self.acking = not was_login
            self.si = self.acking
        elif self.acking and so_high and self.si:
            self.si = False
            self.acking = False
        return (value | SIO_SI) if self.si else (value & ~SIO_SI)

    def write_register(self, address, value):
        return self.writeRegister(address, value)


# ---------------------------------------------------------------------- #

def _pair(rom, adapters=2):
    mgba.log.silence()
    air = Air()
    cores, screens, fakes = [], [], []
    for i in range(2):
        core = mgba.core.load_path(rom)
        screen = mgba.image.Image(SCREEN_W, SCREEN_H)
        core.set_video_buffer(screen)
        core.reset()
        cores.append(core)
        screens.append(screen)
        if i < adapters:
            fake = FakeAdapter(air, core, 0x1000 + 0x111 * (i + 1))
            core.attach_sio(fake, lib.SIO_NORMAL_32)
            fakes.append(fake)
    _KEEP.append((cores, screens, fakes, air))
    order = [0, 1]

    def both(frames, keys=None):
        for _ in range(frames):
            # Two consoles' frames do not line up; which goes first moves.
            random.shuffle(order)
            for i in order:
                cores[i].set_keys(*(keys[i] if keys else []))
                cores[i].run_frame()

    def tap(key, who=None):
        both(4, [[KEYS[key]] if who in (None, i) else [] for i in range(2)])
        both(10, [[], []])

    return cores, air, fakes, both, tap


def _rows(core):
    import run_rom
    return " ".join(run_rom.tilemap_text(core, r) for r in range(8, 16))


def plain_check(rom):
    """A GBA WITH NO ADAPTER does not take itself for one with: the LINK
    CABLE screen is the cable's."""
    cores, air, fakes, both, tap = _pair(rom, adapters=0)
    present, why = symbol(rom, "g_wl_present")
    if present is None:
        print(f"SALTADO: {why}")
        return 0
    both(60)
    if cores[0].memory.u8[present[0]]:
        print("FALLA: una GBA sin adaptador cree tener uno")
        return 1
    tap("START", who=0)
    tap("DOWN", who=0)
    tap("START", who=0)
    both(30)
    text = _rows(cores[0])
    if "LINK CABLE" not in text or "WIRELESS" in text:
        print(f"FALLA: sin adaptador la pantalla no es la del cable: "
              f"{' '.join(text.split())!r}")
        return 1
    print("  sin adaptador: ni lo cree tener, y la pantalla es LINK CABLE")
    return 0


def _play(rom, coop):
    random.seed(7 if coop else 3)
    cores, air, fakes, both, tap = _pair(rom)
    sym, why = symbol(rom, "g_session")
    present, _ = symbol(rom, "g_wl_present")
    if sym is None or present is None:
        print(f"SALTADO: {why}")
        return 0
    addr, size = sym
    game_size = size - 4
    failures = []
    name = "COOPERATIVE" if coop else "2 PLAYER"

    both(60)
    if not all(c.memory.u8[present[0]] for c in cores):
        print("FALLA: con adaptador, el juego no lo encuentra al arrancar")
        return 1
    if not all(f.logins >= 1 for f in fakes):
        failures.append("el adaptador no vio el saludo NINTENDO completo")

    # Both players to the same mode, at different moments.
    tap("START", who=0)
    tap("DOWN", who=0)
    if coop:
        tap("DOWN", who=0)
    tap("START", who=0)
    both(40)
    text = _rows(cores[0])
    if "WIRELESS" not in text or "LOOKING FOR OTHER GBA" not in text:
        failures.append(f"sola, la pantalla no dice que busca: "
                        f"{' '.join(text.split())!r}")
    tap("START", who=1)
    tap("DOWN", who=1)
    if coop:
        tap("DOWN", who=1)
    tap("START", who=1)

    # Nobody chose who hosts: one of them has to end up on LEVEL SETTINGS.
    host = None
    for _ in range(60):
        both(20)
        for i, core in enumerate(cores):
            if "HANDICAP" in _rows(core):
                host = i
        if host is not None:
            break
    if host is None:
        failures.append("las dos no se encuentran por el aire en 1200 frames: "
                        f"{[' '.join(_rows(c).split())[:60] for c in cores]}")
        for f in failures:
            print(f"FALLA ({name}): {f}")
        return 1
    guest = 1 - host
    if "YOU ARE PLAYER 2" not in _rows(cores[guest]):
        failures.append("la invitada no dice YOU ARE PLAYER 2")
    print(f"  {name}: se encuentran solas; aloja la "
          f"{'primera' if host == 0 else 'segunda'}")

    tap("START", who=host)
    started = None
    for f in range(300):
        both(1)
        m = read_bytes(cores[host], addr, game_size)
        s = read_bytes(cores[guest], addr, game_size)
        if m != bytes(game_size) and m == s:
            started = f
            break
    if started is None:
        failures.append("la partida no arranca igual en las dos")
    else:
        keys = ([["LEFT"], ["RIGHT"]], [["DOWN"], []], [["A"], ["B"]],
                [["RIGHT"], ["LEFT"]], [[], ["DOWN"]], [["DOWN"], ["DOWN"]])
        diverged = None
        frame = 0
        for p in keys:
            for _ in range(60):
                both(1, [[KEYS[k] for k in p[i]] for i in range(2)])
                frame += 1
                # Lockstep with a delay: the two are compared only when
                # they stand at the same frame of the match.
                a = read_bytes(cores[0], addr, size)
                b = read_bytes(cores[1], addr, size)
                if a[game_size + 1] == b[game_size + 1] and \
                        a[:game_size] != b[:game_size] and diverged is None:
                    diverged = frame
        fa = read_bytes(cores[host], addr, size)
        fb = read_bytes(cores[guest], addr, size)
        if diverged is not None:
            failures.append(f"divergen en el frame {diverged}")
        if fa[game_size + 2] or fb[game_size + 2]:
            failures.append("una se declara desincronizada")
        if fa[game_size + 1] == 0:
            failures.append("el contador del enlace no avanza")
        settled = sum(1 for i in range(20 * 12 * (1 if coop else 2))
                      if fa[i] not in (0, 15))
        if settled == 0:
            failures.append("no se asento ni una pieza")
        if not failures:
            print(f"  {frame} frames jugados, {air.delivered} paquetes por el "
                  f"aire, {settled} celdas asentadas, la misma partida en las "
                  f"dos byte a byte")
    for f in failures:
        print(f"FALLA ({name}): {f}")
    return 1 if failures else 0


def together_check(rom):
    """BOTH AT ONCE: two consoles that reach 2 PLAYER on the same frame
    both search and both host on the same schedule unless something tells
    them apart; the random wait (wl_random) has to."""
    random.seed(11)
    cores, air, fakes, both, tap = _pair(rom)
    both(60)
    tap("START")
    tap("DOWN")
    tap("START")
    for t in range(100):
        both(20)
        if any("HANDICAP" in _rows(c) for c in cores):
            print(f"  las dos a la vez: se encuentran en {t * 20 + 20} frames")
            return 0
    print("FALLA: entrando a la vez no se encuentran nunca")
    return 1


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: run_wireless.py build/tengen.gba")
    rom = sys.argv[1]
    code = (plain_check(rom) or together_check(rom) or _play(rom, False) or
            _play(rom, True))
    if not code:
        print("OK: dos GBA con adaptador inalambrico (el de prueba) se "
              "encuentran y juegan la misma partida.")
    return code


if __name__ == "__main__":
    code = main()
    # See the end of run_link.py: two cores and their drivers cannot be
    # let go of normally.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
