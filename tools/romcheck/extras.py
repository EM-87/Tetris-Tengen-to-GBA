"""
GAME SELECT's OPTIONS (gba/extras.c): the sixth line, its page and the boxes
over it — centred, all of them — what each does to the sound and the screen,
the battery, the lifetime figures, and the erase that only SELECT reaches, A
asks twice about, and that takes everything kept. And the backgrounds: the
title at night whatever is chosen, and the black the art borrows from the
backdrop kept black on the others.
"""
from .harness import (
    skip, KEYS, PALETTE_ADDR, CELL_BLOCK, CELL_WALL, CHARBLOCK_ADDR,
    GAME_SELECT_TY, MENU_ARROW_TX, LEADER_FIRST_TY,
    REG_SOUND1CNT_H, REG_SOUND2CNT_L, REG_SOUND3CNT_H, SCREENBLOCK_ADDR,
    TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH,
    SCREENBLOCK_OFFSET_ADDR,
    build_has_protos, pixels, check_layout, game_offsets, game_state_address, load,
    oam_visible, press_start, run, start_game, tilemap_text,
)

BANK_CREDIT = 9                 # PAL_MENU_BASE + 1, the credits' grey
BANK_LABEL = 3                  # the words on the boxes
SET_MUSIC, SET_EFFECTS, SET_BRIGHT, SET_FLAGS, SET_BG = range(5)
XP_TOP, XP_BACKGROUND, XP_PROTOS, XP_STATS, XP_SURE, XP_REALLY = range(6)
BG_BLACK, BG_DARK, BG_LIGHT, BG_IVORY = range(4)
SAVE_SETTINGS = 0x0E000000 + 0x406   # SAVE_SETTINGS_OFF, in the SRAM's window
LIFT_PX = 4                     # GAME_SELECT_LIFT_PX
PAGE_TY = 10                    # the OPTIONS page's first line (extras.c)
PAGE_LABEL_TX, PAGE_VALUE_TX, PAGE_VALUE_W = 7, 19, 4   # its block, centred
SCREEN_TW, SCREEN_TH = 30, 20
T_BOX = {0x29: "TL", 0x2A: "T", 0x2B: "TR", 0x2C: "L", 0x2F: "R",
         0x3A: "BL", 0x3B: "B", 0x3C: "BR"}       # the boxes' frame (T_BOX_*)
PROTO_TILES = CHARBLOCK_ADDR + 512 * 32   # PROTO_TILE_BASE's 256 tiles
FRAME = ")*+,/;<:"              # the boxes' frame tiles, read as text (T_BOX_*)


def options_check(rom_path):
    """GAME SELECT, UNDER THE CHORD, HAS A SIXTH LINE: OPTIONS, A PAGE.

    Without the chord the cartridge's five. With it OPTIONS under WITH
    COMPUTER, the list a row higher and its heading lifted. A on it: a page,
    as LEVEL SETTINGS is, of BRIGHTNESS, MUSIC and EFFECTS (bars of four
    boxes), BACKGROUND, PROTOTYPES and STATS (boxes over the page), and ERASE
    DATA a row lower in the credits' grey, which UP and DOWN step over and
    SELECT, going down a line at a time, reaches. ALL OF IT CENTRED: the
    page's names and values one block in the middle of the screen, every box
    in the middle of the screen with its heading and its lines centred in it,
    to the pixel, the second question covering the first. MUSIC and EFFECTS
    each silence their own kind — a tune's voices, the cursor's tick, a
    hand-entered tune — and not the other; LIGHT GREY is the backdrop and the
    words go black; BRIGHTNESS a curve; PROTOTYPES the builds in the order
    they were made, a skin the page wears at once and the title draws with
    its own tiles; STATS what this console's player has played. All of it on
    the battery and back after a power cycle. ERASE DATA: A asks, then asks
    again, NO chosen both times, and only YES twice erases — the tables, the
    lifetime figures, the settings, the saved games, all of it."""
    syms = {}
    for name in ("g_extras_depth", "g_extras_stack", "g_extras_row", "g_settings",
                 "g_lifetime", "g_title_skin", "g_lcd_gamma", "kLcdGamma",
                 "g_options", "g_music", "g_skin_rows"):
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

    def tiles_at(addr, ty):
        return [core.memory.u16[addr + (ty * 32 + x) * 2] & 0x3FF for x in range(SCREEN_TW)]

    def ink_span(ty, x0=0, x1=SCREEN_TW):
        """The pixels a row's text takes, [left, right), on the main map or
        on the offset layer (half a tile over), between columns x0 and x1."""
        xs = []
        for addr, shift in ((SCREENBLOCK_ADDR, 0), (SCREENBLOCK_OFFSET_ADDR, 4)):
            row_tiles = tiles_at(addr, ty)
            for x in range(x0, x1):
                t = row_tiles[x]
                if t and t not in T_BOX:
                    xs += [x * 8 + shift, x * 8 + shift + 8]
        return (min(xs), max(xs)) if xs else None

    def boxes():
        """The frames of the boxes on the screen: (tx, ty, w, h) each."""
        found = []
        for ty in range(SCREEN_TH):
            row_tiles = tiles_at(SCREENBLOCK_ADDR, ty)
            for tx in range(SCREEN_TW):
                if T_BOX.get(row_tiles[tx]) != "TL":
                    continue
                w = next((i for i in range(tx, SCREEN_TW)
                          if T_BOX.get(row_tiles[i]) == "TR"), tx) - tx + 1
                h = next((j for j in range(ty, SCREEN_TH)
                          if T_BOX.get(tiles_at(SCREENBLOCK_ADDR, j)[tx]) == "BL"), ty) - ty + 1
                found.append((tx, ty, w, h))
        return found

    def centred_box(what):
        """The box on top is in the middle of the screen, and so are its
        heading and its lines' block in it, to the pixel; a list of what is
        not, empty when all is."""
        found = boxes()
        if len(found) != 1:
            return [f"{what}: {len(found)} cajas a la vista, no una: {found}"]
        tx, ty, w, h = found[0]
        bad = []
        if 2 * tx + w != SCREEN_TW or 2 * ty + h != SCREEN_TH:
            bad.append(f"{what}: la caja ({tx},{ty}) {w}x{h} no esta en el centro")
        centre = 8 * (2 * tx + w)            # twice the box's middle, in pixels
        headline = ink_span(ty + 1, tx + 1, tx + w - 1)
        if headline is None or sum(headline) != centre:
            bad.append(f"{what}: el titulo {headline} no esta centrado en la caja")
        spans = [ink_span(y, tx + 1, tx + w - 1) for y in range(ty + 3, ty + h - 1)]
        spans = [sp for sp in spans if sp]
        if not spans or min(a for a, _ in spans) + max(b for _, b in spans) != centre:
            bad.append(f"{what}: sus lineas {spans} no son un bloque centrado")
        return bad

    # THE PAGE: its heading and lines, the erase a row lower in grey, the
    # values a bar of four boxes each; the names and values one block in the
    # middle of the screen.
    tap("A", settle=10)
    page_lines = [t for t in lines_of(screen_text(8, 19)) if t]
    want = ["OPTIONS", "BRIGHTNESS", "MUSIC", "EFFECTS", "BACKGROUND", "PROTOTYPES",
            "STATS", "ERASE DATA"]
    erase_ty = PAGE_TY + 7
    erase_bank = core.memory.u16[SCREENBLOCK_ADDR + (erase_ty * 32 + PAGE_LABEL_TX) * 2] >> 12
    sq = check_layout(rom_path)["hold_label"] + 6          # T_SQ_OFF, T_SQ_ON after
    bars = [[tiles_at(SCREENBLOCK_ADDR, PAGE_TY + r)[PAGE_VALUE_TX + i] for i in range(4)]
            for r in range(3)]
    lit = [sum(1 for t in b if t == sq + 1) for b in bars]
    starts = {min(x for x in range(SCREEN_TW)
                  if 0x41 <= tiles_at(SCREENBLOCK_ADDR, PAGE_TY + r)[x] <= 0x5A)
              for r in (0, 1, 2, 3, 4, 5, 7)}
    block = (PAGE_LABEL_TX * 8, (PAGE_VALUE_TX + PAGE_VALUE_W) * 8)
    if depth() != 1 or page() != XP_TOP or page_lines != want or \
            erase_bank != BANK_CREDIT or any(t not in (sq, sq + 1) for b in bars for t in b) \
            or lit != [1, 4, 4]:
        failures.append(f"A en OPTIONS no pone su pagina con sus lineas, sus barras de "
                        f"cuatro casillas y ERASE DATA en gris ({page_lines}, banco "
                        f"{erase_bank}, casillas {bars}, encendidas {lit})")
    elif starts != {PAGE_LABEL_TX} or sum(block) != 8 * SCREEN_TW:
        failures.append(f"los nombres de OPTIONS no empiezan todos en la columna "
                        f"{PAGE_LABEL_TX}, o el bloque no esta centrado ({starts})")
    seen = set()
    for _ in range(12):
        tap("DOWN", settle=4)
        seen.add(row())
    for _ in range(12):
        tap("UP", settle=4)
        seen.add(row())
    if seen != set(range(6)):
        failures.append(f"UP y DOWN no recorren las seis o llegan al borrado: "
                        f"{sorted(seen)}")
    to_line(5)
    tap("SELECT", settle=6)
    on_erase = row()
    tap("SELECT", settle=6)
    wrapped = row()
    for _ in range(6):
        tap("SELECT", settle=4)
    tap("UP", settle=4)
    back_up = row()
    if on_erase != 6 or wrapped != 0 or back_up != 5:
        failures.append(f"SELECT no baja de STATS a ERASE DATA y de ahi arriba, o "
                        f"UP no vuelve a STATS ({on_erase}, {wrapped}, {back_up})")
    elif not failures:
        print("  OPTIONS es una pagina centrada: BRIGHTNESS, MUSIC, EFFECTS, "
              "BACKGROUND, PROTOTYPES, STATS; ERASE DATA en gris, al que solo "
              "llega SELECT")

    # THE ERASE ASKS: A on it, NO chosen; A on NO, back; again, YES, and a
    # second question, NO chosen, over the first and covering it; B, back to
    # the first. Nothing erased. Both in the middle of the screen.
    tap("SELECT", settle=6)
    tap("A", settle=10)
    sure = (page(), row(XP_SURE), " ".join(screen_text()))
    sure_centred = centred_box("ARE YOU SURE?")
    tap("A", settle=10)
    after_no = depth()
    tap("A", settle=10)
    tap("UP", settle=4)
    tap("A", settle=10)
    really = (page(), row(XP_REALLY), " ".join(screen_text()))
    really_centred = centred_box("ARE YOU REALLY SURE?")
    tap("B", settle=10)
    after_b = depth()
    if sure[:2] != (XP_SURE, 1) or "ARE YOU SURE?" not in sure[2] or \
            after_no != 1 or really[:2] != (XP_REALLY, 1) or \
            "ARE YOU REALLY SURE?" not in really[2] or after_b != 2:
        failures.append(f"el borrado no pregunta dos veces con NO elegido "
                        f"({sure[:2]}, {after_no}, {really[:2]}, {after_b})")
    elif sure_centred or really_centred:
        failures += sure_centred + really_centred
    else:
        print("  A en ERASE DATA pregunta ARE YOU SURE? y luego ARE YOU REALLY "
              "SURE?, NO elegido las dos, en el centro de la pantalla, la segunda "
              "tapando la primera; NO vuelve a la pagina, B a la pregunta anterior")
    tap("B", settle=10)

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

    # MUSIC and EFFECTS, on the page: LEFT down to none.
    walk(1, "DOWN")
    for _ in range(4):
        tap("LEFT", settle=4)
    music_off = setting(SET_MUSIC)
    loud_off, tick_on = level_settings(1, 2)       # LOGINSKA, MUSIC OFF
    hand_off, _ = level_settings(5, 0)            # KOROBEINIKI, MUSIC OFF
    walk(1, "DOWN")
    for _ in range(4):
        tap("RIGHT", settle=4)                    # MUSIC back to full
    walk(2, "DOWN")
    for _ in range(4):
        tap("LEFT", settle=4)                     # EFFECTS OFF
    effects_off = setting(SET_EFFECTS)
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
        print("  MUSIC OFF calla LOGINSKA y KOROBEINIKI y deja el tic; EFFECTS "
              "OFF calla el tic y deja LOGINSKA")
    # Effects to half, for the battery's part below.
    walk(2, "DOWN")
    tap("RIGHT", settle=4)
    tap("RIGHT", settle=4)

    # BACKGROUND: a box in the middle, the colours by name; LIGHT GREY is the
    # backdrop, the words black. BRIGHTNESS a curve.
    black_backdrop = palette(0)
    walk(3, "DOWN")
    tap("A", settle=10)
    bg_lines = [t for t in lines_of(screen_text()) if t]
    bg_centred = centred_box("BACKGROUND")
    tap("DOWN", settle=4)
    tap("DOWN", settle=4)
    tap("A", settle=10)
    light = (setting(SET_BG), palette(0), palette(BANK_LABEL * 16 + 1))
    tap("B", settle=6)
    walk(0, "UP")
    tap("RIGHT", settle=10)
    curve = (core.memory.u32[syms["g_lcd_gamma"]] - syms["kLcdGamma"]) // 32
    names = " ".join(bg_lines)
    if not all(n in names for n in ("BLACK", "DARK GREY", "LIGHT GREY", "IVORY")) or \
            "WHITE" in names:
        failures.append(f"BACKGROUND no lista BLACK, DARK GREY, LIGHT GREY, IVORY: "
                        f"{bg_lines}")
    elif bg_centred:
        failures += bg_centred
    elif light[0] != BG_LIGHT or (light[1] & 0x1F) != 24 or (light[2] & 0x7FFF) > 0x0421:
        failures.append(f"BACKGROUND LIGHT GREY no pone el gris claro y las letras "
                        f"negras ({light[0]}, fondo {light[1]:#06x}, letra "
                        f"{light[2]:#06x})")
    elif setting(SET_BRIGHT) != 1 or curve != 1:
        failures.append(f"BRIGHTNESS no pone la segunda curva ({setting(SET_BRIGHT)}, "
                        f"{curve})")
    else:
        print("  BACKGROUND, centrada: BLACK, DARK GREY, LIGHT GREY, IVORY; el gris "
              "claro de fondo y letras negras; BRIGHTNESS a la segunda curva")

    # STATS: a box in the middle.
    walk(5, "DOWN")
    tap("A", settle=10)
    stats_centred = centred_box("STATS")
    tap("B", settle=6)
    if stats_centred:
        failures += stats_centred

    # PROTOTYPES: in the order the builds were made, centred; NINTENDO, the
    # last, put on, which the page wears; the title draws with its own tiles,
    # as it does when the title's chord puts it on.
    if protos:
        walk(4, "DOWN")
        tap("A", settle=10)
        prot = [t for t in lines_of(screen_text()) if t]
        prot_centred = centred_box("PROTOTYPES")
        rows_now = [core.memory.u8[syms["g_skin_rows"] + i] for i in range(4)]
        for _ in range(3):
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
        head = prot.index("PROTOTYPES") if "PROTOTYPES" in prot else 0
        if prot[head + 1:head + 5] != ["RELEASE", "MIRRORSOFT", "ATARI", "NINTENDO"] or \
                rows_now != [0, 3, 2, 1]:
            failures.append(f"PROTOTYPES no lista por orden de fabricacion: {prot}, "
                            f"skins {rows_now}")
        elif prot_centred:
            failures += prot_centred
        elif skin != 1:
            failures.append(f"la ultima linea de PROTOTYPES no pone NINTENDO: skin {skin}")
        elif tiles != want_tiles:
            failures.append("el titulo del prototipo elegido en OPTIONS no lleva "
                            "sus tiles")
        else:
            print("  PROTOTYPES, centrada: RELEASE, MIRRORSOFT, ATARI, NINTENDO; "
                  "elegido uno, el titulo lo dibuja con sus tiles")
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
                (0 if c in (1, TENGEN_PF_WIDTH - 2) else CELL_BLOCK))
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
    to_line(5)
    tap("A", settle=10)
    stats = lines_of(screen_text())
    if after != [4, 2, 1, 0, BG_LIGHT] or saved != after or life_after != (games, pieces):
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
    # menu's switches, the saved games — and the arrow back at the top. A
    # saved game's record planted on the battery (suspend.c's "SAVE").
    core.memory.u8[syms["g_options"]] = 0x08          # GHOST, to be erased
    for i, ch in enumerate(b"SAVE"):
        core.memory.u8[0x0E004800 + i] = ch
    to_line(5)
    tap("SELECT", settle=6)
    tap("A", settle=8)
    tap("UP", settle=4)
    tap("A", settle=8)
    tap("UP", settle=4)
    tap("A", settle=20)
    erased = (depth(), row(), life(1), [setting(i) for i in range(5)],
              core.memory.u8[syms["g_options"]], palette(0),
              bytes(core.memory.u8[0x0E004800 + i] for i in range(4)))
    tap("B", settle=20)                       # the page -> GAME SELECT
    tap("B", settle=40)                       # -> the title
    tap("SELECT", settle=40)                  # the table
    first = " ".join(tilemap_text(core, LEADER_FIRST_TY).split())
    if erased[:2] != (1, 0) or erased[2] or erased[3] != [4, 4, 0, 0, 0] or \
            erased[4] or erased[5] != black_backdrop or "17000" not in first or \
            erased[6] == b"SAVE":
        failures.append(f"SI y SI no lo borra todo (pagina y flecha {erased[:2]}, "
                        f"partidas {erased[2]}, ajustes {erased[3]}, opciones "
                        f"{erased[4]}, fondo {erased[5]:#06x}, tabla {first!r}, "
                        f"partida guardada {erased[6]!r})")
    else:
        print("  SI dos veces: la tabla a las quince del cartucho, las cifras a "
              "cero, los ajustes y las opciones de fabrica, las partidas "
              "guardadas fuera, la flecha arriba")
    del core, screen

    backgrounds_check(rom_path, failures)

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: OPTIONS en GAME SELECT: una pagina centrada con sus cajas, ajustes y "
          "cifras en la bateria, y el borrado solo por SELECT, con dos preguntas, de todo.")
    return 0


OBJ_PALETTE_ADDR = PALETTE_ADDR + 0x200
PAL_OBJ_CLEAR, PAL_OBJ_TITLE, PAL_OBJ_GAME = 4, 5, 9      # gba/port.h
TITLE_OBJ_TILE_BASE = 256
TITLE_FIREWORK_FIRST = 19
BG_INK_INDEX = 15                                         # gba/port.h
T_BRICK = 0x0F


def backgrounds_check(rom_path, failures):
    """THE BACKGROUNDS (bg_apply, ink_sprite_palettes, ink_tile; gba/video.c).

    THE TITLE IS AT NIGHT whatever is chosen: black behind it, and its
    fireworks the cartridge's. ON LIGHT GREY ELSEWHERE — GAME SELECT here —
    the backdrop is the grey, the line-clear puff is the backdrop, and the
    one-player drop points ($37, pale yellow) are inked darker. THE BLACK
    THE ART BORROWS FROM THE BACKDROP STAYS BLACK: the ink's entry is black
    in every bank, the art's tiles have it (the logo's among them), the
    line outside the braid at the screen's corner is black on the grey, and
    the pause's wall has a brick of its own with its grout in it. Back on
    black, the cartridge's; and ERASE DATA from the grey puts black back."""
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    row_at, _ = game_state_address(rom_path, "g_extras_row")
    menu_tiles = None

    def tap(*names, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, 4)
        core.set_keys(); run(core, settle)

    def obj(i):
        return core.memory.u16[OBJ_PALETTE_ADDR + i * 2]

    def bright(c):
        return (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)

    def tile_nibbles(t):
        at = CHARBLOCK_ADDR + t * 32
        return [(core.memory.u8[at + i // 2] >> (4 * (i & 1))) & 15 for i in range(64)]

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
        tap("B", settle=60)

    def title_look():
        """(the backdrop on the title, the fireworks' tile bases)."""
        fw = set()
        for _ in range(400):
            core.run_frame()
            for _, _, t in oam_visible(core, TITLE_FIREWORK_FIRST, 64):
                fw.add(t & ~255)
        return core.memory.u16[PALETTE_ADDR], fw

    def select_look():
        """On GAME SELECT: (backdrop, the puff is the backdrop, drop points'
        colour, the screen's top-left pixel), and back to the title."""
        nonlocal menu_tiles
        press_start(core); run(core, 30)
        backdrop = core.memory.u16[PALETTE_ADDR]
        clear = all(obj(PAL_OBJ_CLEAR * 16 + i) == backdrop for i in (1, 2, 3))
        points = obj((PAL_OBJ_GAME + 3) * 16 + 1)
        corner = pixels(screen)[0][0]
        menu_tiles = [core.memory.u16[SCREENBLOCK_ADDR + (y * 32 + x) * 2] & 0x3FF
                      for y in range(2, 7) for x in range(SCREEN_TW)]
        tap("B", settle=60)
        return backdrop, clear, points, corner

    def erase():
        """From the title: OPTIONS, SELECT down to ERASE DATA, YES and YES,
        and back to the title."""
        press_start(core); run(core, 20)
        tap("UP")
        tap("A", settle=10)
        for _ in range(10):
            if "ERASE" in tilemap_text(core, PAGE_TY + core.memory.u8[row_at] + 1):
                break
            tap("SELECT", settle=4)
        tap("A", settle=10)
        tap("UP", settle=4)
        tap("A", settle=10)
        tap("UP", settle=4)
        tap("A", settle=20)
        tap("B", settle=20)
        tap("B", settle=60)

    run(core, 40)
    press_start(core); run(core, 10)
    tap("L", "R")
    tap("B", settle=60)
    black_title = title_look()
    black = select_look()
    background(BG_LIGHT)
    light_title = title_look()
    light = select_look()
    inks = all(core.memory.u16[PALETTE_ADDR + (b * 16 + BG_INK_INDEX) * 2] == 0
               for b in range(16))
    logo_inked = any(BG_INK_INDEX in tile_nibbles(t) for t in set(menu_tiles) if t < 256)
    wall = check_layout(rom_path).get("wall_brick")
    brick, walled = tile_nibbles(T_BRICK), tile_nibbles(wall) if wall else []
    grout = walled and all((w == BG_INK_INDEX) if b == 0 else (w == b)
                           for b, w in zip(brick, walled)) and 0 in brick
    background(BG_BLACK)
    again = select_look()
    background(BG_LIGHT)
    erase()
    erased = select_look()
    if light_title[0] != black_title[0] or light_title[1] != {TITLE_OBJ_TILE_BASE}:
        failures.append(f"con LIGHT GREY el titulo no sigue de noche (fondo "
                        f"{light_title[0]:#06x}, fuegos {sorted(light_title[1])})")
    elif (light[0] & 0x1F) != 24 or not light[1]:
        failures.append(f"con LIGHT GREY GAME SELECT no lleva el gris claro, o la nube "
                        f"de la linea no es del color del fondo ({light[0]:#06x})")
    elif bright(light[2]) >= bright(black[2]):
        failures.append(f"sobre el gris claro los puntos de la caida no se oscurecen "
                        f"({black[2]:#06x} -> {light[2]:#06x})")
    elif not inks or not logo_inked:
        failures.append(f"la tinta no esta: negro en todos los bancos {inks}, en el "
                        f"logo {logo_inked}")
    elif light[3] != (0, 0, 0):
        failures.append(f"sobre el gris claro la esquina de la pantalla no es negra "
                        f"sino {light[3]}: la linea fuera de la greca coge el fondo")
    elif not grout:
        failures.append("el muro de la pausa no tiene su ladrillo con la junta en "
                        "la tinta")
    elif again[:3] != black[:3]:
        failures.append(f"de vuelta al negro no es lo del cartucho ({again[:3]} "
                        f"y no {black[:3]})")
    elif erased[:3] != black[:3]:
        failures.append(f"borrarlo todo desde el gris claro no vuelve al negro "
                        f"({erased[:3]})")
    else:
        print("  fondos: el titulo de noche con cualquiera; en el gris claro la "
              "nube de la linea del fondo, los puntos oscuros, la tinta negra en "
              "el logo y la esquina, y el muro con su junta; en negro, el "
              "cartucho, y ERASE DATA vuelve a el")
