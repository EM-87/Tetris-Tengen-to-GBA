"""
The pause menu the chord uncovers: its lines, its box, its tune
and its way out.
"""
from .harness import (
    KEYS, LEADER_HEAD_TY, MUSIC_COUNT_MAX, MUSIC_ROW, check_layout,
    PMENU_H, PMENU_TX, PMENU_TY, PMENU_W_T, pmenu_span,
    PM_EXIT, PM_HEAD, PM_L, PM_MUSIC, PM_OPTIONS,
    PM_R, PM_SURE, PMO_BOX, PMO_FIRST, PMO_H, PMO_HEAD, PMO_NAME, PMO_NAMES,
    PMO_TY, PMO_TX, PMO_W, PAL_PIECE_BANK,
    PMT_FIRST, PMT_H, PMT_HEAD, PMT_TY, REG_SOUND1CNT_X,
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

    # MUSIC OPENS THE TUNES' WINDOW, the way EXIT GAME opens its
    # question: every tune on a line of its own under MUSIC, the cursor on
    # the one playing. A takes the one under the cursor and goes back to the
    # menu; B goes back with nothing changed. LEFT and RIGHT, which used to
    # change the tune in place, change nothing now.
    # AND THE WINDOW PLAYS THE TUNE UNDER ITS ARROW, while the menu around it
    # plays nothing: an audition in the one, the pause's silence in the
    # other (tunes_move, tunes_close).
    music, _ = game_state_address(rom_path, "g_music")

    def heard(frames=60):
        n = 0
        for _ in range(frames):
            core.run_frame()
            st = sound_state(core)
            if st["activos"] or st["pulso 1"] or st["pulso 2"] or st["triangulo"]:
                n += 1
        return n

    def tunes():
        return [tilemap_text(core, PMT_FIRST + i, PM_L, PM_R)
                for i in range(PMT_H - 3)]

    def marked():
        got = [i for i, t in enumerate(tunes()) if ">" in t]
        return got[0] if len(got) == 1 else None

    if row(PM_MUSIC).replace("->", "").strip() != "MUSIC":
        failures.append(f"la entrada de la musica no dice MUSIC: "
                        f"{row(PM_MUSIC)!r}")
    tap("RIGHT"); tap("LEFT")
    if music is not None and core.memory.u8[music] != 0:
        failures.append("IZQUIERDA o DERECHA aun cambian la cancion en el menu")
    tap("A")
    names = [t.replace("->", "").strip() for t in tunes()]
    if "MUSIC" not in tilemap_text(core, PMT_HEAD, PM_L, PM_R):
        failures.append(f"A en MUSIC no abre la ventana MUSIC: "
                        f"{tilemap_text(core, PMT_HEAD, PM_L, PM_R)!r}")
    elif names[0] != "NO MUSIC" or names[-1] != "MUSIC MIX" or \
            "KOROBEINIKI" not in names:
        failures.append(f"la ventana no lista las ocho canciones: {names}")
    elif marked() != 0:
        failures.append(f"la flecha no abre en la cancion que suena "
                        f"(NO MUSIC): fila {marked()}")
    else:
        print(f"  MUSIC abre su ventana con las {len(names)} canciones, la "
              f"flecha en la que suena")
        tap("DOWN")
        auditioned = heard()
        tap("DOWN")
        tap("B")
        quiet = heard()
        if core.memory.u8[music] != 0 or "MUSIC" not in row(PM_MUSIC):
            failures.append("B en la ventana cambia la cancion o no vuelve al "
                            "menu")
        elif auditioned < 30:
            failures.append(f"con la flecha en LOGINSKA la ventana no la toca "
                            f"({auditioned} de 60 frames con sonido)")
        elif quiet:
            failures.append(f"de vuelta en el menu de pausa sigue sonando "
                            f"({quiet} de 60 frames)")
        else:
            tap("A")
            tap("DOWN")
            tap("A")
            quiet = heard()
            if core.memory.u8[music] != 1 or "MUSIC" not in row(PM_MUSIC):
                failures.append(f"A en LOGINSKA no la elige o no vuelve al "
                                f"menu: g_music {core.memory.u8[music]}")
            elif quiet:
                failures.append(f"elegida LOGINSKA, el menu de pausa la toca "
                                f"({quiet} de 60 frames)")
            else:
                print("  la flecha toca la cancion que pisa; B vuelve sin tocar "
                      "nada, A la elige, y el menu calla")
    # ...and the window, its eleven rows, gone once it is: no line of it left
    # above or below the menu's box.
    left = [r for r in (PMT_TY + 1, PMT_TY + 2, PMT_TY + PMT_H - 2)
            if tilemap_text(core, r, PM_L, PM_R).strip("- >")
            in ("MUSIC", "NO MUSIC", "MUSIC MIX")]
    if left:
        failures.append(f"al cerrar la ventana quedan lineas suyas en las "
                        f"filas {left}")

    # ...AND MUSIC MIX IS AS QUIET HERE AS IT IS ON THE SETTINGS SCREEN.
    # There is no one tune to audition in a rotation, so the selection screen
    # goes silent on it like it does on NO MUSIC; this menu was starting
    # whichever tune the rotation happened to be on, which made the same
    # choice sound like two different things depending on where you made it.
    # And going quiet must not leave the MATCH quiet: nothing else restarts
    # the engine on the way out of a pause, so the silence is spent on the
    # unpause. The window opens on LOGINSKA, just chosen; UP twice goes past
    # the top of the list and round to it.
    tap("A"); tap("UP"); tap("UP")
    if "MIX" not in tunes()[-1] or marked() != len(tunes()) - 1:
        failures.append("ARRIBA desde arriba no da la vuelta a MUSIC MIX")
    else:
        tap("A")
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
    # ...and SELECT moves it down, the way it does on the settings screen:
    # MUSIC, OPTIONS, EXIT GAME. UP goes back up, round past the top.
    if "OPTIONS" not in row(PM_OPTIONS):
        failures.append(f"el menu no tiene OPTIONS entre MUSIC y EXIT GAME: "
                        f"{row(PM_OPTIONS)!r}")
    tap("SELECT")
    on_options = ">" in row(PM_OPTIONS) and ">" not in row(PM_MUSIC)
    tap("SELECT")
    if not on_options or ">" not in row(PM_EXIT) or ">" in row(PM_OPTIONS):
        failures.append(f"SELECT no baja la flecha de linea en linea: "
                        f"{row(PM_MUSIC)!r} / {row(PM_OPTIONS)!r} / "
                        f"{row(PM_EXIT)!r}")
    else:
        tap("UP"); tap("UP"); tap("UP")
        if ">" not in row(PM_EXIT):
            failures.append("ARRIBA no sube la flecha, o no da la vuelta")
        else:
            print("  la flecha marca la linea, SELECT la baja y ARRIBA la sube")

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
        leftover = [w for w in ("PAUSE", "MUSIC", "OPTIONS", "EXIT", "LOGINSKA", ">")
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
    while "SURE?" in row(PM_SURE):
        tap("DOWN"); tap("A")    # back out of a question it may have opened
    while "EXIT" not in row(PM_EXIT):
        tap("START")
    # A is what takes a choice now; START only ever leaves.
    while ">" not in row(PM_EXIT):
        tap("DOWN")
    tap("A")
    if "SURE?" not in row(PM_SURE):
        failures.append(f"EXIT no pregunta antes de salir: {row(PM_SURE)!r}")
    else:
        print("  EXIT pregunta antes de nada")
        # NO comes back to the game, still paused, still playing.
        tap("A")
        if not core.memory.u8[base + off["paused"]]:
            failures.append("decir NO al salir dejo la partida sin pausa")
        elif "SURE?" in row(PM_SURE):
            failures.append("decir NO no cierra la pregunta")
        else:
            print("  NO vuelve a la partida")
        # ...and YES leaves STRAIGHT to the title: a game you walked out of
        # has not ended, and its score has no business on the board.
        while "SURE?" not in row(PM_SURE):
            while ">" not in row(PM_EXIT):
                tap("DOWN")
            tap("A")
        tap("LEFT"); tap("A"); run(core, 60)
        if "-LINES" in tilemap_text(core, LEADER_HEAD_TY, 0, 30):
            failures.append("salir a la fuerza pasa por la tabla de records")
        elif "EXIT" in row(PM_EXIT) or "PAUSE" in row(PM_HEAD):
            failures.append("decir SI no sale de la partida")
        # ...and a game walked out of is not one to come back to at the next
        # power-on: the paused game gba/suspend.c keeps in the save memory
        # ($0E004000, "SUSP") is gone.
        elif bytes(core.memory.u8[0x0E004000 + i] for i in range(4)) == b"SUSP":
            failures.append("salir con EXIT deja la partida guardada para el "
                            "siguiente encendido")
        else:
            print("  SI sale al titulo, sin pasar por la tabla, y la partida "
                  "guardada se borra")

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
    failures += pause_options(rom_path)
    failures += pause_ticks_hold_the_tune(rom_path)

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

    # BAJO EL ACORDE EL TABLERO ES UN MURO DE LADRILLOS, y un ladrillo
    # tambien enciende pixeles: una fila de la caja que no llegase dejaria ver
    # los de debajo y contaria como dibujada. Asi que ademas de tinta, en la
    # fila no puede haber el gris medio de los ladrillos (el $10 de
    # piecePaletteIndex0), que la caja no usa.
    BRICK = (156, 148, 156)

    def lit(ty, tx0, tx1):
        px = [rows[y][x] for y in range(ty * TILE, (ty + 1) * TILE)
              for x in range(tx0 * TILE, tx1 * TILE)]
        return sum(1 for p in px if p != (0, 0, 0)), sum(p == BRICK for p in px)

    def look(want):
        for ty, what in want:
            n, bricks = lit(ty, PMENU_TX + 2, PMENU_TX + PMENU_W_T - 2)
            if n < 20 or bricks:
                failures.append(f"{what} no llega a la pantalla ({n} pixeles "
                                 f"encendidos en la fila {ty}, {bricks} de "
                                 f"ladrillo): el dibujado se esta pasando del "
                                 f"blanco vertical")
            else:
                print(f"  {what}: {n} pixeles en pantalla")

    # Las filas de la caja, de arriba abajo: el borde, PAUSE, MUSIC,
    # OPTIONS, EXIT GAME y el borde de abajo. Ninguna puede salir vacia.
    look(((PMENU_TY, "el borde de arriba de la caja"),
          (PM_HEAD, "el titulo PAUSE"),
          (PM_MUSIC, "la linea MUSIC"),
          (PM_OPTIONS, "la linea OPTIONS"),
          (PM_EXIT, "la linea EXIT GAME"),
          (PMENU_TY + PMENU_H - 1, "el borde de abajo de la caja")))
    # ...y las de la ventana de canciones, la caja mas grande que hay.
    tap("A"); run(core, 30)
    rows = pixels(screen)
    look(((PMT_TY, "el borde de arriba de la ventana MUSIC"),
          (PMT_HEAD, "el titulo MUSIC"),
          (PMT_FIRST, "la primera cancion"),
          (PMT_TY + PMT_H - 2, "la ultima cancion"),
          (PMT_TY + PMT_H - 1, "el borde de abajo de la ventana")))
    # ...y las de la de opciones, que se pinta entera cada frame igual.
    tap("B"); tap("DOWN"); tap("A"); run(core, 30)
    rows = pixels(screen)
    look(((PMO_TY, "el borde de arriba de la ventana OPTIONS"),
          (PMO_HEAD, "el titulo OPTIONS"),
          (PMO_FIRST, "HARD DROP"),
          (PMO_TY + PMO_H - 2, "SWAP A-B"),
          (PMO_TY + PMO_H - 1, "el borde de abajo de la ventana")))
    del core, screen
    return failures


def pause_options(rom_path):
    """LA VENTANA OPTIONS: SUS CINCO INTERRUPTORES, Y QUE SE QUEDAN.

    OPTIONS, entre MUSIC y EXIT GAME, abre una ventana de dieciseis de ancho
    con un interruptor por linea -- HARD DROP, FAST DOWN, FAST SHIFT, GHOST,
    SWAP A-B --, el nombre a la izquierda y su casilla a la derecha (vacia OFF,
    con punto ON). Todos en OFF la primera vez. A, o IZQUIERDA y DERECHA,
    cambian el de la flecha, y mover la flecha suena como en los menus; B
    vuelve al menu sin dejar nada de la ventana. Lo que cambian se nota en la
    partida al momento: la sombra de la pieza donde caera -- el contorno de la
    pieza misma, pixel a pixel, y en cooperativo tambien la del companero --,
    A y B girando al reves (y el menu leyendo A como A), ABAJO cayendo una
    fila por frame. Y se guardan en la bateria (SAVE_OPTIONS_OFF, $0E000404 y
    su byte de control): tras un reset, con el acorde otra vez, estan como se
    dejaron; sin el acorde, no hay ninguno.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return []
    lay = check_layout(rom_path)
    ghost0, check_off = lay["ghost"], lay["check_off"]
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def row(r):
        return tilemap_text(core, r, *pmenu_span(core))

    def entry(r, c):
        return core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2]

    def switches():
        """Each line of the window as (name, ON?, arrow?), or None: the name
        after the arrow's two columns, the box's two tiles at the right."""
        got = []
        for i, name in enumerate(PMO_NAMES):
            r = PMO_FIRST + i
            t = tilemap_text(core, r, PMO_NAME, PMO_NAME + len(name)).strip()
            box = [entry(r, PMO_BOX + k) & 0x3FF for k in (0, 1)]
            if t != name or box not in ([check_off, check_off + 1],
                                        [check_off + 2, check_off + 3]):
                return None
            arrow = ">" in tilemap_text(core, r, PMO_TX + 1, PMO_TX + 3)
            got.append((name, box[0] == check_off + 2, arrow))
        return got

    def ghosts(which=0):
        lo = ghost0 + 4 * which
        return sum(1 for r in range(20) for c in range(32)
                   if lo <= (entry(r, c) & 0x3FF) < lo + 4)

    def into_game(chord=True):
        run(core, 8)
        press_start(core); run(core, 10)
        if chord:
            tap("L", "R")
        press_start(core); run(core, 12)
        press_start(core); run(core, 40)

    def open_options():
        tap("START")
        while ">" not in row(PM_OPTIONS):
            tap("DOWN")
        tap("A")

    into_game()
    open_options()
    sw = switches()
    top = [c for c in range(32)
           if (entry(PMO_TY, c) & 0x3FF) in (0x29, 0x2A, 0x2B)]
    if "OPTIONS" not in tilemap_text(core, PMO_HEAD, PM_L, PM_R) or sw is None:
        failures.append(f"A en OPTIONS no abre la ventana con sus cinco "
                        f"interruptores: "
                        f"{[tilemap_text(core, PMO_FIRST + i, PM_L, PM_R) for i in range(5)]}")
        del core, screen
        return failures
    if (top[0], top[-1]) != (PMO_TX, PMO_TX + PMO_W - 1):
        failures.append(f"la ventana OPTIONS no tiene {PMO_W} de ancho: columnas "
                        f"{top[0]}-{top[-1]}")
    if any(on for _, on, _ in sw) or [a for _, _, a in sw] != [True] + [False] * 4:
        failures.append(f"la ventana no abre con todo en OFF y la flecha en "
                        f"HARD DROP: {sw}")
    else:
        print("  OPTIONS abre su ventana: cinco interruptores, todos en OFF")

    # MOVING THE ARROW TICKS, as the settings screen's does (cursor_blip):
    # some channel sounds in the frames after a DOWN, and UP puts it back.
    def ticks(key):
        core.set_keys(KEYS[key])
        n = 0
        for f in range(20):
            if f == 4:
                core.set_keys()
            core.run_frame()
            st = sound_state(core)
            n += bool(st["pulso 1"] or st["pulso 2"] or st["ruido"])
        return n
    down, up = ticks("DOWN"), ticks("UP")
    if not down or not up:
        failures.append(f"mover la flecha en OPTIONS no suena ({down} y {up} "
                        f"frames con sonido)")
    else:
        print(f"  mover la flecha suena ({down} frames), como en los menus")

    # A on HARD DROP, then DOWN to GHOST and RIGHT, LEFT, RIGHT: ON, OFF, ON.
    tap("A")
    for _ in range(3):
        tap("DOWN")
    tap("RIGHT"); tap("LEFT")
    mid = switches()
    tap("RIGHT")
    sw = switches()
    want = [True, False, False, True, False]
    saved = (core.memory.u8[0x0E000404], core.memory.u8[0x0E000405])
    if sw is None or [on for _, on, _ in sw] != want or mid[3][1]:
        failures.append(f"A, IZQUIERDA y DERECHA no cambian el interruptor de la "
                        f"flecha: {sw}")
    elif saved[0] != 0x09:
        failures.append(f"las opciones no se guardan en la bateria: "
                        f"$0E000404 = {saved[0]:#04x}")
    else:
        print("  A, IZQUIERDA y DERECHA cambian el de la flecha, y se guarda en "
              "la bateria al momento")

    # SWAP A-B: ON, and B still goes back -- the menu reads its own A and B.
    tap("DOWN"); tap("A")
    tap("B")
    if "OPTIONS" in tilemap_text(core, PMO_HEAD, PM_L, PM_R) or \
            ">" not in row(PM_OPTIONS):
        failures.append("con SWAP A-B en ON, B ya no cierra la ventana: el menu "
                        "no debe girar los botones")
    else:
        # ...and nothing of the window left round the menu's smaller box.
        frame = (0x29, 0x2A, 0x2B, 0x3A, 0x3B, 0x3C)
        stray = [r for r in (PMO_TY, PMO_TY + PMO_H - 1)
                 if any((core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2]
                         & 0x3FF) in frame for c in range(32))]
        if stray:
            failures.append(f"al volver al menu quedan bordes de la ventana en "
                            f"las filas {stray}")
        else:
            print("  B vuelve al menu (con SWAP A-B en ON) y la ventana se va "
                  "entera")

    # In the game: the ghost on the board, and A turning the other way.
    tap("START"); run(core, 10)
    if ghosts() != 4:
        failures.append(f"con GHOST en ON no se ve la sombra de la pieza "
                        f"({ghosts()} celdas)")
    else:
        print("  GHOST: la sombra de la pieza, cuatro celdas, donde caera")
        failures += ghost_fits(core, entry, ghost0)

    def turn():
        """One A on a T held high: the orientation it ends up in."""
        core.memory.u8[base + off["current"]] = 2          # TT_T
        core.memory.u8[base + off["orientation"]] = 0
        core.memory.u8[base + off["y"]] = 8
        tap("A", settle=2)
        return core.memory.u8[base + off["orientation"]]
    swapped = turn()
    open_options()
    for _ in range(4):
        tap("DOWN")
    tap("A"); tap("START"); run(core, 10)                   # SWAP A-B off
    plain = turn()
    if swapped == plain or (swapped + plain) % 4 != 0:
        failures.append(f"SWAP A-B no gira al reves: A deja la T en {swapped} "
                        f"con el y en {plain} sin el")
    else:
        print(f"  SWAP A-B: A gira la T a {swapped}, sin el a {plain}")

    # FAST DOWN: DOWN held falls a row a frame.
    def fall(frames=8):
        core.memory.u8[base + off["current"]] = 3          # TT_O, high up
        core.memory.u8[base + off["y"]] = 7
        y0 = core.memory.u8[base + off["y"]]
        core.set_keys(KEYS["DOWN"]); run(core, frames); core.set_keys(); run(core, 1)
        return core.memory.u8[base + off["y"]] - y0
    slow = fall()
    open_options()
    tap("DOWN"); tap("A"); tap("START"); run(core, 30)      # FAST DOWN on
    fast = fall()
    if fast < 7 or slow > 3:
        failures.append(f"FAST DOWN no acelera ABAJO: {slow} filas en 8 frames "
                        f"sin el, {fast} con el")
    else:
        print(f"  FAST DOWN: ABAJO {slow} filas en 8 frames sin el, {fast} "
              f"con el")

    # THEY STAY. A reset, the chord again, and the window as it was left:
    # HARD DROP, FAST DOWN and GHOST on.
    core.reset()
    into_game()
    if ghosts() != 4:
        failures.append("tras un reset la sombra no vuelve con el acorde")
    open_options()
    sw = switches()
    if sw is None or [on for _, on, _ in sw] != [True, True, False, True, False]:
        failures.append(f"tras un reset las opciones no son las que se dejaron: "
                        f"{sw}")
    else:
        print("  tras un reset y con el acorde, las opciones son las que se "
              "dejaron")
    # IN COOP, THE PARTNER'S TOO: the computer's piece in WITH COMPUTER,
    # with a ghost of its own in its own bank.
    core2, screen2 = load(rom_path)
    run(core2, 8)
    press_start(core2); run(core2, 10)
    for keys in (["L", "R"], ["DOWN"], ["DOWN"], ["DOWN"], ["DOWN"]):
        core2.set_keys(*[KEYS[k] for k in keys]); run(core2, 4)
        core2.set_keys(); run(core2, 12)
    press_start(core2); run(core2, 12)
    press_start(core2); run(core2, 40)
    for k in ("START", "DOWN", "A", "DOWN", "DOWN", "DOWN", "A", "START"):
        core2.set_keys(KEYS[k]); run(core2, 4); core2.set_keys(); run(core2, 12)

    def coop_ghosts():
        got = [0, 0]
        for r in range(20):
            for c in range(32):
                e = core2.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2]
                t, bank = e & 0x3FF, e >> 12
                if ghost0 <= t < ghost0 + 8:
                    got[(t - ghost0) // 4] += 1 if bank == (12, 14)[(t - ghost0) // 4] else 100
        return got
    seen = [0, 0]
    for _ in range(60):
        run(core2, 1)
        seen = coop_ghosts()
        if seen == [4, 4]:
            break
    if seen != [4, 4]:
        failures.append(f"en cooperativo no salen las dos sombras, cada una en "
                        f"su banco: {seen}")
    else:
        print("  en cooperativo, la sombra del companero tambien, en sus colores")
    del core2, screen2

    # ...and without the chord there are none: no ghost on the board. Off
    # the plaque first, or the paused game on the battery comes back with
    # its chord (suspend.c).
    tap("START"); run(core, 5)
    core.reset()
    into_game(chord=False)
    if ghosts():
        failures.append("sin el acorde se ve la sombra: las opciones son del "
                        "acorde")
    else:
        print("  sin el acorde, ninguna: el juego del cartucho")
    del core, screen
    return failures


def pause_ticks_hold_the_tune(rom_path):
    """EL TIC DEL CURSOR EN PAUSA NO DEJA SALIR LA CANCION.

    La pausa amordaza el motor (MUSIC_SUSPEND pone el umbral de clase en $F0)
    y el tic del cursor esta por debajo; para que suene se baja el umbral a
    $40 lo que dura (nes_audio_gag_lower), y las canciones son de clase 7 u 8,
    por debajo de $20. Con LOGINSKA sonando, en pausa y moviendo la flecha
    por el menu y por OPTIONS: suena el tic (pulso 2) y ni el triangulo ni el
    ruido, que en LOGINSKA son la cancion."""
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, 4)
        core.set_keys(); run(core, settle)

    def chans(frames):
        seen = {}
        for _ in range(frames):
            core.run_frame()
            st = sound_state(core)
            for k in ("pulso 1", "pulso 2", "triangulo", "ruido"):
                if st[k]:
                    seen[k] = seen.get(k, 0) + 1
        return seen

    run(core, 8)
    press_start(core); run(core, 10)
    tap("L", "R")
    press_start(core); run(core, 12)
    tap("DOWN"); tap("DOWN"); tap("RIGHT")          # MUSIC: LOGINSKA
    press_start(core); run(core, 60)
    playing = chans(30)
    tap("START", settle=20)
    heard = {}
    for k in ("DOWN", "UP", "DOWN", "A", "DOWN", "DOWN", "UP"):
        core.set_keys(KEYS[k])
        got = chans(4)
        core.set_keys()
        for d in (got, chans(30)):
            for c, n in d.items():
                heard[c] = heard.get(c, 0) + n
    if not playing.get("triangulo"):
        failures.append(f"LOGINSKA no sonaba antes de la pausa: {playing}")
    elif heard.get("triangulo") or heard.get("ruido"):
        failures.append(f"los tics del cursor en pausa dejan salir la cancion: "
                        f"{heard}")
    elif heard.get("pulso 2", 0) < 6 * 6:
        failures.append(f"los tics del cursor en pausa no suenan: {heard}")
    else:
        print(f"  en pausa el cursor hace tic ({heard.get('pulso 2')} frames de "
              f"pulso 2) y LOGINSKA sigue callada")
    del core, screen
    return failures


def ghost_fits(core, entry, ghost0):
    """THE GHOST IS THE PIECE'S OWN OUTLINE, pixel for pixel: the falling
    piece's four tiles (bank 12, the cartridge's block art) make one
    silhouette; outside it is what its edge reaches without crossing ink;
    and the ghost's ink is exactly the silhouette's pixels next to that
    outside, cell for cell. It used to be eight pixels square a cell, a
    pixel proud of the piece above and to the left."""
    def art(t):
        rows = []
        for y in range(8):
            w = int.from_bytes(bytes(core.memory.u8[0x06000000 + t * 32 + y * 4 + i]
                                     for i in range(4)), "little")
            rows.append([(w >> (4 * x)) & 15 for x in range(8)])
        return rows
    for _ in range(90):
        piece, ghost = [], []
        for r in range(20):
            for c in range(32):
                e = entry(r, c)
                t, bank = e & 0x3FF, e >> 12
                if bank == PAL_PIECE_BANK and 1 <= t <= 15:
                    piece.append((r, c, t))
                elif ghost0 <= t < ghost0 + 4:
                    ghost.append((r, c, t))
        if len(piece) == 4 and len(ghost) == 4:
            break
        core.run_frame()
    else:
        return [f"no se ven a la vez la pieza y su sombra enteras "
                f"({len(piece)} y {len(ghost)} celdas)"]

    def layout(cells):
        r0, c0 = min(r for r, _, _ in cells), min(c for _, c, _ in cells)
        return sorted((r - r0, c - c0, t) for r, c, t in cells)
    p, g = layout(piece), layout(ghost)
    if [(r, c) for r, c, _ in p] != [(r, c) for r, c, _ in g]:
        return [f"la sombra no tiene la forma de la pieza: {p} / {g}"]
    ink, drawn = set(), set()
    for (r, c, t), (_, _, gt) in zip(p, g):
        a, b = art(t), art(gt)
        for y in range(8):
            for x in range(8):
                if a[y][x]:
                    ink.add((r * 8 + y, c * 8 + x))
                if b[y][x]:
                    drawn.add((r * 8 + y, c * 8 + x))
    far, todo = set(), [(-1, -1)]
    while todo:
        y, x = todo.pop()
        if (y, x) in far or (y, x) in ink or not (-1 <= y <= 32 and -1 <= x <= 32):
            continue
        far.add((y, x))
        todo += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    edge = {(y, x) for y, x in ink
            if {(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)} & far}
    if drawn != edge:
        return [f"la sombra no es el contorno de la pieza: {len(drawn - edge)} "
                f"pixeles de mas, {len(edge - drawn)} de menos"]
    print(f"  la sombra es el contorno exacto de la pieza ({len(edge)} pixeles)")
    return []


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
