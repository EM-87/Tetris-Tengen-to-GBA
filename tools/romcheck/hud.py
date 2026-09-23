"""
The panels either side of the board: the four HUDs, NEXT, the
braid, the histogram and the level-up show.
"""
from .harness import (
    BRAID_BLUE, CELL_BLOCK, CELL_WALL, HUD_LABEL_TILE_BASE,
    HUD_LABEL_TILE_END, KEYS, OAM_ADDR, PAL_NEXT_BANK,
    PAL_PIECE_BANK, PLAQUE_CORNERS, SCREENBLOCK_ADDR, SCREENBLOCK_OFFSET_ADDR,
    SCREEN_H, STATS_COLUMNS, STATS_ICON_ROWS, STATS_RUN_STEPS,
    STATS_SCREENBLOCK, TENGEN_PF_HEIGHT, TENGEN_PF_WIDTH, TILE,
    fill_rows, game_offsets, game_state_address, load,
    map_row_text, pixels, press_start, run,
    start_game, tilemap_text,
)


def panel_check(rom_path):
    """The left panel's five compartments: one shelf apart, and square in them.

    The panel is the coop screen's now — a tall cell at the top for NEXT and
    four short ones under it for the counters, ruled off with the cartridge's
    own blue LEDGE instead of the 1P header grid's grey line. So there are
    three things to measure and none of them is a constant in this file:

      * the four counters open with the SAME air under their shelf. They ride
        a background scrolled two pixels down (SCREENBLOCK_PANEL) precisely so
        that SCORE, whose shelf is the braid, matches the other three;
      * NEXT is CENTRED in the big cell, which is why it alone is drawn on the
        main layer — two pixels of panel offset is the difference between
        centred and five pixels low;
      * and the shelves are the rope's blue, not the grid's grey.
    """
    core, screen = load(rom_path)
    start_game(core)
    run(core, 40)
    rows = pixels(screen)

    # The left box's interior, in pixels: eight tiles from column 2.
    def ink(y):
        return sum(1 for p in rows[y][16:80] if p != (0, 0, 0))

    def content_ink(y):
        """Lit pixels in the panel's CONTENT columns, clear of the braid."""
        return sum(1 for p in rows[y][0:56] if p != (0, 0, 0))

    def band(y):
        """A shelf: the content columns lit wall to wall, with no gaps."""
        return content_ink(y) == 56

    failures = []

    # The shelves, off the screen rather than off a row number. From y=16,
    # which is under the braid's own bottom edge — that fills the width too.
    shelves = [y for y in range(16, 160) if band(y)]
    runs = []
    for y in shelves:
        if runs and y == runs[-1][-1] + 1:
            runs[-1].append(y)
        else:
            runs.append([y])
    if len(runs) != 4:
        failures.append(f"se esperaban 4 baldas en el cajon izquierdo, se ven "
                         f"{len(runs)}: {[r[0] for r in runs]}")
    else:
        colours = set().union(*({rows[y][x] for y in r for x in range(0, 56)}
                                 for r in runs))
        if BRAID_BLUE not in colours:
            failures.append(f"las baldas no son azules como la greca: "
                             f"{sorted(colours)}")
        else:
            print(f"  cuatro baldas azules, en y={[r[0] for r in runs]}")

    # Each counter's headroom: from the bottom of its shelf to its label's ink.
    floor = min(ink(y) for y in range(60, 158))
    gaps = []
    for r in runs:
        y = r[-1] + 1
        n = 0
        while ink(y + n) <= floor:
            n += 1
        gaps.append((r[-1] + 1, n))

    if len(gaps) == 4 and len({n for _, n in gaps}) != 1:
        failures.append("los contadores no tienen el mismo hueco bajo su balda: "
                         + ", ".join(f"y={y} -> {n}px" for y, n in gaps))
    elif len(gaps) == 4:
        print(f"  los cuatro contadores abren con {gaps[0][1]} pixeles bajo su "
               f"balda (en y={', '.join(str(y) for y, _ in gaps)})")

    # NEXT, centred in the big cell: from the braid's last row to the first
    # shelf, with the label and the preview somewhere in between.
    if runs:
        # The cell runs from under the braid to the shelf's own tile row.
        top, bot = 16, (runs[0][0] // TILE) * TILE
        lit = [y for y in range(top, bot) if content_ink(y)]
        if not lit:
            failures.append("la celda grande del cajon izquierdo esta vacia: "
                             "NEXT no se dibuja donde debe")
        else:
            above, below = lit[0] - top, bot - 1 - lit[-1]
            if abs(above - below) > 1:
                failures.append(f"NEXT no esta centrado en su celda: {above}px "
                                 f"por arriba, {below}px por abajo")
            else:
                print(f"  NEXT centrado en la celda grande: {above}px arriba, "
                       f"{below}px abajo")

    # ...AND THE TWO PANELS ARE MIRRORS. Both are inverted Ls now — rope along
    # the top and down the side facing the board, open at the bottom — so the
    # right one's rope has to run to the screen's last line exactly as the
    # left one's does, and the histogram standing in it has to reach the
    # bottom without anything closing it off.
    core.set_keys(KEYS["SELECT"])
    run(core, 5)
    core.set_keys()
    run(core, 40)
    rows = pixels(screen)

    def rope(x0, x1):
        """The lowest scanline the rope is drawn on in those columns.

        Nearly all of them, not all: the braid's outermost pixel column is
        transparent and shows the backdrop through, on both sides.
        """
        return max((y for y in range(100, SCREEN_H)
                    if sum(1 for x in range(x0, x1)
                           if rows[y][x] != (0, 0, 0)) >= (x1 - x0) - 2),
                   default=None)

    left, right = rope(64, 80), rope(160, 176)
    if left != SCREEN_H - 1 or right != SCREEN_H - 1:
        failures.append(f"la greca no llega al borde inferior en los dos "
                         f"cajones: izq acaba en y={left}, der en y={right}")
    else:
        print("  las dos grecas bajan hasta la ultima linea: los cajones son "
               "L invertidas, espejo la una de la otra")

    icons = max((y for y in range(100, SCREEN_H)
                 if any(rows[y][x] != (0, 0, 0) for x in range(176, 240))),
                default=None)
    if icons is None:
        failures.append("el cajon derecho no dibuja las estadisticas")
    elif icons < SCREEN_H - 4:
        failures.append(f"las estadisticas acaban en y={icons} y el cajon "
                         f"llega a {SCREEN_H - 1}: sobra panel debajo")
    else:
        print(f"  las estadisticas llegan a y={icons}, al pie del cajon")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: SCORE respira igual que LINES, LEVEL y HIGH, y las stats no "
           "pisan la greca.")
    return 0


def palette_bank(core, n):
    return tuple(core.memory.u16[0x05000000 + (n * 16 + i) * 2] for i in (1, 2, 3))


def next_palette_check(rom_path):
    """The preview must be painted in the NEXT piece's colours.

    Both were drawn out of bank 12, which setPiecePalette loads with the piece
    IN PLAY, so the preview wore the falling piece's colours and changed under
    you every time one locked. The bug is invisible whenever the two pieces
    happen to share a palette, which is why this walks a whole game rather
    than sampling once.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    run(core, 20)

    # What each piece id's colours ARE, learned from the bank the falling
    # piece owns — so this never has to hard-code the table.
    known = {}
    failures = []
    pairs = set()
    was = None
    for _ in range(2500):
        core.set_keys(KEYS["DOWN"])
        core.run_frame()
        cur = core.memory.u8[base + off["current"]]
        nxt = core.memory.u8[base + off["next"]]
        stable, was = (cur, nxt) == was, (cur, nxt)
        # mGBA hands the frame back between the port's step and its draw, so
        # on the frame a piece locks the state has moved on and the palettes
        # have not. Only a pair that survived a second frame is settled.
        if not stable:
            continue
        if not (1 <= cur <= 7) or not (1 <= nxt <= 7):
            continue
        known[cur] = palette_bank(core, PAL_PIECE_BANK)
        pairs.add((cur, nxt))
        want = known.get(nxt)
        if want is None:
            continue        # this piece has not fallen yet; nothing to compare
        got = palette_bank(core, PAL_NEXT_BANK)
        if got != want:
            failures.append(f"con {cur} cayendo y {nxt} en NEXT, la vista previa "
                             f"usa {got} y no {want}")
            break
    core.set_keys()

    mixed = sum(1 for c, n in pairs if known.get(c) != known.get(n))
    if not mixed:
        failures.append("no se vio ninguna pareja de piezas con paletas "
                         "distintas: la prueba no prueba nada")
    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print(f"  {len(pairs)} parejas pieza/siguiente, {mixed} de ellas con "
           "paletas distintas")
    print("OK: NEXT se pinta con los colores de la pieza que viene.")
    return 0


def stats_show_check(rom_path):
    """HUD STATS'S LEVEL-UP SHOW, THROUGH THE LEFT BOX, IN EVERY MODE.

    The right box is the histogram, and the histogram is what HUD STATS is
    for, so the troupe comes on through the left one: its counters go for the
    length of the show, the cartridge's solo column of six walks on from the
    screen's edge onto its four ledges plus one drawn in the tall compartment
    and the screen's own bottom edge, and the resident cossack over the
    histogram dances it in place. Run in the three modes whose HUD STATS is
    reachable without a cable: 1 PLAYER, VERSUS COMPUTER (a race; the cast
    counts both boards) and WITH COMPUTER (coop's screen, whose pairs would
    run down both panels — here the solo column and its cap of six).
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    IDLE_OAM_BASE = 124           # gba/port.h
    failures = []
    # (name, GAME SELECT entries down, tetrises planted per board,
    #  boards whose tally counts, left panel's width in pixels)
    for name, down, tetrises, boards, panel_px in (
            ("1 PLAYER", 0, 1, 1, 64),
            ("VERSUS COMPUTER", 3, 1, 2, 64),
            ("WITH COMPUTER", 4, 3, 2, 56)):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        _ = screen

        def tap(key):
            core.set_keys(KEYS[key]); run(core, 4)
            core.set_keys(); run(core, 12)

        run(core, 8)
        press_start(core); run(core, 20)
        # VERSUS and WITH have STATS only behind the chord (hud_set): the
        # cartridge's screens for them carry no histogram.
        if down:
            core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
            core.set_keys(); run(core, 24)
        for _ in range(down):
            tap("DOWN")
        press_start(core); run(core, 12)
        press_start(core); run(core, 60)
        tap("SELECT"); run(core, 20)      # every mode's second HUD is STATS
        for who in (0, 1):
            at = base + off["stride"] * who
            core.memory.u8[at + off["lines"]] = 28
            core.memory.u8[at + off["lines"] + 1] = 0
            core.memory.u8[at + off["counts"] + 3] = tetrises
        fill_rows(core, base + off["field"], (19, 18))
        level = core.memory.u8[base + off["level"]]
        for _ in range(60):
            core.set_keys(KEYS["DOWN"]); run(core, 8)
            core.set_keys(); run(core, 2)
            if core.memory.u8[base + off["level"]] != level:
                break
        else:
            failures.append(f"{name}: el nivel no subio")
            continue
        # Hands off the pad: a press fast-forwards the show.
        run(core, 400)

        want = min(6, 1 + 2 * tetrises * boards)
        seen = []
        for d in range(8):
            at = OAM_ADDR + d * 4 * 8
            if core.memory.u16[at] & 0x0200:
                continue
            y = core.memory.u16[at] & 0xFF
            x = core.memory.u16[at + 2] & 0x1FF
            seen.append((x - 512 if x >= 256 else x, y))
        at = OAM_ADDR + IDLE_OAM_BASE * 8
        resident = None
        if not core.memory.u16[at] & 0x0200:
            resident = core.memory.u16[at + 2] & 0x1FF
        left = " ".join(tilemap_text(core, ty, 0, panel_px // 8)
                        for ty in range(2, 20))

        if len(seen) != want:
            failures.append(f"{name}: salen {len(seen)} cosacos, el reparto "
                             f"es {want}")
        elif any(x < 0 or x + 16 > panel_px for x, _y in seen):
            failures.append(f"{name}: un cosaco fuera del cajon izquierdo: "
                             f"{seen}")
        elif sorted(y for _x, y in seen) != sorted(144 - 24 * d
                                                    for d in range(want)):
            failures.append(f"{name}: los cosacos no estan en las baldas: "
                             f"{seen}")
        elif any(w in left for w in ("SCORE", "LINES", "LEVEL", "NEXT")):
            failures.append(f"{name}: el cajon izquierdo conserva sus "
                             f"contadores durante el baile: {left!r}")
        elif resident is None or resident < 160:
            failures.append(f"{name}: el cosaco del histograma no baila en "
                             f"su cajon (x={resident})")
        else:
            print(f"  {name}: {want} cosacos por el cajon izquierdo, sin "
                  f"contadores, y el del histograma baila en el suyo")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: en HUD Stats el baile sale por el cajon izquierdo, en los tres "
          "modos.")
    return 0


def versus_hud_check(rom_path):
    """THE RACE'S THIRD HUD, and what a paused race shows.

    A race is two boards and the port only ever drew one of them: the other
    player was a single number in the bottom cell of YOUR panel. SELECT now
    walks a third HUD that gives them the right box — their NEXT, score,
    lines and LEVEL, laid out like coop's — and takes the RIVAL cell back out
    of the left one, which goes back to the 1P panel's HIGH.

    And under the chord, PAUSING a race shows the OTHER board. Against the
    computer that is the only way to satisfy yourself it is really playing;
    in any race it is what stops a pause being a free think about your own
    stack.
    """
    off = game_offsets(rom_path)
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0

    failures = []
    LEFT, RIGHT = (0, 10), (20, 30)
    PF = TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH

    def start(cheat):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        _ = screen
        run(core, 8)
        press_start(core); run(core, 20)
        if cheat:
            core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
            core.set_keys(); run(core, 24)
        for _ in range(3):                        # ...down to VERSUS COMPUTER
            core.set_keys(KEYS["DOWN"]); run(core, 4)
            core.set_keys(); run(core, 10)
        press_start(core); run(core, 12)
        press_start(core); run(core, 60)
        # Two scores nothing else can be confused with, planted on both
        # players. Below the table's last entry so a game over does not end
        # up typing initials.
        for who, score in ((0, 1234), (1, 2345)):
            at = base + off["score"] + who * off["stride"]
            for k in range(4):
                core.memory.u8[at + k] = (score >> (8 * k)) & 0xFF
        run(core, 40)
        # WHAT THEY ACTUALLY READ, not what was planted: a piece landing is
        # worth points, so the figure on the panel drifts a little above the
        # one written in.
        now = []
        for who in range(2):
            at = base + off["score"] + who * off["stride"]
            now.append(sum(core.memory.u8[at + k] << (8 * k) for k in range(4)))
        return core, screen, now

    def box(core, cols):
        return " ".join(t for t in (tilemap_text(core, ty, *cols)
                                     for ty in range(8, 20)) if t)

    # WITHOUT THE CHORD A RACE HAS ONE HUD: the cartridge's race screen has no
    # histogram, so HUD STATS is behind the chord, and SELECT changes nothing.
    core, screen, _score = start(cheat=False)
    _ = screen
    before = (box(core, LEFT), box(core, RIGHT))
    core.set_keys(KEYS["SELECT"]); run(core, 4)
    core.set_keys(); run(core, 30)
    after = (box(core, LEFT), box(core, RIGHT))
    if not any(ch.isdigit() for ch in after[1]):
        failures.append(f"sin el acorde SELECT saca HUD STATS en una carrera: "
                        f"{after[1]!r}")
    elif "HIGH" in before[0]:
        failures.append(f"el HUD VERSUS lleva HIGH, y la pantalla de carrera "
                        f"del cartucho no: {before[0]!r}")
    else:
        print("  sin el acorde una carrera tiene un solo HUD, sin HIGH")
    del core, screen

    core, screen, _score = start(cheat=True)
    _ = screen

    def score_now(who):
        at = base + off["score"] + who * off["stride"]
        return sum(core.memory.u8[at + k] << (8 * k) for k in range(4))

    # TWO HUDS IN A RACE, AND IT OPENS ON HUD VERSUS. Read on the frame each
    # is looked at: the board is still being played between one SELECT and
    # the next, so the figure planted at the start is not the figure on the
    # panel a press later.
    seen = []
    cossack_bank = []

    IDLE_OAM_BASE = 124     # gba/port.h

    def idle_bank(core):
        # attr2's palette bits, or None when the figure is hidden.
        at = OAM_ADDR + IDLE_OAM_BASE * 8
        if core.memory.u16[at] & 0x0200:
            return None
        return core.memory.u16[at + 4] >> 12

    for step in range(3):
        if step:
            core.set_keys(KEYS["SELECT"]); run(core, 4)
            core.set_keys(); run(core, 60)
        seen.append((box(core, LEFT), box(core, RIGHT),
                      f"{score_now(0)}", f"{score_now(1)}"))
        cossack_bank.append(idle_bank(core))

    # [0] is the default and it is HUD VERSUS: the rival has the right box,
    # and the left one has no RIVAL cell and no HIGH either — the cartridge's
    # race screen carries neither.
    left, right, MINE, THEIRS = seen[0]
    if THEIRS not in right:
        failures.append(f"el HUD por defecto no trae la puntuacion del rival "
                         f"al cajon derecho: {right!r}")
    elif "RIVAL" in left:
        failures.append(f"el cajon izquierdo conserva su celda RIVAL teniendo "
                         f"el panel al lado: {left!r}")
    elif "RIVAL" in right:
        failures.append(f"el panel del rival sigue rotulado RIVAL en su ultima "
                         f"celda: {right!r}")
    elif "HIGH" in left:
        failures.append(f"el cajon izquierdo lleva HIGH en una carrera: {left!r}")
    elif MINE not in left:
        failures.append(f"el cajon izquierdo dejo de llevar lo tuyo: {left!r}")
    else:
        print("  una carrera abre en HUD VERSUS: el rival tiene el cajon "
               "derecho y el izquierdo no lleva ni RIVAL ni HIGH")

    # TWO COSSACKS IN THE SAME COMPARTMENT, one per HUD, and never the same
    # colours: the rival's is not yours in another hat.
    if None in cossack_bank[:2]:
        failures.append(f"falta un cosaco: bancos {cossack_bank[:2]}")
    elif cossack_bank[0] == cossack_bank[1]:
        failures.append(f"el cosaco del rival lleva los colores del tuyo "
                         f"(banco {cossack_bank[0]})")
    else:
        print(f"  el cosaco del rival va en el banco {cossack_bank[0]}, "
              f"el tuyo en el {cossack_bank[1]}")

    # [1] is HUD STATS — under the chord — which is where the RIVAL cell
    # lives now.
    left, right, MINE, THEIRS = seen[1]
    if "RIVAL" not in left:
        failures.append(f"el segundo HUD no devuelve la celda RIVAL al cajon "
                         f"izquierdo: {left!r}")
    elif any(ch.isdigit() for ch in right):
        failures.append(f"el segundo HUD deberia ser el histograma, no un "
                         f"panel con numeros: {right!r}")
    else:
        print("  y el segundo es HUD STATS, con RIVAL de vuelta a la izquierda")

    # ...and there are only two: HUD BANNER is not one of a race's.
    if seen[2][:2] != seen[0][:2] and "RIVAL" in seen[2][0]:
        failures.append("una carrera ofrece mas de dos HUDs")
    elif not any(ch.isdigit() for ch in seen[2][1]):
        failures.append("SELECT no vuelve al primer HUD: una carrera tiene dos")
    else:
        print("  y son dos: SELECT vuelve al primero, sin pasar por el cartel")

    # AND THE PAUSE SHOWS THE OTHER BOARD, under the chord and only there.
    # Two stacks nothing can confuse: yours on the left of your field, theirs
    # on the right of theirs.
    def planted(core):
        addr = base + off["field"]
        for board in (0, 1):
            for r in range(TENGEN_PF_HEIGHT):
                for c in range(TENGEN_PF_WIDTH):
                    core.memory.u8[addr + board * PF + r * TENGEN_PF_WIDTH + c] = (
                        CELL_WALL if c in (0, TENGEN_PF_WIDTH - 1) else 0)
        for c in range(1, 5):
            core.memory.u8[addr + 19 * TENGEN_PF_WIDTH + c] = 1
        for c in range(6, 10):
            core.memory.u8[addr + PF + 19 * TENGEN_PF_WIDTH + c] = 1
            core.memory.u8[addr + PF + 18 * TENGEN_PF_WIDTH + c] = 1
        run(core, 4)

    # ...AND NEXT GOES WITH IT. Two previews nothing can confuse: an I, one
    # row deep, for you, and an O, two rows deep, for them. The preview is
    # read as the number of rows of the NEXT cell that carry a block tile.
    TT_I_ID, TT_O_ID = 1, 3

    def next_rows(core):
        rows = set()
        for ty in range(1, 8):
            for tx in range(0, 10):
                o = (ty * 32 + tx) * 2
                for sb in (SCREENBLOCK_ADDR, SCREENBLOCK_OFFSET_ADDR):
                    if 1 <= (core.memory.u16[sb + o] & 0x3FF) <= 15:
                        rows.add(ty)
        return len(rows)

    # ...AND THE COLOURS GO WITH IT: the level's palette for the settled
    # blocks (bg bank 0) and the falling piece's own (bank 12) are the board
    # on screen's, not yours. Two levels and two pieces nothing can confuse.
    PAL = 0x05000000

    def colours(bank):
        return [core.memory.u16[PAL + (bank * 16 + i) * 2] for i in (1, 2, 3)]

    palettes = {}
    for cheat, want in ((False, "propio"), (True, "del rival")):
        core, screen, _score = start(cheat)
        _ = screen
        planted(core)
        core.memory.u8[base + off["next"]] = TT_I_ID
        core.memory.u8[base + off["next"] + off["stride"]] = TT_O_ID
        core.memory.u8[base + off["level"]] = 0
        core.memory.u8[base + off["level"] + off["stride"]] = 3
        core.memory.u8[base + off["current"]] = TT_I_ID
        core.memory.u8[base + off["current"] + off["stride"]] = TT_O_ID
        run(core, 2)
        before = (colours(0), colours(12))
        press_start(core); run(core, 30)          # pause
        palettes[cheat] = (before, (colours(0), colours(12)))
        rows = [map_row_text(core, r) for r in (18, 19)]
        mine = rows[1].startswith("####") and "." in rows[0]
        theirs = rows[0].count("#") == 4 and rows[1].count("#") == 4
        got = "del rival" if theirs else ("propio" if mine else f"ni uno {rows!r}")
        if got != want:
            failures.append(f"con acorde={int(cheat)} la pausa muestra el "
                             f"tablero {got}, deberia ser el {want}")
        depth = next_rows(core)
        got_next = {1: "propio", 2: "del rival"}.get(depth, f"{depth} filas")
        if got_next != want:
            failures.append(f"con acorde={int(cheat)} el NEXT de la pausa es "
                             f"el {got_next}, deberia ser el {want}")
        # ...AND THE PANELS SWAP WITH IT: the left box is the board on screen
        # and the right box (HUD VERSUS) the other player, cossack included.
        scores = []
        for who in range(2):
            at = base + off["score"] + who * off["stride"]
            scores.append(str(sum(core.memory.u8[at + k] << (8 * k)
                                  for k in range(4))))
        left_box, right_box = box(core, LEFT), box(core, RIGHT)
        shown, other = (1, 0) if cheat else (0, 1)
        if scores[shown] not in left_box or scores[other] not in right_box:
            failures.append(f"con acorde={int(cheat)} los paneles no son del "
                            f"tablero {want}: {left_box!r} / {right_box!r} "
                            f"(tuyo {scores[0]}, suyo {scores[1]})")
        bank = idle_bank(core)
        want_bank = 0 if cheat else 2      # yours, or the rival's green
        if bank != want_bank:
            failures.append(f"con acorde={int(cheat)} el cosaco del cajon "
                            f"derecho va en el banco {bank}, deberia ir en "
                            f"el {want_bank}")
    (mine0, mine_paused), (_mine1, theirs_paused) = palettes[False], palettes[True]
    if mine_paused != mine0:
        failures.append("sin acorde la pausa cambia los colores del tablero")
    elif theirs_paused[0] == mine0[0]:
        failures.append("bajo el acorde el tablero del rival se pinta con la "
                         "paleta de tu nivel")
    elif theirs_paused[1] == mine0[1]:
        failures.append("bajo el acorde la pieza del rival cae con los colores "
                         "de la tuya")
    if not failures:
        print("  y bajo el acorde la pausa cambia tu tablero por el del rival, "
              "con su NEXT, sus colores, su marcador a la izquierda y tu "
              "cosaco a la derecha")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: la carrera tiene un HUD para el rival, y su pausa ensena el "
           "tablero de enfrente.")
    return 0


def coop_hud_check(rom_path):
    """THE COOP HUD: a panel each, and the board's totals under the chord.

    A shared board has two of everything the core keeps — score, lines, the
    NEXT piece — and the coop screen used to print one of each and give the
    right-hand panel's tall compartment to the idle cossack. Now it is your
    panel on the left and theirs on the right, four cells a side:

        NEXT / SCORE / LINES / LEVEL  |  NEXT / SCORE / LINES / HIGH

    with the last cell of each holding T.LINES and T.SCORE once the cheats
    are uncovered, and standing empty until they are. Against the COMPUTER
    SELECT swaps to a second HUD that hides the partner's panel and turns
    your two counters into the board's totals; over a cable it must not (see
    run_link.py, which checks that half).
    """
    failures = []
    base, why = game_state_address(rom_path)
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    off = game_offsets(rom_path)
    LEFT, RIGHT = (0, 7), (30 - 7, 30)

    def start(cheat):
        core, screen = load(rom_path)
        _ = screen
        run(core, 8)
        press_start(core); run(core, 20)          # title -> GAME SELECT
        if cheat:
            core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
            core.set_keys(); run(core, 24)
        for _ in range(4):                        # ...down to WITH COMPUTER
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 8)
        press_start(core); run(core, 12)
        press_start(core); run(core, 60)
        for who, score, lines in ((0, 12345, 27), (1, 6789, 14)):
            at = base + off["score"] + who * off["stride"]
            for k in range(4): core.memory.u8[at + k] = (score >> (8 * k)) & 0xFF
            at = base + off["lines"] + who * off["stride"]
            for k in range(4): core.memory.u8[at + k] = (lines >> (8 * k)) & 0xFF
        run(core, 60)
        return core

    def panel(core, cols, first=9, last=20):
        return " ".join(t for t in
                         (tilemap_text(core, ty, *cols) for ty in range(first, last))
                         if t)

    core = start(cheat=False)
    left, right = panel(core, LEFT), panel(core, RIGHT)
    if "12345" not in left or "27" not in left:
        failures.append(f"el panel izquierdo no lleva lo del jugador: {left!r}")
    if "6789" not in right or "14" not in right:
        failures.append(f"el panel derecho no lleva lo del companero: {right!r}")
    if "T." in left or "T." in right:
        failures.append("los totales salen sin haber tocado el acorde")
    if not failures:
        print(f"  sin acorde: izquierda {left!r}")
        print(f"              derecha   {right!r}")

    # ...AND WITHOUT THE CHORD, SELECT DOES NOTHING. The cartridge's WITH
    # COMPUTER screen has no histogram, so HUD STATS is one more thing the
    # chord uncovers (hud_set): without it the partner's panel stays.
    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 60)
    left2, right2 = panel(core, LEFT), panel(core, RIGHT)
    if "6789" not in right2:
        failures.append(f"sin el acorde SELECT cambia de HUD en WITH COMPUTER: "
                        f"{right2!r}")
    else:
        print("  ...y sin el acorde SELECT no cambia nada: WITH tiene un HUD")

    core = start(cheat=True)
    left, right = panel(core, LEFT), panel(core, RIGHT)
    # 27 + 14 = 41 lines, and 12345 + 6789 = 19134 plus whatever the computer
    # scored while the frames above ran.
    if "41" not in left:
        failures.append(f"T.LINES no suma las dos: {left!r}")
    elif "1913" not in right and "1914" not in right:
        failures.append(f"T.SCORE no suma las dos: {right!r}")
    else:
        print(f"  con acorde: T.LINES y T.SCORE en la ultima celda de cada uno")

    # ...and SELECT swaps to the HUD that hides them, against the computer.
    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 60)
    left, right = panel(core, LEFT), panel(core, RIGHT)
    if "6789" in right:
        failures.append("SELECT no escondio el panel del companero")
    elif "41" not in left:
        failures.append(f"el HUD de stats no trae los totales: {left!r}")
    else:
        print("  y contra la maquina SELECT esconde al companero y deja los "
               "totales")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: en cooperativo cada jugador tiene su panel, con los totales "
           "bajo el acorde.")
    return 0


def stats_check(rom_path):
    """EL HISTOGRAMA Y EL CARTEL, QUE CADA CONSTRUCCION DIBUJA A SU MANERA.

    El release comparte UNA barra de ocho pasos entre las siete columnas y
    pone debajo una tira de iconos para decir cual es cual. Un prototipo no
    tiene tira: le da a cada pieza su propia tirada de ocho, pintada con el
    patron de bloque de esa pieza, y el patron es la etiqueta. Asi que bajo
    skin las dos filas de la tira son dos filas mas de barra, las siete
    columnas tienen que dibujarse con tiles DISTINTOS entre si, y ninguna
    puede quedar vacia teniendo cuenta.

    Y el cartel de GAME OVER: rojo en el release, azul en los tres
    prototipos. El marco viaja como arte en las ranuras del release, asi que
    lo que se comprueba aqui es que cambia -- y que NEXT no cambia con el,
    que es lo que paso al tomar prestado el banco 3 entero: los prototipos
    tienen su NEXT tan rojo como el release.
    """
    failures = []
    # La capa del histograma es la suya propia (SCREENBLOCK_HISTOGRAM en
    # gba/port.h), asi que no hace falta saber donde cae la caja: se barre
    # entera y lo que haya escrito ES el histograma.
    hist = 0x06000000 + STATS_SCREENBLOCK * 0x800
    # Cuentas bien distintas, ninguna nula, una por pieza.
    counts = (37, 5, 61, 12, 84, 29, 50)
    seen = {}
    for skin in (0, 1):
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        _ = screen

        def tap(*names, hold=4, settle=12):
            core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
            core.set_keys(); run(core, settle)

        run(core, 40)
        if skin:
            tap("L", "R"); run(core, 20)
        press_start(core); run(core, 24)
        press_start(core); run(core, 24)
        press_start(core); run(core, 60)
        tap("SELECT"); run(core, 30)          # al HUD de estadisticas
        off = game_offsets(rom_path)
        base, why = game_state_address(rom_path)
        if base is None:
            print(f"SALTADO: {why}")
            return 0
        # piece_stats es uint8_t[8] y la entrada 0 es TT_NONE.
        for i, n in enumerate(counts):
            core.memory.u8[base + off["stats"] + 1 + i] = n
        run(core, 30)

        used = {}
        for ty in range(32):
            for tx in range(32):
                e = core.memory.u16[hist + ((ty * 32 + tx) * 2)]
                if e & 0x3FF:
                    used.setdefault(tx, []).append((ty, e))
        cols = [sorted(v) for _k, v in sorted(used.items())]
        if len(cols) != STATS_COLUMNS:
            failures.append(f"{'release' if not skin else 'prototipo'}: el "
                             f"histograma ocupa {len(cols)} columnas, no "
                             f"{STATS_COLUMNS}")
            del core, screen
            continue
        name = "release" if not skin else "prototipo"
        # Las alturas siguen a las cuentas: mas piezas, barra mas alta.
        order = sorted(range(len(counts)), key=lambda i: counts[i])
        heights = [len(c) - (STATS_ICON_ROWS if not skin else 0) for c in cols]
        if any(heights[a] > heights[b] for a, b in zip(order, order[1:])):
            failures.append(f"{name}: las alturas {heights} no siguen al orden "
                             f"de las cuentas {counts}")
        else:
            print(f"  {name}: siete barras, alturas {heights} para {counts}")
        seen[skin] = cols
        # El cartel: se entierra el tablero y se mira de que color sale.
        addr = base + off["field"]
        for row in range(TENGEN_PF_HEIGHT):
            for col in range(1, TENGEN_PF_WIDTH - 1):
                core.memory.u8[addr + row * TENGEN_PF_WIDTH + col] = (
                    0 if col == 5 else CELL_BLOCK)
        run(core, 240)
        # El cartel se busca por sus propios tiles: las cuatro esquinas de la
        # caja son $29 $2B $3A $3C en las dos construcciones -- lo que cambia
        # es el dibujo y el banco, no el numero.
        plaque = [core.memory.u16[SCREENBLOCK_ADDR + (i * 2)]
                  for i in range(32 * 20)
                  if (core.memory.u16[SCREENBLOCK_ADDR + (i * 2)] & 0x3FF)
                  in PLAQUE_CORNERS]
        if len(plaque) < len(PLAQUE_CORNERS):
            failures.append(f"{name}: no sale el cartel de GAME OVER")
        else:
            seen[("plaque", skin)] = plaque
            print(f"  {name}: el cartel de GAME OVER esta puesto")
        # ...y la CABECERA sigue en su banco. NEXT, SCORE, LINES, LEVEL y HIGH
        # no son ASCII sino arte propio, a partir de HUD_LABEL_TILE_BASE, asi
        # que se buscan por numero de tile y se anotan sus bancos.
        banks = {core.memory.u16[SCREENBLOCK_ADDR + (i * 2)] >> 12
                 for i in range(32 * 20)
                 if HUD_LABEL_TILE_BASE <= (core.memory.u16[SCREENBLOCK_ADDR +
                                                            (i * 2)] & 0x3FF)
                 < HUD_LABEL_TILE_END}
        if not banks:
            failures.append(f"{name}: no se encuentran las etiquetas del HUD")
        seen[("labels", skin)] = banks
        del core, screen

    if 0 in seen and 1 in seen and not failures:
        # LAS DOS FILAS DE LA TIRA DE ICONOS SON BARRA BAJO SKIN. En el
        # release las dos de abajo de cada columna son el icono, asi que la
        # barra es lo que queda; en un prototipo no hay icono y todo es barra.
        # Las dos llegan igual de abajo -- al suelo de la caja -- y es la barra
        # la que crece.
        rel_floor = max(ty for c in seen[0] for ty, _e in c)
        pro_floor = max(ty for c in seen[1] for ty, _e in c)
        if rel_floor != pro_floor:
            failures.append(f"el histograma no llega igual de abajo con skin "
                             f"({pro_floor}) que sin ella ({rel_floor})")
        rel = max(len(c) for c in seen[0]) - STATS_ICON_ROWS
        pro = max(len(c) for c in seen[1])
        if pro <= rel:
            failures.append(f"bajo skin la barra mas alta mide {pro} filas y sin "
                             f"skin {rel}: la tira de iconos no ha dejado su sitio")
        else:
            print(f"  la barra mas alta pasa de {rel} filas a {pro}: las dos de "
                   "la tira de iconos son barra en el prototipo")
        # ...Y CADA COLUMNA CON SU PROPIA TIRADA bajo skin, que es la
        # diferencia entera: los tiles de una columna no aparecen en ninguna
        # otra. Sin skin es justo al reves -- las siete comparten una sola
        # tirada de ocho pasos, asi que todos sus tiles caben en una ventana
        # de ocho.
        pro_sets = [{e & 0x3FF for _ty, e in c} for c in seen[1]]
        shared = [(i, j) for i in range(len(pro_sets))
                  for j in range(i + 1, len(pro_sets))
                  if pro_sets[i] & pro_sets[j]]
        if shared:
            failures.append(f"bajo skin las columnas {shared} comparten tiles de "
                             f"barra: no llevan el patron de su pieza")
        else:
            print(f"  ...y cada columna con el patron de su pieza: "
                   f"{len(pro_sets)} tiradas sin un tile en comun")
        rel_bars = {e & 0x3FF for c in seen[0] for _ty, e in c[:-STATS_ICON_ROWS]}
        if max(rel_bars) - min(rel_bars) >= STATS_RUN_STEPS:
            failures.append(f"sin skin las columnas deberian compartir una sola "
                             f"tirada y usan {sorted(hex(t) for t in rel_bars)}")

    if ("plaque", 0) in seen and ("plaque", 1) in seen:
        rel = {e >> 12 for e in seen[("plaque", 0)] if e & 0x3FF}
        pro = {e >> 12 for e in seen[("plaque", 1)] if e & 0x3FF}
        if rel == pro:
            failures.append(f"el cartel de GAME OVER usa el mismo banco con y "
                             f"sin skin ({sorted(rel)}): no cambia de color")
        else:
            print(f"  el cartel cambia de paleta con la skin: banco "
                   f"{sorted(rel)} -> {sorted(pro)}")
    if ("labels", 0) in seen and ("labels", 1) in seen:
        if seen[("labels", 0)] != seen[("labels", 1)]:
            failures.append(f"la cabecera cambia de banco con la skin "
                             f"({sorted(seen[('labels', 0)])} -> "
                             f"{sorted(seen[('labels', 1)])}): el cartel se ha "
                             f"llevado NEXT y los contadores con el")
        else:
            print(f"  ...y la cabecera se queda en el banco "
                   f"{sorted(seen[('labels', 0)])}: el cartel no la arrastra")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: el histograma y el cartel son los de cada construccion.")
    return 0


def braid_check(rom_path):
    core, screen = load(rom_path)
    start_game(core)
    run(core, 20)

    def frame_columns(x0, x1):
        px = pixels(screen)
        return [tuple(px[y][x] for x in range(x0, x1)) for y in range(160)]

    LEFT_FRAME = (64, 80)     # the board's left rope: the left panel's own side
    RIGHT_FRAME = (160, 176)  # ...and its right, which is the right panel's

    box = frame_columns(*RIGHT_FRAME)
    box_left = frame_columns(*LEFT_FRAME)

    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 12)
    banner = frame_columns(*RIGHT_FRAME)

    failures = []
    # The banner takes the whole column, so its top and bottom rows are rope
    # where the box has its lid and floor. Everything BETWEEN them is the same
    # vertical run in both modes and has to match pixel for pixel.
    body = range(16, 144)
    diff = sum(1 for y in body if box[y] != banner[y])
    if diff:
        failures.append(
            f"la greca del campo cambia entre caja y banner: {diff} filas distintas")
    else:
        print("  la greca junto al campo es identica con caja y con banner")

    # And the two panels frame the board from opposite sides, so their runs
    # must be MIRRORS of each other, not copies — that is what makes the pair
    # read as two boxes rather than two copies of the same edge.
    same = sum(1 for y in body if box_left[y] == box[y])
    if same > len(list(body)) // 2:
        failures.append(
            "las dos grecas del campo son iguales; deberian ser espejo la una de la otra")
    else:
        print("  las dos grecas son espejo la una de la otra, como en el cartucho")

    core.set_keys(KEYS["SELECT"]); run(core, 4); core.set_keys(); run(core, 12)
    back = frame_columns(*RIGHT_FRAME)
    if any(box[y] != back[y] for y in body):
        failures.append("al volver del banner la greca no queda como estaba")
    else:
        print("  al volver del banner la caja queda como estaba")

    # ...AND THEY MUST BE THE CARTRIDGE'S OWN WALLS, THIS WAY ROUND. Mirrors
    # of each other is not enough: swap the pair and they are still mirrors,
    # and that is exactly the state this port shipped in for a long time --
    # the fret of both walls pointing OUT at the HUD instead of in at the
    # board. The rope's fret is chiral, so which tile goes on which side is
    # the whole of it.
    #
    # The numbers are the cartridge's, read off its own nametable: the 1P
    # screen walls its playfield with $6A $6B at columns 0-1 and $73 $74 at
    # 12-13, and the coop screen hangs the same two off the header's rule with
    # $95 $96 / $99 $9A and $97 $98 / $9B $9C. The port uploads the background
    # tiles at their own indices, so these are the map entries as well.
    WALL_L, WALL_R = (0x6A, 0x6B), (0x73, 0x74)
    HANG_L = ((0x95, 0x96), (0x99, 0x9A))
    HANG_R = ((0x97, 0x98), (0x9B, 0x9C))

    def row_tiles(ty, x0, n):
        return tuple(core.memory.u16[SCREENBLOCK_ADDR + (ty * 32 + x0 + i) * 2]
                      & 0x3FF for i in range(n))

    for name, tx, want in (("izquierda", 8, WALL_L), ("derecha", 20, WALL_R)):
        got = row_tiles(10, tx, 2)
        if got != want:
            failures.append(
                f"la pared {name} del campo es {[hex(t) for t in got]} y el "
                f"cartucho pone {[hex(t) for t in want]}")
        else:
            print(f"  la pared {name} del campo es la del cartucho, "
                  f"${want[0]:02X} ${want[1]:02X}")
    for name, tx, want in (("izquierdo", 8, HANG_L), ("derecho", 20, HANG_R)):
        got = (row_tiles(0, tx, 2), row_tiles(1, tx, 2))
        if got != want:
            failures.append(
                f"el codo del panel {name} no es el del cartucho: {got}")
    if not failures:
        print("  y los dos codos son los de la regla de cabecera del cartucho")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: la greca conserva su sentido en los dos modos.")
    return 0
