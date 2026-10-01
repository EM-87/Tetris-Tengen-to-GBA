"""
A game being played: the board, the line clear, pause and the
cheat codes, points, the counters, game over, the handicap, the
frame budget.
"""
import os
import subprocess

from .harness import (
    CELL_BLOCK, CELL_WALL, CHARBLOCK_ADDR, CLEAR_WORDS,
    CODE_LEVEL_UP, CODE_LONG_BAR, CODE_UNDO, COL_BOX_L,
    COL_BOX_R, COL_FIELD, COL_FRAME_L, COL_FRAME_R,
    COOP_FIELD_TX, FIELD_X0, FIELD_X1, HANDICAP_ROW,
    KEYS, KEY_DOWN, LEADER_HEAD_TY, LEVEL_ROW,
    MENU_ARROW_BANK, OAM_ADDR, ORIENTATION_BITS, PANEL_RUN_SLOTS,
    PAUSE_ROW0, PAUSE_ROW1, PAUSE_TX, PAUSE_TY,
    PAUSE_W, PF_H, PF_PLAYABLE, PF_W,
    SCREENBLOCK_ADDR, SCREEN_H, SCREEN_TW_TILES, SCREEN_W,
    SWEEP_PAL_BANK, SWEEP_TILES, TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH,
    TENGEN_ROM_ROW_ORIGIN, TILE, WALL_L_X, WALL_R_X,
    describe, fill_rows, game_offsets, game_state_address,
    load, map_row_text, oam_visible, pixels,
    press_start, run, start_game, tilemap_text,
)


def selftest(rom_path):
    core, screen = load(rom_path)
    failures = []

    # The title screen must come up first and must not be blank. Boot takes
    # a few frames (the sound engine's 64KB code view is laid out before
    # anything is drawn) and the title's own tile swap hides behind one black
    # frame, so the check waits for it rather than looking at frame 8 exactly.
    run(core, 8)
    for _ in range(30):
        title = pixels(screen)
        if any(p != (0, 0, 0) for row in title for p in row):
            break
        core.run_frame()
    if all(p == (0, 0, 0) for row in title for p in row):
        failures.append("la pantalla de titulo quedo en negro")

    # And START must take it to GAME SELECT, not straight to play.
    press_start(core)
    menu = pixels(screen)
    if menu == title:
        failures.append("START no llevo del titulo al menu")
    core.reset()

    start_game(core)
    rows = pixels(screen)
    cols = describe(rows)

    if all(p == (0, 0, 0) for row in rows for p in row):
        failures.append("la pantalla quedo completamente negra")
    if rows == title:
        failures.append("START no arranco la partida (la pantalla no cambio)")

    # The panels' top run lives in slots the boot upload never touches, so
    # the tilemap can be right while the art is all zeros: the rope along the
    # top of both panels simply missing on a fresh boot, and back the moment
    # a prototype skin has been put on and taken off. Read the ART, not the
    # map — the map was correct all along.
    for slot in PANEL_RUN_SLOTS:
        art = [core.memory.u16[CHARBLOCK_ADDR + slot * 32 + i]
               for i in range(0, 32, 2)]
        if not any(art):
            failures.append(f"la greca superior de los paneles (tile {slot:#x}) "
                            "esta en blanco en el primer arranque")

    def region(bounds):
        return sum(cols[x] for x in range(bounds[0] * TILE, bounds[1] * TILE))

    # Each region must actually have been drawn. A blank one means a tile
    # upload, a palette or a layout index went wrong.
    for name, bounds in (("recuadro izquierdo", COL_BOX_L),
                         ("marco izquierdo", COL_FRAME_L),
                         ("marco derecho", COL_FRAME_R),
                         ("recuadro derecho", COL_BOX_R)):
        painted = region(bounds)
        if painted == 0:
            failures.append(f"{name} quedo vacio")
        else:
            print(f"  {name}: {painted} px")

    # ...and the two boxes must be the SAME WIDTH and the board centred
    # between them. This is the layout's whole claim, so it is asserted
    # against the running ROM rather than left to the extractor's self-test.
    left_margin = COL_FRAME_L[0]
    right_margin = SCREEN_TW_TILES - COL_FRAME_R[1]
    if left_margin != right_margin:
        failures.append(f"los recuadros no son simetricos: {left_margin} "
                        f"columnas a la izquierda y {right_margin} a la derecha")

    # The field runs the FULL height of the screen — that is the whole point
    # of the 160px vertical fit, and the first thing a bad window offset
    # breaks. Checked on the frame columns, which are the walls, per tile ROW
    # rather than per pixel: the braid has transparent corners, so the
    # invariant is that no row of it is missing, not that every pixel is lit.
    for name, x in (("muro izquierdo", WALL_L_X), ("muro derecho", WALL_R_X)):
        empty_rows = [ty for ty in range(SCREEN_H // TILE)
                      if not any(rows[ty * TILE + dy][x + dx] != (0, 0, 0)
                                 for dy in range(TILE) for dx in range(TILE))]
        if empty_rows:
            failures.append(f"al {name} le faltan filas de tiles: {empty_rows}")

    # The playfield must be centred: that is the whole point of resequencing
    # the cartridge's columns, and an off-by-one in SCREEN_SEGMENTS would show
    # up here and nowhere else.
    centre = (FIELD_X0 + FIELD_X1) // 2
    if centre != SCREEN_W // 2:
        failures.append(f"el campo esta centrado en {centre}px, esperado {SCREEN_W // 2}")

    # A piece must actually fall: the screen has to change over time without
    # any input at all.
    before = pixels(screen)
    run(core, 120)
    after = pixels(screen)
    if before == after:
        failures.append("la pantalla no cambio en 120 frames (la pieza no cae)")

    if failures:
        for f in failures:
            print(f"FALLA: {f}")
        return 1
    print("OK: la ROM arranca, dibuja el campo donde corresponde y la pieza cae.")
    return 0


def sweep_sprites(core):
    """Visible sweep sprites as {row: [(column, tile), ...]}."""
    rows = {}
    for i in range(128):
        attr0 = core.memory.u16[OAM_ADDR + i * 8]
        if attr0 & 0x0200:
            continue
        attr1 = core.memory.u16[OAM_ADDR + i * 8 + 2]
        attr2 = core.memory.u16[OAM_ADDR + i * 8 + 4]
        if (attr2 >> 12) != SWEEP_PAL_BANK:
            continue
        row = (attr0 & 0xFF) // TILE
        col = (attr1 & 0x1FF) // TILE - COL_FIELD[0]
        rows.setdefault(row, []).append((col, attr2 & 0x3FF))
    for cols in rows.values():
        cols.sort()
    return rows


def lineclear_check(rom_path, row_count):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    core, screen = load(rom_path)
    start_game(core)

    rows = list(range(PF_H - row_count, PF_H))
    fill_rows(core, base + game_offsets(rom_path)["field"], rows)
    word = CLEAR_WORDS[row_count]
    print(f"filas {rows[0]}..{rows[-1]} completas -> deberia decir {word}")

    # Soft drop until the next piece lands on them and the sweep starts.
    core.set_keys(KEY_DOWN)
    for _ in range(240):
        core.run_frame()
        if sweep_sprites(core):
            break
    else:
        print("FALLA: la animacion nunca arranco")
        return 1
    core.set_keys()

    failures = []
    heads, tails_seen, final_text = [], set(), None
    watched = rows[0]
    for _ in range(60):
        seen = sweep_sprites(core)
        if not seen:
            break
        if sorted(seen) != rows:
            failures.append(f"escobas en las filas {sorted(seen)}, esperaba {rows}")
        for row, cols in seen.items():
            # Adjacent columns carrying consecutive tiles, head ($5F) on the
            # right: the trail the ROM builds up and then lets run off the
            # field, so it is shorter than five at both ends of the sweep.
            expected = [(cols[0][0] + i, cols[0][1] + i) for i in range(len(cols))]
            if cols != expected or not set(t for _, t in cols) <= set(SWEEP_TILES):
                failures.append(f"la escoba de la fila {row} esta rota: {cols}")
            tails_seen.update(t for _, t in cols)
        heads.append(max(c for c, _ in seen[watched]))
        final_text = map_row_text(core, watched)
        print(f"  columna {heads[-1]:2d}  |{final_text}|")
        core.run_frame()

    if tails_seen != set(SWEEP_TILES):
        failures.append(f"tiles usados {sorted(hex(t) for t in tails_seen)}, "
                        f"esperaba {[hex(t) for t in SWEEP_TILES]}")
    if heads != sorted(heads):
        failures.append("la escoba retrocede en algun momento")
    if not heads or max(heads) < PF_PLAYABLE - 1:
        failures.append(f"la escoba solo llego a la columna {max(heads, default=-1)}")
    if final_text is None or word not in final_text:
        failures.append(f"la fila decia |{final_text}|, esperaba que dijera {word}")

    # And then the rows really do come down.
    for _ in range(10):
        core.run_frame()
    after = map_row_text(core, watched)
    if "." not in after:
        failures.append(f"la fila |{after}| sigue completa despues de la animacion")
    if sweep_sprites(core):
        failures.append("quedaron sprites de la escoba en pantalla")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print(f"OK: la escoba cruza {row_count} fila(s), escribe {word} y la fila colapsa.")
    return 0


def pause_box(core, row):
    return [core.memory.u16[SCREENBLOCK_ADDR + ((PAUSE_TY + row) * 32 + PAUSE_TX + x) * 2] & 0x3FF
            for x in range(PAUSE_W)]


def pause_check(rom_path):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    m = core.memory
    off = game_offsets(rom_path)
    OFF_Y, OFF_LEVEL, OFF_CURRENT = off["y"], off["level"], off["current"]
    failures = []

    def tap(name):
        core.set_keys(KEYS[name])
        core.run_frame()
        core.set_keys()
        core.run_frame()

    y_before = m.u8[base + OFF_Y]
    tap("START")
    run(core, 120)
    if m.u8[base + OFF_Y] != y_before:
        failures.append("la pieza siguio cayendo con el juego en pausa")
    if pause_box(core, 0) != PAUSE_ROW0 or pause_box(core, 1) != PAUSE_ROW1:
        failures.append(f"la placa de PAUSE no se dibujo: {pause_box(core, 0)}")
    else:
        print(f"  placa de PAUSE en ({PAUSE_TX},{PAUSE_TY}), tiles de la ROM")

    level = m.u8[base + OFF_LEVEL]
    for button in CODE_LEVEL_UP:
        tap(button)
    if m.u8[base + OFF_LEVEL] != level + 1:
        failures.append(f"el codigo de nivel dejo el nivel en {m.u8[base + OFF_LEVEL]}, "
                        f"esperaba {level + 1}")
    tap("A")   # the ROM leaves the cursor on the last byte, so A repeats it
    if m.u8[base + OFF_LEVEL] != level + 2:
        failures.append("pulsar A otra vez no repitio el codigo de nivel")
    else:
        print(f"  codigo de nivel: {level} -> {m.u8[base + OFF_LEVEL]} (y repite con A)")

    # A press that breaks a sequence is swallowed, so a neutral button is
    # needed before the next code -- that is the ROM's matcher, not a hack.
    tap("SELECT")
    for button in CODE_LONG_BAR:
        tap(button)
    if m.u8[base + OFF_CURRENT] != 1:
        failures.append(f"el codigo de barra larga dio la pieza "
                        f"{m.u8[base + OFF_CURRENT]}, esperaba 1 (I)")
    else:
        print("  codigo de barra larga: la pieza en juego pasa a ser la I")

    # THE THIRD CODE. It is the one that needs a piece to have LANDED, because
    # what it does is take the last locked piece back out of the field — so
    # unpause, hold Down until something settles, and only then ask for it.
    # Once per game and wiped by a line clear (L94E4), which is why this is the
    # last of the three and on a board that has cleared nothing.
    tap("START")
    run(core, 4)
    core.set_keys(KEYS["DOWN"])
    run(core, 240)
    core.set_keys()
    run(core, 12)
    field = base + off["field"]
    settled = sum(1 for i in range(TENGEN_PF_WIDTH * TENGEN_PF_HEIGHT)
                  if core.memory.u8[field + i])
    tap("START")
    run(core, 4)
    tap("SELECT")           # a neutral press, as above
    for button in CODE_UNDO:
        tap(button)
    undone = sum(1 for i in range(TENGEN_PF_WIDTH * TENGEN_PF_HEIGHT)
                 if core.memory.u8[field + i])
    if undone >= settled:
        failures.append(f"el codigo de deshacer no saco la pieza del tablero: "
                         f"{settled} celdas antes, {undone} despues")
    else:
        print(f"  codigo de deshacer: la ultima pieza vuelve al aire "
               f"({settled} -> {undone} celdas)")

    tap("START")
    run(core, 4)
    if pause_box(core, 0) == PAUSE_ROW0:
        failures.append("la placa de PAUSE se quedo despues de despausar")
    run(core, 120)
    if m.u8[base + OFF_Y] == y_before:
        failures.append("el juego no siguio despues de despausar")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: START pausa y despausa, y los codigos de trucos responden.")
    return 0


# ---------------------------------------------------------------------------
# Leaving the title: two things that were wrong for a whole build and that
# nothing here would have caught, so they get their own check.
#
#   * The title is the only screen with SPRITES on it — the cathedral overlay
#     and the fireworks. Nothing else ever writes OAM, so nothing else ever
#     cleared it, and the cathedral's central tower stood in the middle of
#     GAME SELECT and every screen after it.
#
#   * The title's music has to stop. The cartridge stops it by PREVIEWING each
#     tune as the cursor moves over it (LA035 from $A00A), and LA035 always
#     sends MUSIC_SILENCE before the track — handing the engine a new track
#     without silencing the old one leaves both playing.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# THE BRAID HAS A DIRECTION, AND IT MUST NOT CHANGE WITH THE HUD.
#
# The rope is woven and the weave leans, so its runs and its corners only fit
# each other one way round. This has now been got wrong in both directions:
# once with the corners right and the runs mirrored against the cartridge's,
# once the other way, and each time the tell was the HUD swap — the banner
# redraws the
# two columns beside the board as a plain strip, and if that strip is not the
# same tile the panel puts there, the weave visibly flips as the box comes and
# goes.
#
# So this asks the only question that matters: are those two columns THE SAME
# PIXELS in both modes? It reads them off the screen rather than off the map,
# because a matching map with a mismatched palette bank would still look wrong.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# THE STARTING HANDICAP reaches the board.
#
# The rule itself has its own host tests; this asks the other half of the
# question — that the menu row is there, that a shoulder button moves it, and
# that what it says is what the playfield comes up buried under.
# ---------------------------------------------------------------------------
def handicap_check(rom_path):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    failures = []
    for steps, expect_rows in ((0, 0), (1, 3), (3, 9)):
        core, screen = load(rom_path)
        run(core, 20)
        press_start(core); run(core, 10)      # title -> game select
        press_start(core); run(core, 10)      # -> LEVEL SETTINGS
        # The cursor onto HANDICAP, which is what puts the depth line up.
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        # EL MANDO, NO LOS GATILLOS. La NES no tenia L ni R, asi que la pagina
        # se maneja con los cuatro botones que si tenia: arriba/abajo mueven el
        # cursor, izquierda/derecha cambian el valor, y SELECT (abajo) elige
        # cual de los dos numeros en una carrera.
        for _ in range(steps):
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 8)
        # One line: "HANDICAP  r   ROWS", and r is the CARTRIDGE'S OWN NUMBER
        # — its handicap screen offers 0, 3, 6, 9, 12, which are rows. The
        # step (nought to four) is what the ROM counts in and is not on
        # screen; the word BURIES went with it.
        row = tilemap_text(core, HANDICAP_ROW)
        if "HANDICAP" not in row:
            failures.append(f"la fila {HANDICAP_ROW} no es la del handicap: {row!r}")
        if f"HANDICAP {expect_rows}" not in row or "ROWS" not in row:
            failures.append(f"handicap {steps}: la fila dice {row!r}, "
                             f"esperaba HANDICAP {expect_rows} ... ROWS")
        press_start(core); run(core, 40)      # into the game
        # Count the rows of the playfield that came up with anything in them.
        filled = 0
        field = base + game_offsets(rom_path)["field"]
        for y in range(20):
            if any(core.memory.u8[field + y * PF_W + c] for c in range(1, 11)):
                filled += 1
        if filled != expect_rows:
            failures.append(f"handicap {steps}: el campo empieza con {filled} "
                             f"filas ocupadas, no {expect_rows}")
        elif expect_rows:
            # ...and none of them complete, or they would clear on frame one.
            whole = 0
            for y in range(20):
                if all(core.memory.u8[field + y * PF_W + c] for c in range(1, 11)):
                    whole += 1
            if whole:
                failures.append(f"handicap {steps}: {whole} filas llegan completas")
            else:
                print(f"  handicap {steps}: {filled} filas de basura, "
                       "ninguna completa")
        else:
            print("  handicap 0: el campo empieza vacio")

    failures += handicap_two_check(rom_path)

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el handicap entierra tres filas por paso y deja paso en todas.")
    return 0


def handicap_two_check(rom_path):
    """CONTRA LA MAQUINA HAY UN SOLO HANDICAP, Y ENTIERRA LOS DOS TABLEROS.

    La pantalla de handicap del cartucho tiene dos cursores en 2 PLAYER y uno
    solo en VERSUS COMPUTER, y endPlayfieldInit entierra los dos tableros bajo
    ese numero (`bcs @computerIsPlaying`, main.asm.txt:3539-3542): la maquina
    empieza tan enterrada como tu. Trazado contra el cartucho
    (`make trace MODE="versus --handicap 3"`). Los dos numeros de 2 PLAYER,
    que solo se alcanzan con cable, los comprueba tools/run_link.py.
    """
    failures = []
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return failures
    off = game_offsets(rom_path)
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=10):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    run(core, 20)
    press_start(core); run(core, 10)      # title -> GAME SELECT
    for _ in range(3):                     # VERSUS COMPUTER
        tap("DOWN")
    press_start(core); run(core, 12)       # -> LEVEL SETTINGS
    tap("DOWN")                            # el cursor sobre HANDICAP
    row = tilemap_text(core, HANDICAP_ROW)
    if "HANDICAP 0 0" in row or "HANDICAP 0" not in row:
        failures.append(f"en VERSUS la fila deberia traer un solo numero: {row!r}")
        return failures
    tap("RIGHT")
    if "HANDICAP 3" not in tilemap_text(core, HANDICAP_ROW):
        failures.append(f"DERECHA no sube el handicap: "
                        f"{tilemap_text(core, HANDICAP_ROW)!r}")
        return failures
    print("  en VERSUS un solo numero, como en el cartucho")

    press_start(core); run(core, 30)       # -> play
    PF_W, PF_H = 12, 20
    buried = []
    for board in (0, 1):
        at = base + off["field"] + board * PF_W * PF_H
        buried.append(sum(1 for r in range(PF_H)
                          if any(core.memory.u8[at + r * PF_W + c] == 15
                                 for c in range(1, 11))))
    if buried != [3, 3]:
        failures.append(f"el handicap no entierra los dos tableros por igual: "
                        f"filas de basura {buried}")
    else:
        print("  y entierra los dos tableros, el tuyo y el de la maquina, "
              "3 filas cada uno")
    if not failures:
        failures += handicap_cpu_check(rom_path)
    return failures


def handicap_cpu_check(rom_path):
    """BAJO EL ACORDE, VERSUS COMPUTER TIENE UN SEGUNDO NUMERO: EL DE LA MAQUINA.

    Es del port, no del cartucho, y por eso va detras del acorde: sin el, el
    numero unico de arriba. Con el, la fila trae dos, SELECT pasa al segundo,
    y cada tablero se entierra bajo el suyo: 3 filas el tuyo, 6 el de la
    maquina.
    """
    failures = []
    base, why = game_state_address(rom_path)
    if base is None:
        return failures
    off = game_offsets(rom_path)
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=10):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    run(core, 20)
    press_start(core); run(core, 10)      # title -> GAME SELECT
    for _ in range(3):                     # VERSUS COMPUTER
        tap("DOWN")
    press_start(core); run(core, 12)       # -> LEVEL SETTINGS
    tap("L", "R")                          # el acorde: el cursor va a MUSIC
    tap("UP")                              # MUSIC -> HANDICAP
    row = tilemap_text(core, HANDICAP_ROW)
    if "HANDICAP 0 0" not in row:
        failures.append(f"con el acorde, VERSUS deberia traer dos numeros: "
                        f"{row!r}")
        return failures
    tap("RIGHT")                           # el tuyo: 3
    tap("SELECT")                          # al de la maquina
    tap("RIGHT"); tap("RIGHT")             # 6
    row = tilemap_text(core, HANDICAP_ROW)
    if "HANDICAP 3 6" not in row:
        failures.append(f"SELECT y DERECHA no llevan a '3 6': {row!r}")
        return failures
    press_start(core); run(core, 30)       # -> play
    PF_W, PF_H = 12, 20
    buried = []
    for board in (0, 1):
        at = base + off["field"] + board * PF_W * PF_H
        buried.append(sum(1 for r in range(PF_H)
                          if any(core.memory.u8[at + r * PF_W + c] == 15
                                 for c in range(1, 11))))
    if buried != [3, 6]:
        failures.append(f"con el acorde, cada tablero bajo su numero: "
                        f"esperaba [3, 6], hay {buried}")
    else:
        print("  con el acorde, dos numeros: 3 filas el tuyo, 6 el de la "
              "maquina")
    return failures


def sleep_check(rom_path):
    """THE WAIT FOR THE NEXT FRAME SLEEPS, it does not spin.

    vsync() is the BIOS's VBlankIntrWait (SWI 5), which halts the CPU until
    the vertical blank's interrupt. A spin on VCOUNT keeps it executing for
    the whole of the visible frame — about 31,000 of the 43,000 instructions a
    played frame used to take. So: the vector is the program's handler, the
    vertical blank's interrupt is switched on at both ends, and in each frame
    of play the CPU goes into the BIOS and stays there — a handful of steps
    from entering it to the end of the frame, where a spin would be
    thousands.
    """
    addr, why = game_state_address(rom_path, "irq_handler")
    if addr is None:
        print(f"SALTADO: {why}")
        return 0
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    start_game(core)
    run(core, 60)

    vector = core.memory.u32[0x03007FFC]
    ie = core.memory.u16[0x04000200]
    dispstat = core.memory.u16[0x04000004]
    if vector != addr:
        failures.append(f"el vector de interrupciones es {vector:#x}, no "
                         f"irq_handler ({addr:#x})")
    if not ie & 0x0001 or not dispstat & 0x0008:
        failures.append(f"la interrupcion de vblank no esta encendida "
                         f"(IE={ie:#06x}, DISPSTAT={dispstat:#06x})")

    for _ in range(3):
        start = core.frame_counter
        steps = 0
        # Steps from the LAST call out of the cartridge into the BIOS to the
        # end of the frame. Not the first BIOS step: a frame opens still
        # inside the previous frame's VBlankIntrWait, waking up.
        tail = None
        prev = core.cpu.pc
        while core.frame_counter == start:
            core.step()
            steps += 1
            pc = core.cpu.pc
            if prev >= 0x08000000 and pc < 0x4000:
                tail = 0
            elif tail is not None:
                tail += 1
            prev = pc
        if tail is None:
            failures.append(f"un frame de {steps} pasos sin entrar en la BIOS: "
                            f"vsync no llama a VBlankIntrWait")
            break
        if tail > 500:
            failures.append(f"{tail} pasos entre la BIOS y el fin del frame: "
                            f"eso es girar, no dormir")
            break
    else:
        print(f"  un frame de partida: {steps} instrucciones, y {tail} desde "
              f"que entra en VBlankIntrWait hasta el siguiente")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: vsync duerme la CPU hasta el vblank en vez de dar vueltas.")
    return 0


def points_check(rom_path):
    """THE POINTS THE PIECE WAS WORTH, beside the piece.

    L8129 stages three sprites the moment a piece rests and
    stageDropPointSprites keeps them up for $3C frames
    (main.asm.txt:218-313). Three things have to be true of them and all
    three are the cartridge's:

      * the number is the award, which is what the score just went up by;
      * the HEIGHT is the landing height, because in this game the height IS
        the score (the award grows the higher the piece rests); and
      * the SIDE is the player — on a coop board, one to each side of it, in
        each player's own palette.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    POINTS_FIRST, POINTS_COUNT = 118, 6

    def digits(core, first, count):
        """(x, y, value) of a run of point sprites, or None."""
        out = oam_visible(core, first, first + count)
        if not out:
            return None
        out.sort()
        value = 0
        for _x, _y, tile in out:
            value = value * 10 + ((tile - 512) & 0xF)
        return out[0][0], out[0][1], value

    # 1 PLAYER: hold Down, watch a piece land on the floor.
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    score = base + off["score"]

    def read_score():
        return sum(core.memory.u8[score + i] << (8 * i) for i in range(4))

    # The score changes inside tengen_step and the sprites are written at the
    # end of draw_match, and mGBA's frame boundary need not fall between the
    # two — so the award is measured as the last CHANGE in the score rather
    # than as a difference across the frame the sprites turned up on.
    last = read_score()
    gained = 0
    shown = None
    for _ in range(2000):
        core.set_keys(KEYS["DOWN"]); run(core, 1)
        now = read_score()
        if now != last:
            gained = now - last
            last = now
        got = digits(core, POINTS_FIRST, 3)
        if got:
            shown = got
            break
    core.set_keys(); run(core, 2)

    if shown is None:
        failures.append("una pieza se asento y no aparecio su puntuacion al lado")
    else:
        x, y, value = shown
        if value != gained:
            failures.append(f"los sprites dicen {value} y el marcador subio {gained}")
        elif x != (COL_FIELD[1]) * TILE:
            failures.append(f"la puntuacion sale en x={x}, no al borde derecho "
                             f"del tablero ({COL_FIELD[1] * TILE})")
        elif y != (TENGEN_PF_HEIGHT - 1) * TILE:
            failures.append(f"una pieza asentada en el suelo muestra sus puntos "
                             f"en y={y}, no en la ultima fila")
        else:
            print(f"  1 PLAYER: {value} puntos, junto al tablero, a la altura "
                   "en que se poso la pieza")

    # WITH COMPUTER: one board, two players, one to each side of it.
    core2, screen2 = load(rom_path)
    run(core2, 8); press_start(core2); run(core2, 10)
    for _ in range(4):
        core2.set_keys(KEYS["DOWN"]); run(core2, 4); core2.set_keys(); run(core2, 10)
    press_start(core2); run(core2, 12)
    press_start(core2); run(core2, 30)

    sides = {}
    for _ in range(4000):
        run(core2, 1)
        for slot in (0, 1):
            got = digits(core2, POINTS_FIRST + slot * 3, 3)
            if got and slot not in sides:
                sides[slot] = got
        if len(sides) == 2:
            break

    if len(sides) < 2:
        failures.append(f"en el tablero compartido solo salieron los puntos de "
                         f"{len(sides)} jugador(es)")
    else:
        left = sides[0][0] + 8 * 3
        if sides[0][0] >= COOP_FIELD_TX * TILE:
            failures.append("los puntos del jugador 1 no salen a la izquierda "
                             "del tablero compartido")
        elif sides[1][0] < (COOP_FIELD_TX + TENGEN_PF_WIDTH) * TILE:
            failures.append("los puntos del ordenador no salen a la derecha "
                             "del tablero compartido")
        else:
            print(f"  WITH COMPUTER: {sides[0][2]} a la izquierda y "
                   f"{sides[1][2]} a la derecha, uno por jugador (left={left})")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada pieza dice lo que vale, donde y cuando lo dice el cartucho.")
    return 0


def counters_check(rom_path):
    """NO LEADING ZEROS. renderStatistics walks each counter's digits from the
    top and, while it finds a '0', shortens the run and advances the write
    position (main.asm.txt:4067-4082) — the number keeps its place and the
    zeros in front of it are never drawn. The cartridge's own screen reads
    8294 / 30 / 2, not 008294 / 0030 / 02.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    run(core, 20)
    for i in range(4):
        core.memory.u8[base + off["score"] + i] = (8294 >> (8 * i)) & 0xFF
        core.memory.u8[base + off["lines"] + i] = (30 >> (8 * i)) & 0xFF
    core.memory.u8[base + off["level"]] = 2
    run(core, 20)

    failures = []
    for name, ty, want in (("SCORE", 3, "8294"), ("LINES", 6, "30"),
                            ("LEVEL", 9, "2")):
        # columns 2-8 are the counter's own; 0-1 and 9 are the braid,
        # whose tiles happen to fall in the printable range.
        line = tilemap_text(core, ty, 2, 8).strip()
        if line != want:
            failures.append(f"{name} sale como {line!r}, el cartucho lo pinta "
                             f"como {want!r}")
    if failures:
        for f in failures:
            print("FALLA:", f)
        return 1
    print("OK: los contadores no pintan ceros a la izquierda, como el cartucho.")
    return 0


def gameover_check(rom_path):
    """THE WAY OUT. Every mode has to end, and end where the player left.

    This is the check the twenty-one before it did not do: they all started
    games and none of them ever lost one. What a lost game has to do is

      * stop — in coop that means BOTH players, because there is one board and
        the cartridge kills both flags at once (main.asm.txt:83D4-83DD), and
        against the computer it means the computer too;
      * stay stopped — no piece of anybody's moves after the plaque is up;
      * let go — Start goes back to the title from the mode's own screen; and
      * take the HUD swap with it: SELECT is a thing you do to a game in play.

    It buries the board by hand rather than stacking pieces for ten minutes:
    every row solid but one column, so nothing can clear and the next piece
    tops out where it stands. That is the same `game_active` path the player
    walked into, reached in a second instead of a quarter of an hour.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH

    def player(core, slot, key):
        return core.memory.u8[base + off[key] + slot * off["stride"]]

    def board_cells(core, which):
        addr = base + off["field"] + which * PF
        return sum(1 for i in range(PF) if core.memory.u8[addr + i] == CELL_BLOCK)

    def bury(core, which, coop):
        """Solid everywhere but one column, so no row can ever complete."""
        addr = base + off["field"] + which * PF
        gap = 5
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(TENGEN_PF_WIDTH):
                edge = not coop and col in (0, TENGEN_PF_WIDTH - 1)
                value = CELL_WALL if edge else (0 if col == gap else CELL_BLOCK)
                core.memory.u8[addr + row * TENGEN_PF_WIDTH + col] = value

    def enter(entry):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        run(core, 8)
        press_start(core)               # title -> GAME SELECT
        run(core, 10)
        for _ in range(entry):
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)   # -> LEVEL SETTINGS
        press_start(core); run(core, 30)   # -> play
        return core, screen

    def title_face(core):
        """Three rows of the title's own tilemap, which no other screen has."""
        return tuple(tilemap_text(core, r) for r in (4, 6, 8))

    reference, _ref_screen = load(rom_path)
    run(reference, 40)
    face = title_face(reference)

    for name, entry, coop in (("1 PLAYER", 0, False),
                               ("WITH COMPUTER", 4, True),
                               ("VERSUS COMPUTER", 3, False)):
        core, screen = enter(entry)
        bury(core, 0, coop)
        run(core, 240)

        if player(core, 0, "active"):
            failures.append(f"{name}: el tablero del jugador no muere aun enterrado")
            continue
        if coop and player(core, 1, "active"):
            failures.append(f"{name}: el jugador 1 murio y el 2 sigue vivo "
                             "sobre el mismo tablero")
            continue

        # Nothing may move behind the plaque — not the computer, not anybody.
        # UNDER THE PLAQUE'S OWN CLOCK, though: it now leaves for the high
        # scores after 498 frames on its own (GAMEOVER_HOLD_FRAMES, and the
        # cartridge's own number), so this has to say its piece before then.
        # 240 + 150 + the chord below is comfortably inside it whatever frame
        # the board actually died on.
        before = (board_cells(core, 0), board_cells(core, 1))
        run(core, 150)
        after = (board_cells(core, 0), board_cells(core, 1))
        if after != before:
            failures.append(f"{name}: despues del game over se siguio jugando "
                             f"({before} -> {after})")
            continue

        # SELECT is for a game in play. Read the far right column, which is
        # the box the banner would take over.
        hud = tuple(tilemap_text(core, r, 22, 30) for r in range(4, 12))
        core.set_keys(KEYS["SELECT"]); run(core, 6)
        core.set_keys(); run(core, 12)
        if tuple(tilemap_text(core, r, 22, 30) for r in range(4, 12)) != hud:
            failures.append(f"{name}: SELECT todavia cambia el HUD despues del game over")
            continue

        # The cartridge's road out of a game runs through its HIGH SCORES
        # page and only then back to the title (main.asm.txt:2643-2675).
        press_start(core); run(core, 40)
        if "HIGH SCORES" not in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
            failures.append(f"{name}: START no lleva a la tabla de records "
                             "tras el game over")
            continue
        press_start(core); run(core, 40)
        if title_face(core) != face:
            failures.append(f"{name}: no se vuelve al titulo desde la tabla")
            continue
        print(f"  {name}: el tablero muere, todo se para, y START lleva a la "
               "tabla y al titulo")

    # ...AND NEITHER PAGE NEEDS A BUTTON. The cartridge's game over is a
    # countdown, not a prompt: $F9 goes into player1FallTimer as well as into
    # gameState, one decrement every other frame takes 498 to reach zero, and
    # the high scores then underflow the same byte to 255 and hold for 1020
    # at one decrement every fourth. Measured on the cartridge by burying its
    # field and watching gameState: $F9 at frame 48, $F8 at 546, the title at
    # 1571. This port waited for a button on the first and let go of the
    # second after 300 frames.
    #
    # Counted in blocks of 30 frames, so the tolerance below is a block and a
    # half either way rather than a promise about a single frame.
    core, _screen = enter(0)
    bury(core, 0, False)
    dead_at = None
    to_table = to_title = None
    for tick in range(120):                   # 3600 frames, sixty seconds
        run(core, 30)
        if dead_at is None:
            if not player(core, 0, "active"):
                dead_at = tick * 30
            continue
        if to_table is None:
            if "HIGH SCORES" in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
                to_table = tick * 30 - dead_at
            continue
        if title_face(core) == face:
            to_title = tick * 30 - dead_at - to_table
            break

    for what, got, want in (("el game over", to_table, 498),
                             ("la tabla", to_title, 1020)):
        if got is None:
            failures.append(f"{what} no se va solo, hay que pulsar algo")
        elif abs(got - want) > 45:
            failures.append(f"{what} dura {got} frames y el cartucho {want}")
        else:
            print(f"  {what} se va solo tras {got} frames "
                  f"(el cartucho, {want})")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada modo termina, se para del todo, y suelta al jugador.")
    return 0


def falling_piece_check(rom_path):
    """THE PARTNER'S PIECE HAS TO BE ON THE SCREEN WHILE IT FALLS.

    Coop is one board with two pieces coming down it, and the renderer used to
    draw only the one belonging to the player it was showing. On a shared
    board that is the difference between watching somebody play and watching
    pieces appear on the floor out of nowhere — which is exactly what WITH
    COMPUTER looked like.

    Counted rather than eyeballed: the tiles drawn over the board minus the
    cells the playfield buffer actually holds are the falling pieces, and two
    pieces are eight cells.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    run(core, 8)
    press_start(core); run(core, 10)
    for _ in range(4):              # GAME SELECT -> WITH COMPUTER
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 30)

    best = 0
    for _ in range(120):
        run(core, 4)
        drawn = 0
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(TENGEN_PF_WIDTH):
                tile = core.memory.u16[SCREENBLOCK_ADDR +
                                        ((row * 32) + COOP_FIELD_TX + col) * 2] & 0x3FF
                cell = core.memory.u8[base + off["field"] +
                                       row * TENGEN_PF_WIDTH + col]
                if tile and not cell:
                    drawn += 1
        best = max(best, drawn)
        if best >= 8:
            break

    if best < 8:
        print(f"FALLA: solo {best} celdas cayendo sobre el tablero compartido; "
               "una de las dos piezas no se dibuja")
        return 1
    print(f"  {best} celdas cayendo a la vez — las dos piezas de un tablero coop")

    # ...AND IT HAS TO BE WATCHABLE. Drawn is not the same as seen: a piece
    # that waits above the field and then crosses the board in twenty frames
    # is on screen for a tenth of its life, which is what "no veo la caida de
    # sus piezas" was even after both pieces were being drawn. So this
    # measures the fraction of frames the computer's piece spends INSIDE the
    # field, off the piece's own row rather than off the tilemap.
    y_addr = base + off["y"] + off["stride"]
    inside = 0
    frames = 1200
    for _ in range(frames):
        run(core, 1)
        y = core.memory.u8[y_addr]
        if y > 127:
            y -= 256
        if y >= TENGEN_ROM_ROW_ORIGIN:
            inside += 1
    share = inside * 100 // frames
    if share < 60:
        print(f"FALLA: la pieza del ordenador solo esta dentro del campo el "
               f"{share}% del tiempo: no se le ve caer")
        return 1
    print(f"  las dos piezas se dibujan, y la del ordenador esta a la vista "
           f"el {share}% del tiempo")

    # ...AND THEY ARE SOLID TO EACH OTHER. The playfield buffer holds only
    # settled blocks, so without checkCoopCollision (main.asm.txt:1827-1924)
    # the two pieces of a shared board walk straight through each other --
    # which they did. Measured off the SCREEN rather than off the rule: every
    # frame, the cells each falling piece covers, and no cell may be in both.
    #
    # The port draws them in two palette banks so the partner's piece can be
    # told apart, but a cell is a cell: this reads the two pieces' own
    # positions out of the game state and intersects them, which is the same
    # question checkCoopCollision answers and a completely different route to
    # it.
    def piece_cells(slot):
        b = base + slot * off["stride"]
        piece = core.memory.u8[b + off["current"]]
        if not piece:
            return set()
        y = core.memory.u8[b + off["y"]]
        if y > 127:
            y -= 256
        x = core.memory.u8[b + off["x"]]
        if x > 127:
            x -= 256
        orientation = core.memory.u8[b + off["orientation"]] & 3
        bits = ORIENTATION_BITS[piece][orientation]
        return {(y + r, x + c)
                for r in range(4) for c in range(4)
                if bits & (0x8000 >> (r * 4 + c))}

    overlaps = 0
    ghost = None
    for _ in range(2400):
        run(core, 1)
        both = piece_cells(0) & piece_cells(1)
        if both:
            overlaps += 1
            if ghost is None:
                ghost = sorted(both)
    if overlaps:
        print(f"FALLA: las dos piezas del tablero compartido se atraviesan: "
               f"{overlaps} frames con celdas en comun, la primera en {ghost}")
        return 1
    print(f"OK: las dos piezas se dibujan, se ven caer, y no se atraviesan.")
    return 0


# ---------------------------------------------------------------------------
# TETRIS TENGEN XE, which rides the same L+R as the tunes and the pause menu.
#
# The mod is levels 18 and 19 and nothing else — see TENGEN_MAX_LEVEL_XE in
# src/tengen_core.h, where its ten IPS records are accounted for one by one.
# So there are three things to check and the first is the one that matters:
#
#   * before the chord the LEVEL field still stops at 9, which is the
#     cartridge's own menu range;
#   * after it, the field runs 0-19 and wraps there;
#   * and a game started on 19 really starts on 19, which is the flag having
#     reached the core rather than just the menu.
# ---------------------------------------------------------------------------
def xe_check(rom_path):
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    def tap(*names, settle=8):
        core.set_keys(*[KEYS[n] for n in names])
        run(core, 4)
        core.set_keys()
        run(core, settle)

    def level_field():
        text = tilemap_text(core, LEVEL_ROW)
        digits = "".join(c for c in text.split("LEVEL", 1)[-1] if c.isdigit())
        return digits

    run(core, 8)
    press_start(core); run(core, 10)
    press_start(core); run(core, 10)          # LEVEL SETTINGS, cursor on LEVEL

    seen = set()
    for _ in range(24):
        seen.add(level_field())
        tap("RIGHT")
    if seen != {str(n) for n in range(10)}:
        failures.append(f"antes del acorde el nivel ofrece {sorted(seen)}, no 0-9")
    else:
        print("  antes del acorde: el nivel llega a 9, como en el cartucho")

    # The chord leaves the cursor on MUSIC — that is what it does on this
    # screen, so the tune it has just uncovered is the one under the arrow.
    # One more DOWN wraps back round to LEVEL.
    tap("L", "R", settle=10)
    tap("DOWN")
    seen = set()
    for _ in range(40):
        seen.add(level_field())
        tap("RIGHT")
    if seen != {str(n) for n in range(20)}:
        failures.append(f"tras el acorde el nivel ofrece {len(seen)} valores, no 20: "
                         f"{sorted(seen, key=int)}")
    else:
        print("  tras el acorde: 0-19, que es todo lo que anade Tetris Tengen XE")

    # ...and it has to be the GAME's level, not just the menu's.
    for _ in range(40):
        if level_field() == "19":
            break
        tap("RIGHT")
    press_start(core); run(core, 30)
    off = game_offsets(rom_path)
    level = core.memory.u8[base + off["level"]]
    if level != 19:
        failures.append(f"la partida empezo en el nivel {level}, no en el 19")
    else:
        print("  ...y la partida arranca de verdad en el nivel 19")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el acorde abre los niveles 18 y 19, y solo eso.")
    return 0


def vblank_check(rom_path):
    """EL DIBUJADO DE CADA FRAME TERMINA DENTRO DEL BLANCO VERTICAL.

    draw_match espera al vblank, escribe el mapa de tiles, el OAM y las
    paletas, y al final llama a audio_frame. La linea de barrido (VCOUNT) en
    ese momento es donde acabo de dibujar: tiene que estar en 160-227. Una que
    ya ha vuelto a 0-159 es un dibujado que se paso, y pasarse no avisa: se
    ve como que falta la parte de arriba de lo ultimo que se dibujo (asi se
    perdio una vez el titulo del menu de pausa). --onscreen caza el sintoma;
    esto mide el margen, en los frames mas pesados que hay: el menu de pausa
    cambiando de ancho y de caja, quitar la pausa (que repinta la pantalla
    entera) y una limpieza de cuatro filas.
    """
    elf = os.path.splitext(rom_path)[0] + ".elf"
    try:
        out = subprocess.check_output(
            [os.environ.get("NM", "arm-none-eabi-nm"), elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"SALTADO: no pude leer {elf}: {exc}")
        return 0
    audio = next((int(l.split()[0], 16) for l in out.splitlines()
                  if l.endswith(" audio_frame")), None)
    base, why = game_state_address(rom_path)
    if audio is None or base is None:
        print(f"SALTADO: {why or 'el ELF no exporta audio_frame'}")
        return 0
    off = game_offsets(rom_path)

    def measure(core, frames):
        ends, f0, seen = [], core.frame_counter, None
        while core.frame_counter < f0 + frames:
            core.step()
            fc = core.frame_counter
            if (core.cpu.pc & ~1) in (audio, audio + 4) and seen != fc:
                seen = fc
                ends.append(core.memory.u16[0x04000006])
        return ends

    def tap_measure(core, keys, frames=14):
        core.set_keys(*[KEYS[k] for k in keys])
        got = measure(core, 4)
        core.set_keys()
        return got + measure(core, frames)

    results = {}
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    run(core, 8)
    press_start(core); run(core, 10)
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4); core.set_keys(); run(core, 12)
    press_start(core); run(core, 12)
    press_start(core); run(core, 30)
    got = tap_measure(core, ["START"])
    for _ in range(8):                        # every tune: every box width
        got += tap_measure(core, ["RIGHT"])
    results["menu de pausa, de cancion en cancion"] = got
    got = []
    for keys in (["DOWN"], ["A"], ["B"], ["A"], ["START"]):
        got += tap_measure(core, keys)
    results["la pregunta EXIT?, volver, y quitar la pausa"] = got
    del core, screen

    core, screen = load(rom_path)
    start_game(core)
    fill_rows(core, base + off["field"], [16, 17, 18, 19])
    core.set_keys(KEYS["DOWN"])
    results["una limpieza de cuatro filas"] = measure(core, 120)
    del core, screen

    failures = []
    for what, ends in results.items():
        late = [v for v in ends if v < 160]
        inside = [v for v in ends if v >= 160]
        if not ends:
            failures.append(f"{what}: no se vio terminar ningun dibujado")
        elif late:
            failures.append(f"{what}: {len(late)} de {len(ends)} frames acaban "
                            f"fuera del blanco (linea {late})")
        else:
            print(f"  {what}: {len(ends)} frames, el ultimo acaba en la linea "
                  f"{max(inside)} de 227")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el dibujado de cada frame termina dentro del blanco vertical.")
    return 0


def idle_blink_check(rom_path):
    """EL COSACO DE HUD STATS NO PARPADEA CUANDO TERMINA UNA LIMPIEZA.

    El barrido de la limpieza usa el OAM de abajo, y el frame en que acaba lo
    oculta entero. Se hacia despues de draw_panel, asi que se llevaba tambien
    al cosaco que el panel acababa de poner (OAM 124-127), y se veia apagarse
    un frame al final de cada limpieza. Se lee el OAM donde termina
    draw_match, que es lo que se ve durante ese frame, a lo largo de una
    limpieza de cuatro filas en HUD STATS (SELECT desde la de por defecto).
    """
    elf = os.path.splitext(rom_path)[0] + ".elf"
    try:
        out = subprocess.check_output(
            [os.environ.get("NM", "arm-none-eabi-nm"), elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"SALTADO: no pude leer {elf}: {exc}")
        return 0
    audio = next((int(l.split()[0], 16) for l in out.splitlines()
                  if l.endswith(" audio_frame")), None)
    base, why = game_state_address(rom_path)
    if audio is None or base is None:
        print(f"SALTADO: {why or 'el ELF no exporta audio_frame'}")
        return 0
    off = game_offsets(rom_path)
    IDLE = 124                       # IDLE_OAM_BASE, gba/port.h

    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    start_game(core)
    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 8)
    fill_rows(core, base + off["field"], [16, 17, 18, 19])
    core.set_keys(KEYS["DOWN"])
    shown, swept, seen, f0 = [], [], None, core.frame_counter
    while core.frame_counter < f0 + 120:
        core.step()
        fc = core.frame_counter
        if (core.cpu.pc & ~1) in (audio, audio + 4) and seen != fc:
            seen = fc
            shown.append(bool(oam_visible(core, IDLE, IDLE + 4)))
            swept.append(bool(oam_visible(core, 0, 4)))
    core.set_keys()
    del core, screen

    if not any(swept):
        print("FALLA: no se vio el barrido de la limpieza")
        return 1
    gone = [i for i, s in enumerate(shown) if not s]
    if not shown[0] or gone:
        print(f"FALLA: el cosaco de HUD STATS falta en {len(gone)} de "
              f"{len(shown)} frames (frames {gone[:8]})")
        return 1
    print(f"  {len(shown)} frames, {sum(swept)} de barrido: el cosaco esta en "
          "todos")
    print("OK: el cosaco no parpadea al terminar la limpieza.")
    return 0


def system_check(rom_path):
    """A+B+START+SELECT STARTS OVER; L+R+SELECT, PAUSED, SLEEPS AND WAKES.

    The restart is the port's own (soft_reset_check, gba/video.c): from the
    middle of a match, holding the four and letting go lands on the title,
    and START from there opens GAME SELECT with nothing of the match left.

    The sleep is the BIOS's Stop, which this emulator's stand-in BIOS does
    not implement — it returns at once — so what can be checked here is
    that the way in and out of it does not hang, leaves the match paused
    where it was, and puts the screen, the sound and the interrupts back as
    it found them. Whether the console really sleeps is for the hardware.
    """
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen
    io = core._native.memory.io
    failures = []

    def hold(keys, frames):
        core.set_keys(*[KEYS[k] for k in keys]); run(core, frames)

    def regs():
        return {"DISPCNT": io[0], "SOUNDCNT_L": io[0x80 >> 1],
                "IE": io[0x200 >> 1], "KEYCNT": io[0x132 >> 1]}

    run(core, 20)
    press_start(core); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 60)          # 1 PLAYER, playing
    hold(["START"], 4); hold([], 20)
    if not core.memory.u8[base + off["paused"]]:
        failures.append("START no pauso la partida")
    before = regs()
    y = core.memory.u8[base + off["y"]]
    hold(["L", "R", "SELECT"], 6); hold([], 40)
    after = regs()
    if not core.memory.u8[base + off["paused"]]:
        failures.append("tras dormir y despertar la partida ya no esta en pausa")
    elif after != before:
        failures.append(f"dormir no deja los registros como estaban: "
                        f"{before} -> {after}")
    elif core.memory.u8[base + off["y"]] != y:
        failures.append("la pieza se movio mientras la partida dormia")
    else:
        print("  L+R+SELECT en pausa: entra y sale del reposo, la partida "
              "sigue en pausa donde estaba y la pantalla, el sonido y las "
              "interrupciones vuelven como estaban")

    hold(["A", "B", "START", "SELECT"], 6); hold([], 120)
    active = core.memory.u8[base + off["active"]]
    press_start(core); run(core, 20)
    rows = " ".join(tilemap_text(core, r) for r in range(20))
    if active:
        failures.append("A+B+START+SELECT no reinicio: la partida sigue viva")
    elif "GAME" not in rows or "SELECT" not in rows:
        failures.append(f"tras reiniciar, START no lleva a GAME SELECT: {rows[:120]!r}")
    else:
        print("  A+B+START+SELECT en plena partida: vuelve al titulo, y START "
              "lleva a GAME SELECT")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el reinicio por software y el reposo hacen lo que deben.")
    return suspend_check(rom_path)


def suspend_check(rom_path):
    """A PAUSED GAME OUTLIVES THE POWER SWITCH, AND THE LOGO OPENS A COLD ONE.

    L+R+SELECT in a match that is running pauses it and then sleeps (the
    stand-in BIOS's Stop returns at once). On the plaque the game goes to
    the battery (gba/suspend.c): the console switched off and on (a reset,
    which keeps the save memory) comes back to the same game, paused, piece
    and score where they were. Played on, it is gone: the next power cycle
    is the title. And a console switched on shows the publisher's logo on
    white before the title (gba/splash.c) — if the build has one — which
    every other check skips (harness.SHOW_SPLASH).
    """
    from . import harness
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen
    failures = []

    def hold(keys, frames):
        core.set_keys(*[KEYS[k] for k in keys]); run(core, frames)

    def u8(k):
        return core.memory.u8[base + off[k]]

    run(core, 20)
    press_start(core); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 90)          # 1 PLAYER, playing
    hold(["L", "R", "SELECT"], 6); hold([], 30)
    if not u8("paused"):
        failures.append("L+R+SELECT con la partida en marcha no la pausa")
    y, piece = u8("y"), u8("current")
    score = bytes(core.memory.u8[base + off["score"] + i] for i in range(4))

    core.reset()                              # off and on: the battery stays
    run(core, 60)
    rows = " ".join(tilemap_text(core, r) for r in range(20))
    back = bytes(core.memory.u8[base + off["score"] + i] for i in range(4))
    if not u8("active") or not u8("paused"):
        failures.append(f"al encender no vuelve la partida en pausa "
                        f"(activa {u8('active')}, pausa {u8('paused')}): "
                        f"{' '.join(rows.split())[:80]!r}")
    elif (u8("y"), u8("current"), back) != (y, piece, score):
        failures.append("la partida que vuelve no es la que se apago")
    else:
        print("  apagar en pausa y encender: la misma partida, en pausa")
        hold(["START"], 4); hold([], 30)
        if u8("paused"):
            failures.append("START no reanuda la partida recuperada")
        core.reset()
        run(core, 60)
        if u8("active") and u8("paused"):
            failures.append("jugada la partida, el siguiente encendido "
                            "la vuelve a traer")
        else:
            print("  reanudada, el siguiente encendido ya no la trae")

    if os.path.exists(os.path.join(os.path.dirname(rom_path), "..", "gba",
                                   "splash_logo.h")):
        harness.SHOW_SPLASH[0] = True
        try:
            core2, screen2 = load(rom_path)
            # The Game Boy Player's logo first, when the build has it
            # (about a second and a half), then the publisher's.
            run(core2, 160)
            px = pixels(screen2)
            white = px[5][5]
            reds = sum(1 for row in px for (r, g, b) in row
                       if r > 200 and g < 80 and b < 80)
            run(core2, 240)
            if white[0] < 230 or white[1] < 230 or white[2] < 230 or reds < 500:
                failures.append(f"al encender no sale el logo sobre blanco "
                                f"(esquina {white}, {reds} px rojos)")
            elif core2._native.memory.io[0] & 0x87 != 0:   # mode 0, lit
                failures.append("tras el logo no llega el titulo")
            else:
                print(f"  al encender: el logo sobre blanco ({reds} px rojos), "
                      f"y luego el titulo")
        finally:
            harness.SHOW_SPLASH[0] = False
    else:
        print("  (sin gba/splash_logo.h: esta construccion no tiene logo)")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la partida en pausa sobrevive al apagado, y el logo abre el "
          "encendido.")
    return erase_check(rom_path)


def erase_check(rom_path):
    """L+R+B AT POWER-ON ERASES THE HIGH SCORES, ASKED TWICE.

    The question comes up with NO chosen; B (or NO) goes on to the game as
    if nothing was asked — here, the paused game kept on the battery comes
    back. YES and YES again puts every table back to the cartridge's cold
    one and drops the paused game with them (gba/frontend.c,
    erase_records_prompt).
    """
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    core, screen = load(rom_path)
    _ = screen
    failures = []

    def hold(keys, frames):
        core.set_keys(*[KEYS[k] for k in keys]); run(core, frames)

    def tap(k):
        hold([k], 4); hold([], 12)

    def susp():
        return bytes(core.memory.u8[0x0E004000 + i] for i in range(4)) == b"SUSP"

    def text():
        return " ".join(" ".join(tilemap_text(core, r).split()) for r in range(8, 17))

    run(core, 20)
    press_start(core); run(core, 10)
    press_start(core); run(core, 12)
    press_start(core); run(core, 60)
    tap("START")                              # paused: on the battery
    if not susp():
        failures.append("no hay partida guardada de la que partir")
    core.reset(); hold(["L", "R", "B"], 10); hold([], 20)
    if "ERASE ALL HIGH SCORES?" not in text() or "> NO" not in text():
        failures.append(f"L+R+B al encender no pregunta, con NO elegido: "
                        f"{text()[:80]!r}")
    tap("B"); run(core, 40)
    if not susp() or not core.memory.u8[base + off["paused"]]:
        failures.append("decir que no borra algo, o no sigue a la partida")
    else:
        print("  L+R+B al encender pregunta con NO elegido; B sigue a la "
              "partida guardada")
    core.reset(); hold(["L", "R", "B"], 10); hold([], 20)
    tap("DOWN"); tap("A")
    if "ARE YOU SURE?" not in text() or "> NO" not in text():
        failures.append(f"no pregunta otra vez con NO elegido: {text()[:80]!r}")
    tap("DOWN"); tap("A"); run(core, 30)
    magic = bytes(core.memory.u8[0x0E000000 + i] for i in range(4))
    if susp() or magic != b"LOGG":
        failures.append(f"SI y SI no deja la memoria borrada y escrita "
                        f"(partida {susp()}, firma {magic!r})")
    else:
        print("  SI dos veces: tablas de fabrica escritas y la partida "
              "guardada borrada")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: borrar los records pregunta dos veces y solo borra con SI.")
    return gbp_check(rom_path)


# GBATEK's table ("GBA Gameboy Player"): what the Player sends, and what the
# game must answer — the answer goes out on the NEXT transfer. The second
# row's high half is anything at all ("xxxx").
GBP_TABLE = [
    (0x0000494E, 0x494EB6B1), (0xFFFF494E, 0x494EB6B1),
    (0xB6B1494E, 0x544EB6B1), (0xB6B1544E, 0x544EABB1),
    (0xABB1544E, 0x4E45ABB1), (0xABB14E45, 0x4E45B1BA),
    (0xB1BA4E45, 0x4F44B1BA), (0xB1BA4F44, 0x4F44B0BB),
    (0xB0BB4F44, 0x8000B0BB), (0xB0BB8002, 0x10000010),
    (0x10000010, 0x20000013), (0x20000013, 0x40000004),
]


class FakePlayer:
    """A GAME BOY PLAYER, as far as the game can tell. While its logo is on
    the screen it holds all four directions one frame in three (030Fh,
    GBATEK); once the game has its port in 32-bit normal mode with the
    start bit set, it clocks one transfer a frame through GBP_TABLE and
    then 30000003h for ever, keeping what the game answered."""

    def __init__(self, core, logo):
        self.core = core
        self.logo = logo            # the PNG's pixels, or None
        self.logo_frames = 0
        self.sent = 0
        self.answers = []

    def frame(self, keys=()):
        from mgba._pylib import lib
        core = self.core
        on_logo = self.logo is not None and self.logo == self.screen_pixels()
        if on_logo:
            self.logo_frames += 1
        press = list(keys)
        if on_logo and self.logo_frames % 3 == 0:
            press += [KEYS["LEFT"], KEYS["RIGHT"], KEYS["UP"], KEYS["DOWN"]]
        core.set_keys(*press)
        core.run_frame()
        io = core._native.memory.io
        cnt = io[0x128 >> 1]
        if (cnt & 0x3000) == 0x1000 and (cnt & 0x0081) == 0x0080:
            out = io[0x120 >> 1] | (io[0x122 >> 1] << 16)
            if self.sent:
                self.answers.append(out)
            word = (GBP_TABLE[self.sent][0] if self.sent < len(GBP_TABLE)
                    else 0x30000003)
            io[0x120 >> 1] = word & 0xFFFF
            io[0x122 >> 1] = word >> 16
            io[0x128 >> 1] = cnt & ~0x0080
            self.sent += 1
            lib.GBARaiseIRQ(core._native, 7, 0)
            for _ in range(1500):
                core.step()

    def screen_pixels(self):
        return self.pixels_of(self.screen)

    @staticmethod
    def pixels_of(screen):
        return screen.to_pil().convert("RGB").tobytes()


def gbp_check(rom_path):
    """THE GAME BOY PLAYER: ITS LOGO, ITS ANSWER, ITS HANDSHAKE, ITS RUMBLE.

    A console switched on shows the Player's logo before the publisher's
    (gba/gbp.c); a Player (FakePlayer) answers on the pad while it is up,
    and the game takes the port for it: GBATEK's handshake answered row by
    row, then "rumble off" until a clear, which turns it on. A plain GBA —
    nobody answering — leaves the port to the cable. Skipped without the
    logo, which is Nintendo's and generated (tools/make_gbp_logo.py)."""
    from . import harness
    import os as _os
    root = _os.path.join(_os.path.dirname(rom_path), "..")
    if not _os.path.exists(_os.path.join(root, "gba", "gbp_logo.h")):
        print("SALTADO: sin gba/gbp_logo.h esta construccion no busca un "
              "Game Boy Player")
        return 0
    png = _os.path.join(_os.path.dirname(rom_path), "gbp_logo.png")
    if not _os.path.exists(png):
        print(f"SALTADO: no encuentro {png} para reconocer el logo")
        return 0
    from PIL import Image
    logo = Image.open(png).convert("RGB").tobytes()
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    failures = []

    harness.SHOW_SPLASH[0] = True
    try:
        core, screen = load(rom_path)
    finally:
        harness.SHOW_SPLASH[0] = False
    player = FakePlayer(core, logo)
    player.screen = screen
    for _ in range(400):
        player.frame()
    if player.logo_frames < 3:
        failures.append(f"el logo del Game Boy Player no sale entero "
                        f"({player.logo_frames} frames)")
    want = [r for _, r in GBP_TABLE]
    got = player.answers[:len(want)]
    if got != want:
        bad = next((i for i in range(min(len(got), len(want)))
                    if got[i] != want[i]), len(got))
        failures.append(f"el saludo se tuerce en la fila {bad}: "
                        f"{[hex(x) for x in got[:bad + 1]]}")
    elif any(a != 0x40000004 for a in player.answers[len(want):]):
        failures.append("sin jugar, la vibracion no esta apagada")
    else:
        print(f"  el logo {player.logo_frames} frames; el Player lo ve, y el "
              f"saludo NINTENDO sale fila por fila; vibracion apagada")

    # A game, four rows ready to go, a piece dropped on them: the clear
    # turns the motor on, and it goes off again.
    # Title, GAME SELECT, and on a Player the settings' three pages
    # (tv_menu_check).
    for wait in (10, 12, 12, 12, 60):
        player.frame([KEYS["START"]])
        for _ in range(wait):
            player.frame()
    fill_rows(core, base + off["field"], [16, 17, 18, 19])
    mark = len(player.answers)
    for _ in range(240):
        player.frame([KEYS["DOWN"]])
    after = player.answers[mark:]
    if 0x40000026 not in after:
        failures.append("una limpieza no enciende la vibracion")
    elif after[-1] != 0x40000004:
        failures.append("la vibracion no se apaga despues")
    else:
        n = after.count(0x40000026)
        print(f"  un TETRIS: vibracion encendida {n} frames, y apagada luego")

    # And a plain GBA: nobody answers the logo, the port stays the cable's.
    harness.SHOW_SPLASH[0] = True
    try:
        core2, screen2 = load(rom_path)
    finally:
        harness.SHOW_SPLASH[0] = False
    run(core2, 400)
    cnt = core2._native.memory.io[0x128 >> 1]
    if (cnt & 0x3000) != 0x2000:
        failures.append(f"en una GBA sin Player el puerto no queda en "
                        f"multijugador (SIOCNT {cnt:#06x})")
    else:
        print("  en una GBA sin Player: el puerto sigue siendo del cable")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el Game Boy Player se reconoce, saluda y vibra.")
    return tv_menu_check(rom_path)


def tv_menu_check(rom_path, shots=None):
    """ON A GAME BOY PLAYER, LEVEL SETTINGS IS THE CARTRIDGE'S THREE PAGES.

    Found a Player (FakePlayer), the settings come one to a page — LEVEL,
    HANDICAP, MUSIC — each a column with the arrow beside the choice (the
    ROM's own shape, for a television across a room): DOWN moves down the
    column, START turns the page, B turns it back, and START on the last one
    plays, with what the pages chose. On a plain GBA it stays one page.
    `shots`, a directory, keeps a picture of each page.
    """
    from . import harness
    import os as _os
    png = _os.path.join(_os.path.dirname(rom_path), "gbp_logo.png")
    root = _os.path.join(_os.path.dirname(rom_path), "..")
    if not (_os.path.exists(png) and
            _os.path.exists(_os.path.join(root, "gba", "gbp_logo.h"))):
        print("SALTADO: sin el logo no hay Game Boy Player que encontrar")
        return 0
    from PIL import Image
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    failures = []

    def boot():
        harness.SHOW_SPLASH[0] = True
        try:
            core, screen = load(rom_path)
        finally:
            harness.SHOW_SPLASH[0] = False
        player = FakePlayer(core, Image.open(png).convert("RGB").tobytes())
        player.screen = screen
        for _ in range(400):
            player.frame()
        return core, screen, player

    def text(core):
        return " ".join(" ".join(tilemap_text(core, r).split()) for r in range(7, 18))

    def snap(screen, name):
        if shots:
            screen.to_pil().convert("RGB").resize((480, 320)).save(
                _os.path.join(shots, name))

    core, screen, player = boot()

    def tap(k, wait=12):
        player.frame([KEYS[k]])
        for _ in range(wait):
            player.frame()

    tap("START", 10)                       # title -> GAME SELECT (1 PLAYER)
    tap("START", 12)                       # -> LEVEL SETTINGS, page one
    t = text(core)
    if "LEVEL" not in t or "> 0" not in t or "HANDICAP" in t:
        failures.append(f"la primera pagina no es LEVEL en columna: {t[:80]!r}")
    for _ in range(3):
        tap("DOWN")
    snap(screen, "tv_level.png")
    tap("START")
    t = text(core)
    if "HANDICAP" not in t or "> 0" not in t:
        failures.append(f"la segunda pagina no es HANDICAP: {t[:80]!r}")
    tap("DOWN")
    snap(screen, "tv_handicap.png")
    tap("B")
    if "LEVEL" not in text(core) or "> 3" not in text(core):
        failures.append("B no vuelve a LEVEL con el 3 elegido")
    tap("START")
    tap("START")
    t = text(core)
    if "MUSIC" not in t or "LOGINSKA" not in t:
        failures.append(f"la tercera pagina no es MUSIC: {t[:80]!r}")
    snap(screen, "tv_music.png")
    tap("START", 60)
    level = core.memory.u8[base + off["level"]]
    if not core.memory.u8[base + off["active"]]:
        failures.append("START en MUSIC no empieza la partida")
    elif level != 3:
        failures.append(f"la partida no empieza en el nivel elegido ({level})")
    else:
        print("  LEVEL, HANDICAP y MUSIC, cada uno su pagina en columna; B "
              "vuelve, START pasa y en MUSIC juega, en el nivel elegido")

    # A race against the computer behind the chord: two handicap columns,
    # and XE's twenty levels in two columns of ten.
    core, screen, player = boot()
    tap("START", 10)
    tap("L", 0); player.frame([KEYS["L"], KEYS["R"]]); tap("R", 12)
    for _ in range(3):
        tap("DOWN")                        # 1P -> 2P -> COOP -> VERSUS
    tap("START", 12)
    snap(screen, "tv_level_xe.png")
    if "19" not in text(core):
        failures.append("con el acorde, LEVEL no ensena los veinte niveles")
    tap("START")
    tap("RIGHT")
    tap("DOWN")
    t = text(core)
    snap(screen, "tv_handicap_vs.png")
    if "1P" not in t or "2P" not in t:
        failures.append(f"en VERSUS con el acorde no hay dos columnas: {t[:80]!r}")
    else:
        print("  con el acorde: veinte niveles en dos columnas, y en VERSUS un "
              "handicap por columna")

    # And without a Player, the one page.
    core2, screen2 = load(rom_path)
    run(core2, 20); press_start(core2); run(core2, 10)
    press_start(core2); run(core2, 12)
    if "MUSIC" not in " ".join(tilemap_text(core2, r) for r in range(8, 18)):
        failures.append("sin Player ya no es la pagina unica")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: en un Game Boy Player los ajustes son las tres paginas del "
          "cartucho.")
    return 0
