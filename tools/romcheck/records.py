"""
The HIGH SCORES table and its per-build pages.
"""
from .harness import (
    CELL_BLOCK, CELL_WALL, KEYS, LEADER_FIRST_TY,
    TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH, game_offsets, game_state_address,
    load, press_start, run, start_game,
    tilemap_text,
)


def _report(failures):
    for f in failures:
        print("FALLA:", f)
    return 1 if failures else 0


def leaderboard_check(rom_path):
    """THE HIGH SCORES TABLE, which the cartridge keeps in memory.

    Four things, all of them the ROM's:

      * the table it comes up with cold — @resetHighScores builds fifteen
        entries of AAA from 17000 down to 3000 in thousands
        (main.asm.txt:5670-5700), which is why HIGH SCORE opens at 017000 and
        not at nothing;
      * a score that beats one of them goes IN, at the right row, pushing the
        rest down and the last off the bottom (L81FF, :342-378);
      * the three initials are typed with Left and Right and taken with A or B
        (L9234, :2709-2762); and
      * the table is still there for the next game.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []

    def rows(core):
        return [tilemap_text(core, LEADER_FIRST_TY + i, 0, 30)
                for i in range(15)]

    def to_gameover(core, score):
        """Plant a score, bury the board, and take the plaque's way out."""
        for i in range(4):
            core.memory.u8[base + off["score"] + i] = (score >> (8 * i)) & 0xFF
        for r in range(TENGEN_PF_HEIGHT):
            for c in range(TENGEN_PF_WIDTH):
                core.memory.u8[base + off["field"] + r * TENGEN_PF_WIDTH + c] = (
                    CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                    else (0 if c == 5 else CELL_BLOCK))
        run(core, 240)
        press_start(core); run(core, 30)

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)

    # The cold table, seen through the panel's own HIGH counter first.
    to_gameover(core, 20)
    table = rows(core)
    wanted = [f"{17 - i}000" for i in range(15)]
    got = ["".join(ch for ch in row if ch.isdigit())[-9:-3] for row in table]
    if not all(w in g for w, g in zip(wanted, got)):
        failures.append(f"la tabla fria no es la del cartucho: {got[:3]} ...")
    elif "AAA" not in table[0]:
        failures.append(f"la primera entrada no sale como AAA: {table[0]!r}")
    else:
        print("  arranca con las quince del cartucho, de 17000 a 3000, todas AAA")

    # ...and back out, then a score that belongs on it.
    press_start(core); run(core, 40)
    press_start(core); run(core, 10)   # title -> game select
    press_start(core); run(core, 12)   # -> level settings
    press_start(core); run(core, 30)   # -> play
    to_gameover(core, 50000)
    table = rows(core)
    if "050" not in table[0] or "017000" not in table[1]:
        failures.append(f"un 50000 no entra en cabeza: {table[0]!r} / {table[1]!r}")
    elif "003000" in "".join(table):
        failures.append("la ultima entrada no se cayo de la tabla")
    else:
        print("  un 50000 entra el primero y empuja a las demas una fila abajo")

    # The initials: Right walks the alphabet, A takes the letter.
    def tap(name, times=1):
        for _ in range(times):
            core.set_keys(KEYS[name]); run(core, 3); core.set_keys(); run(core, 6)

    # UP and DOWN walk the alphabet, LEFT and RIGHT pick the letter, B undoes
    # the last change and A takes the name. B with nothing to undo is an
    # alarm, which cannot be read off the tilemap — the core check for that is
    # that it does not change anything.
    tap("UP", 1); tap("RIGHT")     # B
    tap("UP", 2); tap("RIGHT")     # C
    tap("UP", 3)                   # D
    tap("B")                       # ...and take the D back
    if "BC" not in tilemap_text(core, LEADER_FIRST_TY, 0, 30):
        failures.append(f"B no deshizo solo la ultima letra: "
                         f"{tilemap_text(core, LEADER_FIRST_TY, 0, 30)!r}")
    tap("UP", 3); tap("A")
    run(core, 20)
    if " BCD " not in tilemap_text(core, LEADER_FIRST_TY, 0, 30):
        failures.append("las iniciales no se escriben: "
                         f"{tilemap_text(core, LEADER_FIRST_TY, 0, 30)!r}")
    else:
        print("  arriba/abajo la letra, izquierda/derecha el hueco, B deshace, "
               "A acepta")

    # ...AND A ONLY ACEPTA ON THE LAST LETTER. Everywhere else it walks to the
    # next one, which is what A means in every other entry field there is; it
    # used to take the whole name from wherever the cursor stood, so the
    # obvious "press A to keep this letter" threw the other two away. The tell
    # is that UP still bites after the press: if A had finished the name,
    # nothing would answer the pad. START finishes from anywhere, which is
    # what it already meant here.
    # A low score, so this entry lands at the bottom and leaves BCD in the
    # first row for the two checks below.
    press_start(core); run(core, 40)
    press_start(core); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 30)
    to_gameover(core, 4000)

    def table_over(frames=40):
        """The whole table, over a blink: the cursor blanks the letter it is
        on for half of every 32 frames, so one reading can miss a letter
        that is there."""
        seen = set()
        for _ in range(frames):
            core.run_frame()
            seen.update(rows(core))
        return seen

    tap("A")                       # first letter: this must NOT take the name
    tap("UP", 1)                   # so this lands on the SECOND letter
    if not any(" AB" in t for t in table_over()):
        failures.append("A en la primera letra no paso a la segunda: "
                         f"{sorted(t for t in table_over() if 'A' in t)!r}")
    else:
        tap("START")               # ...and START takes the name from here
        run(core, 20)
        taken = table_over()
        if not any(" AB" in t for t in taken):
            failures.append("START no cerro el nombre")
        else:
            tap("UP", 1)
            if table_over() != taken:
                failures.append("despues de START el pad sigue escribiendo")
            else:
                print("  A pasa de letra y solo cierra en la tercera; "
                       "START cierra desde cualquiera")

    # AND IT SURVIVES THE POWER GOING OFF, which is the one thing the NES
    # cartridge wanted and could not have: its magic at $04F7 only carries the
    # table across a RESET. Here it is in the GBA's battery-backed SRAM, under
    # the same four letters, and core.reset() is the console being switched
    # off and on again as far as that memory is concerned.
    sram = bytes(core.memory.u8[0x0E000000 + i] for i in range(8))
    if sram[:4] != b"LOGG":
        failures.append(f"la tabla no se guarda en SRAM: {sram[:4]!r}")
    else:
        core.reset(); run(core, 40)
        start_game(core)
        to_gameover(core, 10)
        if "BCD" not in rows(core)[0]:
            failures.append("la tabla no sobrevive al apagado")
        else:
            print("  y sobrevive a apagar la consola, en la SRAM de la pila")
        if failures:
            return _report(failures)
        print("OK: la tabla de records es la del cartucho, se escribe en ella, "
               "y se guarda.")
        return 0

    # And it survives the next game.
    press_start(core); run(core, 40)
    press_start(core); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 30)
    to_gameover(core, 20)
    if "BCD" not in rows(core)[0]:
        failures.append("la tabla no sobrevive a la siguiente partida")
    else:
        print("  y sigue ahi despues de la siguiente partida")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: la tabla de records es la del cartucho, y se escribe en ella.")
    return 0


def tables_check(rom_path):
    """ONE HIGH SCORES TABLE PER BUILD, and they must not see each other.

    A prototype is a different game — the level climbs every ten lines and a
    row goes the frame it completes — so a score made on one does not belong
    beside a score made on the release, and with one table between them the
    easiest build simply owned the page. See LEADER_TABLES in gba/port.h.

    What this does is put a score on the release's table, walk the title's
    skin round to a prototype, and look: the prototype must open on the
    cartridge's own fifteen. Then it puts a different score on the
    prototype's and walks back, and the release's must be exactly as it was
    left — neither overwritten nor joined.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)

    def tap(*keys, hold=4, settle=12):
        core.set_keys(*keys); run(core, hold)
        core.set_keys(); run(core, settle)

    def table():
        return [tilemap_text(core, LEADER_FIRST_TY + i, 0, 30) for i in range(15)]

    def play_and_lose(score):
        """A game on whatever skin is up, ended with `score` on the board."""
        start_game(core)
        for i in range(4):
            core.memory.u8[base + off["score"] + i] = (score >> (8 * i)) & 0xFF
        for r in range(TENGEN_PF_HEIGHT):
            for c in range(TENGEN_PF_WIDTH):
                core.memory.u8[base + off["field"] + r * TENGEN_PF_WIDTH + c] = (
                    CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1)
                    else (0 if c == 5 else CELL_BLOCK))
        run(core, 240)
        press_start(core); run(core, 30)     # the plaque -> HIGH SCORES
        seen = table()
        tap(KEYS["START"]); run(core, 20)    # ...take the name and leave
        press_start(core); run(core, 40)
        return seen

    # The board is played for a few frames on the way to being buried, so the
    # score that lands is the planted one plus whatever those frames paid.
    run(core, 40)
    release_first = play_and_lose(80000)
    if "0800" not in release_first[0]:
        failures.append(f"el 80000 no entro en la tabla del release: "
                         f"{release_first[0]!r}")

    # The chord uncovers the skins; L walks to the first prototype.
    tap(KEYS["L"], KEYS["R"]); run(core, 30)
    proto_first = play_and_lose(60000)
    # The TOP ROW, not the whole table: 8000 is one of the cartridge's own
    # fifteen and would match an 80000 anywhere looser than this. If the two
    # builds shared a table the release's 80000 would be sitting above the
    # prototype's 60000, which is exactly what this asks.
    if "0800" in proto_first[0]:
        failures.append(f"la tabla del prototipo abre con el record del "
                         f"release: {proto_first[0]!r}")
    elif "0600" not in proto_first[0]:
        failures.append(f"el 60000 no entro en la tabla del prototipo: "
                         f"{proto_first[0]!r}")
    elif "017000" not in proto_first[1]:
        failures.append("bajo el record del prototipo no estan las quince del "
                         f"cartucho: {proto_first[1]!r}")
    else:
        print("  el prototipo abre con las quince del cartucho, no con las "
               "del release")

    # ...and back to the release, whose own table must be untouched. L walks
    # the ring backwards, so one press from the first prototype is it.
    tap(KEYS["L"]); run(core, 24)
    back = play_and_lose(10)
    if "0600" in back[0]:
        failures.append(f"la tabla del release abre con el record del "
                         f"prototipo: {back[0]!r}")
    elif "0800" not in back[0]:
        failures.append(f"el release perdio su propio record: {back[0]!r}")
    else:
        print("  y el release conserva el suyo, sin el del prototipo")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada build lleva su propia tabla de records.")
    return 0
