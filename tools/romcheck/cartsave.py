"""
The save, on every kind of cartridge (gba/cartsave.c): the emulator's, a
flash cart's, and the boards with a flash chip and no battery, where the
save has to go into the chip the game itself is in.

None of that last kind can be tried on the real thing from here, and a
stock emulator's cartridge is a ROM that takes no commands. flashchip.py
stands a chip in; these are the things asked of the port's own routines
against it. What they hold is what the datasheets and the tools that
already write these boards say a chip does — a chip that does something
else is not in them, and the first power-on on real hardware is still the
first.
"""
from .harness import (game_offsets, game_state_address, skip, stack_bounds,
                      stack_depth, stack_paint, tilemap_text)
from .flashchip import (Cart, FlashChip, PowerCut, ramcode_faults, ROM,
                        WIRE_D0D1, WIRE_D0D1_D6D7, WIRE_STRAIGHT)

IMAGE = 0x7FE0          # gba/cartsave.h CARTSAVE_IMAGE
FIRST = 0x100000        # CARTSAVE_FIRST
SLOT = 0x40000          # CARTSAVE_SLOT
MARK = b"TNVS"

# gba/cartsave.h's enums.
SILENT, IS_RAM, TABLE = 0, 1, 2
USED, NO_CHIP, BAD_TABLE, COMMANDS, PLACE, NO_ID, TAKEN = range(7)
PLAIN, LEVEL, RAM_NEWER, FROM_FLASH, FIRST_KEPT, FIRST_EMPTY = range(6)
BOOT = ["la RAM sola", "RAM y diario iguales", "la RAM iba por delante",
        "desde la flash", "primer diario, con lo que habia en la RAM",
        "primer diario, vacio"]

# The board this was written for, as the flasher's own table has it
# (FlashGBX, fc_AGB_MSP55LV128M.txt: "AGB-E05-01 with MSP55LV128M"): AMD's
# commands with D0 and D1 crossed, 16MB in sectors of 128KB, and an
# identity that reads 0x0002/0x227D through those crossed lines.
THE_BOARD = dict(wiring=WIRE_D0D1, size_log2=24, regions=((128, 0x20000),))


# ---------------------------------------------------------------------------
# The journal, read a second time. tengen_nvlog.c's format written out again
# ON PURPOSE (see harness.ORIENTATION_BITS): what the cartridge wrote to the
# chip is read here by something that shares none of its code.
# ---------------------------------------------------------------------------
def _crc16(crc, data):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _seal(crc):
    return 0xFFFE if crc == 0xFFFF else crc


def _u16(b, at):
    return b[at] | (b[at + 1] << 8)


def _u32(b, at):
    return _u16(b, at) | (_u16(b, at + 2) << 16)


def journal_read(sector):
    """A sector as a journal: None if it is not one with a commit, else a
    dict of its generation, the image as of its last commit, where that
    commit ends, its number and hash, and whether anything follows it."""
    if sector[0:4] != b"TNV1" or _u16(sector, 8) != _seal(_crc16(0x1D0F, sector[0:8])):
        return None
    image, pending = bytearray(b"\xff" * IMAGE), []
    pos, last = 16, None
    while pos + 8 <= len(sector):
        off, length = _u16(sector, pos), _u16(sector, pos + 2)
        head = _seal(_crc16(0x2A5F, sector[pos:pos + 4]))
        if _u16(sector, pos + 4) != head:
            break
        if off == 0xC0DE:
            if length != 8:
                break
        elif length == 0 or off + length > IMAGE:
            break
        padded = (length + 1) & ~1
        if pos + 6 + padded + 2 > len(sector):
            break
        data = sector[pos + 6:pos + 6 + padded]
        if _u16(sector, pos + 6 + padded) != _seal(_crc16(head, data)):
            break
        pos += 6 + padded + 2
        if off == 0xC0DE:
            for o, d in pending:
                image[o:o + len(d)] = d
            pending = []
            last = dict(gen=_u32(sector, 4), image=bytes(image), end=pos,
                        seq=_u32(data, 0), hash=_u32(data, 4))
        else:
            pending.append((off, data[:length]))
    if last is None:
        return None
    last["clean"] = sector[last["end"]:] == b"\xff" * (len(sector) - last["end"])
    return last


def journal_commits(sector):
    """The runs each commit of a sector's journal is made of, as lists of
    (offset, length): how a save was cut up on its way to the chip."""
    commits, pending, pos = [], [], 16
    while pos + 8 <= len(sector):
        off, length = _u16(sector, pos), _u16(sector, pos + 2)
        if _u16(sector, pos + 4) != _seal(_crc16(0x2A5F, sector[pos:pos + 4])):
            break
        pos += 6 + ((length + 1) & ~1) + 2
        if off == 0xC0DE:
            commits.append(pending)
            pending = []
        else:
            pending.append((off, length))
    return commits


def journal_record(off, data):
    """One record, as the journal writes it."""
    head = bytes((off & 0xFF, off >> 8, len(data) & 0xFF, len(data) >> 8))
    seal = _seal(_crc16(0x2A5F, head))
    body = data + (b"\xff" if len(data) & 1 else b"")
    return head + bytes((seal & 0xFF, seal >> 8)) + body + \
        _seal(_crc16(seal, body)).to_bytes(2, "little")


def image_hash(image):
    """nv_hash, the same way: every byte times its place's number."""
    h, w = 0, 0x7F4A7C15
    for b in image:
        h = (h + b * (w | 1)) & 0xFFFFFFFF
        w = (w + 0x9E3779B1) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------------------
# A console, a cartridge and somebody playing
# ---------------------------------------------------------------------------
class Play:
    def __init__(self, rom_path, **cart):
        self.rom_path = rom_path
        self.cart = Cart(rom_path, **cart)
        self.base = game_state_address(rom_path)[0]
        self.off = game_offsets(rom_path)
        self.image_at = game_state_address(rom_path, "g_cartsave_image")[0]
        self.linked_at = game_state_address(rom_path, "g_linked")[0]

    def u8(self, key):
        return self.cart.core.memory.u8[self.base + self.off[key]]

    def game(self):
        """(a match is on, it is paused, the piece, its row, the score)."""
        m = self.cart.core.memory.u8
        score = bytes(m[self.base + self.off["score"] + i] for i in range(4))
        return (self.u8("active"), self.u8("paused"), self.u8("current"), self.u8("y"), score)

    def boot(self, frames=70):
        """Power-on, and what it owes the chip paid: a journal to be
        written afresh is written at the first frame, a call a frame here
        (see settle)."""
        self.cart.run(frames)
        for _ in range(400):
            r = self.cart.report()
            if not (r["flash"] and r["afresh"]) or r["dead"]:
                break
            self.cart.run(1)
        return self.cart.report()

    def to_the_game(self):
        """The title, GAME SELECT, 1 PLAYER, and a moment of play."""
        self.cart.tap("START", 4, 10)
        self.cart.tap("START", 4, 12)
        self.cart.tap("START", 4, 90)

    def settle(self, limit=400):
        """Frames until nothing is on its way to the chip. Here every call
        into the RAM routines costs the console a frame (flashchip.py), so
        a save of many calls — one that has to start again in the other
        sector — is many frames; on the board it is milliseconds."""
        if not self.cart.chip or self.linked_at is None:
            return
        for _ in range(limit):
            r = self.cart.report()
            if not r["dirty"] or r["dead"] or r["waiting"] or not r["flash"]:
                return
            self.cart.run(1)

    def pause(self):
        """L+R+SELECT: the match pauses, goes to the save and the console
        sleeps (the emulator's Stop returns at once)."""
        self.cart.run(6, "L", "R", "SELECT")
        self.cart.core.set_keys()
        self.cart.run(30)
        self.settle()

    def play_on(self):
        self.cart.tap("START", 4, 30)
        self.settle()

    def image(self):
        """The save as the game has it in memory (flash carts only)."""
        m = self.cart.core.memory.u8
        return bytes(m[self.image_at + i] for i in range(IMAGE))

    def sector(self, half):
        size = self.cart.report()["half_bytes"]
        base = FIRST + half * SLOT
        return bytes(self.cart.mem[base:base + size])

    def journal(self):
        """The newer of the chip's two sectors that is a journal."""
        found = [j for j in (journal_read(self.sector(h)) for h in (0, 1)) if j]
        return max(found, key=lambda j: j["gen"]) if found else None

    def set_linked(self, on):
        self.cart.core.memory.u8[self.linked_at] = 1 if on else 0


def _clean(play, failures, what, strays=1, journal=True):
    """What must hold of any cartridge with a chip after anything at all."""
    cart = play.cart
    for f in cart.faults + cart.chip.faults:
        failures.append(f"{what}: {f}")
    # The only writes that mean nothing to the chip are the query's second
    # reset, Intel's 0xFF, once a query (and the query a board with D6 and
    # D7 crossed does not hear).
    odd = [(a, v) for a, v in cart.chip.strays
           if not (a == 0xAA and v in (0xFF, 0x98, 0x58, 0xF0))]
    if odd:
        failures.append(f"{what}: escrituras que el chip no entiende: "
                        + ", ".join(f"{v:#x} en {a:#x}" for a, v in odd[:4]))
    lo, hi = cart.chip.program_span
    half = cart.report()["half_bytes"]
    inside = lambda a: FIRST <= a < FIRST + half or FIRST + SLOT <= a < FIRST + SLOT + half
    if lo is not None and not (inside(lo) and inside(hi - 2)):
        failures.append(f"{what}: se programo fuera del diario: {lo:#x}..{hi:#x}")
    if cart.chip.doomed or cart.chip.pointless:
        failures.append(f"{what}: {cart.chip.doomed} palabras programadas donde no caben "
                        f"y {cart.chip.pointless} que ya estaban")
    if cart.chip.told_while_stuck:
        failures.append(f"{what}: {cart.chip.told_while_stuck} escrituras a un chip que "
                        "se habia rendido y esperaba su reset")
    if any(not inside(base) for base in cart.chip.erased):
        failures.append(f"{what}: se borro fuera del diario: "
                        + ", ".join(f"{b:#x}" for b in cart.chip.erased))
    now = bytes(cart.mem[0:len(cart.image)])
    if now != _with_traps(cart):
        at = next(i for i in range(len(now)) if now[i] != _with_traps(cart)[i])
        failures.append(f"{what}: el programa en el chip ha cambiado, en {at:#x}")
    if journal:
        j = play.journal()
        if j is None:
            failures.append(f"{what}: no hay diario en el chip")
        elif j["image"] != play.image():
            failures.append(f"{what}: el diario, leido aparte, no es la partida guardada")
        elif j["hash"] != image_hash(j["image"]):
            failures.append(f"{what}: el diario no lleva la suma de su imagen")


def _with_traps(cart):
    """The ROM image as the rig left it: the game's, with the doors of the
    four RAM routines in it replaced."""
    if not hasattr(cart, "_trapped_image"):
        image = bytearray(cart.image)
        lma = game_state_address(cart.rom_path, "__ramcode_lma")[0] - ROM
        for entry in cart.traps:
            at = lma + entry - cart.ram_start
            image[at:at + 2] = b"\xfe\xe7"
        cart._trapped_image = bytes(image)
    return cart._trapped_image


def _round_trip(rom_path, failures, what, expect_block, expect_half, **cart):
    """The whole of it on one board: a first power-on, a game paused and
    switched off, the same game back, played on and switched off again."""
    play = Play(rom_path, **cart)
    c = play.cart
    r = play.boot()
    wiring = cart["chip"].get("wiring", WIRE_D0D1)
    if (r["probe"], r["why"], r["flash"]) != (TABLE, USED, 1):
        failures.append(f"{what}: el chip no se usa (pregunta {r['probe']}, motivo {r['why']})")
        return None
    if r["wiring"] != wiring:
        failures.append(f"{what}: cableado {r['wiring']}, no {wiring}")
    if (r["block_bytes"], r["half_bytes"]) != (expect_block, expect_half):
        failures.append(f"{what}: sectores de {r['block_bytes']:#x} y mitades de "
                        f"{r['half_bytes']:#x}, no {expect_block:#x} y {expect_half:#x}")
    if r["boot"] != FIRST_EMPTY:
        failures.append(f"{what}: el primer encendido es '{BOOT[r['boot']]}'")
    if r["dead"] or r["refused"]:
        failures.append(f"{what}: el chip se da por perdido en el primer encendido")
    # What RAM holds with no battery is not a save: the journal starts
    # empty and the RAM is wiped to match it.
    if play.image() != b"\xff" * IMAGE or c.ram(0, IMAGE) != b"\xff" * IMAGE:
        failures.append(f"{what}: el primer diario no empieza vacio, o la RAM no queda igual")
    play.to_the_game()
    if not play.game()[0]:
        failures.append(f"{what}: no llego a la partida")
        return None
    words = c.chip.programmed
    play.pause()
    paused = play.game()
    r = c.report()
    if not paused[1]:
        failures.append(f"{what}: L+R+SELECT no pausa")
    if r["dirty"] or c.chip.programmed == words:
        failures.append(f"{what}: la pausa no llega a la flash "
                        f"(pendiente {r['dirty']}, palabras {c.chip.programmed - words})")
    _clean(play, failures, f"{what}, en pausa")
    saved = play.image()

    c.power_cycle()
    r = play.boot(90)
    want = LEVEL if c.battery else FROM_FLASH
    if r["boot"] != want:
        failures.append(f"{what}: al encender, '{BOOT[r['boot']]}' y no '{BOOT[want]}'")
    if play.game() != paused:
        failures.append(f"{what}: tras apagar no vuelve la misma partida en pausa "
                        f"({play.game()[:4]} por {paused[:4]})")
    if play.image() != saved:
        failures.append(f"{what}: lo guardado no es lo que habia al apagar")
    if c.ram(0, IMAGE) != play.image():
        failures.append(f"{what}: la RAM de guardado no queda igual que la partida guardada")
    if c.ram(IMAGE, 4) != MARK:
        failures.append(f"{what}: la RAM de guardado queda sin su marca")
    _clean(play, failures, f"{what}, de vuelta")

    play.play_on()
    if play.game()[1]:
        failures.append(f"{what}: START no reanuda la partida recuperada")
    c.power_cycle()
    play.boot(90)
    if play.game()[0] and play.game()[1]:
        failures.append(f"{what}: jugada la partida, el siguiente encendido la vuelve a traer")
    _clean(play, failures, f"{what}, jugada")
    return play


def cartsave_check(rom_path):
    """THE SAVE REACHES WHATEVER THE CARTRIDGE CAN KEEP IT IN.

    The routines that write the cartridge's own flash never leave RAM. An
    emulator's cartridge, and a flash cart's whether its ROM ignores writes
    or keeps them, is asked one question and saves to its RAM as before. A
    board with a flash chip and no battery — the one this was written for,
    and its relations with the lines crossed otherwise, other sector sizes
    and small sectors at one end — gets a journal a megabyte into the chip:
    a paused game comes back after the power has been off with the save
    RAM gone, nothing is written but the journal's own sectors, and nothing
    of the cartridge's runs while the chip is not a ROM. Chips whose
    commands are not spoken are left alone. The power cut at any point of
    a save leaves the save before it or the save after. A sector that
    stops taking words is left for the other, and a chip that takes none
    is left alone with the game going on.
    """
    if game_state_address(rom_path, "g_cartsave")[0] is None:
        skip("el ELF no tiene g_cartsave: no es la construccion de cartucho")
        return 0
    failures = []

    for f in ramcode_faults(rom_path):
        failures.append(f"rutinas en RAM: {f}")
    if not failures:
        print("  las rutinas que hablan con el chip no salen de la RAM: ni una "
              "llamada ni un salto fuera de su seccion")

    _plain_cartridges(rom_path, failures)
    _the_boards(rom_path, failures)
    _no_save_ram(rom_path, failures)
    _chips_left_alone(rom_path, failures)
    _another_game(rom_path, failures)
    _with_a_battery(rom_path, failures)
    _half_kept_ram(rom_path, failures)
    _an_older_save(rom_path, failures)
    _power_cuts(rom_path, failures)
    _refusals(rom_path, failures)
    _a_full_sector(rom_path, failures)
    _waits_and_restarts(rom_path, failures)
    _the_page(rom_path, failures)
    _what_a_save_costs(rom_path, failures)

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la partida se guarda en lo que el cartucho tenga: su RAM, o la "
          "flash del propio juego donde no hay pila.")
    return 0


def _plain_cartridges(rom_path, failures):
    """The emulator's cartridge and the flash carts': a ROM that ignores
    writes, and one that is RAM and keeps them."""
    asked = [(0xAA, 0x98), (0xAA, 0xF0), (0xAA, 0xFF),
             (0xAA, 0x58), (0xAA, 0xF0), (0xAA, 0xFF)]
    play = Play(rom_path, battery=True)
    c = play.cart
    r = play.boot()
    if (r["probe"], r["flash"], r["boot"]) != (SILENT, 0, PLAIN):
        failures.append(f"una ROM que no contesta: pregunta {r['probe']}, flash {r['flash']}")
    play.to_the_game()
    play.pause()
    paused = play.game()
    c.power_cycle()
    play.boot(90)
    if not paused[1] or play.game() != paused:
        failures.append("una ROM que no contesta: la pausa no vuelve por la RAM de guardado")
    if c.stores != asked * 2:
        failures.append("una ROM que no contesta: lo escrito al cartucho no es la pregunta "
                        "y nada mas: " + ", ".join(f"{v:#x} en {a:#x}" for a, v in c.stores[:14]))
    if c.ram(IMAGE, 4) == MARK:
        failures.append("una ROM que no contesta: la RAM de guardado lleva marca, y antes no")
    if c.faults:
        failures.extend(f"una ROM que no contesta: {f}" for f in c.faults)
    else:
        print("  una ROM que no contesta (emulador, flash cart): una pregunta de "
              "seis escrituras a una sola palabra, y la RAM de guardado como siempre")

    play = Play(rom_path, rom_is_ram=True, battery=True)
    c = play.cart
    r = play.boot()
    if (r["probe"], r["flash"]) != (IS_RAM, 0):
        failures.append(f"una ROM que es RAM: pregunta {r['probe']}, flash {r['flash']}")
    if bytes(c.mem[0:len(c.image)]) != _with_traps(c):
        failures.append("una ROM que es RAM: la pregunta deja el programa cambiado")
    if c.stores[:4] != [(0xAA, 0x98), (0xAA, 0xF0), (0xAA, 0xFF), (0xAA, r["probe_word"][0])] \
            or len(c.stores) != 4:
        failures.append("una ROM que es RAM: no se deja como estaba y en paz: "
                        + ", ".join(f"{v:#x} en {a:#x}" for a, v in c.stores[:8]))
    play.to_the_game()
    play.pause()
    paused = play.game()
    c.power_cycle()
    play.boot(90)
    if not paused[1] or play.game() != paused or len(c.stores) != 8:
        failures.append("una ROM que es RAM: la pausa no vuelve por la RAM de guardado")
    elif not c.faults:
        print("  una ROM que es RAM y se queda lo escrito: se nota, se deja "
              "como estaba y no se toca mas")
    failures.extend(f"una ROM que es RAM: {f}" for f in c.faults)


def _the_boards(rom_path, failures):
    boards = [
        ("la placa (AMD, D0/D1 cruzadas, 16MB, sectores de 128KB), sin pila",
         THE_BOARD, 0x20000, 0x20000),
        ("lineas sin cruzar, sectores de 64KB",
         dict(wiring=WIRE_STRAIGHT, regions=((256, 0x10000),)), 0x10000, 0x10000),
        ("D0/D1 y D6/D7 cruzadas, 32MB en sectores de 128KB",
         dict(wiring=WIRE_D0D1_D6D7, size_log2=25, regions=((256, 0x20000),)),
         0x20000, 0x20000),
        ("8MB con sectores pequenos al principio",
         dict(wiring=WIRE_D0D1, size_log2=23, regions=((8, 0x2000), (127, 0x10000)), boot=2),
         0x10000, 0x10000),
        ("sectores de 256KB",
         dict(wiring=WIRE_STRAIGHT, regions=((64, 0x40000),)), 0x40000, 0x40000),
        ("sectores de 32KB, dos por mitad",
         dict(wiring=WIRE_D0D1, regions=((512, 0x8000),)), 0x8000, 0x10000),
        ("un chip de 2MB",
         dict(wiring=WIRE_STRAIGHT, size_log2=21, regions=((32, 0x10000),)),
         0x10000, 0x10000),
    ]
    for what, chip, block, half in boards:
        before = len(failures)
        play = _round_trip(rom_path, failures, what, block, half, chip=chip)
        if play is None or len(failures) != before:
            continue
        c = play.cart
        r = c.report()
        ident = r["id"]
        if chip is THE_BOARD and ident != [0x0002, 0x227D]:
            failures.append(f"{what}: se identifica como {ident[0]:#06x}/{ident[1]:#06x}, "
                            "y el grabador lee 0x0002/0x227d en esta placa")
        programs = [cy for name, _, cy in c.calls if name == "cartsave_ram_program"]
        longest = max(programs) * 1000 / 16777216
        # The interrupts are let back in every few milliseconds, however
        # much there is to write (at the model's sixty microseconds a word).
        if longest > 6:
            failures.append(f"{what}: {longest:.1f} ms seguidos con las interrupciones quitadas")
        print(f"  {what}: la pausa vuelve tras apagar; {c.chip.programmed} palabras "
              f"en {len(programs)} llamadas, la mas larga "
              f"{longest:.1f} ms, {len(c.chip.erased)} borrados")

    # ...and with whatever was in that part of the chip before: a board
    # that had a bigger game on it. The first power-on has to erase — on
    # each wiring, since it is the erase's own commands that cross.
    quiet = True
    for wiring in (WIRE_D0D1, WIRE_D0D1_D6D7, WIRE_STRAIGHT):
        what = f"un chip con restos de otro juego (cableado {wiring})"
        play = Play(rom_path, chip=dict(THE_BOARD, wiring=wiring))
        c = play.cart
        c.mem[FIRST:FIRST + 0x30000] = c.rng.randbytes(0x30000)
        c.mem[FIRST + SLOT + 0x100:FIRST + SLOT + 0x200] = b"\x00" * 0x100
        r = play.boot(120)
        before = len(failures)
        if r["flash"] != 1 or r["boot"] != FIRST_EMPTY or c.chip.erased != [FIRST]:
            failures.append(f"{what}: flash {r['flash']}, "
                            f"borrados {[hex(b) for b in c.chip.erased]}")
        play.to_the_game()
        play.pause()
        paused = play.game()
        c.power_cycle()
        play.boot(90)
        if not paused[1] or play.game() != paused:
            failures.append(f"{what}: la pausa no vuelve")
        _clean(play, failures, what)
        if bytes(c.mem[FIRST + SLOT + 0x100:FIRST + SLOT + 0x200]) != b"\x00" * 0x100:
            failures.append(f"{what}: se borro tambien la otra mitad, que no hacia falta")
        quiet = quiet and len(failures) == before
    if quiet:
        print("  con restos de otro juego donde va el diario, en los tres "
              "cableados: se borra ese sector, y solo ese, y la pausa vuelve")


def _no_save_ram(rom_path, failures):
    """A board with the flash chip and NO SAVE RAM AT ALL: what is written
    there is gone at once and what is read is noise. The game never reads
    its save from there once it has a journal — the image is in its own
    memory — so it plays, pauses and comes back all the same."""
    play = Play(rom_path, chip=THE_BOARD, no_ram=True)
    c = play.cart
    r = play.boot(90)
    if (r["flash"], r["boot"]) != (1, FIRST_EMPTY):
        failures.append(f"sin RAM de guardado: flash {r['flash']}, '{BOOT[r['boot']]}'")
        return
    play.to_the_game()
    if not play.game()[0]:
        failures.append("sin RAM de guardado: no llega a la partida")
        return
    play.pause()
    paused, saved = play.game(), play.image()
    c.power_cycle()
    r = play.boot(90)
    if r["boot"] != FROM_FLASH or play.game() != paused or play.image() != saved:
        failures.append(f"sin RAM de guardado: la pausa no vuelve ('{BOOT[r['boot']]}')")
    play.play_on()
    play.pause()
    again = play.game()
    c.power_cycle()
    play.boot(90)
    if not again[1] or play.game() != again:
        failures.append("sin RAM de guardado: la segunda pausa no vuelve")
    before = len(failures)
    _clean(play, failures, "sin RAM de guardado")
    if len(failures) == before and not any("sin RAM" in f for f in failures):
        print("  una placa sin RAM de guardado ninguna: se juega, se pausa y se "
              "vuelve igual; lo guardado se lee de la memoria del juego y de la "
              "flash, nunca de alli")


def _another_game(rom_path, failures):
    """A flash cart that keeps several games in the one chip, the next of
    them where the journal would go or running into it: left alone, every
    byte of it. A game is known by the boot logo every GBA ROM starts
    with."""
    quiet = True
    header = None
    # At the megabyte itself; in the second half; and from before the
    # journal, a game that starts in the gap and runs through it.
    for where in (FIRST, FIRST + SLOT, 0xC0000, FIRST + SLOT + 0x18000):
        what = f"otro juego en {where:#x}"
        play = Play(rom_path, chip=THE_BOARD, battery=True)
        c = play.cart
        # Another game's header: the logo every one has, after a first
        # instruction of its own (a branch to wherever ITS code starts).
        header = b"\x06\x00\x00\xea" + bytes(c.mem[4:0xC0])
        other = header + c.rng.randbytes(0x60000 - len(header))
        c.mem[where:where + len(other)] = other
        kept = bytes(c.mem[0x80000:0x200000])
        r = play.boot()
        if (r["probe"], r["why"], r["flash"]) != (TABLE, TAKEN, 0):
            failures.append(f"{what}: pregunta {r['probe']}, motivo {r['why']}, flash {r['flash']}")
            quiet = False
        play.to_the_game()
        play.pause()
        paused = play.game()
        c.power_cycle()
        play.boot(90)
        if not paused[1] or play.game() != paused:
            failures.append(f"{what}: la pausa no vuelve por la RAM de guardado")
            quiet = False
        if bytes(c.mem[0x80000:0x200000]) != kept or c.chip.programmed or c.chip.erased:
            failures.append(f"{what}: se ha escrito en el chip")
            quiet = False
        for f in c.faults + c.chip.faults:
            failures.append(f"{what}: {f}")
            quiet = False
    # ...and a logo that is not quite one, or one past the journal's end,
    # is not a game in the way.
    for what, where, spoil in (("un logo a medias", FIRST, 100),
                               ("otro juego pasado el diario", FIRST + SLOT + 0x20000, None)):
        play = Play(rom_path, chip=THE_BOARD)
        c = play.cart
        c.mem[where:where + 0xC0] = header
        if spoil is not None:
            c.mem[where + 4 + spoil] = bytes((header[4 + spoil] ^ 0x10,))
        r = play.boot(200)
        if (r["why"], r["flash"]) != (USED, 1):
            failures.append(f"{what}: el diario no se usa (motivo {r['why']})")
            quiet = False
    if quiet:
        print("  otro juego en el chip donde iria el diario, o que llega hasta "
              "el (un flash cart de varios juegos): no se toca ni un byte, y la "
              "RAM de guardado como siempre")


def _chips_left_alone(rom_path, failures):
    """Chips that answer the query and are not written: Intel's command
    sets, a chip with no room for the journal, and a table that makes no
    sense. The save RAM carries on, and nothing is sent after the query."""
    cases = [
        ("un chip de comandos Intel (0x0001)", dict(command_set=1, wiring=WIRE_STRAIGHT), COMMANDS),
        ("un chip de comandos Intel (0x0003), lineas cruzadas",
         dict(command_set=3, wiring=WIRE_D0D1), COMMANDS),
        ("un chip de comandos 0x0200", dict(command_set=0x200, wiring=WIRE_STRAIGHT), COMMANDS),
        ("un chip de 1MB, sin sitio para el diario",
         dict(wiring=WIRE_D0D1, size_log2=20, regions=((16, 0x10000),)), PLACE),
        ("un chip de sectores de 512KB",
         dict(wiring=WIRE_STRAIGHT, regions=((32, 0x80000),)), PLACE),
        ("una tabla sin regiones", dict(wiring=WIRE_STRAIGHT, regions=()), BAD_TABLE),
        # AMD's commands at addresses the cartridge's sequences do not
        # reach (word 0x5555, as some older chips want): it answers the
        # query, and then not its own identify — which is as far as it goes.
        ("un chip que no atiende al desbloqueo",
         dict(wiring=WIRE_STRAIGHT, unlock_bits=0x7FFF), NO_ID),
    ]
    quiet = True
    for what, chip, why in cases:
        play = Play(rom_path, chip=chip, battery=True)
        c = play.cart
        r = play.boot()
        if (r["probe"], r["why"], r["flash"]) != (TABLE, why, 0):
            failures.append(f"{what}: pregunta {r['probe']}, motivo {r['why']} (se esperaba "
                            f"{why}), flash {r['flash']}")
            quiet = False
            continue
        asked = len(c.stores)
        play.to_the_game()
        play.pause()
        paused = play.game()
        c.power_cycle()
        play.boot(90)
        if not paused[1] or play.game() != paused:
            failures.append(f"{what}: la pausa no vuelve por la RAM de guardado")
            quiet = False
        # Nothing after the question — but for the one that got as far as
        # asking the chip who it is: three writes and a reset, at each of
        # the two power-ons, and no more.
        knock = [(0xAAA, 0xAA), (0x554, 0x55), (0xAAA, 0x90), (0x0, 0xF0)]
        sent = c.stores[asked:len(c.stores) - asked]
        if why == NO_ID and c.stores[3:asked] != knock:
            sent = c.stores[3:asked]
        elif why != NO_ID and asked != 3:
            sent = c.stores[3:asked]
        if sent or c.chip.programmed or c.chip.erased or c.chip.idents:
            failures.append(f"{what}: se le mando algo mas que la pregunta: "
                            + ", ".join(f"{v:#x} en {a:#x}" for a, v in sent[:6]))
            quiet = False
        for f in c.faults + c.chip.faults:
            failures.append(f"{what}: {f}")
            quiet = False
    if quiet:
        print("  los chips que contestan y no se usan (comandos Intel, sin sitio, "
              "tabla sin sentido, sordo al desbloqueo): la pregunta — y quien "
              "eres, al ultimo — y nada mas, y la RAM de guardado")


def _with_a_battery(rom_path, failures):
    """A board with a chip AND a battery: the RAM is believed while it adds
    up, and the flash is kept level with it."""
    before = len(failures)
    play = _round_trip(rom_path, failures, "la placa con pila", 0x20000, 0x20000,
                       chip=THE_BOARD, battery=True)
    if play is None or len(failures) != before:
        return
    # ...and paused with START alone, which does not put the console to
    # sleep (the sleep sends everything to the chip on its own account):
    # the RAM's mark is kept level frame by frame, not only then.
    play = Play(rom_path, chip=THE_BOARD, battery=True)
    c = play.cart
    play.boot()
    play.to_the_game()
    c.tap("START", 4, 30)
    play.settle()
    paused = play.game()
    c.power_cycle()
    r = play.boot(90)
    if not paused[1] or r["boot"] != LEVEL or not r["ram_kept"] or play.game() != paused:
        failures.append(f"con pila, en pausa con START: al encender '{BOOT[r['boot']]}', "
                        f"RAM conservada {r['ram_kept']}")
        return
    _clean(play, failures, "con pila, en pausa con START")
    print("  con pila: la RAM de guardado manda mientras cuadre con su marca, "
          "y la flash va a la par")


def _half_kept_ram(rom_path, failures):
    """RAM THAT HALF-SURVIVED: a second without power, most of it still
    there and a bit of it not. It is not believed for looking like a save —
    not with one bit of the save wrong, nor with its mark's letters, the
    hash in it or the hash's inverse a bit out — and what the journal has
    is put back over it."""
    play = Play(rom_path, chip=THE_BOARD, battery=True)
    c = play.cart
    play.boot()
    play.to_the_game()
    play.pause()
    paused, saved = play.game(), play.image()
    m8 = c.core.memory.u8
    quiet = True
    for what, at in (("un bit de la partida", 0x4123), ("un bit de un record", 0x0007),
                     ("una letra de la marca", IMAGE + 2), ("la suma de la marca", IMAGE + 5),
                     ("la suma invertida", IMAGE + 10)):
        was = m8[0x0E000000 + at]
        m8[0x0E000000 + at] = was ^ 0x08
        c.power_cycle()
        r = play.boot(90)
        if r["boot"] != FROM_FLASH or r["ram_kept"]:
            failures.append(f"RAM a medias ({what}): se la cree ('{BOOT[r['boot']]}')")
            quiet = False
        if play.game() != paused or play.image() != saved:
            failures.append(f"RAM a medias ({what}): lo guardado no es lo del diario")
            quiet = False
        if c.ram(0, IMAGE) != saved or c.ram(IMAGE, 4) != MARK:
            failures.append(f"RAM a medias ({what}): la RAM no queda como el diario, con su marca")
            quiet = False
    # ...and whole again, it is believed again.
    c.power_cycle()
    r = play.boot(90)
    if r["boot"] != LEVEL or not r["ram_kept"]:
        failures.append(f"RAM entera tras rehacerla: '{BOOT[r['boot']]}'")
        quiet = False
    _clean(play, failures, "RAM a medias")
    if quiet:
        print("  una RAM de guardado que sobrevive a medias (un bit de la partida, "
              "o de su marca): no se la cree, y se rehace desde el diario")


def _an_older_save(rom_path, failures):
    """A board with a battery that an older build has been saving to: no
    journal, no mark, and a save in the RAM. It is where the journal
    starts — and RAM that only holds noise is not."""
    old = Play(rom_path, battery=True)
    old.boot()
    old.to_the_game()
    old.pause()
    paused = old.game()
    ram = old.cart.ram(0, 0x8000)
    if ram[0x700:0x704] != b"STAT" or ram[IMAGE:IMAGE + 4] == MARK:
        failures.append("una partida de una construccion anterior: no consigo fabricarla")
        return
    play = Play(rom_path, chip=THE_BOARD, battery=True)
    c = play.cart
    m8 = c.core.memory.u8
    for i, b in enumerate(ram):
        m8[0x0E000000 + i] = b
    r = play.boot(200)
    if (r["flash"], r["boot"]) != (1, FIRST_KEPT):
        failures.append(f"una partida anterior en la RAM: '{BOOT[r['boot']]}'")
    if play.game() != paused:
        failures.append("una partida anterior en la RAM: no se recupera")
    _clean(play, failures, "una partida anterior en la RAM")
    c.battery = False
    c.power_cycle()
    r = play.boot(90)
    if r["boot"] != FROM_FLASH or play.game() != paused:
        failures.append("una partida anterior, ya en el diario: sin pila no vuelve")
    else:
        print("  una partida de una construccion anterior, en la RAM de una placa "
              "con pila: pasa al diario, y vuelve aunque la pila falte despues")


def _power_cuts(rom_path, failures):
    """The power goes at points all through a save, and through the first
    power-on's erase: what is found afterwards is the save as some commit
    left it, and the game comes up."""
    ref = Play(rom_path, chip=THE_BOARD)
    ref.boot()
    ref.to_the_game()
    start = ref.cart.steps
    if ref.journal() is None:
        failures.append("cortes de corriente: no hay diario que cortar")
        return
    states = [ref.journal()["image"]]
    ref.cart.run(6, "L", "R", "SELECT")
    ref.cart.core.set_keys()
    for _ in range(30):
        ref.cart.run(1)
        image = ref.journal()["image"]
        if image != states[-1]:
            states.append(image)
    end = ref.cart.steps
    paused = ref.game()
    if len(states) < 2 or end - start < 2000:
        failures.append("cortes de corriente: la pausa de referencia no escribe")
        return
    cuts = sorted({start + 1 + (end - start - 2) * i // 23 for i in range(24)})
    landed = {i: 0 for i in range(len(states))}
    for cut in cuts:
        play = Play(rom_path, chip=THE_BOARD)
        c = play.cart
        play.boot()
        play.to_the_game()
        c.cut_after = cut
        try:
            play.pause()
            failures.append(f"cortes de corriente: el corte en el paso {cut} no llega")
            continue
        except PowerCut:
            pass
        c.power_cycle()
        r = play.boot(200)
        image = play.image()
        what = f"corte en el paso {cut - start} de {end - start}"
        if r["flash"] != 1 or r["dead"]:
            failures.append(f"{what}: el chip no se usa despues")
        elif image not in states:
            failures.append(f"{what}: lo que queda no es la partida de antes ni la de "
                            "despues de ningun guardado")
        else:
            landed[states.index(image)] += 1
            whole = states.index(image) == len(states) - 1
            if whole and play.game() != paused:
                failures.append(f"{what}: el guardado entero y la pausa no vuelve")
            if not whole and play.game()[0] and play.game()[1] and len(states) == 2:
                failures.append(f"{what}: sin el guardado y vuelve una pausa")
        _clean(play, failures, what)
    if landed[0] == 0 or landed[len(states) - 1] == 0:
        failures.append(f"cortes de corriente: no cayeron a los dos lados ({landed})")

    # The first power-on of a chip that has to be erased, cut in the erase.
    erased_ok = 0
    for cut in (40, 400, 4000, 20000):
        play = Play(rom_path, chip=dict(THE_BOARD, erase_cycles=200000))
        c = play.cart
        c.mem[FIRST:FIRST + 0x20000] = c.rng.randbytes(0x20000)
        probe_steps = 700
        c.cut_after = probe_steps + cut
        try:
            play.boot(200)
            continue                    # the erase was over before the cut
        except PowerCut:
            pass
        c.power_cycle()
        r = play.boot(200)
        if r["flash"] != 1 or r["dead"] or r["boot"] != FIRST_EMPTY:
            failures.append(f"corte en el primer borrado (paso {cut}): flash {r['flash']}, "
                            f"'{BOOT[r['boot']]}'")
        else:
            erased_ok += 1
        _clean(play, failures, f"corte en el primer borrado (paso {cut})")
    if not erased_ok:
        failures.append("cortes de corriente: ninguno cayo dentro del primer borrado")
    if not any("corte" in f for f in failures):
        print(f"  la corriente cortada en {len(cuts)} puntos de un guardado y en "
              f"{erased_ok} del primer borrado: queda el guardado de antes "
              f"({landed[0]}) o el de despues ({landed[len(states) - 1]}), entero")


def _refusals(rom_path, failures):
    """A sector that stops taking words in the middle of a session, and a
    chip that takes none."""
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    c.chip.deaf.add(FIRST)
    at = len(c.calls)
    play.pause()
    paused = play.game()
    r = c.report()
    if r["dead"] or r["dirty"] or c.chip.erased != [] or r["fresh"] != 2:
        failures.append("un sector que deja de aceptar palabras: el guardado no pasa a la "
                        f"otra mitad (perdido {r['dead']}, nuevos {r['fresh']})")
    # The word the sector refused was waited for as long as the cartridge
    # has patience for a word — some twenty milliseconds — and no longer:
    # the first call is that wait.
    waited = c.calls[at][2] * 1000 / 16777216 if len(c.calls) > at else 0
    if not 5 < waited < 60:
        failures.append("un sector que deja de aceptar palabras: la palabra rechazada se "
                        f"espera {waited:.1f} ms")
    faults = [f for f in c.faults + c.chip.faults]
    if c.chip.told_while_stuck:
        faults.append(f"{c.chip.told_while_stuck} escrituras a un chip que se habia rendido")
    if faults:
        failures.extend(f"un sector que deja de aceptar palabras: {f}" for f in faults)
    c.power_cycle()
    r = play.boot(90)
    if play.game() != paused:
        failures.append("un sector que deja de aceptar palabras: la pausa no vuelve")
    elif not faults:
        print("  un sector que deja de aceptar palabras a mitad de partida: el "
              "guardado pasa a la otra mitad y la pausa vuelve")

    play = Play(rom_path, chip=THE_BOARD, battery=True)
    c = play.cart
    play.boot()
    play.to_the_game()
    c.chip.deaf.update((FIRST, FIRST + SLOT))
    play.pause()
    paused = play.game()
    r = c.report()
    calls = len(c.calls)
    if not r["dead"] or r["refused"] != 1:
        failures.append(f"un chip que no acepta nada: no se da por perdido "
                        f"(perdido {r['dead']}, rechazos {r['refused']})")
    play.play_on()
    play.pause()
    if len(c.calls) != calls:
        failures.append("un chip que no acepta nada: se le sigue escribiendo")
    if not play.game()[1]:
        failures.append("un chip que no acepta nada: el juego no sigue")
    faults = [f for f in c.faults + c.chip.faults]
    failures.extend(f"un chip que no acepta nada: {f}" for f in faults)
    paused = play.game()
    c.chip.deaf.clear()
    c.power_cycle()
    r = play.boot(140)
    if play.game() != paused or r["boot"] != RAM_NEWER:
        failures.append("un chip que no acepta nada: con pila, la RAM no trae la pausa "
                        f"('{BOOT[r['boot']]}')")
    # THE RAM WAS AHEAD OF THE FLASH, then, and the power-on that found it
    # so brought the flash level: in the other half, the old one untouched.
    _clean(play, failures, "la RAM por delante de la flash")
    if c.chip.erased != [] or r["fresh"] != 1 or r["gen"] != 2:
        failures.append(f"la RAM por delante: el diario no se reescribe en la otra mitad "
                        f"(nuevos {r['fresh']}, generacion {r['gen']})")
    c.battery = False
    c.power_cycle()
    r = play.boot(90)
    if r["boot"] != FROM_FLASH or play.game() != paused:
        failures.append("la RAM por delante: puesta al dia la flash, sin pila la pausa no vuelve")
    elif not faults:
        print("  un chip que no acepta nada: se deja en paz hasta el siguiente "
              "encendido, el juego sigue y la RAM de guardado guarda; con pila, "
              "lo que no llego a la flash llega al encender")

    # A WORD THAT IS NOT ERASED WHERE THE JOURNAL GOES ON — something no
    # reading at power-on shows, since it was put there after. The chip is
    # not asked to do what it cannot: the word is looked at first, and the
    # save goes to the other half.
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    if play.journal() is None:
        failures.append("una palabra sin borrar donde sigue el diario: no hay diario")
        return
    end = FIRST + play.journal()["end"]
    c.mem[end:end + 2] = b"\x00\x00"
    play.pause()
    paused = play.game()
    r = c.report()
    if r["dead"] or r["fresh"] != 2 or c.chip.doomed:
        failures.append(f"una palabra sin borrar donde sigue el diario: nuevos {r['fresh']}, "
                        f"{c.chip.doomed} programadas donde no caben")
    c.power_cycle()
    play.boot(90)
    if play.game() != paused:
        failures.append("una palabra sin borrar donde sigue el diario: la pausa no vuelve")
    _clean(play, failures, "una palabra sin borrar donde sigue el diario")

    # A SECTOR THAT SAYS IT ERASED AND DID NOT QUITE, at the first power-on
    # of a chip with another game's leavings: it is read back before it is
    # believed, and the journal starts in the other one.
    play = Play(rom_path, chip=dict(THE_BOARD, weak=(FIRST,)))
    c = play.cart
    c.mem[FIRST:FIRST + 0x20000] = c.rng.randbytes(0x20000)
    r = play.boot(200)
    j = journal_read(play.sector(1))
    if r["dead"] or r["flash"] != 1 or j is None or journal_read(play.sector(0)):
        failures.append(f"un sector que borra mal: el diario no empieza en el otro "
                        f"(perdido {r['dead']}, error {r['error']})")
    play.to_the_game()
    play.pause()
    paused = play.game()
    c.power_cycle()
    play.boot(90)
    if play.game() != paused:
        failures.append("un sector que borra mal: la pausa no vuelve")
    _clean(play, failures, "un sector que borra mal")
    # ONE WORD THAT WILL NOT TAKE, a little way into a long record: what
    # is left of the record is not sent after it. The save starts again in
    # the other half, and this one gets nothing more.
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    if play.journal() is None:
        failures.append("una palabra que no entra: no hay diario")
        return
    bad = FIRST + play.journal()["end"] + 6 + 32
    c.chip.deaf_words.add(bad)
    play.pause()
    paused = play.game()
    r = c.report()
    log = c.chip.program_log
    after = [a for a in log[log.index(bad) + 1:] if FIRST <= a < FIRST + SLOT] if bad in log else None
    if after is None or after or r["dead"] or r["fresh"] != 2:
        failures.append(f"una palabra que no entra: tras ella se siguen mandando "
                        f"{len(after) if after is not None else '?'} a la misma mitad "
                        f"(nuevos {r['fresh']})")
    c.power_cycle()
    play.boot(90)
    if play.game() != paused:
        failures.append("una palabra que no entra: la pausa no vuelve")
    _clean(play, failures, "una palabra que no entra")

    # A WORD THAT READS BACK RIGHT AS IT IS WRITTEN AND WRONG A MOMENT
    # LATER — a cell that barely took. Every word is read again once the
    # chip is a ROM, and what does not hold is written afresh elsewhere.
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    if play.journal() is None:
        failures.append("una palabra que no aguanta: no hay diario")
        return
    c.chip.fickle.add(FIRST + play.journal()["end"] + 8)
    play.pause()
    paused = play.game()
    r = c.report()
    if r["dead"] or r["fresh"] != 2 or c.chip.fickle:
        failures.append(f"una palabra que no aguanta: nuevos {r['fresh']}, perdido {r['dead']}")
    c.power_cycle()
    play.boot(90)
    if play.game() != paused:
        failures.append("una palabra que no aguanta: la pausa no vuelve")
    _clean(play, failures, "una palabra que no aguanta")

    # ONE BYTE THAT IS NOT ERASED, in a sector otherwise clean — where a
    # look that skipped any of it would miss it. In the sector about to be
    # written it means an erase first; past the end of a journal it means
    # nothing more is added there.
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    c.mem[FIRST + 0x1005] = b"\x00"
    r = play.boot(200)
    if r["flash"] != 1 or c.chip.erased != [FIRST]:
        failures.append(f"un byte sin borrar en un sector limpio: no se borra antes de usarlo "
                        f"({[hex(b) for b in c.chip.erased]})")
    play.to_the_game()
    play.pause()
    paused = play.game()
    c.mem[FIRST + 0x500D] = b"\x7f"
    c.power_cycle()
    r = play.boot(200)
    if play.game() != paused or r["fresh"] != 1 or r["gen"] != 2:
        failures.append(f"un byte sin borrar pasado el diario: se sigue anadiendo ahi "
                        f"(nuevos {r['fresh']}, generacion {r['gen']})")
    _clean(play, failures, "un byte sin borrar")

    # A HALF OF TWO SECTORS, both with another game's leavings: both are
    # erased, not the first alone.
    play = Play(rom_path, chip=dict(wiring=WIRE_D0D1, regions=((512, 0x8000),)))
    c = play.cart
    c.mem[FIRST:FIRST + 0x10000] = c.rng.randbytes(0x10000)
    r = play.boot(200)
    if r["flash"] != 1 or c.chip.erased != [FIRST, FIRST + 0x8000] or \
            journal_read(play.sector(0)) is None:
        failures.append(f"una mitad de dos sectores con restos: borrados "
                        f"{[hex(b) for b in c.chip.erased]}")
    _clean(play, failures, "una mitad de dos sectores con restos")

    # ...AND ONE THAT WILL NOT ERASE AT ALL: waited for as long as the
    # cartridge has patience for a sector and no longer, the chip brought
    # back to reading before anything else, and the other sector. (The
    # patience is a dozen seconds on the console; cut short here, once the
    # cartridge has set it, or this is forty million instructions.)
    play = Play(rom_path, chip=dict(THE_BOARD, deaf=(FIRST,)))
    c = play.cart
    c.mem[FIRST:FIRST + 0x20000] = c.rng.randbytes(0x20000)
    for _ in range(30):
        if len(c.calls) >= 2:           # asked what it is, and who
            break
        c.run(1)
    patience = c.core.memory.u32[c.report()["erase_patience_at"]]
    # Between seven seconds and half a minute of looks, at the 1.7
    # microseconds one takes: more than a sector at its slowest, and less
    # than anybody would wait with the console in their hand.
    if len(c.calls) != 2 or not 0x400000 <= patience <= 0x1000000:
        failures.append(f"un sector que no se deja borrar: la paciencia es {patience:#x} "
                        f"miradas tras {len(c.calls)} llamadas")
    c.set_erase_patience(20000)
    r = play.boot(200)
    erase = [steps for name, steps, _ in c.calls if name == "cartsave_ram_erase"]
    if r["dead"] or r["flash"] != 1 or journal_read(play.sector(1)) is None:
        failures.append(f"un sector que no se deja borrar: el diario no empieza en el otro "
                        f"(perdido {r['dead']}, error {r['error']})")
    if len(erase) != 1 or not 20000 * 4 <= erase[0] <= 20000 * 12:
        failures.append(f"un sector que no se deja borrar: {len(erase)} intentos, "
                        f"{erase[:1]} instrucciones esperando")
    play.to_the_game()
    play.pause()
    paused = play.game()
    c.power_cycle()
    play.boot(90)
    if play.game() != paused:
        failures.append("un sector que no se deja borrar: la pausa no vuelve")
    for f in c.faults + c.chip.faults:
        failures.append(f"un sector que no se deja borrar: {f}")
    # ...and with BOTH halves in need of an erase it will not give, the
    # patience is spent once, not once a half: the second is not tried.
    play = Play(rom_path, chip=dict(THE_BOARD, deaf=(FIRST, FIRST + SLOT)), battery=True)
    c = play.cart
    c.mem[FIRST:FIRST + 0x100] = c.rng.randbytes(0x100)
    c.mem[FIRST + SLOT:FIRST + SLOT + 0x100] = c.rng.randbytes(0x100)
    for _ in range(30):
        if len(c.calls) >= 2:
            break
        c.run(1)
    c.set_erase_patience(20000)
    r = play.boot(200)
    erase = [steps for name, steps, _ in c.calls if name == "cartsave_ram_erase"]
    if not r["dead"] or r["refused"] != 1 or len(erase) != 1 or c.chip.erased != [FIRST]:
        failures.append(f"dos sectores que no se dejan borrar: {len(erase)} esperas "
                        f"(perdido {r['dead']})")
    play.to_the_game()
    play.pause()
    if not play.game()[1] or len(c.calls) != 3:
        failures.append("dos sectores que no se dejan borrar: el juego no sigue sin la flash")
    for f in c.faults + c.chip.faults:
        failures.append(f"dos sectores que no se dejan borrar: {f}")
    if not any("sin borrar" in f or "borra mal" in f or "deja borrar" in f or
               "dejan borrar" in f or "no entra" in f or
               "no aguanta" in f or "dos sectores" in f or "un byte" in f for f in failures):
        print("  una palabra sin borrar donde sigue el diario, otra que no "
              "aguanta lo escrito, un sector que borra mal al estrenarse y otro "
              "que no se deja borrar: no se le pide al chip lo que no puede, no "
              "se le cree sin releer ni se le espera de mas, y el guardado va a "
              "la otra mitad")

    # A chip that never finishes a word. The routine gives up on the word
    # and then CANNOT go back: the cartridge it would return to is not a
    # ROM while the chip is busy. It stays in RAM asking, and says so the
    # only way left to it — a red screen, and the power switch.
    play = Play(rom_path, chip=dict(THE_BOARD, slow_words=1 << 40), battery=True)
    c = play.cart
    c.step_limit = 2_500_000
    play.boot(30)
    mem = c.core.memory
    if c.faults != ["cartsave_ram_program: never came back"] or c.chip.mode != FlashChip.PROGRAM:
        failures.append(f"un chip que no termina una palabra: {c.faults[:2]}")
    elif mem.u16[0x04000000] != 0 or mem.u16[0x05000000] != 0x001F:
        failures.append("un chip que no termina una palabra: la rutina sigue en RAM "
                        "preguntando y la pantalla no lo dice")
    else:
        print("  un chip que no termina una palabra: la rutina no vuelve a un "
              "cartucho que no es una ROM; se queda en RAM preguntando, con la "
              "pantalla en rojo")


def _a_full_sector(rom_path, failures):
    """A journal with under a quarter of its sector left is written afresh
    in the other one at power-on, where nobody is waiting for it."""
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    play.pause()
    paused = play.game()
    j = play.journal()
    half = c.report()["half_bytes"]
    if j is None or not half:
        failures.append("un sector casi lleno: no hay diario que llenar")
        return
    # Records and commits of nothing new, written as the journal would
    # have, until three quarters of the sector and a little are taken.
    pos, seq = j["end"], j["seq"]
    filler = journal_record(0x7000, bytes(range(200)))
    image = bytearray(j["image"])
    image[0x7000:0x7000 + 200] = bytes(range(200))
    while pos < half * 3 // 4 + 0x400:
        seq += 1
        commit = journal_record(0xC0DE, seq.to_bytes(4, "little")
                                + image_hash(image).to_bytes(4, "little"))
        c.mem[FIRST + pos:FIRST + pos + len(filler) + len(commit)] = filler + commit
        pos += len(filler) + len(commit)
    if journal_read(play.sector(0))["end"] != pos:
        failures.append("un sector casi lleno: no consigo fabricarlo")
        return
    c.power_cycle()
    r = play.boot(400)
    j = play.journal()
    if j is None:
        failures.append("un sector casi lleno: tras encender no queda diario")
        return
    if c.chip.erased != [] or r["fresh"] != 1 or r["gen"] != 2 or j["gen"] != 2:
        failures.append(f"un sector casi lleno: al encender no se pasa a la otra mitad "
                        f"(generacion {r['gen']}, nuevos {r['fresh']})")
    if bytes(c.mem[FIRST + pos - 8:FIRST + pos]) == b"\xff" * 8:
        failures.append("un sector casi lleno: se borro la mitad vieja, que es la reserva")
    if j["image"] != bytes(image) or play.game() != paused:
        failures.append("un sector casi lleno: lo guardado no pasa entero a la otra mitad")
    _clean(play, failures, "un sector casi lleno")
    # ...and the time after that it is the first half's turn, erased now.
    pos, seq = j["end"], j["seq"]
    while pos < half * 3 // 4 + 0x400:
        seq += 1
        commit = journal_record(0xC0DE, seq.to_bytes(4, "little")
                                + image_hash(image).to_bytes(4, "little"))
        c.mem[FIRST + SLOT + pos:FIRST + SLOT + pos + len(filler) + len(commit)] = filler + commit
        pos += len(filler) + len(commit)
    c.power_cycle()
    r = play.boot(400)
    j = play.journal() or dict(gen=0, image=b"")
    if c.chip.erased != [FIRST] or j["gen"] != 3 or j["image"] != bytes(image) \
            or play.game() != paused:
        failures.append(f"un sector casi lleno, la segunda vez: borrados "
                        f"{[hex(b) for b in c.chip.erased]}, generacion {j['gen']}")
    elif not any("casi lleno" in f for f in failures):
        erase = max(cy for name, _, cy in c.calls if name == "cartsave_ram_erase")
        print("  un diario con menos de un cuarto de sector libre se reescribe "
              "en la otra mitad al encender, y la vieja queda de reserva hasta "
              f"que le toca (un borrado, {erase * 1000 / 16777216:.0f} ms del modelo)")
    _clean(play, failures, "un sector casi lleno, la segunda vez")


def _waits_and_restarts(rom_path, failures):
    """A restart by the four keys takes what it changes with it: it lets go
    of the paused game, and that goes to the flash before the save is read
    again from the beginning. (That the flash waits for a match over the
    cable to end is run_link.py's flash_wait_check, with two consoles.)"""
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    play.pause()
    if not play.game()[1]:
        failures.append("el reinicio de las cuatro teclas: no llego a la pausa")
    words = c.chip.programmed
    c.run(4, "A", "B", "START", "SELECT")
    c.core.set_keys()
    c.run(90)
    r = c.report()
    # The save RAM kept its contents — nothing was switched off — so it is
    # believed, and is level with the flash only if the flush happened.
    if r["boot"] != LEVEL or r["fresh"] or c.chip.erased or c.chip.programmed == words:
        failures.append(f"el reinicio de las cuatro teclas: lo que cambia no llega a la "
                        f"flash antes ('{BOOT[r['boot']]}', nuevos {r['fresh']})")
    if play.game()[0] and play.game()[1]:
        failures.append("el reinicio de las cuatro teclas: vuelve la partida en pausa")
    _clean(play, failures, "el reinicio de las cuatro teclas")
    c.power_cycle()
    play.boot(90)
    if play.game()[0] and play.game()[1]:
        failures.append("el reinicio de las cuatro teclas: apagada despues, vuelve la pausa")
    if not any("reinicio" in f for f in failures):
        print("  el reinicio de las cuatro teclas suelta la partida en pausa, y "
              "eso llega a la flash antes de reiniciar")


def _page(cart):
    """The screen's rows of text, as name -> the rest of the line."""
    out = {}
    for r in range(20):
        words = tilemap_text(cart.core, r).split()
        if len(words) > 1:
            for n in (2, 1):
                name = " ".join(words[:n])
                if name in ("FLASH CHIP", "CHIP ID", "SAVE RAM") or n == 1:  # two-word names first
                    out.setdefault(name, " ".join(words[n:]))
                    break
        elif words:
            out[words[0]] = ""
    return out


def _the_page(rom_path, failures):
    """L+R+A held at power-on: a page of what the cartridge turned out to
    be, BEFORE ANYTHING IS WRITTEN TO ITS CHIP — on a board that does not
    take to being written, this is as far as it gets, and it has to get
    this far. START goes on and the chip is written; SELECT goes on
    without the chip for the session."""
    # The first power-on of all: the journal is still to be made.
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    c.run(70, "L", "R", "A")
    page = _page(c)
    want = {"FLASH CHIP": "ANSWERS", "QUERY": "0052 0051 005A", "WIRING": "D0 D1",
            "COMMANDS": "AMD 0002", "CHIP": "16384K 128X128K", "CHIP ID": "0002 227D",
            "JOURNAL": "IN USE", "STARTED": "FIRST: EMPTY", "SAVE RAM": "LOST",
            "GENERATION": "0 SAVE 0", "USED": "0 OF 128K", "NEXT": "WRITE AFRESH",
            "START:": "GO ON", "SELECT:": "PLAY WITHOUT FLASH"}
    wrong = {k: page.get(k) for k, v in want.items() if page.get(k) != v}
    if wrong:
        failures.append(f"la pagina del cartucho, en el primer encendido: {wrong}")
    if c.chip.programmed or c.chip.erased or len(c.calls) != 2:
        failures.append(f"la pagina del cartucho: se ha escrito en el chip antes de ensenarla "
                        f"({c.chip.programmed} palabras, {len(c.calls)} llamadas)")
    c.core.set_keys()
    c.run(60)
    if "CARTRIDGE" not in tilemap_text(c.core, 1) or c.chip.programmed or len(c.calls) != 2:
        failures.append("la pagina del cartucho: se va sola, o se escribe en el chip con ella puesta")
    c.tap("START", 4, 40)
    play.boot(30)
    r = c.report()
    if "CARTRIDGE" in tilemap_text(c.core, 1) or not c.chip.programmed or r["afresh"] \
            or play.journal() is None:
        failures.append("START en la pagina del cartucho: no sigue, o el diario no se escribe")
    play.to_the_game()
    play.pause()
    paused = play.game()
    _clean(play, failures, "tras la pagina del cartucho")

    # The next power-on: read from the flash, and nothing owed.
    c.power_cycle()
    c.run(70, "L", "R", "A")
    page = _page(c)
    want = {"JOURNAL": "IN USE", "STARTED": "FROM FLASH", "SAVE RAM": "LOST",
            "NEXT": "ADD TO IT", "CHIP ID": "0002 227D"}
    wrong = {k: page.get(k) for k, v in want.items() if page.get(k) != v}
    if wrong or page.get("USED", "").split()[-2:] != ["OF", "128K"] \
            or page.get("GENERATION", "").split()[0] != "1":
        failures.append(f"la pagina del cartucho, en el segundo encendido: {wrong or page}")
    c.core.set_keys()
    c.run(10)
    c.tap("START", 4, 40)
    if "CARTRIDGE" in tilemap_text(c.core, 1) or play.game() != paused:
        failures.append("START no sale de la pagina del cartucho a la partida en pausa")
    _clean(play, failures, "la pagina del cartucho")

    # SELECT: on without the chip. Not a word to it all session, the save
    # RAM as on any other cartridge — and with a battery, what it kept is
    # what the next power-on starts the journal from.
    play = Play(rom_path, chip=THE_BOARD, battery=True)
    c = play.cart
    c.run(70, "L", "R", "A")
    c.core.set_keys()
    c.run(10)
    c.tap("SELECT", 4, 40)
    r = c.report()
    if "CARTRIDGE" in tilemap_text(c.core, 1) or not (r["off"] and r["dead"]):
        failures.append(f"SELECT en la pagina del cartucho: no sigue sin el chip "
                        f"(apagado {r['off']}, perdido {r['dead']})")
    play.to_the_game()
    play.pause()
    paused = play.game()
    play.play_on()
    play.pause()
    if not paused[1] or c.chip.programmed or c.chip.erased or len(c.calls) != 2:
        failures.append(f"SELECT en la pagina del cartucho: se escribe en el chip igualmente "
                        f"({c.chip.programmed} palabras)")
    paused = play.game()
    c.power_cycle()
    r = play.boot(200)
    if r["boot"] != FIRST_KEPT or r["off"] or play.game() != paused:
        failures.append(f"tras una sesion sin el chip, con pila: '{BOOT[r['boot']]}', y la "
                        "pausa no vuelve")
    _clean(play, failures, "tras una sesion sin el chip")

    play = Play(rom_path, battery=True)
    c = play.cart
    c.run(40, "L", "R", "A")
    page = _page(c)
    want = {"FLASH CHIP": "SILENT", "QUERY": "0993 20CE 4610", "JOURNAL": "NO CHIP",
            "STARTED": "SAVE RAM ONLY", "PRESS": "START"}
    wrong = {k: page.get(k) for k, v in want.items() if page.get(k) != v}
    if wrong or "WIRING" in page or "CHIP ID" in page or "SELECT:" in page:
        failures.append(f"la pagina del cartucho, sin chip: {wrong or page}")
    c.core.set_keys()
    c.run(10)
    c.tap("SELECT", 4, 20)
    if "CARTRIDGE" not in tilemap_text(c.core, 1):
        failures.append("la pagina del cartucho, sin chip: SELECT sale de ella")
    c.tap("START", 4, 40)
    title = " ".join(tilemap_text(c.core, r) for r in range(20))
    if "CARTRIDGE" in title or play.game()[0]:
        failures.append("START no sale de la pagina del cartucho al titulo")

    # A chip that answers and is not used says why, and the bytes it went by.
    play = Play(rom_path, chip=dict(command_set=1, wiring=WIRE_STRAIGHT), battery=True)
    play.cart.run(40, "L", "R", "A")
    page = _page(play.cart)
    if (page.get("COMMANDS"), page.get("JOURNAL"), page.get("TABLE")) != \
            ("INTEL 0001", "NOT AMD", "18 01 7F 00 00 02"):
        failures.append(f"la pagina del cartucho, con un chip Intel: {page}")
    # ...and one never asked for, when the keys are not held, is not shown.
    play = Play(rom_path, chip=THE_BOARD)
    play.cart.run(70, "L", "R")
    if "CARTRIDGE" in tilemap_text(play.cart.core, 1):
        failures.append("la pagina del cartucho sale sin las tres teclas")
    # ASKED FOR, IT COMES AT ONCE: not after the publishers' logos, which
    # every other check here skips and a console at power-on does not.
    from . import harness
    harness.SHOW_SPLASH[0] = True
    try:
        play = Play(rom_path, chip=THE_BOARD)
        play.cart.run(70, "L", "R", "A")
    finally:
        harness.SHOW_SPLASH[0] = False
    if "CARTRIDGE" not in tilemap_text(play.cart.core, 1):
        failures.append("la pagina del cartucho espera a que pasen los logos")
    if not any("pagina" in f or "sesion sin el chip" in f for f in failures):
        print("  L+R+A al encender: la pagina dice que contesto el chip, como "
              "esta cableado y de donde se ha leido la partida, antes de "
              "escribirle una palabra; START sigue, y SELECT sigue sin la flash")


def _what_a_save_costs(rom_path, failures):
    """How long the interrupts are off for the saves a game makes, at the
    model's pace (a word in sixty microseconds), and how much stack a save
    takes on a board with a chip: the plain cartridge's checks never see
    that path."""
    play = Play(rom_path, chip=THE_BOARD)
    c = play.cart
    play.boot()
    play.to_the_game()
    bounds = stack_bounds(rom_path)
    if bounds is None:
        skip("el ELF no dice donde estan las pilas")
        return
    stack_paint(c.core, bounds)
    ms = lambda calls: sum(cy for _, _, cy in calls) * 1000 / 16777216
    at = len(c.calls)
    play.pause()
    first = c.calls[at:]
    at = len(c.calls)
    play.play_on()
    resume = c.calls[at:]
    c.run(120)
    at = len(c.calls)
    play.pause()
    again = c.calls[at:]
    used, irq = stack_depth(c.core, bounds)
    bss_end, sp_usr, _ = bounds
    if not first or not resume or not again:
        failures.append("lo que cuesta guardar: alguna de las tres no escribe")
        return
    # THE SECOND PAUSE IS A SAVE IN PIECES: the paused game differs from the
    # last one here and there, and only the blocks that changed go — several
    # runs in one commit, every one of which has to arrive.
    runs = max(len(commit) for commit in journal_commits(play.sector(0))[-3:])
    if runs < 2:
        failures.append("lo que cuesta guardar: la segunda pausa ya no se guarda a trozos, "
                        "y nada comprueba que lleguen todos")
    _clean(play, failures, "la segunda pausa, a trozos")
    longest = max(cy for _, _, cy in first + resume + again) * 1000 / 16777216
    if ms(resume) > 4 or ms(again) > ms(first) or ms(first) > 120 or longest > 6:
        failures.append(f"lo que cuesta guardar: la primera pausa {ms(first):.1f} ms, "
                        f"reanudar {ms(resume):.1f} ms, otra pausa {ms(again):.1f} ms")
    if used > (sp_usr - bss_end) - 1024:
        failures.append(f"guardar en la flash usa {used} bytes de pila de "
                        f"{sp_usr - bss_end}: menos de 1KB de sobra")
    else:
        print(f"  lo que cuesta escribir en el chip: la primera pausa "
              f"{ms(first):.1f} ms, reanudar {ms(resume):.1f} ms, otra pausa "
              f"{ms(again):.1f} ms, nunca mas de {longest:.1f} ms seguidos con las "
              f"interrupciones quitadas; {used} bytes de pila de {sp_usr - bss_end}")
