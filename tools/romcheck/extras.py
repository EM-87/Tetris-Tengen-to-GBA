"""
GAME SELECT's OPTIONS (gba/extras.c): the sixth line, its page and the boxes
over it, what each does to the sound and the screen, the battery, the
lifetime figures, and the erase that only SELECT reaches, A asks twice
about, and that takes everything kept.
"""
from .harness import (
    skip, KEYS, PALETTE_ADDR, CELL_BLOCK, CELL_WALL, CHARBLOCK_ADDR,
    GAME_SELECT_TY, MENU_ARROW_TX, LEADER_FIRST_TY,
    REG_SOUND1CNT_H, REG_SOUND2CNT_L, REG_SOUND3CNT_H, SCREENBLOCK_ADDR,
    TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH,
    build_has_protos, game_offsets, game_state_address, load, oam_visible,
    press_start, run, start_game, tilemap_text,
)

BANK_CREDIT = 9                 # PAL_MENU_BASE + 1, the credits' grey
BANK_LABEL = 3                  # the words on the boxes
SET_MUSIC, SET_EFFECTS, SET_BRIGHT, SET_FLAGS, SET_BG = range(5)
XP_TOP, XP_SOUND, XP_BACKGROUND, XP_PROTOS, XP_STATS, XP_SURE, XP_REALLY = range(7)
SAVE_SETTINGS = 0x0E000000 + 0x406   # SAVE_SETTINGS_OFF, in the SRAM's window
LIFT_PX = 4                     # GAME_SELECT_LIFT_PX
PAGE_TY = 9                     # the OPTIONS page's first line (extras.c)
PROTO_TILES = CHARBLOCK_ADDR + 512 * 32   # PROTO_TILE_BASE's 256 tiles
FRAME = ")*+,/;<:"              # the boxes' frame tiles, read as text (T_BOX_*)


def options_check(rom_path):
    """GAME SELECT, UNDER THE CHORD, HAS A SIXTH LINE: OPTIONS, A PAGE.

    Without the chord the cartridge's five. With it OPTIONS under WITH
    COMPUTER, the list a row higher and its heading lifted. A on it: a page,
    as LEVEL SETTINGS is, of GAMMA, SOUND, BACKGROUND, PROTOTYPES and STATS,
    and ERASE SCORES a row lower in the credits' grey, which UP and DOWN
    step over and SELECT, going down a line at a time, reaches. SOUND,
    BACKGROUND, PROTOTYPES and STATS are boxes over the page. SOUND's two
    volumes each silence their own kind — a tune's voices, the cursor's
    tick, a hand-entered tune — and not the other; BACKGROUND's white is the
    backdrop and the words go black; GAMMA a curve; PROTOTYPES a skin the
    page wears at once and the title draws with its own tiles; STATS what
    this console's player has played. All of it on the battery and back
    after a power cycle. ERASE SCORES: A asks, then asks again, NO chosen
    both times, and only YES twice erases — the tables, the lifetime
    figures, the settings, all of it."""
    syms = {}
    for name in ("g_extras_depth", "g_extras_stack", "g_extras_row", "g_settings",
                 "g_lifetime", "g_title_skin", "g_lcd_gamma", "kLcdGamma",
                 "g_options", "g_music"):
        addr, why = game_state_address(rom_path, name)
        if addr is None:
            skip(f"{why}")
            return 0
        syms[name] = addr
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    io = core._native.memory.io
    protos = build_has_protos(rom_path)
    music_at = syms["g_music"]

    def reg(addr):
        return io[(addr - 0x04000000) >> 1]

    def tap(*names, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, 4)
        core.set_keys(); run(core, settle)

    def depth():
        return core.memory.u8[syms["g_extras_depth"]]

    def page():
        return core.memory.u8[syms["g_extras_stack"] + depth() - 1] if depth() else None

    def row(p=XP_TOP):
        return core.memory.u8[syms["g_extras_row"] + p]

    def setting(i):
        return core.memory.u8[syms["g_settings"] + i]

    def life(i):
        return core.memory.u32[syms["g_lifetime"] + 4 * i]

    def screen_text(first=0, last=20):
        """Rows inside the menu's own frame (columns 2-27)."""
        return [" ".join(tilemap_text(core, r, 2, 28).split()) for r in range(first, last)]

    def lines_of(texts):
        """Lines of text, the arrows and the boxes' frame tiles taken off."""
        out = []
        for t in texts:
            words = [w.strip(FRAME) for w in t.replace("->", " ").replace(">", " ").split()]
            out.append(" ".join(w for w in words if w))
        return out

    def palette(i):
        return core.memory.u16[PALETTE_ADDR + i * 2]

    def listen(frames):
        """The loudest each channel got over `frames` frames."""
        top = {"p1": 0, "p2": 0, "tri": 0, "noise": 0}
        for _ in range(frames):
            core.run_frame()
            top["p1"] = max(top["p1"], reg(REG_SOUND1CNT_H) >> 12)
            top["p2"] = max(top["p2"], reg(REG_SOUND2CNT_L) >> 12)
            v = reg(REG_SOUND3CNT_H)
            top["tri"] = max(top["tri"], 4 if v & 0x8000 else (0, 4, 2, 1)[(v >> 13) & 3])
            top["noise"] = max(top["noise"], reg(0x04000078) >> 12)
        return top

    def to_options():
        """From GAME SELECT, the chord rung: OPTIONS, UP from 1 PLAYER."""
        tap("UP")
        tap("A", settle=10)

    def walk(n, key="DOWN"):
        """The page's arrow to line n by `key` — a failure, not a hang, if
        it never gets there (the page gone, a line missing)."""
        for _ in range(12):
            if row() == n:
                return True
            tap(key, settle=4)
        failures.append(f"la flecha de OPTIONS no llega a la linea {n} "
                        f"(profundidad {depth()}, fila {row()})")
        return False

    def to_line(n):
        return walk(n)

    # WITHOUT THE CHORD, THE CARTRIDGE'S FIVE.
    run(core, 40)
    press_start(core); run(core, 20)
    plain = screen_text(GAME_SELECT_TY - 1, GAME_SELECT_TY + 6)
    if "OPTIONS" in " ".join(plain):
        failures.append(f"sin el acorde GAME SELECT ya trae OPTIONS: {plain}")
    tap("UP")
    arrow_on_last = core.memory.u16[SCREENBLOCK_ADDR + ((GAME_SELECT_TY + 4) * 32 +
                                                        MENU_ARROW_TX) * 2] & 0x3FF
    if arrow_on_last != ord(">"):
        failures.append("sin el acorde UP desde 1 PLAYER no da la vuelta a WITH "
                        "COMPUTER")
    tap("DOWN")

    # ...WITH IT, SIX: the list a row higher, OPTIONS last, the heading
    # lifted. UP from 1 PLAYER goes round to it.
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
    core.set_keys(); run(core, 20)
    six = screen_text(GAME_SELECT_TY - 1, GAME_SELECT_TY + 5)
    lifted = reg(0x04000016)
    tap("UP")
    arrow = core.memory.u16[SCREENBLOCK_ADDR + ((GAME_SELECT_TY + 4) * 32 +
                                                MENU_ARROW_TX) * 2] & 0x3FF
    if not six[0].endswith("1 PLAYER") or not six[-1].endswith("OPTIONS") or \
            lifted != LIFT_PX or arrow != ord(">"):
        failures.append(f"con el acorde GAME SELECT no tiene OPTIONS como sexta "
                        f"linea, una fila mas arriba y el titulo levantado "
                        f"({six}, {lifted}px, flecha {arrow:#x})")
    else:
        print("  con el acorde GAME SELECT tiene seis lineas, OPTIONS la ultima; "
              "UP desde 1 PLAYER da la vuelta a ella")

    # THE PAGE: its heading and lines, the erase a row lower in grey, the
    # arrow's walk; no credits on it.
    tap("A", settle=10)
    page_lines = lines_of(screen_text(8, 18))
    want = ["OPTIONS", "GAMMA", "SOUND", "BACKGROUND", "PROTOTYPES", "STATS", "",
            "ERASE SCORES"]
    erase_ty = PAGE_TY + 6
    erase_bank = core.memory.u16[SCREENBLOCK_ADDR + (erase_ty * 32 + 7) * 2] >> 12
    gamma_boxes = [core.memory.u16[SCREENBLOCK_ADDR + (PAGE_TY * 32 + 16 + i) * 2] & 0x3FF
                   for i in range(3)]
    if depth() != 1 or page() != XP_TOP or page_lines[:8] != want or \
            erase_bank != BANK_CREDIT or len(set(gamma_boxes)) != 2:
        failures.append(f"A en OPTIONS no pone su pagina con sus lineas, GAMMA con "
                        f"sus tres casillas y ERASE SCORES en gris ({page_lines}, "
                        f"banco {erase_bank}, casillas {gamma_boxes})")
    seen = set()
    for _ in range(12):
        tap("DOWN", settle=4)
        seen.add(row())
    for _ in range(12):
        tap("UP", settle=4)
        seen.add(row())
    if seen != set(range(5)):
        failures.append(f"UP y DOWN no recorren las cinco o llegan al borrado: "
                        f"{sorted(seen)}")
    to_line(4)
    tap("SELECT", settle=6)
    on_erase = row()
    tap("SELECT", settle=6)
    wrapped = row()
    for _ in range(5):
        tap("SELECT", settle=4)
    tap("UP", settle=4)
    back_up = row()
    if on_erase != 5 or wrapped != 0 or back_up != 4:
        failures.append(f"SELECT no baja de STATS a ERASE SCORES y de ahi arriba, o "
                        f"UP no vuelve a STATS ({on_erase}, {wrapped}, {back_up})")
    elif not failures:
        print("  OPTIONS es una pagina: GAMMA, SOUND, BACKGROUND, PROTOTYPES, "
              "STATS; ERASE SCORES en gris, al que solo llega SELECT")

    # THE ERASE ASKS: A on it, NO chosen; A on NO, back; again, YES, and a
    # second question, NO chosen; B, back to the first. Nothing erased.
    tap("SELECT", settle=6)
    tap("A", settle=10)
    sure = (page(), row(XP_SURE), " ".join(screen_text()))
    tap("A", settle=10)
    after_no = depth()
    tap("A", settle=10)
    tap("UP", settle=4)
    tap("A", settle=10)
    really = (page(), row(XP_REALLY), " ".join(screen_text()))
    tap("B", settle=10)
    after_b = depth()
    if sure[:2] != (XP_SURE, 1) or "ARE YOU SURE?" not in sure[2] or \
            after_no != 1 or really[:2] != (XP_REALLY, 1) or \
            "ARE YOU REALLY SURE?" not in really[2] or after_b != 2:
        failures.append(f"el borrado no pregunta dos veces con NO elegido "
                        f"({sure[:2]}, {after_no}, {really[:2]}, {after_b})")
    else:
        print("  A en ERASE SCORES pregunta ARE YOU SURE? y luego ARE YOU REALLY "
              "SURE?, NO elegido las dos; NO vuelve a la pagina, B a la pregunta "
              "anterior")
    tap("B", settle=10)
    walk(1, "UP")

    # SOUND, a box over the page: MUSIC and EFFECTS, four boxes each.
    tap("A", settle=10)
    sound = (page(), lines_of(screen_text()))
    for _ in range(4):
        tap("LEFT", settle=4)
    music_off = setting(SET_MUSIC)
    tap("B", settle=6)
    if sound[0] != XP_SOUND or not any("MUSIC" in t.split() for t in sound[1]) or \
            not any("EFFECTS" in t.split() for t in sound[1]):
        failures.append(f"SOUND no es su caja con MUSIC y EFFECTS: {sound}")

    def level_settings(tune, ticks):
        """Off the page and down to LEVEL SETTINGS: what `tune` (NO MUSIC 0,
        LOGINSKA 1, ... KOROBEINIKI 5) and the cursor's tick sound like;
        back on the page after."""
        tap("B", settle=20)                       # the page -> GAME SELECT
        tap("DOWN", settle=6)                     # OPTIONS -> 1 PLAYER
        tap("START", settle=20)
        tap("DOWN", settle=8); tap("DOWN", settle=8)
        for _ in range(8):                        # the page keeps its last tune
            if core.memory.u8[music_at] == tune:
                break
            tap("RIGHT", settle=8)
        run(core, 30)
        heard = listen(120)
        tick = 0
        for _ in range(ticks):
            core.set_keys(KEYS["UP"]); core.run_frame(); core.set_keys()
            tick = max(tick, listen(12)["p2"])
            core.set_keys(KEYS["DOWN"]); core.run_frame(); core.set_keys()
            tick = max(tick, listen(12)["p2"])
        tap("B", settle=20)                       # -> GAME SELECT
        to_options()
        return heard, tick

    loud_off, tick_on = level_settings(1, 2)       # LOGINSKA, MUSIC OFF
    hand_off, _ = level_settings(5, 0)            # KOROBEINIKI, MUSIC OFF
    tap("DOWN", settle=4)
    tap("A", settle=10)
    for _ in range(4):
        tap("RIGHT", settle=4)                    # MUSIC back to full
    tap("DOWN", settle=4)
    for _ in range(4):
        tap("LEFT", settle=4)                     # EFFECTS OFF
    effects_off = setting(SET_EFFECTS)
    tap("B", settle=6)
    loud_on, _ = level_settings(1, 0)             # LOGINSKA, MUSIC full
    _, tick_off = level_settings(0, 2)            # NO MUSIC, the tick alone
    if music_off != 0 or loud_off["p1"] or loud_off["tri"] or not tick_on:
        failures.append(f"MUSIC OFF no calla LOGINSKA ({loud_off}) o calla el tic "
                        f"({tick_on}); ajuste {music_off}")
    elif any(hand_off[k] for k in ("p1", "p2", "tri")):
        failures.append(f"con MUSIC OFF KOROBEINIKI se sigue oyendo {hand_off}")
    elif effects_off != 0 or tick_off or not loud_on["p1"]:
        failures.append(f"EFFECTS OFF no calla el tic ({tick_off}) o calla "
                        f"LOGINSKA ({loud_on}); ajuste {effects_off}")
    else:
        print("  SOUND: MUSIC OFF calla LOGINSKA y KOROBEINIKI y deja el tic; "
              "EFFECTS OFF calla el tic y deja LOGINSKA")
    # Effects to half, for the battery's part below.
    tap("DOWN", settle=4)
    tap("A", settle=10)
    tap("DOWN", settle=4)
    tap("RIGHT", settle=4)
    tap("RIGHT", settle=4)
    tap("B", settle=6)

    # BACKGROUND: WHITE is the backdrop, the words black; GAMMA a curve.
    black_backdrop = palette(0)
    to_line(2)
    tap("A", settle=10)
    tap("DOWN", settle=4)
    tap("A", settle=10)
    white = (setting(SET_BG), palette(0), palette(BANK_LABEL * 16 + 1))
    tap("B", settle=6)
    walk(0, "UP")
    tap("RIGHT", settle=10)
    curve = (core.memory.u32[syms["g_lcd_gamma"]] - syms["kLcdGamma"]) // 32
    if white[0] != 1 or white[1] == black_backdrop or (white[1] & 0x1F) < 28 or \
            (white[2] & 0x7FFF) > 0x0421:
        failures.append(f"BACKGROUND WHITE no pone el fondo blanco y las letras "
                        f"negras ({white[0]}, fondo {white[1]:#06x}, letra "
                        f"{white[2]:#06x})")
    elif setting(SET_BRIGHT) != 1 or curve != 1:
        failures.append(f"GAMMA no pone la segunda curva ({setting(SET_BRIGHT)}, "
                        f"{curve})")
    else:
        print("  BACKGROUND WHITE: fondo blanco y letras negras; GAMMA a la "
              "segunda curva")

    # PROTOTYPES: one put on, which the page wears; the title draws with its
    # own tiles, as it does when the title's chord puts it on.
    if protos:
        to_line(3)
        tap("A", settle=10)
        prot = lines_of(screen_text())
        tap("DOWN", settle=4)
        tap("A", settle=20)
        skin = core.memory.u8[syms["g_title_skin"]]
        tap("B", settle=6)
        tap("B", settle=20)                       # the page -> GAME SELECT
        tap("B", settle=60)                       # -> the title
        tiles = bytes(core.memory.u8[PROTO_TILES + i] for i in range(0, 256 * 32, 7))
        ref, ref_screen = load(rom_path)
        _r = ref_screen
        run(ref, 40)
        ref.set_keys(KEYS["L"], KEYS["R"]); run(ref, 4); ref.set_keys(); run(ref, 40)
        want_tiles = bytes(ref.memory.u8[PROTO_TILES + i] for i in range(0, 256 * 32, 7))
        listed = " ".join(prot)
        if skin != 1 or not all(n in listed for n in ("RELEASE", "NINTENDO", "ATARI",
                                                      "MIRRORSOFT")) \
                or "UNUSED" in listed:
            failures.append(f"PROTOTYPES no pone NINTENDO o no lista por editor: "
                            f"{prot}, skin {skin}")
        elif tiles != want_tiles:
            failures.append("el titulo del prototipo elegido en OPTIONS no lleva "
                            "sus tiles")
        else:
            print("  PROTOTYPES: RELEASE, NINTENDO, ATARI, MIRRORSOFT; elegido "
                  "uno, el titulo lo dibuja con sus tiles")
        del ref, ref_screen
    else:
        skip("esta ROM no trae prototipos: PROTOTYPES solo tiene RELEASE")
        tap("B", settle=20)
        tap("B", settle=60)

    # A GAME, AND THE LIFETIME FIGURES: a game played and lost with lines
    # cleared on the way, its record on the table; STATS says so.
    off = game_offsets(rom_path)
    base, _ = game_state_address(rom_path)
    start_game(core)
    for r in (18, 19):
        for c in range(TENGEN_PF_WIDTH):
            core.memory.u8[base + off["field"] + r * TENGEN_PF_WIDTH + c] = (
                CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1) else CELL_BLOCK)
    core.set_keys(KEYS["DOWN"]); run(core, 120); core.set_keys()
    for i in range(4):
        core.memory.u8[base + off["score"] + i] = (90000 >> (8 * i)) & 0xFF
    for r in range(TENGEN_PF_HEIGHT):
        for c in range(TENGEN_PF_WIDTH):
            core.memory.u8[base + off["field"] + r * TENGEN_PF_WIDTH + c] = (
                CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1) else
                (0 if c == 5 else CELL_BLOCK))
    run(core, 300)
    press_start(core); run(core, 30)              # the plaque -> HIGH SCORES
    tap("START", settle=20)
    press_start(core); run(core, 40)
    games, frames, pieces = life(1), life(0), life(2)
    lines = life(3) + 2 * life(4) + 3 * life(5) + 4 * life(6)
    if games != 1 or frames < 100 or pieces < 1 or lines < 1:
        failures.append(f"las cifras de toda la vida no cuentan la partida "
                        f"(partidas {games}, frames {frames}, piezas {pieces}, "
                        f"lineas {lines})")
    else:
        print(f"  una partida: {games} partida, {frames} frames, {pieces} piezas, "
              f"{lines} linea(s) en las cifras de toda la vida")

    # ...ALL OF IT ON THE BATTERY, and STATS says what it counted.
    saved = [core.memory.u8[SAVE_SETTINGS + i] for i in range(5)]
    core.reset()
    run(core, 60)
    after = [setting(i) for i in range(5)]
    life_after = (life(1), life(2))
    press_start(core); run(core, 20)
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4); core.set_keys(); run(core, 20)
    to_options()
    to_line(4)
    tap("A", settle=10)
    stats = lines_of(screen_text())
    if after != [4, 2, 1, 0, 1] or saved != after or life_after != (games, pieces):
        failures.append(f"los ajustes o las cifras no sobreviven a apagar: "
                        f"{saved} -> {after}, {life_after} y no {(games, pieces)}")
    elif not any(t.split()[-2:] == ["GAMES", "1"] for t in stats if t) or \
            not any("TETRIS RATE" in t for t in stats):
        failures.append(f"STATS no dice lo contado: {stats}")
    else:
        print("  ajustes y cifras en la bateria, de vuelta al encender; STATS "
              "dice GAMES 1")
    tap("B", settle=6)

    # THE ERASE, YES TWICE: everything kept goes — the tables back to the
    # cartridge's fifteen, the lifetime figures, the settings, the pause
    # menu's switches — and the arrow back at the top.
    core.memory.u8[syms["g_options"]] = 0x08          # GHOST, to be erased
    to_line(4)
    tap("SELECT", settle=6)
    tap("A", settle=8)
    tap("UP", settle=4)
    tap("A", settle=8)
    tap("UP", settle=4)
    tap("A", settle=20)
    erased = (depth(), row(), life(1), [setting(i) for i in range(5)],
              core.memory.u8[syms["g_options"]], palette(0))
    tap("B", settle=20)                       # the page -> GAME SELECT
    tap("B", settle=40)                       # -> the title
    tap("SELECT", settle=40)                  # the table
    first = " ".join(tilemap_text(core, LEADER_FIRST_TY).split())
    if erased[:2] != (1, 0) or erased[2] or erased[3] != [4, 4, 0, 0, 0] or \
            erased[4] or erased[5] != black_backdrop or "17000" not in first:
        failures.append(f"SI y SI no lo borra todo (pagina y flecha {erased[:2]}, "
                        f"partidas {erased[2]}, ajustes {erased[3]}, opciones "
                        f"{erased[4]}, fondo {erased[5]:#06x}, tabla {first!r})")
    else:
        print("  SI dos veces: la tabla a las quince del cartucho, las cifras a "
              "cero, los ajustes y las opciones de fabrica, la flecha arriba")
    del core, screen

    sprites_on_light(rom_path, failures)

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: OPTIONS en GAME SELECT: una pagina con sus cajas, ajustes y cifras "
          "en la bateria, y el borrado solo por SELECT, con dos preguntas, de todo.")
    return 0


OBJ_PALETTE_ADDR = PALETTE_ADDR + 0x200
PAL_OBJ_CLEAR, PAL_OBJ_TITLE, PAL_OBJ_GAME = 4, 5, 9      # gba/port.h
TITLE_OBJ_TILE_BASE, TITLE_OBJ_INKED_BASE = 256, 768
TITLE_FIREWORK_FIRST = 19


def sprites_on_light(rom_path, failures):
    """THE SPRITES DRAWN FOR A BLACK BACKDROP, on a white one
    (ink_sprite_palettes, gba/video.c): the line-clear puff is the backdrop,
    the one-player drop points ($37, pale yellow) are inked darker, and the
    title's fireworks draw from the inked copy of their tiles while the
    cathedral's sprites keep the picture's. Back on black, all of it is the
    cartridge's again — by BACKGROUND's own BLACK, and by the erase."""
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    row_at, _ = game_state_address(rom_path, "g_extras_row")

    def tap(*names, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, 4)
        core.set_keys(); run(core, settle)

    def obj(i):
        return core.memory.u16[OBJ_PALETTE_ADDR + i * 2]

    def bright(c):
        return (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)

    def background(n):
        """From the title, the chord rung: OPTIONS' BACKGROUND, the n-th,
        and back to the title."""
        press_start(core); run(core, 20)
        tap("UP")
        tap("A", settle=10)
        for _ in range(8):
            if "BACKGROUND" in tilemap_text(core, PAGE_TY + core.memory.u8[row_at]):
                break
            tap("DOWN", settle=4)
        tap("A", settle=10)
        for _ in range(8):
            if core.memory.u8[row_at + XP_BACKGROUND] == n:
                break
            tap("DOWN", settle=4)
        else:
            failures.append(f"BACKGROUND no llega a la linea {n}")
        tap("A", settle=10)
        tap("B", settle=6)
        tap("B", settle=20)
        tap("B", settle=40)

    def look():
        """(clear's colours are the backdrop's, drop points' ink, the
        fireworks' tile bases, the cathedral's)."""
        backdrop = core.memory.u16[PALETTE_ADDR]
        clear = all(obj(PAL_OBJ_CLEAR * 16 + i) == backdrop for i in (1, 2, 3))
        points = obj((PAL_OBJ_GAME + 3) * 16 + 1)
        fw, cat = set(), set()
        for _ in range(400):
            core.run_frame()
            for _, _, t in oam_visible(core, TITLE_FIREWORK_FIRST, 64):
                fw.add(t & ~255)
            for _, _, t in oam_visible(core, 0, TITLE_FIREWORK_FIRST):
                cat.add(t & ~255)
        return clear, points, fw, cat

    def erase():
        """From the title: OPTIONS, SELECT down to ERASE SCORES, YES and
        YES, and back to the title."""
        press_start(core); run(core, 20)
        tap("UP")
        tap("A", settle=10)
        for _ in range(8):
            if "ERASE" in tilemap_text(core, PAGE_TY + core.memory.u8[row_at] + 1):
                break
            tap("SELECT", settle=4)
        tap("A", settle=10)
        tap("UP", settle=4)
        tap("A", settle=10)
        tap("UP", settle=4)
        tap("A", settle=20)
        tap("B", settle=20)
        tap("B", settle=40)

    run(core, 40)
    press_start(core); run(core, 10)
    tap("L", "R")
    tap("B", settle=30)
    black_backdrop = core.memory.u16[PALETTE_ADDR]
    black = look()
    background(1)
    white = look()
    background(0)
    again = look()
    # ...AND THE ERASE PUTS THE BLACK BACK, from WHITE with the curve
    # untouched — where nothing but the erase's own bg_apply repaints it.
    background(1)
    erase()
    erased = look()
    erased_backdrop = core.memory.u16[PALETTE_ADDR]
    want_fw = {TITLE_OBJ_INKED_BASE}
    if not white[0]:
        failures.append("sobre blanco la nube de la linea sigue negra, no del "
                        "color del fondo")
    elif bright(white[1]) >= bright(black[1]):
        failures.append(f"sobre blanco los puntos de la caida no se oscurecen "
                        f"({black[1]:#06x} -> {white[1]:#06x})")
    elif white[2] != want_fw or white[3] != {TITLE_OBJ_TILE_BASE}:
        failures.append(f"sobre blanco los fuegos no salen de la copia entintada "
                        f"o la catedral si: fuegos {sorted(white[2])}, catedral "
                        f"{sorted(white[3])}")
    elif again[1] != black[1] or again[2] != {TITLE_OBJ_TILE_BASE} or \
            again[3] != {TITLE_OBJ_TILE_BASE} or black[2] != {TITLE_OBJ_TILE_BASE}:
        failures.append(f"de vuelta al negro los sprites no son los del cartucho "
                        f"(puntos {again[1]:#06x}, fuegos {sorted(again[2])})")
    elif erased_backdrop != black_backdrop or erased != again:
        failures.append(f"borrarlo todo desde WHITE no vuelve al negro (fondo "
                        f"{erased_backdrop:#06x}, sprites {erased[:2]})")
    else:
        print("  sobre blanco: la nube de la linea del color del fondo, los "
              "puntos de la caida oscuros, los fuegos entintados y la catedral "
              "intacta; en negro, los del cartucho, y borrarlo todo desde "
              "blanco vuelve a el")
