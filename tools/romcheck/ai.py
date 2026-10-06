"""
The computer player, alone and as a partner.
"""
import os

from .harness import (
    AI_COOP_AWARE, AI_PLAN_HAVE, AI_SMART, AI_SOFT_DROP, AI_TARGET_X, DEMO_START_FRAME,
    KEYS, LEADER_HEAD_TY, TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH,
    game_offsets, game_state_address, load, press_start,
    run, tilemap_text,
)


def computer_check(rom_path):
    """VERSUS and WITH COMPUTER: the two modes that need no second console.

    The computer is player 2 in both (main.asm.txt:3736-3749), and nothing
    here touches the pad — so every cell that appears on its board was placed
    by computerMove. What this asks is that it PLAYS: pieces land, they do not
    all land in one column, and the board it lands them on is the right one
    for the mode (its own in a race, the shared twelve-wide one in WITH).
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH

    def start(entry, frames):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        run(core, 8)
        press_start(core)               # title -> game select
        run(core, 10)
        for _ in range(entry):          # down to the mode
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)   # -> LEVEL SETTINGS
        press_start(core); run(core, frames)
        return core, screen

    def board(core, which):
        addr = base + off["field"] + which * PF
        return [[core.memory.u8[addr + y * TENGEN_PF_WIDTH + x]
                 for x in range(TENGEN_PF_WIDTH)] for y in range(TENGEN_PF_HEIGHT)]

    # HOW LONG TO WATCH, AND WHY IT IS NOT LONGER. This used to run six
    # thousand frames — a hundred seconds at level 0 — which was fine while a
    # finished game sat on its plaque for ever. It does not any more: the
    # plaque counts down to the high scores and the high scores count down to
    # the title (GAMEOVER_HOLD_FRAMES), and from the title the attract demo's
    # own clock starts a fresh 1 PLAYER game, whose player-2 field is walled.
    # So a board that filled up before the sample came back with forty cells
    # in the second field and this read it as "two boards". Long enough for
    # the computer to stack a corner, short enough that it is still stacking.
    WATCH = 2500

    # VERSUS: two boards. The computer plays its own; ours stays as it was
    # apart from the piece gravity drops on it.
    # Watched as it goes, not read once at the end: a computer that has just
    # cleared a row has little left on its board, and when that happens
    # depends on the deal, which moves with how long the boot took (the
    # seed advances once per main-loop turn).
    core, screen = start(3, 0)
    cols, cells = set(), 0
    for _ in range(WATCH // 50):
        run(core, 50)
        comp = board(core, 1)
        cols |= {x for row in comp for x in range(1, TENGEN_PF_WIDTH - 1) if row[x]}
        cells = max(cells, sum(1 for row in comp
                               for x in range(1, TENGEN_PF_WIDTH - 1) if row[x]))
    if cells < 8:
        failures.append(f"en VERSUS el ordenador solo asento {cells} celdas: no juega")
    elif len(cols) < 3:
        failures.append(f"el ordenador amontona todo en {len(cols)} columna(s): "
                         "no esta eligiendo")
    else:
        print(f"  VERSUS: el ordenador asento {cells} celdas en {len(cols)} columnas")

    # WITH: one twelve-wide board, and the computer plays into it.
    core2, screen2 = start(4, WATCH)
    shared = board(core2, 0)
    other = sum(1 for row in board(core2, 1) for v in row if v)
    wide = sum(1 for row in shared if row[0] or row[TENGEN_PF_WIDTH - 1])
    if "-LINES" in tilemap_text(core2, LEADER_HEAD_TY, 0, 30):
        failures.append("la partida de WITH COMPUTER ya habia terminado cuando "
                         "se miro el tablero: no prueba nada")
    elif other:
        failures.append(f"WITH COMPUTER usa dos campos ({other} celdas en el "
                         "segundo): deberia compartir uno")
    elif not any(v for row in shared for v in row):
        failures.append("en WITH COMPUTER no se asento nada")
    else:
        print(f"  WITH COMPUTER: un solo campo compartido, {wide} filas "
               "llegan a las columnas que solo existen en coop")

    # ...AND IT STILL PLAYS AFTER THE ATTRACT DEMO HAS RUN. The demo puts the
    # computer on PLAYER 1, which is the cartridge's own arrangement, and for
    # a while nothing here put it back — so a console left alone for the
    # twenty-two seconds the demo's clock takes started VERSUS and WITH
    # COMPUTER with a computer that never moved. Every check above drove the
    # menus faster than that clock, so every one of them passed.
    core3, screen3 = load(rom_path)
    run(core3, DEMO_START_FRAME + 400)
    if "TETRIS" in tilemap_text(core3, 4, 0, 30):
        failures.append("la demo no arranco: esta comprobacion no prueba nada")
    press_start(core3); run(core3, 20)        # out of the demo -> GAME SELECT
    for _ in range(4):
        core3.set_keys(KEYS["DOWN"]); run(core3, 4)
        core3.set_keys(); run(core3, 10)
    press_start(core3); run(core3, 12)
    press_start(core3); run(core3, 30)
    # PLAYING IS MOVING. At the cartridge's pace — no soft drop, gravity
    # alone — a few thousand frames is only a handful of pieces, so counting
    # cells says little; a computer that is playing shifts its piece off the
    # spawn column, and one left on the demo's slot never does.
    x_at = base + off["x"] + off["stride"]
    xs = set()
    for _ in range(2500):
        core3.run_frame()
        xs.add(core3.memory.u8[x_at])
    if len(xs) < 3:
        failures.append(f"tras la demo el ordenador no juega: su pieza solo "
                         f"ha estado en las columnas {sorted(xs)}")
    else:
        print(f"  ...y sigue jugando despues de la demo de atraccion "
               f"(su pieza pasa por {len(xs)} columnas)")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el jugador COMPUTER juega solo, en el tablero de cada modo.")
    return 0


def coop_ai_check(rom_path):
    """THE COMPUTER AS A PARTNER, and the door it is behind.

    `computerMove` reads the settled board and nothing else. On the shared
    twelve-wide field of WITH COMPUTER that leaves out the one thing that
    decides everything — the OTHER falling piece, which is solid to this one
    — so both players score the same columns with the same routine, pick the
    same one, and shoulder each other all the way down. Measured on the host:
    48% of every shift either of them asked for was refused, and nine in ten
    of those by the partner. See TengenAi's `coop_aware`.

    That is what the cartridge does, though, so the fix is behind the same
    chord as the pause menu and the hidden tunes, and this checks BOTH
    halves of that door on a running ROM.

    The fixture is one board and three partner positions: flat at row 10
    everywhere, with a two-row well at columns 8 and 9. The well is the
    deepest place on the board and the computer wants it — unless the
    partner's piece is hanging over it, in which case what is about to be in
    it is exactly what fills it. Without the chord the computer takes the
    well whatever the partner does; with it, never.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    ai, why = game_state_address(rom_path, "g_ai")
    last_piece, why2 = game_state_address(rom_path, "g_ai_last_piece")
    if ai is None or last_piece is None:
        print(f"SALTADO: {why or why2}")
        return 0

    failures = []
    WELL = (8, 9)              # the two columns of the well, in storage terms
    WELL_TARGET = 10           # ...as computerMove numbers them: +2 for the walls
    TT_O = 3

    def start(entry, chord):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        run(core, 8)
        press_start(core); run(core, 10)
        if chord:
            # The chord is rung on GAME SELECT, which is where unlock_cheats
            # lives; at the title it only cycles the skin.
            core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
            core.set_keys(); run(core, 24)
        for _ in range(entry):
            core.set_keys(KEYS["DOWN"]); run(core, 4)
            core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)
        press_start(core); run(core, 60)
        return core, screen

    def plant(core, partner_x):
        addr = base + off["field"]
        for y in range(TENGEN_PF_HEIGHT):
            for x in range(TENGEN_PF_WIDTH):
                floor = 12 if x in WELL else 10
                core.memory.u8[addr + y * TENGEN_PF_WIDTH + x] = 15 if y >= floor else 0
        # The partner, hanging over the well.
        core.memory.u8[base + off["current"]] = TT_O
        core.memory.u8[base + off["orientation"]] = 0
        core.memory.u8[base + off["x"]] = partner_x
        core.memory.u8[base + off["y"]] = 6
        # ...and a fresh piece in the computer's hand, which is what makes
        # ai_input call tengen_ai_choose on the very next frame.
        st = off["stride"]
        core.memory.u8[base + off["current"] + st] = TT_O
        core.memory.u8[base + off["orientation"] + st] = 0
        core.memory.u8[base + off["y"] + st] = 4
        core.memory.u8[base + off["x"] + st] = 9
        # ...AND TELL IT SO. ai_input chooses again when the piece in its hand
        # differs from the last one it chose for, and this plants an O: if
        # the game happened to have dealt it an O already, it never chose at
        # all and the column read back was from another board. Which piece
        # that is depends on the seed, and the seed on how many main-loop
        # turns the menus took — which a faster build of the same code
        # changes. So the fixture says "new piece" outright.
        core.memory.u8[last_piece] = 0
        run(core, 2)

    for chord in (False, True):
        seen = []
        for partner_x in (9, 10, 11):
            core, screen = start(4, chord)
            _ = screen
            # At the cartridge's pace: computerMove never holds Down.
            if core.memory.u8[ai + AI_SOFT_DROP] != 0:
                failures.append("la maquina baja sus piezas con abajo: el "
                                 "cartucho no lo hace (soft_drop deberia ser 0)")
                break
            plant(core, partner_x)
            aware = core.memory.u8[ai + AI_COOP_AWARE]
            if aware != int(chord):
                failures.append(f"con acorde={int(chord)} la maquina lee "
                                 f"coop_aware={aware}")
            smart = core.memory.u8[ai + AI_SMART]
            if smart != int(chord):
                failures.append(f"con acorde={int(chord)} la maquina lee "
                                 f"smart={smart}")
            if chord:
                # The port's computer plans over a few frames (a slice
                # each): its target is the one it has once it has one, not
                # the last piece's still sitting there.
                for _ in range(40):
                    if core.memory.u8[ai + AI_PLAN_HAVE]:
                        break
                    run(core, 1)
                else:
                    failures.append("bajo el acorde la maquina no llego a "
                                     "elegir en 40 frames")
            seen.append(core.memory.u8[ai + AI_TARGET_X])
        if failures:
            break
        if not chord and any(t != WELL_TARGET for t in seen):
            failures.append(f"sin el acorde la maquina deberia ir al pozo "
                             f"siempre: eligio {seen}")
        # Under the chord it is the port's own computer (`smart`), which
        # prices each column it shares with where the partner is landing
        # (W_CROSS) instead of walling the partner's whole corridor off. The
        # O is two columns from its x: at 10 it hangs over the whole well,
        # and that it never takes; at 9 and 11 it shares one column, and
        # filling the well before the partner lands flat on top is a fair
        # trade it is allowed to make.
        if chord and seen[1] == WELL_TARGET:
            failures.append(f"con el acorde la maquina se mete en el pozo que "
                             f"el companero va a tapar: eligio {seen}")
        if not failures:
            print(f"  {'con' if chord else 'sin'} el acorde, con el companero "
                   f"sobre el pozo: columnas {seen}")

    # ...AND A RACE IS NOT A SHARED BOARD. VERSUS has two fields and what
    # falls on the other one is none of this column's business, so the knob
    # stays off there whatever the chord says.
    if not failures:
        core, screen = start(3, True)
        _ = screen
        if core.memory.u8[ai + AI_COOP_AWARE] != 0:
            failures.append("en VERSUS la maquina lee el campo del rival")
        else:
            print("  ...y en VERSUS sigue apagado: son dos tableros")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: bajo el acorde el ordenador lee a su companero, y solo en "
           "el tablero compartido.")
    return 0


def ai_frame_check(rom_path):
    """THE PORT'S COMPUTER THINKS INSIDE A FRAME.

    Under the chord VERSUS and WITH COMPUTER play the port's own computer
    (`smart`), which plans over several frames a slice at a time
    (TENGEN_AI_SMART_BUDGET). A slice too big makes the main loop's turn
    spill into a second frame: the game slows for that frame and the seed,
    which advances once a turn, moves. Measured by the turn: the frames
    between one draw_match and the next must always be one. The first
    version, sixteen placements a frame, took two frames on 140 turns of
    3000 in WITH COMPUTER.
    """
    import subprocess
    elf = rom_path[:-4] + ".elf"
    try:
        out = subprocess.check_output(
            [os.environ.get("NM", "arm-none-eabi-nm"), elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"SALTADO: no pude leer {elf}: {exc}")
        return 0
    dm = next((int(l.split()[0], 16) & ~1 for l in out.splitlines()
               if l.endswith(" draw_match")), None)
    if dm is None:
        print("SALTADO: el ELF no exporta draw_match")
        return 0
    failures = []
    for entry, name in ((3, "VERSUS COMPUTER"), (4, "WITH COMPUTER")):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        run(core, 8)
        press_start(core); run(core, 10)
        core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
        core.set_keys(); run(core, 12)
        for _ in range(entry):
            core.set_keys(KEYS["DOWN"]); run(core, 4)
            core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)
        press_start(core); run(core, 20)
        gaps, last, f0 = {}, None, core.frame_counter
        while core.frame_counter < f0 + 2000:
            core.step()
            # The pipeline: the instruction at draw_match is executing when
            # the PC reads four past it.
            if (core.cpu.pc & ~1) == dm + 4:
                fc = core.frame_counter
                if last is not None:
                    gaps[fc - last] = gaps.get(fc - last, 0) + 1
                last = fc
        turns = sum(gaps.values())
        slow = turns - gaps.get(1, 0)
        if turns < 1500:
            failures.append(f"{name}: solo {turns} vueltas en 2000 frames")
        elif slow:
            failures.append(f"{name}: {slow} de {turns} vueltas ocupan mas de "
                             f"un frame ({gaps})")
        else:
            print(f"  {name}: {turns} vueltas, todas en un frame")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el ordenador del puerto piensa dentro de cada frame.")
    return 0
