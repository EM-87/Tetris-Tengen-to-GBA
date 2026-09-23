"""
The pause menu the chord uncovers: its lines, its box, its tune
and its way out.
"""
from .harness import (
    KEYS, LEADER_HEAD_TY, MUSIC_COUNT_MAX, MUSIC_ROW,
    PMENU_H, PMENU_TX, PMENU_TY, PMENU_W_T, pmenu_span,
    PM_EXIT, PM_HEAD, PM_L, PM_MUSIC,
    PM_R, PM_SURE, PM_TUNE, REG_SOUND1CNT_X,
    SCREENBLOCK_ADDR, SCREEN_TW_TILES, SCREEN_W, TILE,
    game_offsets, game_state_address, load, pixels,
    press_start, run, sound_state, start_game,
    tilemap_text,
)


def pausemenu_check(rom_path):
    """THE PAUSE MENU, AND THE CHORD THAT IS NOT IN THE GAME.

    Not the cartridge's — its PAUSE is a plaque and nothing else — so what
    this checks is that it behaves: that MUSIC really changes the tune while
    the game is held, that EXIT asks before it does anything, that NO comes
    back, and that YES leaves by the same road a finished game leaves by. And
    that the cheat codes cannot be typed through it, which is the one way it
    could break something that already worked.

    THE CHORD IS RUNG ON THE MENUS, not on the plaque. L+R on GAME SELECT or
    on LEVEL SETTINGS uncovers this menu and the hidden tunes together; in
    play the same chord swaps the HUD and uncovers nothing, so a plaque that
    has not been unlocked stays a plaque however long you hold the shoulders
    on it. Both halves are checked: that the game screen does NOT open it, and
    that the menu screen does.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def row(r):
        """Only the pause box's own columns: see PM_L and pmenu_span."""
        return tilemap_text(core, r, *pmenu_span(core))

    # FIRST: a game with the chord never rung. The plaque must stay a plaque.
    start_game(core)
    run(core, 30)
    tap("START")
    if not core.memory.u8[base + off["paused"]]:
        failures.append("START no pausa")
    tap("L", "R")
    if "PAUSE" in row(PM_HEAD) and "EXIT" in row(PM_EXIT):
        failures.append("L+R en la partida abre el menu: el acorde es de los "
                         "menus, y en juego solo cambia el HUD")
    else:
        print("  L+R en la partida no abre nada: el acorde ya no vive aqui")

    # ...now ring it where it lives, and come back into a game.
    core, screen = load(rom_path)
    run(core, 8)
    press_start(core); run(core, 10)    # title -> GAME SELECT
    tap("L", "R")                        # the chord, on the first menu
    press_start(core); run(core, 12)     # -> LEVEL SETTINGS
    press_start(core); run(core, 30)     # -> play
    tap("START")
    if ("PAUSE" not in row(PM_HEAD) or not row(PM_MUSIC).strip()
            or "EXIT" not in row(PM_EXIT)):
        failures.append(f"el acorde en GAME SELECT no abre el menu de pausa: "
                         f"{row(PM_HEAD)!r} / {row(PM_MUSIC)!r} / "
                         f"{row(PM_EXIT)!r}")
        print("FALLA:", failures[-1])
        return 1
    print("  L+R en GAME SELECT destapa el menu, y la pausa ya es el menu")

    # AND THE BOX IS CENTRED ON THE BOARD, which an odd width cannot be: the
    # playfield's ten columns run 10-19, so its middle is x=120, and so is the
    # screen's. Measured off the framebuffer rather than off PMENU_TX, because
    # what went wrong before was the arithmetic and not the drawing.
    # Off the box's OWN tiles, not off lit pixels: the HUD panels are lit on
    # this scanline too. $29/$2A/$2B are the game-over plaque's top-left, top
    # and top-right, which is what the box is framed with.
    top = [c for c in range(SCREEN_TW_TILES)
           if (core.memory.u16[SCREENBLOCK_ADDR + (PMENU_TY * 32 + c) * 2] & 0x3FF)
           in (0x29, 0x2A, 0x2B)]
    if not top:
        failures.append("no se encuentra el borde superior de la caja de pausa")
    else:
        x0, x1 = top[0] * TILE, (top[-1] + 1) * TILE
        middle = (x0 + x1) / 2
        if abs(middle - SCREEN_W / 2) > 1:
            failures.append(f"la caja de pausa no esta centrada: va de x={x0} "
                             f"a {x1 - 1}, centro {middle}, y la pantalla "
                             f"{SCREEN_W / 2}")
        else:
            print(f"  la caja va de x={x0} a {x1 - 1}, centro {middle}: "
                   f"centrada en el tablero")

    before = row(PM_TUNE)
    tap("RIGHT")
    if row(PM_TUNE) == before:
        failures.append("DERECHA no cambia la cancion en el menu de pausa")
    else:
        print(f"  la musica se cambia sin salir: {before.strip()!r} -> "
               f"{row(PM_TUNE).strip()!r}")

    # ...AND MUSIC MIX IS AS QUIET HERE AS IT IS ON THE SETTINGS SCREEN.
    # There is no one tune to audition in a rotation, so the selection screen
    # goes silent on it like it does on NO MUSIC; this menu was starting
    # whichever tune the rotation happened to be on, which made the same
    # choice sound like two different things depending on where you made it.
    # And going quiet must not leave the MATCH quiet: nothing else restarts
    # the engine on the way out of a pause, so the silence is spent on the
    # unpause.
    for _ in range(12):
        if "MIX" in row(PM_TUNE):
            break
        tap("RIGHT")
    if "MIX" not in row(PM_TUNE):
        failures.append("el menu de pausa no ofrece MUSIC MIX")
    else:
        loud = 0
        for _ in range(120):
            core.run_frame()
            st = sound_state(core)
            loud |= st["activos"] | st["pulso 1"] | st["pulso 2"] | st["triangulo"]
        if loud:
            failures.append(f"MUSIC MIX suena en el menu de pausa "
                             f"(canales {loud:#06b}); en el de niveles calla")
        else:
            print("  MUSIC MIX no suena aqui, igual que en la pantalla de "
                  "seleccion")
            # The match has to come back with it, though.
            tap("START", settle=30)
            back = 0
            for _ in range(180):
                core.run_frame()
                st = sound_state(core)
                if st["activos"] or st["pulso 1"] or st["pulso 2"] or st["triangulo"]:
                    back += 1
            if not back:
                failures.append("tras elegir MUSIC MIX en la pausa la partida "
                                 "vuelve muda")
            else:
                print(f"  ...y al reanudar la partida suena "
                      f"({back}/180 frames)")
            # Back into the menu for the checks below.
            tap("START", settle=20)
            tap("L", "R", settle=20)

    # THE CURSOR IS AN ARROW, and it has to be ON the line it marks and on no
    # other -- picking the line out by palette read as a colour scheme rather
    # than as a cursor. $3E is the cartridge's own right arrow and the tileset
    # is ASCII-indexed, so it comes back from tilemap_text as '>'.
    if ">" not in row(PM_MUSIC) or ">" in row(PM_EXIT):
        failures.append(f"la flecha no esta en MUSIC: {row(PM_MUSIC)!r} / "
                         f"{row(PM_EXIT)!r}")
    # ...and SELECT moves it, the way it does on the settings screen.
    tap("SELECT")
    if ">" not in row(PM_EXIT) or ">" in row(PM_MUSIC):
        failures.append(f"SELECT no mueve la flecha: {row(PM_MUSIC)!r} / "
                         f"{row(PM_EXIT)!r}")
    else:
        print("  la flecha marca la linea, y SELECT la mueve")

    # START IS THE WAY OUT FROM EVERY LINE OF IT, the EXIT line included: it
    # is the button that put the plaque up. The cursor is sitting on EXIT
    # right now, which is where it used to open the question instead.
    tap("START")
    if core.memory.u8[base + off["paused"]]:
        failures.append("START sobre EXIT no reanuda la partida")
    else:
        # AND IT TAKES THE WHOLE MENU WITH IT. The frame and the lines that
        # centre exactly are on the main background and a repaint covers
        # those; PAUSE, the tune's name and EXIT are on the counter and
        # offset layers, which the static screen never writes in the middle
        # of the board -- so the window vanished and the words stayed.
        # The board's own frame art decodes as the odd stray letter inside
        # these columns, so this looks for the menu's WORDS and its cursor.
        seen = " ".join(row(r) for r in range(PMENU_TY, PMENU_TY + PMENU_H))
        leftover = [w for w in ("PAUSE", "MUSIC", "EXIT", "LOGINSKA", ">")
                    if w in seen]
        if leftover:
            failures.append("START reanuda pero deja texto del menu en "
                             f"pantalla: {leftover} en {seen!r}")
        else:
            print("  START cierra el menu desde cualquier linea, y no deja "
                   "nada escrito")
    tap("START"); tap("L", "R")

    # The cheat codes must not be reachable through it. The level-up code is
    # Up Down Up Down Left Right B B A; typed here it moves the cursor and
    # picks tunes, and the level must not move.
    level = core.memory.u8[base + off["level"]]
    for name in ("UP", "DOWN", "UP", "DOWN", "LEFT", "RIGHT", "B", "B", "A"):
        tap(name)
    if core.memory.u8[base + off["level"]] != level:
        failures.append("el codigo de subir nivel se cuela por el menu de pausa")
    else:
        print("  los codigos de trucos no atraviesan el menu")

    # Walk to EXIT, which must ASK. (The cheat-code sequence above left the
    # game in whatever state its last button put it in, so make sure it is
    # held before driving the menu.)
    if not core.memory.u8[base + off["paused"]]:
        tap("START")
    while "EXIT?" in row(PM_SURE):
        tap("DOWN"); tap("A")    # back out of a question it may have opened
    while "EXIT" not in row(PM_EXIT):
        tap("START")
    # A is what takes a choice now; START only ever leaves.
    while ">" not in row(PM_EXIT):
        tap("DOWN")
    tap("A")
    if "EXIT?" not in row(PM_SURE):
        failures.append(f"EXIT no pregunta antes de salir: {row(PM_SURE)!r}")
    else:
        print("  EXIT pregunta antes de nada")
        # NO comes back to the game, still paused, still playing.
        tap("A")
        if not core.memory.u8[base + off["paused"]]:
            failures.append("decir NO al salir dejo la partida sin pausa")
        elif "EXIT?" in row(PM_SURE):
            failures.append("decir NO no cierra la pregunta")
        else:
            print("  NO vuelve a la partida")
        # ...and YES leaves STRAIGHT to the title: a game you walked out of
        # has not ended, and its score has no business on the board.
        while "EXIT?" not in row(PM_SURE):
            while ">" not in row(PM_EXIT):
                tap("DOWN")
            tap("A")
        tap("LEFT"); tap("A"); run(core, 60)
        if "HIGH SCORES" in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
            failures.append("salir a la fuerza pasa por la tabla de records")
        elif "EXIT" in row(PM_EXIT) or "PAUSE" in row(PM_HEAD):
            failures.append("decir SI no sale de la partida")
        else:
            print("  SI sale al titulo, sin pasar por la tabla")

    # ONCE FOUND, STILL FOUND. The chord is a thing you discover, not a thing
    # you should have to remember to do at the start of every game — so the
    # next game's PAUSE is the menu, with no chord. And the HUD you picked is
    # still the HUD you picked.
    press_start(core); run(core, 10)    # title -> game select
    press_start(core); run(core, 12)    # -> level settings
    press_start(core); run(core, 30)    # -> play
    tap("START")
    if "PAUSE" not in row(PM_HEAD) or "EXIT" not in row(PM_EXIT):
        failures.append("el menu de pausa se pierde al empezar otra partida")
    else:
        print("  y en la siguiente partida la pausa ya es el menu, sin acorde")
    tap("START")

    # ...and so does the HUD. The banner's letters are art, not ASCII, so this
    # reads the tilemap directly: the right column carries them in HUD Banner
    # and is blank there in HUD Stats.
    def hud():
        row10 = [core.memory.u16[SCREENBLOCK_ADDR + ((10 * 32 + c) * 2)] & 0x3FF
                 for c in range(24, 28)]
        return "BANNER" if any(row10) else "STATS"

    if hud() != "BANNER":
        failures.append(f"la partida no abre en HUD Banner sino en {hud()}")
    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 20)
    if hud() != "STATS":
        failures.append("SELECT en juego no cambia el HUD")
    else:
        # quit out and come back
        tap("START")
        while ">" not in row(PM_EXIT):
            tap("DOWN")
        tap("A"); tap("LEFT"); tap("A"); run(core, 60)
        press_start(core); run(core, 10)
        press_start(core); run(core, 12)
        press_start(core); run(core, 30)
        if hud() != "STATS":
            failures.append("el HUD elegido se pierde al empezar otra partida")
        else:
            print("  y el HUD que elegiste sigue siendo el tuyo")

    failures += pause_menu_visible(rom_path)
    failures += pause_resume_music(rom_path)

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el menu de pausa cambia la musica, sale preguntando, y se queda.")
    return 0


def pause_menu_visible(rom_path):
    """EL MAPA DE TILES NO ES LO QUE SE VE, Y AQUI ESO IMPORTA.

    Todo lo que este fichero comprueba del menu de pausa lo lee del mapa de
    tiles, y el mapa mintio: durante un rato el menu estaba escrito entero y
    en pantalla le faltaban el titulo PAUSE, la linea MUSIC y el borde de
    arriba de la caja. La causa no era el dibujo sino el RELOJ -- partir
    gba/main.c en cinco ficheros le quito al compilador el inline entre ellos,
    el frame crecio un 5% y el dibujado empezo a pasarse del blanco vertical.
    Pasarse no se ve como lentitud: se ve como que falta la PARTE DE ARRIBA de
    lo ultimo que se dibuja, porque el haz ya ha pasado por esas lineas cuando
    se escriben.

    Asi que esto mira PIXELES. La caja se dibuja la ultima de todo el frame,
    de modo que es el canario: si su fila de arriba y su titulo llegan a la
    pantalla, el dibujado cabe en el blanco.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=16):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    run(core, 40)
    press_start(core); run(core, 30)
    tap("L", "R"); run(core, 20)            # el acorde, en el menu
    press_start(core); run(core, 24)
    press_start(core); run(core, 90)
    tap("START"); run(core, 60)
    rows = pixels(core if False else screen)

    def lit(ty, tx0, tx1):
        return sum(1 for y in range(ty * TILE, (ty + 1) * TILE)
                   for x in range(tx0 * TILE, tx1 * TILE)
                   if rows[y][x] != (0, 0, 0))

    # Las filas de la caja, de arriba abajo: el borde, PAUSE, MUSIC, el nombre
    # de la cancion, EXIT y el borde de abajo. Ninguna puede salir vacia.
    want = ((PMENU_TY, "el borde de arriba de la caja"),
            (PM_HEAD, "el titulo PAUSE"),
            (PM_MUSIC, "la linea MUSIC"),
            (PM_TUNE, "el nombre de la cancion"),
            (PM_EXIT, "la linea EXIT"),
            (PMENU_TY + PMENU_H - 1, "el borde de abajo de la caja"))
    for ty, what in want:
        n = lit(ty, PMENU_TX + 2, PMENU_TX + PMENU_W_T - 2)
        if n < 20:
            failures.append(f"{what} no llega a la pantalla ({n} pixeles "
                             f"encendidos en la fila {ty}): el dibujado se "
                             f"esta pasando del blanco vertical")
        else:
            print(f"  {what}: {n} pixeles en pantalla")
    del core, screen
    return failures


def pause_resume_music(rom_path):
    """UNA PAUSA SUSPENDE LA CANCION; NO LA REBOBINA.

    Las canciones del cartucho ya lo hacian solas: pausar manda MUSIC_SUSPEND
    y reanudar MUSIC_RESUME, y el motor vuelve donde estaba. Las dos entradas a
    mano tienen su propio secuenciador, y la salida de la pausa llamaba a
    handtune_start, que devuelve las dos voces al primer compas: Korobeiniki y
    Katiuska empezaban de nuevo tras cada pausa y tras cada visita a la linea
    MUSIC del menu, que es justo lo que se reporto.

    No se compara frame contra frame -- el retardo al reanudar es variable y
    eso derrota cualquier comparacion sin alinear. Se graba la melodia UNA vez
    desde una partida limpia como lista de notas, y se le pregunta al caso con
    pausa por que entrada de esa lista sigue: continuar es la nota siguiente a
    la ultima oida, reiniciar es la entrada cero.
    """
    failures = []
    for want in ("KOROBEINIKI", "KATIUSKA"):
        got = tune_notes(rom_path, want, pause_at=None, frames=1400)
        if got is None:
            failures.append(f"no se puede elegir {want} en LEVEL SETTINGS")
            continue
        melody = got
        heard, after = tune_notes(rom_path, want, pause_at=600, frames=400)
        n = len(heard)
        if heard != melody[:n]:
            failures.append(f"{want}: lo oido antes de pausar no sigue a la "
                             f"melodia de referencia")
            continue
        # La nota que sonaba al caer la pausa se sostiene a traves de ella, asi
        # que una continuacion puede retomar una entrada a un lado u otro de
        # donde se quedo la cuenta.
        at = [i for i in range(len(melody) - 5) if melody[i:i + 6] == after[:6]]
        if not at:
            failures.append(f"{want}: al reanudar suena algo que no esta en la "
                             f"melodia: {after[:6]}")
        elif at == [0]:
            failures.append(f"{want}: al reanudar la cancion EMPIEZA DE NUEVO")
        elif not any(abs(i - n) <= 2 for i in at):
            failures.append(f"{want}: al reanudar salta a la nota {at}, y se "
                             f"habia quedado en la {n}")
        else:
            print(f"  {want} se reanuda en la nota {min(at, key=lambda i: abs(i - n))} "
                   f"de {len(melody)}, donde la dejo la pausa")
    return failures


def tune_notes(rom_path, want, pause_at, frames):
    """Las notas que suenan, una entrada por nota y no por frame.

    Con pause_at a None devuelve la lista entera de una partida sin tocar.
    Con un numero de frames devuelve (lo oido antes de pausar, lo oido tras
    reanudar), con la pausa echada en ese frame.
    """
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def notes(n):
        out, last = [], None
        for _ in range(n):
            core.run_frame()
            p = core.memory.u16[REG_SOUND1CNT_X] & 0x7FF
            if p and p != last:
                out.append(p)
            last = p
        return out

    run(core, 40)
    tap("START"); run(core, 20)
    tap("L", "R"); run(core, 20)         # destapa las canciones extra
    tap("START"); run(core, 20)
    tap("SELECT"); tap("SELECT")          # el cursor sobre MUSIC
    for _ in range(MUSIC_COUNT_MAX):
        if want in tilemap_text(core, MUSIC_ROW):
            break
        tap("RIGHT")
    if want not in tilemap_text(core, MUSIC_ROW):
        return None
    tap("START"); run(core, 30)
    if pause_at is None:
        return notes(frames)
    heard = notes(pause_at)
    tap("START", settle=30)               # pausa
    run(core, 120)                        # un rato sobre el cartel
    tap("START", settle=30)               # reanuda
    return heard, notes(frames)
