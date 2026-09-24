"""
Everything before a match: the title, its skins and sprites,
GAME SELECT, the credits, the demo, the cable lobby alone.
"""
from .harness import (
    CATHEDRAL_SPRITES, CELL_BLOCK, CREDITS, CREDIT_TY,
    DEMO_START_FRAME, DEMO_WATCH_FRAMES, GAME_SELECT_LONGEST, GAME_SELECT_ROWS,
    GAME_SELECT_TX, GAME_SELECT_TY, HANDICAP_ROW, KEYS,
    LEVEL_ROW, LIMIT_SKINS, LINK_MSG_ROW, LINK_TIMEOUT_FRAMES,
    MENU_ARROW_TX, MENU_TEXT_BANK, MUSIC_ROW, OAM_ADDR,
    ONSCREEN_MIN_INK, PALETTE_ADDR, PM_EXIT, PM_L, pmenu_span,
    PM_R, SCREENBLOCK_ADDR, SCREEN_H, SCREEN_TW_TILES,
    SCREEN_W, TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH, TILE,
    TITLE_PAL_BASE, TITLE_SHOW_FRAMES, game_offsets, game_state_address,
    load, oam_visible, pixels, press_start,
    run, sound_state, tilemap_text, to_music_page,
)


def title_check(rom_path):
    core, screen = load(rom_path)           # `screen` must stay alive; see load()
    failures = []

    run(core, 30)
    # NOT ALL EIGHTEEN HAVE TO SHOW. The overlay is placed in NES screen
    # pixels over NES rows the composition may not carry: it drops the top of
    # the cathedral's thin spire (see TITLE_ROW_BLOCKS), and a sprite whose row
    # went with it has nothing left to overlay, so kTitleRowMap hides it rather
    # than dropping it somewhere it does not belong. Two is the most the
    # current composition can account for; more than that means the map is
    # wrong, not that the artwork was recut.
    cathedral = oam_visible(core, 0, CATHEDRAL_SPRITES)
    if len(cathedral) < CATHEDRAL_SPRITES - 2:
        failures.append(
            f"la catedral pone {len(cathedral)} de {CATHEDRAL_SPRITES} sprites")
    off = [c for c in cathedral if not (0 <= c[0] < SCREEN_W and 0 <= c[1] < SCREEN_H)]
    if off:
        failures.append(f"sprites de la catedral fuera de pantalla: {off[:3]}")
    if cathedral:
        print(f"  catedral: {len(cathedral)} sprites, de ({cathedral[0][0]},"
              f"{cathedral[0][1]}) a ({cathedral[-1][0]},{cathedral[-1][1]})")

    # The fireworks live in slots 19 and up. Watch a while: bursts come every
    # 8-71 frames and each lasts a few, so a couple of hundred frames sees
    # several, and the tile ids have to CHANGE — a burst that froze on one
    # frame of its animation would still be a lot of sprites.
    seen_tiles, peak, bursts, was_up = set(), 0, 0, False
    # ...AND BEHIND THE PICTURE. Every burst sprite the cartridge writes has
    # attribute bit 5, "behind the background": the braid, the cathedral and
    # the logo cover the parts of a burst that cross them, which is how the
    # frame contains it. So OBJ priority 2 on the fireworks (both backgrounds
    # on the title are 0 or 1), and the cathedral's own sprites in front.
    def prio(i):
        return (core.memory.u16[OAM_ADDR + i * 8 + 4] >> 10) & 3

    burst_prios, cathedral_prios = set(), set()
    for _ in range(300):
        core.run_frame()
        vis = oam_visible(core, 19, 64)
        peak = max(peak, len(vis))
        for v in vis:
            seen_tiles.add(v[2])
        for i in range(19, 64):
            if not core.memory.u16[OAM_ADDR + i * 8] & 0x0200:
                burst_prios.add(prio(i))
        for i in range(0, 19):
            if not core.memory.u16[OAM_ADDR + i * 8] & 0x0200:
                cathedral_prios.add(prio(i))
        up = len(vis) > 0
        if up and not was_up:
            bursts += 1
        was_up = up
    if peak < 20:
        failures.append(f"los fuegos artificiales nunca pasan de {peak} sprites")
    if bursts < 2:
        failures.append(f"solo {bursts} explosion(es) en 300 frames")
    if len(seen_tiles) < 20:
        failures.append(f"los fuegos no se animan: solo {len(seen_tiles)} tiles distintos")
    print(f"  fuegos: {bursts} explosiones en 300 frames, hasta {peak} sprites, "
          f"{len(seen_tiles)} tiles distintos")
    if burst_prios != {2}:
        failures.append(f"los fuegos no van detras del dibujo: prioridades "
                        f"{sorted(burst_prios)}, deberian ser [2]")
    elif cathedral_prios - {0}:
        failures.append(f"la catedral se ha ido detras del fondo: prioridades "
                        f"{sorted(cathedral_prios)}")
    else:
        print("  y detras del dibujo, como el bit 5 del cartucho: la greca, la "
              "catedral y el logo los tapan")

    # The show has to STOP, the way the ROM stops it.
    run(core, TITLE_SHOW_FRAMES)
    still = max(len(oam_visible(core, 19, 64)) for _ in [core.run_frame() for _ in range(120)])
    if still:
        failures.append("los fuegos siguen despues de que la ROM los termina")
    else:
        print(f"  la funcion termina sola pasados {TITLE_SHOW_FRAMES} frames, como en el cartucho")

    # ...and start again on the next visit to the title.
    for name in ("START", "B"):
        core.set_keys(KEYS[name]); run(core, 4); core.set_keys(); run(core, 8)
    seen = 0
    for _ in range(300):
        core.run_frame()
        seen = max(seen, len(oam_visible(core, 19, 64)))
    if seen < 20:
        failures.append("al volver al titulo los fuegos no vuelven a empezar")
    else:
        print("  al volver al titulo la funcion vuelve a empezar")

    # And the budget. Running two more of the ROM's subroutines on top of the
    # sound engine, every frame, is the kind of thing that quietly costs a
    # vblank; the symptom would be the show advancing slower than the screen.
    # g_title_frame is the counter draw_title_sprites feeds the ROM, so if it
    # does not go up exactly once per emulated frame, a frame was missed.
    addr, why = game_state_address(rom_path, "g_title_frame")
    if addr is None:
        print(f"  (sin comprobar el presupuesto de CPU: {why})")
    else:
        before = core.memory.u16[addr]
        run(core, 300)
        advanced = (core.memory.u16[addr] - before) & 0xFFFF
        if advanced != 300:
            failures.append(
                f"la pantalla de titulo avanzo {advanced} frames de 300: se pierden vblanks")
        else:
            print("  presupuesto de CPU: 300 frames de pantalla, 300 de la funcion")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la catedral y los fuegos son el codigo del cartucho, corriendo.")
    return 0


def skin_check(rom_path):
    core, screen = load(rom_path)           # `screen` must stay alive; see load()
    failures = []

    # WHAT TELLS THE TWO SCREENS APART: the TILEMAP, not the picture. The
    # release title has fireworks on it, so two frames of it are rarely
    # identical and comparing pixels would call an unchanged screen "changed"
    # every time; and both frames now reach the screen's edges, so counting lit
    # edge pixels cannot tell them apart either. Their frames are drawn from
    # different tiles, and tiles do not animate.
    #
    # THE WHOLE MAP, THOUGH, AND NOT THREE COLUMNS OF FRAME. Reading the
    # border alone was enough while every dump had a border of its own; a
    # fourth arrived whose screen differs from a third's in NOTHING BUT THE
    # CATHEDRAL — same fret, same heading, same copyright lines, 127 cells of
    # building — and three columns of identical rule read as "two skins
    # drawing the same tiles". The picture is what is different, so the
    # picture is what gets read.
    def frame_tiles():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2]
                for r in range(SCREEN_H // TILE) for c in range(SCREEN_TW_TILES)]

    def tap(*keys, settle=12):
        core.set_keys(*keys)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 40)
    release = frame_tiles()

    # L+R LA ENCUENTRA; L Y R A SOLAS LA MANEJAN DESPUES. Antes de que suene
    # el acorde, un gatillo suelto no hace absolutamente nada: la skin es algo
    # que se descubre, y el titulo no se cambia por apoyar un dedo.
    tap(KEYS["L"])
    if frame_tiles() != release:
        failures.append("L a solas cambia la skin sin haber sonado el acorde")
    tap(KEYS["R"])
    if frame_tiles() != release:
        failures.append("R a solas cambia la skin sin haber sonado el acorde")

    tap(KEYS["L"], KEYS["R"])
    proto = frame_tiles()
    if proto == release:
        print("  esta ROM se construyo sin prototipo: L+R no tiene skin que poner")
        for f in failures:
            print(f"FALLA: {f}")
        if failures:
            return 1
        print("OK: sin skin de prototipo, y el titulo no se rompe por pulsar L o R.")
        return 0
    print("  L+R pone el marco del prototipo, que es de otros tiles; "
           "antes del acorde un hombro suelto no hace nada")

    # The release's fireworks and cathedral overlay belong to the release
    # picture; on the prototype's they must be gone.
    if oam_visible(core, 0, 64):
        failures.append("los sprites del release siguen encima de la skin")
    else:
        print("  los sprites del release (catedral y fuegos) se retiran con ella")

    # HOWEVER MANY SKINS the build was given, R must walk all of them and come
    # back — and each must be its OWN screen, not the same tiles twice, which
    # is what a swap that forgot to re-upload the prototype's pattern table
    # would look like.
    seen = [release, proto]
    for _ in range(LIMIT_SKINS):
        tap(KEYS["R"])
        now = frame_tiles()
        if now == release:
            break
        if now in seen:
            failures.append("dos skins del ciclo dibujan los mismos tiles")
            break
        seen.append(now)
    else:
        failures.append(f"el ciclo de skins no vuelve al release en "
                         f"{LIMIT_SKINS} pulsaciones de R")
    if not failures:
        print(f"  tras el acorde, R recorre {len(seen) - 1} skin(s) distinta(s) "
               "y vuelve al titulo del release")

    # ...Y L VUELVE, que es de lo que iba el cambio: con cuatro skins, volver a
    # la que acabas de pasar no puede costar dar la vuelta a las otras tres.
    order = seen + [release]
    tap(KEYS["R"])
    if frame_tiles() != order[1]:
        failures.append("R no avanza a la primera skin desde el release")
    tap(KEYS["L"])
    if frame_tiles() != release:
        failures.append("L no vuelve a la skin anterior")
    else:
        tap(KEYS["L"])
        if frame_tiles() != order[-2]:
            failures.append("L desde el release no da la vuelta a la ultima skin")
        else:
            print("  L retrocede y R avanza, y la lista da la vuelta por los dos "
                   "lados")

    # ...and the choice must survive leaving the title and coming back.
    tap(KEYS["R"])
    proto = frame_tiles()
    for name in ("START", "B"):
        core.set_keys(KEYS[name]); run(core, 4); core.set_keys(); run(core, 10)
    if frame_tiles() != proto:
        failures.append("la skin se pierde al salir del titulo y volver")
    else:
        print("  la skin se mantiene al salir del titulo y volver")

    # AND IT OPENS NOTHING ELSE. The skin is its own chord on its own screen:
    # a player who finds the prototype title has not thereby found the hidden
    # tunes, which are still four until the chord is rung on a menu.
    core.set_keys(KEYS["START"]); run(core, 4); core.set_keys(); run(core, 12)
    core.set_keys(KEYS["START"]); run(core, 4); core.set_keys(); run(core, 16)
    for _ in range(2):      # the cursor: LEVEL -> HANDICAP -> MUSIC
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 8)
    if "NO MUSIC" not in tilemap_text(core, MUSIC_ROW):
        failures.append(f"la fila de MUSIC no dice NO MUSIC: "
                         f"{tilemap_text(core, MUSIC_ROW)!r}")
    else:
        seen = set()
        for _ in range(12):
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 8)
            seen.add(tilemap_text(core, MUSIC_ROW))
        if len(seen) != 5:
            failures.append(f"el acorde del titulo destapo las canciones: "
                             f"el menu ofrece {len(seen)}, no 5")
        else:
            print("  ...y no destapa nada mas: el menu sigue ofreciendo las "
                   "cinco entradas del cartucho")

    # ...AND IT REACHES THE BOARD, which is the half of the feature that was
    # missing for a long time: the skin was the title art and nothing else,
    # and the player who had gone looking for it got the release's blue braid
    # and shaded blocks the moment the game started.
    #
    # Measured off the tilemap on both counts. The FRAME: the panel's wall
    # beside the board and the elbow over it are the release's own $6A $6B /
    # $95 $96 slots in either skin — the art in them changes, not the number —
    # so this compares the PIXELS of those columns instead. The BLOCKS: a
    # prototype writes all four cells of a settled piece with ONE tile (its
    # piece id) where the release writes up to four different joined-block
    # graphics, so a stacked board in a skin has fewer distinct tile ids in it.
    def stack_a_board(chords):
        """A fresh console taken into a game with `chords` skin swaps first,
        with enough soft drop behind it to leave a stack to look at.

        Returns the pixels of the board's left wall and the set of tile ids
        the settled blocks are drawn with.
        """
        this, this_screen = load(rom_path)
        _ = this_screen                      # must stay alive; see load()
        run(this, 40)
        for _i in range(chords):
            this.set_keys(KEYS["L"], KEYS["R"]); run(this, 4)
            this.set_keys(); run(this, 20)
        press_start(this); run(this, 20)      # title -> GAME SELECT
        press_start(this); run(this, 20)      # -> LEVEL SETTINGS
        press_start(this); run(this, 60)      # -> playing
        this.set_keys(KEYS["DOWN"]); run(this, 700)
        this.set_keys(); run(this, 40)
        px = pixels(this_screen)
        wall = [tuple(px[y][x] for x in range(64, 80)) for y in range(16, 144)]
        blocks = {this.memory.u16[SCREENBLOCK_ADDR + (r * 32 + c) * 2] & 0x3FF
                  for r in range(10, 20) for c in range(10, 20)} - {0}
        return wall, blocks

    # ...AND THE FRONT END WEARS IT TOO. The menu frame and the HIGH SCORES
    # frame are the same twenty-four charblock slots the board's is, so a skin
    # that stopped at the board left them two thirds in the release's blue
    # braid and one third in the prototype's fret — half-changed, which is
    # worse than either. Counted by colour rather than by tile, because the
    # slots are the same numbers in both and only the art in them differs.
    def menu_colours(chords):
        this, this_screen = load(rom_path)
        _ = this_screen
        run(this, 40)
        for _i in range(chords):
            this.set_keys(KEYS["L"], KEYS["R"]); run(this, 4)
            this.set_keys(); run(this, 20)
        press_start(this); run(this, 24)          # -> GAME SELECT
        px = pixels(this_screen)
        # The frame's own columns, top to bottom, down the left edge.
        return [tuple(px[y][x] for x in range(0, 16)) for y in range(0, 160)]

    plain_menu = menu_colours(0)
    skin_menu = menu_colours(1)
    same = sum(1 for a, b in zip(plain_menu, skin_menu) if a == b)
    if same > len(plain_menu) // 4:
        failures.append(f"la greca del MENU no cambia con la skin: {same} de "
                         f"{len(plain_menu)} filas identicas")
    else:
        print("  ...y la greca de los menus cambia con ella")

    plain_frame, plain_blocks = stack_a_board(0)
    skin_frame, skin_blocks = stack_a_board(1)

    same = sum(1 for a, b in zip(plain_frame, skin_frame) if a == b)
    if same > len(plain_frame) // 4:
        failures.append(f"la greca del tablero no cambia con la skin: "
                         f"{same} de {len(plain_frame)} filas identicas")
    else:
        print("  ...y la greca del tablero es la del prototipo, no la trenza")
    if not plain_blocks or not skin_blocks:
        failures.append("no se asento nada: esta comprobacion no prueba nada")
    elif skin_blocks - set(range(1, 8)):
        failures.append(f"la skin asienta tiles fuera de $01-$07 "
                         f"({sorted(skin_blocks)}): sus bloques solo llegan a $07")
    elif not (plain_blocks - set(range(1, 8))):
        failures.append("el release no asento ningun tile por encima de $07: "
                         "los dos modos estarian usando la misma codificacion")
    else:
        print(f"  ...y sus piezas son una sola por tetromino "
              f"({sorted(skin_blocks)}) donde el release usa "
              f"{len(plain_blocks)} graficos unidos")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: L+R recorre el titulo del release y los de los prototipos, "
          "y la skin llega al tablero.")
    return 0


def demo_check(rom_path):
    """The attract demo: the title starts playing by itself.

    demoStart is reached from the title's own clock at frameCounterHigh 5,
    frameCounterLow $20 (main.asm.txt:4154-4160) — 1312 frames — and sets
    playMode 0, suspends the music and drops into the ordinary game init. So
    what this checks is the three things that make it a demo: it starts on
    its own, the board fills without the pad being touched, and a press on
    the pad is the way OUT rather than a move.
    """
    flag, why = game_state_address(rom_path, "g_demo")
    if flag is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    started = None
    for f in range(DEMO_WATCH_FRAMES):
        core.run_frame()
        if core.memory.u8[flag]:
            started = f
            break
    if started is None:
        failures.append(f"la demo no arranca sola en {DEMO_WATCH_FRAMES} frames")
        print("FALLA:", failures[-1])
        return 1
    if not (1250 <= started <= 1400):
        failures.append(f"la demo arranca en el frame {started}, no cerca de "
                         f"{DEMO_START_FRAME} (frameCounterHigh 5, low $20)")
    else:
        print(f"  arranca sola en el frame {started}, con el titulo sin tocar")

    # Nothing touches the pad from here: every cell that appears was the
    # computer's.
    run(core, 6000)
    cells = sum(1 for y in range(TENGEN_PF_HEIGHT)
                for x in range(1, TENGEN_PF_WIDTH - 1)
                if core.memory.u8[base + off["field"] + y * TENGEN_PF_WIDTH + x])
    if cells < 4:
        failures.append(f"la demo solo asento {cells} celdas: no esta jugando")
    else:
        print(f"  juega sola: {cells} celdas asentadas sin tocar el mando")

    # ...and START is the way out, to GAME SELECT, not a pause.
    core.set_keys(KEYS["START"])
    run(core, 4)
    core.set_keys()
    run(core, 20)
    if core.memory.u8[flag]:
        failures.append("START no saca de la demo")
    elif "GAME SELECT" not in tilemap_text(core, 8):
        failures.append(f"START saca de la demo a {tilemap_text(core, 8)!r}, "
                         "no a GAME SELECT")
    else:
        print("  START sale de la demo a GAME SELECT, como en el cartucho")
    del core, screen

    # ...Y TAMBIEN DESDE UN TITULO CON SKIN, que es donde no llegaba. El reloj
    # del titulo vivia dentro de la rutina de sprites, de la que una skin sale
    # antes de tiempo, asi que sobre una pantalla de prototipo el contador no
    # se movia y la demo no llegaba nunca: el titulo del release se ponia a
    # jugar solo a los veintidos segundos y el del prototipo se quedaba ahi
    # para siempre.
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    run(core, 40)
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4); core.set_keys(); run(core, 20)
    started = None
    for f in range(DEMO_WATCH_FRAMES):
        core.run_frame()
        if core.memory.u8[flag]:
            started = f
            break
    if started is None:
        failures.append(f"con skin la demo no arranca en {DEMO_WATCH_FRAMES} "
                         f"frames: el reloj del titulo no corre bajo ella")
    else:
        print(f"  y con la skin puesta arranca igual, en el frame {started}")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el titulo se pone a jugar solo y se sale con un boton.")
    return 0


def loans_check(rom_path):
    """LOS BANCOS QUE UNA SKIN TIENE PRESTADOS TIENEN QUE DEVOLVERSE.

    Tres paletas de una skin son PRESTAMOS de las cuatro del titulo -- la del
    logo de los menus, la de las tiradas del histograma y la del cartel de
    GAME OVER -- porque nunca hay un titulo y un tablero en pantalla a la vez.
    Eso solo funciona si cada lado las recupera al entrar, y durante un tiempo
    ninguno de los dos lo hacia:

      * el titulo instalaba sus cuatro bancos solo al CAMBIAR de skin, asi que
        volver de los menus lo dibujaba con lo que los menus habian dejado --
        el marco gris de la pantalla con licencia de Nintendo volvia con el
        borde de arriba y el de la derecha azules;
      * y apply_skin se saltaba el trabajo entero cuando la skin ya estaba
        cargada, asi que una vez arreglado lo anterior, la segunda visita a
        los menus dibujaba el logo con los colores del titulo.

    Asi que esto no mira una pantalla: da la vuelta entera -- titulo, menus,
    titulo, menus, partida, titulo -- y compara los dieciseis colores de los
    cuatro bancos del titulo con los que tenia la primera vez.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen

    def tap(*names, hold=4, settle=16):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def title_banks():
        return [core.memory.u16[PALETTE_ADDR + ((TITLE_PAL_BASE + b) * 16 + i) * 2]
                for b in range(4) for i in range(4)]

    run(core, 40)
    tap("L", "R"); run(core, 24)          # una skin puesta
    first = title_banks()
    if not any(first):
        print("SALTADO: esta ROM no trae skins")
        return 0

    tap("START"); run(core, 30)            # -> GAME SELECT
    menu_first = title_banks()
    tap("B"); run(core, 60)                # -> titulo
    if title_banks() != first:
        failures.append("el titulo no recupera sus bancos al volver de los menus")
    else:
        print("  el titulo recupera sus cuatro bancos al volver de los menus")

    tap("START"); run(core, 30)            # -> GAME SELECT otra vez
    if title_banks() != menu_first:
        failures.append("la segunda visita a los menus no repone los prestamos: "
                         "apply_skin se los salta por tener la skin ya cargada")
    else:
        print("  ...y los menus reponen los suyos en cada visita")

    # ...y lo mismo pasando por una partida, que pide prestados otros dos.
    # EL ACORDE, AQUI: el del titulo cambia la skin y no destapa nada mas, asi
    # que sin este el cartel de pausa es un cartel y no hay por donde salir.
    tap("L", "R"); run(core, 20)
    press_start(core); run(core, 24)
    press_start(core); run(core, 60)       # -> jugando
    run(core, 60)
    tap("START"); run(core, 20)            # pausa, que dibuja el cartel
    tap("L", "R"); run(core, 20)           # ...y el acorde lo vuelve menu
    # EXIT, y SI. START aqui seria reanudar, no salir.
    for _ in range(3):
        if ">" in tilemap_text(core, PM_EXIT, *pmenu_span(core)):
            break
        tap("DOWN")
    tap("A")                                # abre la pregunta
    tap("LEFT")                             # SI
    tap("A"); run(core, 120)
    if title_banks() != first:
        failures.append("el titulo no recupera sus bancos al volver de una partida")
    else:
        print("  ...y tambien al volver de una partida")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: los bancos prestados vuelven a su sitio en los dos sentidos.")
    return 0


def onscreen_check(rom_path):
    """LO QUE ESTA ESCRITO TIENE QUE LLEGAR A LA PANTALLA.

    Todas las comprobaciones de este fichero leen el MAPA DE TILES, y el mapa
    puede decir la verdad mientras la pantalla miente. Paso: al partir
    gba/main.c en cinco ficheros el compilador perdio el inline entre ellos,
    el dibujado de un frame crecio un cinco por ciento y empezo a pasarse del
    blanco vertical -- y pasarse no se ve como lentitud, se ve como que falta
    la PARTE DE ARRIBA de lo ultimo que se dibuja, porque el haz ya ha pasado
    por esas lineas cuando se escriben. Faltaban el titulo del menu de pausa y
    el borde de arriba de su caja, con el mapa entero y correcto.

    Asi que esto cruza las dos cosas en las pantallas que importan: para cada
    fila de tiles suma la TINTA que el mapa promete -- mirando el arte de cada
    tile en las cuatro capas -- y la compara con los pixeles encendidos de esa
    fila. Una fila que promete tinta y no enciende nada es una fila que no
    llego a tiempo.
    """
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=16):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    # El arte de cada tile, una vez: cuantos pixeles suyos no son el color 0.
    def tile_ink(cache, tile):
        if tile in cache:
            return cache[tile]
        n = 0
        for i in range(16):
            v = core.memory.u16[0x06000000 + tile * 32 + i * 2]
            for s in range(0, 16, 4):
                if (v >> s) & 0xF:
                    n += 1
        cache[tile] = n
        return n

    def audit(name):
        cache = {}
        rows = pixels(screen)
        # CELDA A CELDA, no fila a fila: una fila con las paredes de una caja
        # a los lados enciende pixeles aunque su contenido no llegue, y eso es
        # justo lo que hay que cazar.
        bad = []
        for ty in range(SCREEN_H // TILE):
            for tx in range(SCREEN_TW_TILES):
                promised = 0
                for sb in (28, 29, 30, 31):
                    base = 0x06000000 + sb * 0x800
                    t = core.memory.u16[base + ((ty * 32 + tx) * 2)] & 0x3FF
                    if t:
                        promised = max(promised, tile_ink(cache, t))
                if promised < ONSCREEN_MIN_INK:
                    continue
                # Las capas van tres pixeles a un lado y dos al otro, asi que
                # se mira la celda y su vecindad inmediata.
                lit = sum(1 for y in range(ty * TILE, (ty + 1) * TILE + 2)
                          for x in range(tx * TILE - 3, (tx + 1) * TILE + 3)
                          if 0 <= y < SCREEN_H and 0 <= x < SCREEN_W
                          and rows[y][x] != (0, 0, 0))
                if lit == 0:
                    bad.append((ty, tx))
        if bad:
            failures.append(f"{name}: {len(bad)} celdas con tiles escritos no "
                             f"encienden un solo pixel, la primera en "
                             f"{bad[0]}")
        else:
            print(f"  {name}: todo lo escrito esta en pantalla")

    run(core, 40)
    audit("el titulo")
    press_start(core); run(core, 30)
    tap("L", "R"); run(core, 20)
    audit("GAME SELECT")
    press_start(core); run(core, 30)
    audit("LEVEL SETTINGS")
    press_start(core); run(core, 90)
    core.set_keys(KEYS["DOWN"]); run(core, 200); core.set_keys(); run(core, 30)
    audit("una partida")
    tap("START"); run(core, 40)
    audit("el menu de pausa")
    tap("START"); run(core, 20)
    # ...y el game over, que dibuja su propia placa encima de todo.
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is not None:
        addr = base + off["field"]
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(1, TENGEN_PF_WIDTH - 1):
                core.memory.u8[addr + row * TENGEN_PF_WIDTH + col] = (
                    0 if col == 5 else CELL_BLOCK)
        run(core, 240)
        audit("el game over")
    del core, screen

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: lo que el mapa promete, la pantalla lo dibuja.")
    return 0


def credits_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []
    run(core, 8)
    press_start(core)               # title -> game select
    run(core, 20)

    # Long enough for every one of them to come round twice over.
    seen = []
    for _ in range(1600):
        pair = (tilemap_text(core, CREDIT_TY).strip(),
                tilemap_text(core, CREDIT_TY + 1).strip())
        if not seen or pair != seen[-1]:
            seen.append(pair)
        core.run_frame()

    def shown(role, name):
        return any(role in a and name in b for a, b in seen)

    missing = [f"{role} {name}" for role, name in CREDITS if not shown(role, name)]
    if missing:
        failures.append("no aparecen estos creditos del cartucho: "
                         + "; ".join(missing))
    else:
        print(f"  los seis creditos del cartucho salen, uno a uno "
              f"({len(seen)} cambios en 1600 frames)")

    # The invented line is gone: the cartridge spells him PAZHITNOV, on its
    # own level screen, and the port used to print a PAJITNOV of its own.
    if any("PAJITNOV" in a or "PAJITNOV" in b for a, b in seen):
        failures.append("sigue saliendo PAJITNOV, que no es como lo escribe el "
                         "cartucho")

    # ...and nothing of it touches the frame. The bottom braid starts at
    # y=145; the credit's second line rides the lifted layer to clear it.
    rows = pixels(screen)
    braid = None
    for y in range(120, 160):
        lit = sum(1 for x in range(24, 216) if tuple(rows[y][x]) != (0, 0, 0))
        if lit > 180:
            braid = y
            break
    if braid is None:
        failures.append("no encuentro el borde inferior del marco")
    else:
        ink = [y for y in range(120, braid)
               if any(tuple(rows[y][x]) != (0, 0, 0) for x in range(24, 216))]
        if ink and ink[-1] >= braid - 1:
            failures.append(f"el credito toca la greca: tinta hasta y={ink[-1]}, "
                             f"greca en y={braid}")
        else:
            print(f"  y la ultima linea deja aire sobre la greca "
                  f"(tinta hasta y={ink[-1] if ink else '-'}, greca en y={braid})")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: los creditos del cartucho estan, con sus palabras.")
    return 0


def menu_check(rom_path):
    """LEVEL SETTINGS: three fields, and nothing touching the braid.

    The frame's bottom run starts at y=145, so a line on tile row 17 ends one
    pixel short of it — which is what "START TO PLAY pisa la greca" was. The
    last row with air under it is 16, and this measures that rather than
    trusting a constant.
    """
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    run(core, 8)
    press_start(core)               # title -> game select
    press_start(core)               # -> LEVEL SETTINGS
    run(core, 20)

    failures = []
    for row, want in ((LEVEL_ROW, "LEVEL"), (HANDICAP_ROW, "HANDICAP"),
                       (MUSIC_ROW, "MUSIC"), (16, "PRESS START TO PLAY")):
        if want not in tilemap_text(core, row):
            failures.append(f"la fila {row} no dice {want}: "
                             f"{tilemap_text(core, row)!r}")

    # Where the braid's bottom run actually begins, found by looking for the
    # first scanline the frame fills right across the interior.
    rows = pixels(screen)
    braid_y = None
    for y in range(120, 160):
        if all(rows[y][x] != (0, 0, 0) for x in range(24, 216)):
            braid_y = y
            break
    if braid_y is None:
        failures.append("no se encuentra la greca de abajo")
    else:
        last = max((y for y in range(100, braid_y)
                    if any(rows[y][x] != (0, 0, 0) for x in range(24, 216))),
                   default=None)
        if last is None:
            failures.append("no hay texto en la mitad de abajo de la pantalla")
        elif braid_y - last < 4:
            failures.append(f"el texto llega a y={last} y la greca empieza en "
                             f"y={braid_y}: se tocan")
        else:
            print(f"  la ultima linea acaba en y={last}, la greca empieza en "
                   f"y={braid_y}: {braid_y - last - 1} pixeles de aire")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: LEVEL SETTINGS trae sus tres campos y no pisa la greca.")
    return 0


def leaving_title_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    run(core, 40)
    if not oam_visible(core, 0, 64):
        failures.append("la pantalla de titulo no dibuja ningun sprite")

    press_start(core)               # title -> game select
    run(core, 10)
    left = oam_visible(core, 0, 64)
    if left:
        failures.append(f"quedan {len(left)} sprites del titulo sobre GAME SELECT")
    else:
        print("  al salir del titulo no queda ni un sprite suyo por la pantalla")

    # The engine's request ring: $0200-$0207 with its indices at $0208/$0209
    # (setMusicOrSoundEffect, main.asm.txt:CFB1). Reading it says exactly what
    # the port asked the cartridge to play, which is better than guessing from
    # the sound registers.
    ram, why = game_state_address(rom_path, "g_nes_ram")
    if ram is None:
        print(f"  (sin comprobar la musica: {why})")
    else:
        # setMusicOrSoundEffect stores AT the incremented write index
        # ($0209), so that slot holds the last thing asked for and the one
        # before it the one before that. $0208 is the READ index and says how
        # far the engine has got, which is a different question.
        # The requests in the ring, newest last. $01 and $02 are SUSPEND and
        # RESUME — transport, not tracks — and $0E and up are sound effects
        # (constants.asm.txt:37-61); both are stepped over when the question
        # is "which tune was asked for".
        def recent_tracks(n):
            write = core.memory.u8[ram + 0x209]
            out = []
            for back in range(8):
                v = core.memory.u8[ram + 0x200 + (write - back) % 8]
                if 0x02 < v < 0x0E:
                    out.append(v)
                    if len(out) == n:
                        break
            return tuple(reversed(out))

        def last_two():
            pair = recent_tracks(2)
            return pair if len(pair) == 2 else (0, 0)

        # A screen that is already playing the right thing queues nothing,
        # which is correct and has to read as correct.
        def last_music():
            got = recent_tracks(1)
            return got[0] if got else 0

        press_start(core)           # game select -> LEVEL, the first setup page
        run(core, 12)
        # Arriving at the selection screen settles the music: silence, then
        # whatever the cursor shows.
        recent = last_two()
        if recent[0] != 0x08:
            failures.append(
                f"al entrar en la seleccion no se manda MUSIC_SILENCE (se mando ${recent[0]:02X})")
        elif recent[1] == 0x08:
            failures.append("se manda el silencio pero no la cancion detras")
        else:
            print(f"  al entrar en la seleccion: silencio y despues ${recent[1]:02X}")

        # ...and the tune is picked with LEFT/RIGHT once the cursor is on the
        # MUSIC row, two rows down.
        for _ in range(2):
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 10)
        core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 10)
        moved = last_two()
        if moved[0] != 0x08 or moved[1] == 0x08:
            failures.append("mover el cursor no toca la cancion nueva")
        else:
            print(f"  mover el cursor toca la cancion: silencio y despues ${moved[1]:02X}")

        # WHICH SLOTS ARE HELD, which is the question underneath all of the
        # others. The engine keeps eleven voice slots at $0292 and the top
        # bits of each are the song's PRIORITY CLASS: 7 for the four in-game
        # tunes, 8 for the title theme and the game-over tune, 29 and 62 for
        # the effects. MUSIC_SILENCE only frees class 7, and a class-7 tune
        # can never evict a class-8 one — so "is the theme still there" is not
        # a question about volume, it is a question about who holds the slots,
        # and asking it that way is what finally cornered this.
        def music_classes():
            return sorted({core.memory.u8[ram + 0x292 + y] >> 2
                            for y in range(11)
                            if core.memory.u8[ram + 0x292 + y]})

        # AND IT HAS TO STOP COMING BACK OUT WITH YOU — which is a question
        # about the SOUND, not about the request, and asking only about the
        # request is how this passed for so long while the theme went on
        # playing. So this LISTENS for two seconds and then asks who holds the
        # voice slots, which is the question the volume could not answer.
        core.set_keys(KEYS["B"]); run(core, 4); core.set_keys(); run(core, 14)
        heard = 0
        for _ in range(120):
            core.run_frame()
            st = sound_state(core)
            heard |= st["activos"] | st["pulso 1"] | st["pulso 2"]
            heard |= st["triangulo"] | st["ruido"]
        klass = music_classes()
        if heard or klass:
            failures.append(
                "la musica del titulo sigue en GAME SELECT "
                f"(canales {heard:#06b}, clases {klass})")
        else:
            print("  al volver a GAME SELECT se calla, y suelta sus voces")

        core.set_keys(KEYS["B"]); run(core, 4); core.set_keys(); run(core, 14)
        if last_music() != 0x09:
            failures.append(
                f"en el titulo no esta sonando su tema (${last_music():02X})")
        else:
            # ...and FROM THE TOP. The silence has to be the request before
            # it, because that is what resets the engine; picking the theme up
            # from wherever a suspend froze it is not coming back to a title.
            pair = last_two()
            if pair != (0x08, 0x09):
                failures.append(
                    "el tema del titulo no se reinicia al volver "
                    f"(se pidio ${pair[0]:02X} y luego ${pair[1]:02X}, "
                    "deberia ser $08 y $09)")
            else:
                print("  y en el titulo sigue siendo el suyo, desde el principio")

        # NO MUSIC HAS TO BE NO MUSIC. musicSelectTable's first entry is the
        # silence, which resets the engine without quieting it, so this is the
        # one menu choice that must leave the engine suspended rather than
        # resumed — and it is where a resumed title theme used to surface,
        # since nothing came after it to take the speaker back.
        to_music_page(core, 12)
        # ...onto a tune that is a tune: NO MUSIC and MUSIC MIX are both quiet
        # on this screen by design, so neither can answer the next question.
        for _ in range(8):
            row = tilemap_text(core, MUSIC_ROW)
            if "NO MUSIC" not in row and "MUSIC MIX" not in row:
                break
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 12)
        # THE TUNE HAS TO BE ALONE. This is the shape the bug actually had:
        # the theme kept its class-8 slots, the chosen tune could not take
        # them, and it played on to its END before the tune was heard.
        klass = music_classes()
        if klass != [7]:
            failures.append(
                f"en LEVEL SELECT las voces estan en las clases {klass}, "
                "no solo en la 7: el tema del titulo sigue ocupandolas")
        else:
            print("  en LEVEL SELECT solo suena la clase 7, la cancion elegida")

        for _ in range(8):
            if "NO MUSIC" in tilemap_text(core, MUSIC_ROW):
                break
            core.set_keys(KEYS["LEFT"]); run(core, 4); core.set_keys(); run(core, 12)
        run(core, 20)
        heard = 0
        for _ in range(120):
            core.run_frame()
            st = sound_state(core)
            heard |= st["activos"] | st["pulso 1"] | st["pulso 2"]
            heard |= st["triangulo"] | st["ruido"]
        if heard:
            failures.append(
                f"con NO MUSIC elegido algo sigue sonando (canales {heard:#06b})")
        else:
            print("  NO MUSIC deja el motor callado, sin tema de titulo debajo")

    # THE BUTTONS THE CARTRIDGE ANSWERS TO. processMenuInput takes SELECT as
    # well as START on the title ($9FA4), and takes SELECT as a cursor MOVE on
    # the menus, the same direction as DOWN ($9FBC, and LA048's carry-set add).
    # Both were missing, which is the whole of "SELECT does not select".
    core, screen = load(rom_path)
    run(core, 40)

    def tap(key, settle=12):
        core.set_keys(key)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    tap(KEYS["SELECT"])
    if "GAME SELECT" not in tilemap_text(core, 8):
        failures.append("SELECT no avanza desde el titulo")
    else:
        print("  SELECT avanza desde el titulo, como START")

    # THE LIST IS ONE BLUE AND THE CURSOR IS THE ARROW. Measured off the
    # cartridge: every entry on its GAME SELECT is (48,50,236), chosen or not,
    # and the only white on the screen is the arrow. So this asserts the
    # colour as well as the movement — the port had the list in white with the
    # choice picked out in the CREDIT's orange, which is two mistakes wearing
    # each other's clothes and neither of them shows up in a text comparison.
    def entry_banks():
        return {core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2] >> 12
                for r in range(GAME_SELECT_TY, GAME_SELECT_TY + GAME_SELECT_ROWS)
                for x in range(GAME_SELECT_TX,
                                GAME_SELECT_TX + GAME_SELECT_LONGEST)
                if core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2] & 0x3FF}

    banks = entry_banks()
    if banks != {MENU_TEXT_BANK}:
        failures.append(f"las entradas de GAME SELECT no van todas en el azul "
                        f"del cartucho (bancos {sorted(banks)})")
    else:
        print("  las cinco entradas van en el azul del cartucho, sin resaltado")

    def menu_rows():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2]
                for r in (GAME_SELECT_TY, GAME_SELECT_TY + 1)
                for x in range(4, 26)]

    before = menu_rows()
    tap(KEYS["SELECT"])
    if menu_rows() == before:
        failures.append("SELECT no mueve el cursor en GAME SELECT")
    else:
        print("  SELECT mueve el cursor en GAME SELECT")

    # ...and back onto 1 PLAYER, because SELECT just moved it and every other
    # entry goes to the cable instead of the settings screen. SELECT only ever
    # moves DOWN (LA048's carry-set add), so this walks it round rather than
    # counting the entries — there are three now and there may be five when
    # the COMPUTER player lands.
    def chosen_row():
        """Which GAME SELECT row the cursor ARROW stands beside.

        It used to be "which row is drawn in a palette of its own", and that
        stopped being a question the moment the screen went back to the
        cartridge's colours: there every entry is the same blue and the arrow
        is the whole of the cursor. Asking about the palette instead answered
        "the first row" for every row, which walked this check straight past a
        cursor that was somewhere else entirely and pressed A on 2 PLAYER.
        """
        for row in range(GAME_SELECT_TY, GAME_SELECT_TY + GAME_SELECT_ROWS):
            e = core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + MENU_ARROW_TX) * 2]
            if e & 0x3FF:
                return row
        return None

    for _ in range(GAME_SELECT_ROWS + 1):
        if chosen_row() == GAME_SELECT_TY:
            break
        tap(KEYS["SELECT"])
    if chosen_row() != GAME_SELECT_TY:
        failures.append("no se puede volver a 1 PLAYER con SELECT")
    tap(KEYS["A"])
    if "LEVEL" not in tilemap_text(core, LEVEL_ROW):
        failures.append("A no confirma en GAME SELECT")
    else:
        print("  A confirma, ademas de START")

    # On through the cartridge's three setup screens to the one SELECT is
    # being asked about.
    # SELECT moves the CURSOR down the settings, the way DOWN does ($9FBC).
    # The cursor is an arrow tile a couple of columns left of the labels; read
    # the whole gutter so moving the column does not silently break this.
    def cursor_gutter():
        return [core.memory.u16[SCREENBLOCK_ADDR + (r * 32 + x) * 2]
                for r in (8, 11, 14) for x in range(2, 6)]

    before = cursor_gutter()
    tap(KEYS["SELECT"])
    if cursor_gutter() == before:
        failures.append("SELECT no mueve el cursor en LEVEL SETTINGS")
    else:
        print("  SELECT mueve el cursor en LEVEL SETTINGS")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: al salir del titulo no quedan sprites ni musica suyos, y "
          "SELECT y A hacen lo suyo.")
    return 0


def link_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    def tap(name, settle=6):
        core.set_keys(KEYS[name])
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 8)
    tap("START")                      # title -> game select
    if "GAME SELECT" not in tilemap_text(core, 8):
        failures.append("no aparece GAME SELECT tras el titulo")
    # The cartridge's five, on consecutive rows the way it lists them
    # (gameSelectArrowPpuAddrs, $A0AB).
    for i, want in enumerate(("1 PLAYER", "2 PLAYER", "COOPERATIVE",
                               "VERSUS COMPUTER", "WITH COMPUTER")):
        if want not in tilemap_text(core, GAME_SELECT_TY + i):
            failures.append(f"GAME SELECT no ofrece {want}")

    # The credits sit under whatever the last entry is, at the foot of the
    # frame, so they move when an entry is added. Which one is up depends on
    # the frame count, so this only asks that SOMETHING is there — the six of
    # them are credits_check's business.
    if not (tilemap_text(core, CREDIT_TY).strip()
            and tilemap_text(core, CREDIT_TY + 1).strip()):
        failures.append("faltan los creditos al pie de GAME SELECT")

    tap("DOWN")                       # 1 PLAYER -> 2 PLAYER
    # 2 PLAYER goes STRAIGHT to the cable now: on a link game only one of the
    # two players picks the level and the tune, and neither console knows which
    # one that is until the cable has said so, so the choosing happens after
    # the connecting and only on the master.
    tap("START")                      # game select -> the cable

    if "LINK CABLE" not in tilemap_text(core, 8):
        failures.append("2 PLAYER no lleva a la pantalla de cable link")

    # Nothing is plugged in, so the lobby must be spinning without a partner.
    # If any serial wait lacked a timeout the emulator would stop advancing
    # here; comparing two frames a hundred apart is how that shows up.
    before = pixels(screen)
    run(core, 100)
    if pixels(screen) != before:
        failures.append("la pantalla de espera no es estable")

    # AND IT KEEPS WAITING. It used to give up after ten seconds, and on two
    # real consoles that was the other player still walking through the
    # menus: the lobby now waits for a partner who has never answered for as
    # long as it takes, and B is the way out. (The timeout is for a partner
    # that answered and then went quiet; run_link.py's late_check covers the
    # meeting.)
    run(core, LINK_TIMEOUT_FRAMES + 60)
    msg = tilemap_text(core, LINK_MSG_ROW)
    if "WAITING" not in msg:
        failures.append(f"el lobby deja de esperar sin cable (fila "
                        f"{LINK_MSG_ROW}: {msg!r})")
    else:
        print(f"  sin cable: sigue '{msg.strip()}' tras "
              f"{LINK_TIMEOUT_FRAMES + 60} frames")

    tap("B")                          # back out of the link screen
    if "GAME SELECT" not in tilemap_text(core, 8):
        failures.append("B no vuelve del cable link al menu")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: el modo 2 jugadores llega al cable, espera sin colgarse, y se sale.")
    return 0


def fireworks_check(rom_path):
    """THE FIREWORKS STAY INSIDE THE TITLE'S FRAME.

    They are drawn behind the background, as on the NES, which is what lets
    the opaque blue frame hide a burst that spreads over it. The brick columns
    either side are not opaque — the gaps in the bricks are the backdrop —
    and a big burst near the edge showed sparks through them. Two cores run
    the same title in step, one with the sprite layer switched off: wherever
    they differ, a sprite is on screen, and over three thousand frames it
    must never be in the columns (x < 17 or x >= 224). gba/video.c's
    title_window clips them.
    """
    import numpy as np
    from mgba import ffi

    a, sa = load(rom_path)   # the screens must stay alive; see load()
    b, sb = load(rom_path)
    b._core.enableVideoLayer(b._core, 4, False)   # mGBA's layer 4: OBJ

    def frame(screen):
        raw = ffi.buffer(screen.buffer, screen.stride * screen.height * 4)
        return np.frombuffer(raw, dtype=np.uint32).reshape(
            screen.height, screen.stride)[:, :screen.width].copy()

    leaks, seen = [], 0
    for f in range(3000):
        a.run_frame()
        b.run_frame()
        if f < 30:
            continue
        diff = frame(sa) != frame(sb)
        seen += int(diff.any())
        side = int(diff[:, :17].sum() + diff[:, 224:].sum())
        if side:
            leaks.append((f, side))
    if not seen:
        print("FALLA: en 3000 frames de titulo no se vio ni un sprite")
        return 1
    if leaks:
        print(f"FALLA: los fuegos asoman sobre las columnas en {len(leaks)} "
              f"frames (el primero, {leaks[0][1]} pixeles en el frame "
              f"{leaks[0][0]})")
        return 1
    print(f"  {seen} frames con sprites en el titulo, ninguno fuera del marco")
    print("OK: los fuegos artificiales no asoman por los ladrillos.")
    return 0
