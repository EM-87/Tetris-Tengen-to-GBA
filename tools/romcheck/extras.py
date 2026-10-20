"""
The title's EXTRAS (gba/extras.c): the box, its pages, the sound test, the
settings and what they do to the sound and the screen, the battery, and the
erase that only SELECT reaches.
"""
from .harness import (
    skip, KEYS, PALETTE_ADDR, DEMO_START_FRAME,
    REG_SOUND1CNT_H, REG_SOUND2CNT_L, REG_SOUND3CNT_H,
    SCREENBLOCK_ADDR, SCREENBLOCK_OFFSET_ADDR,
    build_has_protos, game_state_address, load, ring_chord_from_title, run,
    tilemap_text,
)

# The box: eighteen by eight, centred across, two rows below the middle
# (XB_W, XB_H, XB_TX, XB_TY in gba/extras.c). The heading on its second
# row, the lines under it, the erase on the last of them.
XB_W, XB_H = 18, 8
XB_TX, XB_TY = (30 - XB_W) // 2, (20 - XB_H) // 2 + 2
HEAD_ROW = XB_TY + 1
LINE_ROW = XB_TY + 2
ERASE_ROW = XB_TY + XB_H - 2
BANK_CREDIT = 9                 # PAL_MENU_BASE + 1, the credits' grey
SET_MUSIC, SET_EFFECTS, SET_BRIGHT = 0, 1, 2
SAVE_SETTINGS = 0x0E000000 + 0x406   # SAVE_SETTINGS_OFF, in the SRAM's window


def extras_check(rom_path):
    """L OR R ON THE TITLE, UNDER THE CHORD, PUTS UP THE EXTRAS.

    Before the chord a lone shoulder does nothing. After it: a box with
    PROTOTYPES (on a build that has them), SOUND TEST and SETTINGS, the
    arrow walking only those, and ERASE SCORES under them in the credits'
    grey; the title stands still behind it (no demo however long it stays
    up) and comes back whole when it goes. The SOUND TEST stands the title's
    tune down and plays the five sounds the cartridge carries and never
    asks for. SETTINGS sets the tunes' volume and the effects' apart — the
    engine's channels told apart by the voice writing them — and the
    screen's curve; all three kept on the battery and back after a power
    cycle. SELECT, and only SELECT, takes the erase's pages, which ask with
    NO chosen."""
    syms = {}
    for name in ("g_extras_open", "g_extras_page", "g_extras_row", "g_settings",
                 "g_title_frame", "g_demo", "g_lcd_gamma", "kLcdGamma"):
        addr, why = game_state_address(rom_path, name)
        if addr is None:
            skip(f"{why}")
            return 0
        syms[name] = addr
    failures = []
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    _ = screen
    io = core._native.memory.io

    def reg(addr):
        return io[(addr - 0x04000000) >> 1]

    def tap(*names, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, 4)
        core.set_keys(); run(core, settle)

    def is_open():
        return core.memory.u8[syms["g_extras_open"]] != 0

    def row_text(r):
        """A row of the box inside its frame (whose tiles read as text)."""
        return " ".join(tilemap_text(core, r, XB_TX + 1, XB_TX + XB_W - 1).split())

    def box_text():
        return [row_text(r) for r in range(HEAD_ROW, ERASE_ROW + 1)]

    def setting(i):
        return core.memory.u8[syms["g_settings"] + i]

    def box_cells():
        return [(core.memory.u16[a + (r * 32 + c) * 2])
                for a in (SCREENBLOCK_ADDR, SCREENBLOCK_OFFSET_ADDR)
                for r in range(XB_TY, XB_TY + XB_H)
                for c in range(XB_TX, XB_TX + XB_W)]

    run(core, 60)
    tap("R"); tap("L")
    if is_open():
        failures.append("antes del acorde un hombro suelto abre los EXTRAS")
    ring_chord_from_title(core)
    run(core, 30)
    title = box_cells()
    # A SHOULDER WITH SOMETHING ELSE JOINING IT IS NOT THE BOX: the sleep's
    # L+R+SELECT begins with L down a frame early, and that frame put the
    # box up and its SELECT took the sleep to the erase. It opens as a
    # shoulder is let go, alone (released_shoulder_alone). B joins it here,
    # the one button the title itself does nothing with.
    core.set_keys(KEYS["L"]); run(core, 3)
    core.set_keys(KEYS["L"], KEYS["B"]); run(core, 3)
    core.set_keys(); run(core, 20)
    if is_open() or box_cells() != title:
        failures.append("L con B sumandose abre la caja")
    tap("R")
    text = box_text()
    protos = build_has_protos(rom_path)
    want = (["PROTOTYPES"] if protos else []) + ["SOUND TEST", "SETTINGS"]
    lines = [t.lstrip("-> ") for t in text[1:1 + len(want)]]
    if not is_open() or text[0] != "EXTRAS" or lines != want:
        failures.append(f"R en el titulo no pone la caja de EXTRAS: {text}")
    erase_bank = core.memory.u16[SCREENBLOCK_ADDR + (ERASE_ROW * 32 + XB_TX + 3) * 2] >> 12
    if row_text(ERASE_ROW) != "ERASE SCORES" or erase_bank != BANK_CREDIT:
        failures.append(f"ERASE SCORES no esta en la ultima fila en el gris de "
                        f"los creditos ({row_text(ERASE_ROW)!r}, banco {erase_bank})")
    # THE ARROW WALKS THE CHOICES AND NEVER LANDS ON THE ERASE.
    rows_seen = set()
    for _ in range(2 * len(want) + 1):
        tap("DOWN", settle=6)
        rows_seen.add(core.memory.u8[syms["g_extras_row"]])
        if "->" in row_text(ERASE_ROW):
            break
    if "->" in row_text(ERASE_ROW) or rows_seen != set(range(len(want))):
        failures.append(f"la flecha llega a ERASE SCORES o no recorre las "
                        f"opciones ({sorted(rows_seen)})")
    # The cathedral's sprites have a hole the box's size (title_window_hole).
    win0h = reg(0x04000040)
    if win0h != ((XB_TX * 8) << 8 | (XB_TX + XB_W) * 8) or reg(0x04000048) != 0x3F2F:
        failures.append(f"la caja no hace hueco a los sprites del titulo "
                        f"(WIN0H {win0h:#06x}, WININ {reg(0x04000048):#06x})")
    # ...and the title stands still behind it: no demo.
    clock = core.memory.u16[syms["g_title_frame"]]
    run(core, DEMO_START_FRAME + 120)
    if core.memory.u8[syms["g_demo"]] or not is_open() or \
            core.memory.u16[syms["g_title_frame"]] != clock:
        failures.append("con la caja arriba el titulo sigue su reloj y "
                        "arranca la demo")
    if not failures:
        print(f"  R pone EXTRAS: {', '.join(want)}, y ERASE SCORES en gris, "
              f"fuera del alcance de la flecha; el titulo se para detras")

    # ...AND B TAKES IT DOWN, THE TITLE WHOLE AGAIN.
    tap("B", settle=20)
    if is_open() or box_cells() != title:
        failures.append("B no quita la caja, o el titulo no vuelve entero")
    elif reg(0x04000048) != 0x003F:
        failures.append("sin la caja el hueco de los sprites sigue ahi")
    else:
        print("  B la quita y el titulo vuelve entero, sus sprites sin hueco")

    # THE SOUND TEST. The title's tune stands down on the page; each of the
    # five plays (two looked at: the jingle, a tune on the pulses, and the
    # swipe, noise); B, and the title's tune is back.
    def to_page(n):
        tap("R")
        for _ in range(n):
            tap("DOWN", settle=6)
        tap("A", settle=20)

    def loudest(frames, which):
        top = 0
        for _ in range(frames):
            core.run_frame()
            if which == "p1":
                top = max(top, reg(REG_SOUND1CNT_H) >> 12)
            elif which == "p2":
                top = max(top, reg(REG_SOUND2CNT_L) >> 12)
            elif which == "tri":
                v = reg(REG_SOUND3CNT_H)
                top = max(top, 4 if v & 0x8000 else (0, 4, 2, 1)[(v >> 13) & 3])
            elif which == "noise":
                top = max(top, reg(0x04000078) >> 12)
        return top

    # The title's tune from its first bar — it plays once and stops, and
    # the box has been up a while — by way of GAME SELECT and back.
    tap("START", settle=10)
    tap("B", settle=10)
    playing = loudest(90, "p1")
    to_page(1 if protos else 0)
    page = box_text()
    names = [t.lstrip("-> ") for t in page[1:6]]
    quiet = loudest(90, "p1") + loudest(1, "tri") + loudest(1, "noise")
    tap("DOWN", settle=0)
    tap("A", settle=0)
    swipe = loudest(20, "noise")
    tap("UP", settle=0)
    tap("A", settle=0)
    jingle = loudest(40, "p1")
    if page[0] != "SOUND TEST" or names != ["0C LEVEL UP", "0F SWIPE", "10 CHIRP",
                                            "11 DING", "12 ALARM"]:
        failures.append(f"la prueba de sonido no lista los cinco: {page}")
    elif not playing or quiet or not swipe or not jingle:
        failures.append(f"en la prueba de sonido el tema del titulo ({playing} antes) "
                        f"sigue ({quiet}) o no suenan SWIPE ({swipe}) y LEVEL UP "
                        f"({jingle})")
    else:
        print("  SOUND TEST: los cinco por su numero; el tema se calla, y "
              "suenan el barrido y la fanfarria")
    tap("B", settle=10)
    tap("B", settle=10)
    back = loudest(90, "p1")
    if back == 0 or is_open():
        failures.append("al salir de la prueba de sonido el tema del titulo no vuelve")

    # SETTINGS: the tunes at OFF and the effects at full — the tune silent
    # and the cursor's tick heard — then the other way round. On the title
    # the theme is pulse 1, the triangle and the noise, and the tick pulse 2.
    to_page(2 if protos else 1)
    if box_text()[0] != "SETTINGS" or "RUMBLE" in " ".join(box_text()):
        failures.append(f"SETTINGS no es su pagina, o enseña RUMBLE en una GBA "
                        f"sin Game Boy Player: {box_text()}")
    for _ in range(4):
        tap("LEFT", settle=6)
    run(core, 30)
    music_off = (loudest(60, "p1"), loudest(1, "tri"))
    tap("DOWN", settle=0)
    tick_on = loudest(10, "p2")
    tap("UP", settle=0)
    if setting(SET_MUSIC) != 0 or any(music_off) or not tick_on:
        failures.append(f"MUSIC OFF no calla el tema ({music_off}) o calla el "
                        f"tic ({tick_on}); ajuste {setting(SET_MUSIC)}")
    for _ in range(4):
        tap("RIGHT", settle=6)
    tap("DOWN", settle=6)
    for _ in range(4):
        tap("LEFT", settle=6)
    run(core, 30)
    music_on = loudest(90, "p1")
    tap("UP", settle=0)
    tick_off = loudest(10, "p2")
    tap("DOWN", settle=0)
    if setting(SET_EFFECTS) != 0 or not music_on or tick_off:
        failures.append(f"EFFECTS OFF no calla el tic ({tick_off}) o calla el "
                        f"tema ({music_on}); ajuste {setting(SET_EFFECTS)}")
    if not failures:
        print("  MUSIC OFF calla el tema y deja el tic; EFFECTS OFF calla el "
              "tic y deja el tema: cada canal a su volumen")

    # ...the effects at half and the screen's curve at LOW, and both there
    # after the power goes off; the palette through the brighter curve.
    tap("RIGHT", settle=6)
    tap("RIGHT", settle=6)
    pal_before = core.memory.u16[PALETTE_ADDR + (4 * 16 + 3) * 2]
    tap("DOWN", settle=6)
    tap("RIGHT", settle=20)
    pal_after = core.memory.u16[PALETTE_ADDR + (4 * 16 + 3) * 2]
    curve = (core.memory.u32[syms["g_lcd_gamma"]] - syms["kLcdGamma"]) // 32
    lifted = all(((pal_after >> s) & 31) >= ((pal_before >> s) & 31)
                 for s in (0, 5, 10)) and pal_after != pal_before
    if setting(SET_BRIGHT) != 1 or curve != 1 or not lifted:
        failures.append(f"BRIGHT LOW no pone la segunda curva o no aclara el "
                        f"titulo (curva {curve}, {pal_before:#06x} -> {pal_after:#06x})")
    saved = [core.memory.u8[SAVE_SETTINGS + i] for i in range(4)]
    core.reset()
    run(core, 60)
    after = [setting(i) for i in range(4)]
    curve = (core.memory.u32[syms["g_lcd_gamma"]] - syms["kLcdGamma"]) // 32
    if after != [4, 2, 1, 0] or saved != after or curve != 1:
        failures.append(f"los ajustes no sobreviven a apagar: {saved} -> {after}, "
                        f"curva {curve}")
    else:
        print("  EFFECTS 50% y BRIGHT LOW, en la bateria y de vuelta al "
              "encender, la paleta por la curva mas clara")

    # THE ERASE: SELECT on the box, the power-on's own pages, NO chosen; B
    # is NO, and the title is back with the box gone.
    ring_chord_from_title(core)
    run(core, 20)
    tap("R")
    tap("A", settle=6)                  # into the first page, which is not it
    tap("SELECT", settle=6)
    stray = is_open() and core.memory.u8[syms["g_extras_page"]] != 0
    tap("B", settle=10)
    tap("SELECT", settle=20)
    asked = " ".join(" ".join(tilemap_text(core, r).split()) for r in range(8, 17))
    if not stray or "ERASE ALL HIGH SCORES?" not in asked or "> NO" not in asked:
        failures.append(f"SELECT en la caja no lleva al borrado con NO elegido "
                        f"(fuera de su pagina: {stray}): {asked[:80]!r}")
    tap("B", settle=40)
    if is_open() or "ERASE" in " ".join(tilemap_text(core, r) for r in range(8, 17)):
        failures.append("decir que no al borrado no vuelve al titulo")
    elif not failures:
        print("  SELECT, y solo SELECT y en la primera pagina, lleva al "
              "borrado, que pregunta con NO elegido; B vuelve al titulo")

    # ...AND THE HAND-ENTERED TUNES ARE TUNES: MUSIC OFF, and KOROBEINIKI's
    # audition on LEVEL SETTINGS is silent on all three of its channels.
    tap("R")
    for _ in range(2 if protos else 1):
        tap("DOWN", settle=6)
    tap("A", settle=10)
    for _ in range(4):
        tap("LEFT", settle=6)
    tap("B"); tap("B", settle=20)
    tap("START", settle=20)                 # GAME SELECT
    tap("START", settle=20)                 # LEVEL SETTINGS
    tap("DOWN", settle=8); tap("DOWN", settle=8)
    for _ in range(5):                      # NO MUSIC .. KOROBEINIKI
        tap("RIGHT", settle=8)
    run(core, 30)
    hand = (loudest(120, "p1"), loudest(1, "p2"), loudest(1, "tri"))
    if setting(SET_MUSIC) != 0 or any(hand):
        failures.append(f"con MUSIC OFF KOROBEINIKI se sigue oyendo {hand}")
    else:
        print("  MUSIC OFF calla tambien las canciones a mano (KOROBEINIKI)")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: los EXTRAS del titulo: prototipos, prueba de sonido, ajustes "
          "que se guardan, y el borrado solo con SELECT.")
    return 0
