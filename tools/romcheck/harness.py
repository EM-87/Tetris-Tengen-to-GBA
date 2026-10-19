"""
The machinery every check shares: an mGBA core, the ROM's own
layout and memory map, and the helpers that drive and read it.
"""
import os
import subprocess
import sys

try:
    import mgba.core
    import mgba.image
    import mgba.log
except ImportError as exc:  # pragma: no cover - environment problem, not logic
    sys.exit(f"mGBA python bindings unavailable ({exc}).\n"
             "Install with: pip install pygba && apt-get install libmgba0.10")

# THE SPLASH IS SKIPPED, as a soft reset skips it: every check was written
# against a console that reaches the title at once, and the logo would eat
# their first presses. After each reset the word gba/splash.c looks for is
# put in external WRAM, where its ELF says it lives. `splash_check` turns
# this off to see the logo itself (SHOW_SPLASH).
SPLASH_SEEN = 0x54454E47
SHOW_SPLASH = [False]
_splash_addr = {}


def _splash_word(rom_path):
    if rom_path not in _splash_addr:
        elf = os.path.splitext(rom_path)[0] + ".elf"
        addr = None
        try:
            out = subprocess.check_output(
                [os.environ.get("NM", "arm-none-eabi-nm"), elf],
                stderr=subprocess.DEVNULL).decode()
            for line in out.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[2] == "g_splash_seen":
                    addr = int(parts[0], 16)
        except (OSError, subprocess.CalledProcessError):
            pass
        _splash_addr[rom_path] = addr
    return _splash_addr[rom_path]


_load_path = mgba.core.load_path
_reset = mgba.core.Core.reset


def _load_path_kept(path):
    core = _load_path(path)
    if core is not None:
        core._tengen_rom = path
    return core


def _reset_past_splash(self):
    _reset(self)
    addr = _splash_word(getattr(self, "_tengen_rom", ""))
    if addr is not None and not SHOW_SPLASH[0]:
        for b in range(4):
            self.memory.u8[addr + b] = (SPLASH_SEEN >> (8 * b)) & 0xFF


mgba.core.load_path = _load_path_kept
mgba.core.Core.reset = _reset_past_splash


# L+R ON THE TITLE WITHOUT THE CHORD, the audit (ROMCHECK_AUDIT=1). Since the
# prototypes went behind the game's chord, L+R on the title does nothing
# until the chord has been rung at GAME SELECT or LEVEL SETTINGS, and a
# check that still pressed it there to put a skin on went on passing,
# checking the cartridge's title twice. This says where: on the frame after
# a press of both shoulders, if the title's clock moved (g_title_frame) and
# the chord is not rung (g_pause_unlocked), the check's own line.
_set_keys = mgba.core.Core.set_keys
_run_frame = mgba.core.Core.run_frame
_AUDIT = os.environ.get("ROMCHECK_AUDIT") == "1"
_audit_syms = {}


def _audit_addr(core, name):
    rom = getattr(core, "_tengen_rom", "")
    if (rom, name) not in _audit_syms:
        _audit_syms[rom, name] = game_state_address(rom, name)[0]
    return _audit_syms[rom, name]


def _set_keys_audited(self, *keys):
    _set_keys(self, *keys)
    if 8 in keys and 9 in keys:
        import traceback
        here = [f for f in traceback.extract_stack()[:-1]
                if "harness.py" not in f.filename]
        clock = _audit_addr(self, "g_title_frame")
        self._tengen_audit = (here[-2:],
                              clock is not None and self.memory.u16[clock])


def _run_frame_audited(self):
    _run_frame(self)
    pending = getattr(self, "_tengen_audit", None)
    if pending is None:
        return
    self._tengen_audit = None
    where, before = pending
    clock = _audit_addr(self, "g_title_frame")
    chord = _audit_addr(self, "g_pause_unlocked")
    if clock is None or chord is None:
        return
    if self.memory.u16[clock] != before and not self.memory.u8[chord]:
        print("AUDITORIA: L+R en el titulo sin el acorde, en " +
              " <- ".join(f"{os.path.basename(f.filename)}:{f.lineno}"
                          for f in reversed(where)))


if _AUDIT:
    mgba.core.Core.set_keys = _set_keys_audited
    mgba.core.Core.run_frame = _run_frame_audited

# SALTADO, AND THE STRICT RUN. A check that cannot run here — a symbol the
# ELF lacks, a file the build was not given — says SALTADO and passes. On a
# build that has everything that hides a check that has stopped checking:
# skin_check went on passing that way, looking for a skin it never put on.
# So every skip goes through skip(), and under ROMCHECK_STRICT=1, which
# `make gba-check` sets unless it is given STRICT=0, a run that skipped
# anything fails (strict_verdict, at each script's end). A check that did
# not REACH what it checks — the cable's match never starting, a board that
# would not die — is no skip at all but a failure, and says FALLA.
STRICT = os.environ.get("ROMCHECK_STRICT") == "1"
SKIPPED = []


def skip(why):
    print(f"SALTADO: {why}")
    SKIPPED.append(why)


def strict_verdict(code):
    """A script's exit status, failed if it skipped anything in a strict
    run."""
    if code in (0, None) and STRICT and SKIPPED:
        for why in SKIPPED:
            print(f"FALLA: saltado en una pasada estricta (STRICT=0 lo "
                  f"permite): {why}")
        return 1
    return code


SCREEN_W, SCREEN_H = 240, 160
TILE = 8
SCREEN_TW_TILES = SCREEN_W // TILE

# The screen layout, in tile columns. These mirror gba/port.h and the reflow
# done by tools/extract_assets.py; if they drift apart, the checks below stop
# meaning anything, so they are asserted against the running ROM rather than
# assumed.
# The cartridge's columns are resequenced to put the playfield in the middle
# with the HUD split either side; see SCREEN_SEGMENTS in extract_assets.py.
# 10 | 10 | 10 — a closed rectangle of the board's own blue braid, the ten
# playable columns, and another rectangle. The braid that used to run down the
# board's edges as two bare strips is now those boxes' inner sides, so the
# playfield is framed exactly where it always was; the rope simply carries on
# round the HUD. Symmetric by construction.
COL_BOX_L = (0, 10)       # score / lines / level / high score / next
COL_FRAME_L = (8, 10)     # the board's left frame — the box's inner side
COL_FIELD = (10, 20)      # the ten playable columns — dead centre
COL_FRAME_R = (20, 22)    # the board's right frame — the other box's
COL_BOX_R = (20, 30)      # the piece statistics, in two ranks

# The walls are the cartridge's own frame art, drawn once with the screen, not
# blocks painted from the playfield buffer — see the note in gba/hud.c.
FIELD_TILES_W = COL_FIELD[1] - COL_FIELD[0]
FIELD_X0 = COL_FIELD[0] * TILE
FIELD_X1 = COL_FIELD[1] * TILE
WALL_L_X = COL_FRAME_L[0] * TILE
WALL_R_X = (COL_FRAME_R[1] - 1) * TILE


def load(rom_path):
    """Returns (core, screen).

    KEEP THE SCREEN. mGBA renders into that buffer and its bindings do not
    hold a Python reference to it, so dropping the returned Image — binding it
    to `_`, say — lets the garbage collector free memory the emulator is still
    writing to, and the next run_frame() segfaults. Every caller here names it
    and keeps it alive for as long as it runs frames.
    """
    mgba.log.silence()
    core = mgba.core.load_path(rom_path)
    if core is None:
        sys.exit(f"mGBA could not load {rom_path}")
    screen = mgba.image.Image(SCREEN_W, SCREEN_H)
    core.set_video_buffer(screen)
    core.reset()
    return core, screen


def pixels(screen):
    """Framebuffer as a list of rows of (r, g, b) tuples.

    mGBA hands back a raw 32-bit-per-pixel buffer whose row length is
    `stride`, not `width` — reading it as width-sized rows silently skews the
    image, so the stride has to be honoured here.
    """
    import numpy as np
    from mgba import ffi

    raw = ffi.buffer(screen.buffer, screen.stride * screen.height * 4)
    flat = np.frombuffer(raw, dtype=np.uint32).reshape(screen.height, screen.stride)
    frame = flat[:, :screen.width]
    r = (frame & 0xFF).astype(int)
    g = ((frame >> 8) & 0xFF).astype(int)
    b = ((frame >> 16) & 0xFF).astype(int)
    return [[(int(r[y][x]), int(g[y][x]), int(b[y][x])) for x in range(screen.width)]
            for y in range(screen.height)]


def run(core, frames, keys=0):
    if keys:
        core.set_keys(keys)
    for _ in range(frames):
        core.run_frame()


def describe(rows):
    distinct = {p for row in rows for p in row}
    print(f"colores distintos en pantalla: {len(distinct)}")
    non_black = sum(1 for row in rows for p in row if p != (0, 0, 0))
    print(f"pixeles no negros: {non_black} / {SCREEN_W * SCREEN_H}")

    # Column occupancy profile, split by region: the field has a fixed
    # allocation, the HUD legitimately extends past it.
    cols = [sum(1 for y in range(SCREEN_H) if rows[y][x] != (0, 0, 0))
            for x in range(SCREEN_W)]
    lit = [x for x, c in enumerate(cols) if c > 0]
    if not lit:
        print("columnas con contenido: ninguna (pantalla vacia)")
        return cols

    print(f"columnas con contenido: {min(lit)}..{max(lit)}")
    for name, bounds in (("recuadro izq", COL_BOX_L), ("marco izq", COL_FRAME_L),
                         ("campo", COL_FIELD), ("marco der", COL_FRAME_R),
                         ("recuadro der", COL_BOX_R)):
        x0, x1 = bounds[0] * TILE, bounds[1] * TILE
        print(f"  {name:10s} x={x0:3d}..{x1 - 1:3d}")
    return cols


KEY_START = 3  # set_keys takes bit indices, not a mask


def press_start(core):
    core.set_keys(KEY_START)
    run(core, 4)
    core.set_keys()
    run(core, 6)


def ring_chord_from_title(core, settle=24):
    """The game's chord, from the title and back to it: START to GAME
    SELECT, L+R there, B. Everything the port adds waits for it — the
    prototypes' titles on the title's own L and R among it."""
    press_start(core); run(core, 10)
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
    core.set_keys(); run(core, 12)
    core.set_keys(KEYS["B"]); run(core, 4)
    core.set_keys(); run(core, settle)


def build_has_protos(rom_path):
    """Whether this build was given the prototype dumps (make assets
    PROTO=...): gba/screen_proto.h, beside the ROM's build directory, says
    so. A check that wants a skin and finds none skips on a build without
    them and fails on one with them."""
    proto_h = os.path.join(os.path.dirname(rom_path), "..", "gba", "screen_proto.h")
    return (os.path.exists(proto_h) and
            "#define SCREEN_PROTO_AVAILABLE 1" in open(proto_h).read())


def put_skin_on(core, settle=24):
    """From the title, a prototype's title on it: the game's chord rung
    (ring_chord_from_title) and then L+R, which finds the skins. True when
    one went on (g_title_skin), False on a build without them. Pressed on
    the title without the chord, L+R does nothing at all — which a check or
    two went on doing, and checked the cartridge's title twice."""
    ring_chord_from_title(core)
    core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
    core.set_keys(); run(core, settle)
    rom = getattr(core, "_tengen_rom", "")
    skin, _ = game_state_address(rom, "g_title_skin")
    return skin is not None and core.memory.u8[skin] != 0


def start_game(core, tune=0):
    """Gets past the title and the selection screens into a solo game.

    Three presses: a title screen, then GAME SELECT (whose first entry is
    1 PLAYER, so START takes it), then LEVEL SETTINGS — one page carrying the
    level, the handicap and the tune — and then play.

    `tune` picks a tune on the way through. The port now opens on NO MUSIC
    (musicSelectTable's own first entry), so a check that needs to HEAR
    something has to ask for it: the cursor goes down to the MUSIC row and
    RIGHT walks the list from there.
    """
    run(core, 8)
    press_start(core)   # title -> game select
    press_start(core)   # game select (1 PLAYER) -> LEVEL SETTINGS
    if tune:
        for _ in range(2):      # cursor: LEVEL -> HANDICAP -> MUSIC
            core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, 8)
        for _ in range(tune):   # NO MUSIC -> LOGINSKA -> ...
            core.set_keys(KEYS["RIGHT"]); run(core, 4); core.set_keys(); run(core, 8)
    press_start(core)   # LEVEL SETTINGS -> play
    run(core, 8)


# ---------------------------------------------------------------------------
# The line-clear animation
#
# The ROM does not simply delete completed rows: a puff of smoke crosses each
# one and leaves SINGLE / DOUBLE / TRIPLE / TETRIS written where the blocks
# were (see reference/NOTES.md). That is five sprites and twelve tile writes
# per row per step, all of it easy to get subtly wrong and impossible to see
# in a host test, so it is checked here against the running ROM.
#
# Reaching a line clear by PLAYING would need a bot that stacks well, and the
# piece sequence depends on when Start was pressed. Instead the check writes
# completed rows straight into the game's playfield — the first member of
# `g_session`, whose address comes out of the ELF — and lets the next piece to
# land trigger the clear. Nothing in the ROM is modified; this is a fixture in
# the harness, the same way a unit test constructs a board.
#
# `g_session` is a TengenLink, whose first member is the TengenGame, which in
# turn starts with the playfields. So the symbol's own address IS the board's:
# a solo game and a linked one are the same struct at the same place, which is
# what lets one fixture serve both.
# ---------------------------------------------------------------------------
PF_W, PF_H = 12, 20            # must match TENGEN_PF_WIDTH / _HEIGHT
PF_PLAYABLE = PF_W - 2         # what is actually drawn; the walls are frame art
CELL_WALL, CELL_BLOCK = 15, 1
SCREENBLOCK_ADDR = 0x0600E000  # screenblock 28, as gba/main.c sets BG0CNT
CHARBLOCK_ADDR = 0x06000000    # charblock 0, the game's tiles
# The panel's own top run: two slots above the cartridge's 256 that nothing
# but apply_skin writes. See SKIN_PANEL_RUN_BASE in gba/screen_proto.h.
PANEL_RUN_SLOTS = (0x320, 0x321)
OAM_ADDR = 0x07000000
SWEEP_TILES = (0x5B, 0x5C, 0x5D, 0x5E, 0x5F)  # main.asm.txt:1274-1338
SWEEP_PAL_BANK = 4   # gba/port.h PAL_OBJ_CLEAR; banks 0-3 are the dancers
CLEAR_WORDS = {1: "SINGLE", 2: "DOUBLE", 3: "TRIPLE", 4: "TETRIS"}

KEY_DOWN = 7


def game_state_address(rom_path, name="g_session"):
    """Address of a symbol in the built ROM, read out of the ELF beside it."""
    elf = os.path.splitext(rom_path)[0] + ".elf"
    if not os.path.exists(elf):
        return None, f"no encuentro {elf} (hace falta para localizar el estado)"
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    try:
        out = subprocess.check_output([nm, elf]).decode()
    except (OSError, subprocess.CalledProcessError) as exc:
        return None, f"no pude ejecutar {nm}: {exc}"
    for line in out.splitlines():
        if line.endswith(" " + name):
            return int(line.split()[0], 16), None
    return None, f"el ELF no exporta {name}"


def fill_rows(core, base, rows):
    """Plant complete rows straight into the playfield.

    `base` is the address of the FIELD, which is TengenGame's first member
    today and need not stay that way — see game_offsets.
    """
    for row in rows:
        for col in range(PF_W):
            value = CELL_WALL if col in (0, PF_W - 1) else CELL_BLOCK
            core.memory.u8[base + row * PF_W + col] = value


def map_row_text(core, row):
    """The playfield row as it is actually on screen, read back from VRAM.

    Tile ids in this game's set are ASCII for letters and digits, so a row
    the sweep has written reads as text. Everything else comes back as '#'
    for a block and '.' for the empty tile, which is id 0.
    """
    out = []
    for col in range(PF_PLAYABLE):
        tile = core.memory.u16[SCREENBLOCK_ADDR + (row * 32 + COL_FIELD[0] + col) * 2] & 0x3FF
        out.append("." if tile == 0 else (chr(tile) if 0x20 <= tile < 0x7F else "#"))
    return "".join(out)


# ---------------------------------------------------------------------------
# Pause and the cheat codes
#
# Start pauses; the codes go in while paused. The rules are tested on the host
# (tests/test_tengen.c); what is checked here is that the GBA layer wires them
# up at all and draws the ROM's own PAUSE plaque where it should.
# ---------------------------------------------------------------------------
# The plaque is CENTRED, both ways — on the cartridge its eight columns are
# 12..19 of 32, which is the middle of the screen, and that relationship is
# what the port keeps rather than the column number. gba/port.h derives these
# the same way.
# The braid's own mid blue, which the shelves are drawn in. Sampled rather
# than named: it is palette bank 2 colour 2 of the cartridge's game set.
# THE SCREEN'S CURVE, as gba/palette.h's kLcdGamma applies it to every colour
# the ROM writes, and mGBA's 5-to-8-bit expansion after it: what a colour the
# cartridge names comes out as in the framebuffer. Keep in step with
# kLcdGamma.
LCD_GAMMA = list(range(32))   # the identity today; see kLcdGamma


def on_screen(rgb):
    """An 8-bit NES RGB colour as it reaches mGBA's framebuffer."""
    return tuple((LCD_GAMMA[c >> 3] << 3) | (LCD_GAMMA[c >> 3] >> 2) for c in rgb)


BRAID_BLUE = on_screen((76, 154, 236))   # the braid's NES $21

PAUSE_W = 8
PAUSE_H = 2
PAUSE_TX = (SCREEN_TW_TILES - PAUSE_W) // 2
PAUSE_TY = ((SCREEN_H // TILE) - PAUSE_H) // 2
PAUSE_ROW0 = [0x10, 0x11, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0x12]  # main.asm.txt:8061
PAUSE_ROW1 = [0x13, 0x14, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0x15]

KEYS = {"A": 0, "B": 1, "SELECT": 2, "START": 3,
        "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7,
        # The GBA's two extra buttons. The game proper never reads them —
        # they have no NES equivalent — so they are the port's own switches:
        # under the game's chord L or R swaps the title skin and in a match
        # L+R changes the cossack; SELECT swaps the right-hand HUD box.
        "R": 8, "L": 9}
CODE_LEVEL_UP = "UP DOWN UP DOWN LEFT RIGHT B B A".split()
CODE_LONG_BAR = "DOWN DOWN LEFT RIGHT LEFT RIGHT B A".split()
CODE_UNDO = "LEFT DOWN RIGHT UP LEFT DOWN RIGHT B A".split()

# Offsets into g_session.game, from the structs in src/tengen_core.h. The compiler is
# arm-none-eabi with the EABI's default -fshort-enums, so a TengenTetromino is
# one byte; a mismatch would show up immediately as nonsense readings, which
# the checks below would catch.
# THE ELF KNOWS THE OFFSETS, not this file. They used to be constants here and
# a single new field in TengenGame moved all of them, which showed up as three
# unrelated cheat-code checks failing. gba/match.c exports kGameProbe; this
# reads it, the way the audio checks read kNes6502Probe.
_GAME_PROBE_CACHE = {}


_LAYOUT_CACHE = {}
_LAYOUT_DEFAULT = {
    "question": 0xF0, "percent": 0xF1, "raised_base": 960, "arrow_tail": 970,
    "arrow_head": 971, "leader_head_ty": 2, "leader_first_ty": 3,
    "leader_entries": 15, "skin_tables": 4, "mode_tables": 4,
    "save_data": 5, "save_table_bytes": 135, "save_entry_bytes": 9,
    "save_name": 0x400, "save_mode": 0x410, "save_mode_sums": 0x41C + 4 * 135,
    "raised": "PAUSEXIT?R",
    "ai_target_x": 6, "ai_target_o": 7, "ai_settle": 8, "ai_soft_drop": 9,
    "ai_coop_aware": 10, "ai_smart": 12, "ai_plan_have": 420,
    "ai_plan_stage": 0, "ghost": 989, "check_off": 997, "hold_label": 1001,
}


def check_layout(rom_path):
    """The port's own numbers the checks read the screen and the save by:
    kCheckProbe and kCheckRaised (gba/video.c), read out of the ROM FILE at
    the address the ELF gives them, so nothing has to boot. They used to be
    written out here by hand; a change in C that this missed made a check
    read the wrong tile and say nothing. Without an ELF (an old build) the
    numbers below, which were right when this was written."""
    if not rom_path:
        return _LAYOUT_DEFAULT
    if rom_path not in _LAYOUT_CACHE:
        addr, _ = game_state_address(rom_path, "kCheckProbe")
        raised, _ = game_state_address(rom_path, "kCheckRaised")
        if addr is None or raised is None:
            _LAYOUT_CACHE[rom_path] = _LAYOUT_DEFAULT
            return _LAYOUT_DEFAULT
        base = 0x08000000 if addr >= 0x08000000 else 0x02000000
        data = open(rom_path, "rb").read()
        v = [int.from_bytes(data[addr - base + 2 * i:addr - base + 2 * i + 2], "little")
             for i in range(27)]
        at = raised - base
        text = data[at:data.index(b"\0", at)].decode("ascii")
        keys = ("question", "percent", "raised_base", "arrow_tail", "arrow_head",
                "leader_head_ty", "leader_first_ty", "leader_entries",
                "skin_tables", "mode_tables", "save_data", "save_table_bytes",
                "save_entry_bytes", "save_name", "save_mode", "save_mode_sums",
                "ai_target_x", "ai_target_o", "ai_settle", "ai_soft_drop",
                "ai_coop_aware", "ai_smart", "ai_plan_have", "ai_plan_stage",
                "ghost", "check_off", "hold_label")
        layout = dict(zip(keys, v))
        layout["raised"] = text
        _LAYOUT_CACHE[rom_path] = layout
    return _LAYOUT_CACHE[rom_path]


def game_offsets(rom_path):
    """Byte offsets into TengenGame, read out of the built ELF."""
    if rom_path not in _GAME_PROBE_CACHE:
        addr, why = game_state_address(rom_path, "kGameProbe")
        if addr is None:
            raise RuntimeError(why)
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        (field, player, stride, cur, y, level, stats,
         paused, held, nxt, alive, x, score, lines,
         counts, orient, piece_ids, proto_rules, total,
         hard_drop, fast_shift, fast_soft, hold, held) = (
            core.memory.u16[addr + i * 2] for i in range(24))
        _GAME_PROBE_CACHE[rom_path] = {
            "field": field, "player": player, "stride": stride,
            "current": player + cur, "y": player + y,
            "level": player + level, "stats": player + stats,
            "paused": paused, "held": player + held, "next": player + nxt,
            "active": player + alive, "x": player + x,
            "score": player + score,
            "lines": player + lines, "counts": player + counts,
            "orientation": player + orient,
            "piece_ids": piece_ids, "proto_rules": proto_rules,
            "total": player + total,
            "hard_drop": hard_drop, "fast_shift": fast_shift,
            "fast_soft": fast_soft, "hold": hold, "held": player + held,
        }
    return _GAME_PROBE_CACHE[rom_path]


# The pieces' own 4x4 bitmaps, from orientationTable ($86C3) — the same bytes
# src/tengen_core.c carries, written out again here ON PURPOSE: a check that
# read the core's answer for what a piece covers would be asking the code
# under test to mark its own work. Bit 15 is (row 0, column 0).
ORIENTATION_BITS = [
    [0x0000, 0x0000, 0x0000, 0x0000],   # none
    [0xF000, 0x4444, 0xF000, 0x4444],   # I
    [0xE400, 0x8C80, 0x4E00, 0x4C40],   # T
    [0xCC00, 0xCC00, 0xCC00, 0xCC00],   # O
    [0xE200, 0xC880, 0x8E00, 0x44C0],   # J
    [0xE800, 0x88C0, 0x2E00, 0xC440],   # L
    [0x6C00, 0x8C40, 0x6C00, 0x8C40],   # S
    [0xC600, 0x4C80, 0xC600, 0x4C80],   # Z
]


# ---------------------------------------------------------------------------
# The sound engine
#
# The port plays the cartridge's own music by running the cartridge's own 6502
# sound engine (see gba/nes_audio.h). The risky part of that is the hand-written
# interpreter, so this check does not listen to the output — it reads the
# emulated APU register file straight out of the running ROM and compares it,
# frame by frame and byte for byte, with a golden recording that
# tools/extract_assets.py made with the reference interpreter in nes_cpu.py.
# A wrong flag or addressing mode shows up as a mismatched frame here instead
# of as music that is subtly wrong in a way nobody notices.
# ---------------------------------------------------------------------------
APU_REGS = 0x18
GOLDEN_PATH = "gba/audio_golden.bin"
AUDIO_ALIGN_SEARCH = 90   # frames of the ROM to look through for the match
AUDIO_GOLDEN_SKIP = 60    # ...and of the golden, whose first frames the ROM
                          # covers with the screen-switch effect

# The GBA sound registers the checks read back are all together below, with
# what each one says (REG_SOUND1CNT_H and the rest).


# Restarting a GBA sound channel resets its phase and reloads its volume, so
# doing it every frame chops every held note into 60Hz slices — which is what
# it sounds like, and what this measures. It looks at the AMPLITUDE ENVELOPE,
# not the signal: the signal's own 60Hz band is just bass notes. Measured at
# 41% when the port applied channels on every register WRITE, and 10% once it
# only applied them on a register CHANGE, so the limit sits between.
AUDIO_RATE = 32768


# ---------------------------------------------------------------------------
# Pausing has to actually stop the sound.
#
# This engine ends a note by clearing the channel's bit in $4015, not by
# letting a length counter run out, and suspending the music clears them all.
# A port that only re-applies a channel when its period or volume registers
# change would never notice, and the last note would drone on under the pause
# and come back layered over the music on the way out — which is what "the
# music sounds doubled when I pause" is.
#
# This reads the GBA's own sound registers rather than listening to the
# output. That is a deliberate retreat: the mGBA binding here does hand back
# real samples (with the sound switched off at REG_SOUNDCNT_X the RMS is
# exactly zero), but the same code over the same build gives a silent pause on
# one run and a loud one on the next, so a measurement of the waveform cannot
# be trusted to mean anything. The registers are reproducible and they are
# the thing the port actually controls: every channel's volume nibble at zero
# and no channel flagged as sounding IS silence, whatever the buffer says.
# ---------------------------------------------------------------------------
REG_SOUND1CNT_H = 0x04000062     # channels 1, 2 and 4 keep their volume in
REG_SOUND2CNT_L = 0x04000068     # bits 12-15 of these
REG_SOUND3CNT_L = 0x04000070     # bit 7 is the wave channel's own on/off
REG_SOUND3CNT_H = 0x04000072     # ...and bits 13-14 its volume code
REG_SOUND4CNT_L = 0x04000078
REG_SOUNDCNT_X  = 0x04000084     # bits 0-3: which channels are sounding
REG_SOUND1CNT_X = 0x04000064     # ...and channel 1's pitch, in bits 0-10


def sound_state(core):
    io = core._native.memory.io

    def reg(addr):
        return io[(addr - 0x04000000) >> 1]

    return {
        "pulso 1": (reg(REG_SOUND1CNT_H) >> 12) & 0xF,
        "pulso 2": (reg(REG_SOUND2CNT_L) >> 12) & 0xF,
        "triangulo": 1 if (reg(REG_SOUND3CNT_L) & 0x80) else 0,
        "ruido": (reg(REG_SOUND4CNT_L) >> 12) & 0xF,
        "activos": reg(REG_SOUNDCNT_X) & 0x0F,
    }


# ---------------------------------------------------------------------------
# The two-player front end, with no cable attached.
#
# A real linked match needs two consoles and a cable, which no headless
# emulator here can supply. What CAN be checked, and is worth checking, is the
# half of it that a single GBA reaches on its own: that GAME SELECT offers 2
# PLAYER, that choosing it reaches the link screen, that the ROM keeps running
# at full speed while the lobby finds nothing on the other end — a serial wait
# without a timeout would hang here and nowhere else — that it eventually says
# so instead of waiting forever, and that B gets back out.
#
# The handshake's own logic is tested on the host, two lobbies against each
# other (tests/test_tengen.c); this is the wiring around it.
# ---------------------------------------------------------------------------
LINK_MSG_ROW = 10
LINK_TIMEOUT_FRAMES = 600   # TENGEN_LOBBY_TIMEOUT in src/tengen_link.h


# The second map, four pixels along, which the menus use to centre their
# odd-length lines and the play screen uses for the statistics. See
# SCREENBLOCK_STATS in gba/port.h.
SCREENBLOCK_OFFSET_ADDR = SCREENBLOCK_ADDR + 0x800
# ...and screenblock 31, the histogram's, which outside a match carries the
# one line the port wants two pixels higher than the grid (see
# set_credit_layer in gba/video.c).
SCREENBLOCK_LIFTED_ADDR = SCREENBLOCK_ADDR + 0x1800
# ...and screenblock 30, the counters', which is where SCORE, LINES, LEVEL
# and HIGH are actually written.
SCREENBLOCK_PANEL_ADDR = SCREENBLOCK_ADDR + 0x1000


def tilemap_text(core, row, first=0, last=30):
    """The row of the tilemap as text, ACROSS EVERY TEXT LAYER.

    The tileset's letters sit at their ASCII codes (see ascii_tile in
    gba/video.c), so a tile id IS a character. A menu line of odd length is
    drawn on the offset layer instead of the main one, and the GAME SELECT
    credit on the lifted one — reading only the main map would report an empty
    row and every menu check would quietly stop checking anything.

    The two characters the cartridge does not keep at their ASCII codes are
    the question mark and the per cent sign: they live at TILES_GAME_QUESTION
    and TILES_GAME_PERCENT (gba/tiles_game.h), so ascii_tile maps them there
    and this maps them back. Without that, "EXIT?" reads as "EXIT" and the
    pause menu's question looks like its EXIT line.
    """
    lay = check_layout(getattr(core, "_tengen_rom", None))
    QUESTION = lay["question"]     # TILES_GAME_QUESTION in gba/tiles_game.h
    PERCENT = lay["percent"]       # TILES_GAME_PERCENT
    # ...and the pause menu's headings, drawn with copies of their letters one
    # pixel higher (PMENU_RAISED_BASE / PMENU_RAISED_CHARS in gba/port.h).
    RAISED_BASE, RAISED = lay["raised_base"], lay["raised"]
    # ...and its arrow, moved three pixels closer across two tiles
    # (T_ARROW_TAIL / T_ARROW_HEAD): read as "->".
    ARROW = {lay["arrow_tail"]: "-", lay["arrow_head"]: ">"}
    # ...and the end page's colon, drawn at boot after HOLD's four letters
    # (T_COLON): the cartridge's $3A is box art; and after it the title
    # font's copyright sign (T_COPYRIGHT).
    COLON = lay["hold_label"] + 4
    COPYRIGHT = lay["hold_label"] + 5

    def readable(t):
        return (32 <= t < 127 or t in (QUESTION, PERCENT, COLON, COPYRIGHT)
                or t in ARROW or RAISED_BASE <= t < RAISED_BASE + len(RAISED))

    out = []
    for x in range(first, last):
        off = (row * 32 + x) * 2
        tile = core.memory.u16[SCREENBLOCK_ADDR + off] & 0x3FF
        if not readable(tile):
            tile = core.memory.u16[SCREENBLOCK_OFFSET_ADDR + off] & 0x3FF
        if not readable(tile):
            tile = core.memory.u16[SCREENBLOCK_LIFTED_ADDR + off] & 0x3FF
        if not readable(tile):
            tile = core.memory.u16[SCREENBLOCK_PANEL_ADDR + off] & 0x3FF
        if tile == QUESTION:
            out.append("?")
        elif tile == PERCENT:
            out.append("%")
        elif tile == COLON:
            out.append(":")
        elif tile == COPYRIGHT:
            out.append("\u00a9")
        elif RAISED_BASE <= tile < RAISED_BASE + len(RAISED):
            out.append(RAISED[tile - RAISED_BASE])
        elif tile in ARROW:
            out.append(ARROW[tile])
        else:
            out.append(chr(tile) if 32 <= tile < 127 else " ")
    return "".join(out).strip()


def heading_unraised(core, row, first=0, last=30):
    """The letters of a window's HEADING drawn with the plain glyph.

    A heading is drawn a pixel higher than the lines under it, with copies of
    the letters shifted up inside their tiles (PMENU_RAISED_CHARS), and a
    letter with no copy fell back to the plain one: CABLE LOST came out with
    its A, E, S and T a pixel above its C, B, L and O, one line at two
    heights. tilemap_text reads both the same, so this is what tells them
    apart: every letter or ? on the row that is NOT a raised copy, on any of
    the layers a line can be drawn on.
    """
    lay = check_layout(getattr(core, "_tengen_rom", None))
    plain = []
    for x in range(first, last):
        for addr in (SCREENBLOCK_ADDR, SCREENBLOCK_OFFSET_ADDR,
                     SCREENBLOCK_LIFTED_ADDR, SCREENBLOCK_PANEL_ADDR):
            t = core.memory.u16[addr + (row * 32 + x) * 2] & 0x3FF
            if 0x41 <= t <= 0x5A or t == lay["question"]:
                plain.append("?" if t == lay["question"] else chr(t))
    return plain


# ---------------------------------------------------------------------------
# The title screen's sprites: the cathedral overlay and the fireworks.
#
# Neither is drawn by the port. Both are subroutines of the cartridge run on
# the same 6502 interpreter as the sound engine, filling oamStaging, which
# gba/frontend.c copies into OAM (see draw_title_sprites). What can be checked
# from outside is exactly what matters: that the eighteen cathedral sprites
# land on the picture, that bursts actually happen and animate, that the show
# ends where the ROM ends it (frameCounterHigh = 4, about 1024 frames), that
# entering the title again restarts it — initializeTitleScreen zeroes the
# frame counter, so without that it would only ever play once — and that the
# extra 6502 work still fits in a frame.
# ---------------------------------------------------------------------------
CATHEDRAL_SPRITES = 18
TITLE_SHOW_FRAMES = 1024        # frameCounterHigh reaches 4


def oam_visible(core, first, last):
    """(x, y, tile) of every visible sprite in a range of OAM slots."""
    out = []
    for i in range(first, last):
        a0 = core.memory.u16[OAM_ADDR + i * 8]
        if (a0 & 0x0300) == 0x0200:
            continue                        # the hidden bit
        a1 = core.memory.u16[OAM_ADDR + i * 8 + 2]
        a2 = core.memory.u16[OAM_ADDR + i * 8 + 4]
        out.append((a1 & 0x1FF, a0 & 0xFF, a2 & 0x3FF))
    return out


# ---------------------------------------------------------------------------
# The title-skin easter egg: L or R swaps the release's title screen for the
# prototype cartridge's, which has another cathedral, another logo and a green
# fret instead of the blue braid.
#
# A ROM built without a prototype dump has only one skin, and this says so
# rather than failing — SCREEN_PROTO_AVAILABLE is 0 and L/R do nothing.
# ---------------------------------------------------------------------------

# How far the chord is followed before a cycle that never comes home is called
# broken. The number of skins is whatever the build was given, so this is a
# ceiling rather than an expectation.
LIMIT_SKINS = 12


# ---------------------------------------------------------------------------
# The tunes that are not on the cartridge.
#
# Tengen's four are Loginska, Bradinsky, Karinka and Troika. Korobeiniki and
# Katyusha are not among them, so both are entered by hand in gba/handtunes.c
# and hidden behind L+R on the selection screen. That makes them the only
# content in the port that was not extracted from the ROM, and the only sound
# that does not come out of the cartridge's own engine, so they are worth
# checking rather than assuming:
#
#   * the menu offers the cartridge's five until the code is entered, and
#     eight after — the two tunes and MUSIC MIX;
#   * choosing one actually produces notes, and DIFFERENT notes over time (a
#     stuck channel would still read as "sounding");
#   * the two are different tunes and not one score played twice, which is
#     what a bad index into the tune table would look like;
#   * PAUSE silences them, which needs its own stop because MUSIC_SUSPEND only
#     reaches the cartridge's engine;
#   * and the cartridge's engine is still running underneath, because the
#     sound effects are still meant to be the ROM's.
# ---------------------------------------------------------------------------
# GAME SELECT's entries start at row 10, two rows apart. The port offers the
# cartridge's first three; the last two want the COMPUTER player.
GAME_SELECT_ROWS = 5
GAME_SELECT_TY = 10             # ...and they start here, one row apart
# THE LIST IS FLUSH LEFT AND PUSHED RIGHT, the cartridge's own way of setting
# it: every entry starts at the same column whatever its length, and VERSUS
# COMPUTER — the longest at fifteen characters — ends one blank column short
# of the frame. gba/port.h's GAME_SELECT_TX. The arrow is two columns before
# it, so the scan below has to start AT the list rather than a few columns
# left of it, or it reads the cursor's white as an entry in the wrong bank.
GAME_SELECT_TX = 12
GAME_SELECT_LONGEST = 15
# The bank EVERY menu line is drawn in, chosen or not. It is bank 0 of
# bgPalette1 -- kRomPalette_bg_menu's `0F 12 0F 0F`, the cartridge's menu BLUE.
# Measured off the running cartridge: on its GAME SELECT all five entries come
# out (48,50,236) to the pixel and the only white anywhere is the cursor
# arrow's (236,238,236). It was PAL_MENU_BASE + 3 here for a long while, which
# had the port writing the list in white and marking the choice in the ORANGE
# that bank 1 holds for the credit line alone.
MENU_TEXT_BANK = 8              # PAL_MENU_BASE + 0, the cartridge's menu blue
# ...and the one white on these screens, the cursor's: gba/port.h's BANK_ARROW.
# The handicap's chosen number borrows it, which is what says which of the two
# the pad is moving.
MENU_ARROW_BANK = 11            # PAL_MENU_BASE + 3
# La memoria de paletas de fondo, y el primero de los cuatro bancos del titulo
# (PAL_TITLE_BASE en gba/port.h). Una skin toma prestados tres de esos cuatro.
PALETTE_ADDR = 0x05000000
TITLE_PAL_BASE = 4
# ...and the cursor's column, gba/frontend.c's GAME_SELECT_ARROW_TX.
MENU_ARROW_TX = GAME_SELECT_TX - 2
# The shared coop board starts one column further left than the ten-wide
# one, because it is twelve wide (SCREEN_COOP_FIELD_TX in the generated
# header, and COOP_FIELD_TX in gba/port.h).
COOP_FIELD_TX = 9
# The HIGH SCORES page: the heading's row and the first entry's, as
# gba/screen_leaderboard.h generates them.
LEADER_HEAD_TY = 2
LEADER_FIRST_TY = 3
TENGEN_PF_WIDTH = 12
TENGEN_PF_HEIGHT = 20
TENGEN_ROM_ROW_ORIGIN = 6   # the ROM row the visible field starts at

# The attract demo's own clock: frameCounterHigh 5, frameCounterLow $20.
DEMO_START_FRAME = 5 * 256 + 0x20
DEMO_WATCH_FRAMES = 2000

# LEVEL SETTINGS, two rows apart and hung off HANDICAP -- gba/port.h's
# MENU_FIELD_TY(f) = 9 + 2f. Three rows apart read as three announcements
# rather than as one block to choose from.
LEVEL_ROW = 9
HANDICAP_ROW = 11               # value AND, in one player, what it buries
MUSIC_ROW = 13
# Vueltas de sobra para dar la lista de canciones entera, destapada o no.
MUSIC_COUNT_MAX = 14

# El histograma tiene mapa para el solo (SCREENBLOCK_HISTOGRAM en gba/port.h),
# asi que lo que este escrito ahi ES el histograma y no hay que saber donde
# cae la caja. Siete columnas, una por tetromino.
STATS_SCREENBLOCK = 31
STATS_COLUMNS = 7
# Las cuatro esquinas de la caja del cartel de GAME OVER (T_BOX_TL/TR/BL/BR en
# gba/port.h). Son los mismos numeros con skin y sin ella: una skin cambia el
# DIBUJO que hay en esas ranuras y el banco de paleta, nunca el numero.
PLAQUE_CORNERS = (0x29, 0x2B, 0x3A, 0x3C)
# Las etiquetas del HUD (NEXT, SCORE, LINES, LEVEL, HIGH) no son ASCII sino
# arte propio: HUD_LABEL_TILE_BASE en gba/port.h y las veintidos que siguen.
HUD_LABEL_TILE_BASE = 768
HUD_LABEL_TILE_END = 790
# Y las dos filas que el release gasta en la tira de iconos del histograma,
# con los ocho pasos que tiene una tirada de barra en las dos construcciones.
STATS_ICON_ROWS = 2
STATS_RUN_STEPS = 8
# Cuanto se escucha tras pulsar: el mas largo de los efectos dura 15 frames.
EFFECT_WATCH_FRAMES = 30
# Cuanta tinta tiene que prometer una CELDA para que su vacio sea sospechoso.
# Media celda: menos que eso puede quedar tapado por otra capa sin que pase
# nada, media letra no.
ONSCREEN_MIN_INK = 32


def to_music_page(core, settle=10):
    """Title -> GAME SELECT -> LEVEL SETTINGS, cursor on MUSIC.

    One page carries all three settings; UP/DOWN/SELECT move the cursor
    between them and LEFT/RIGHT change the one it is on, so reaching the tune
    means two STARTs and then walking the cursor down to it."""
    press_start(core); run(core, settle)    # title -> game select
    press_start(core); run(core, settle)    # -> LEVEL SETTINGS
    for _ in range(2):                      # cursor: LEVEL -> HANDICAP -> MUSIC
        core.set_keys(KEYS["DOWN"]); run(core, 4); core.set_keys(); run(core, settle)


# Palette banks 12 and 13: the falling piece's colours and the preview's.
# setPiecePalette (main.asm.txt:5338) indexes kRomPiecePalettes by PIECE ID,
# so the two banks must differ exactly when the two pieces do.
PAL_PIECE_BANK, PAL_NEXT_BANK = 12, 13
# ...and the bricks' own in a match (PAL_BRICK_BANK, the credits' bank 9).
PAL_BRICK_BANK = 9


# TengenAi's fields the checks read: out of the ROM (kCheckProbe, through
# check_layout); see ai_offsets in romcheck/ai.py.


# Where the pause menu's lines land, derived the way gba/port.h derives them:
# a box PMENU_H tall centred on a 20-row screen, with the column inside it.
# The lines do NOT all live on the same background — the heading and the
# question's second line ride the counters' layer two pixels down, the
# even-length ones the offset layer three across — but tilemap_text reads all
# four, so these are just rows.
PMENU_H = 6
PMENU_TY = (SCREEN_H // TILE - PMENU_H) // 2
# SEVEN ROWS NOW, not ten: the cartridge's own PAUSE is eight columns by two
# and its GAME OVER plaque six by four, and a box half the height of the
# screen for three lines of text is out of proportion with both. The blank
# rows came out; see PMENU_H in gba/port.h.
# FIVE ROWS AND THREE LINES. The word MUSIC went — the tune's NAME is the
# entry, and a label over a value that is itself the choice says nothing —
# and with it the blank rows. The question is one line too, "EXIT?", with the
# mark doing what SURE? did.
# ...AND THE TUNE'S NAME WENT TOO, to a window of its own: the entry is
# MUSIC again, and A on it opens a window with every tune on a line.
# ...AND OPTIONS BETWEEN THEM, a row more: six rows centre on row 7 as five
# did, so nothing above EXIT GAME moved.
PM_HEAD = PMENU_TY + 1       # PAUSE
PM_MUSIC = PMENU_TY + 2      # MUSIC
PM_OPTIONS = PMENU_TY + 3    # OPTIONS
PM_EXIT = PMENU_TY + 4       # EXIT GAME
# The options' window: the heading and a line per switch (OPT_COUNT in
# gba/port.h), each name at the left after the arrow's two columns and its
# box at the right (T_CHECK_OFF: two tiles, the right half one more), in
# this order. Sixteen wide, as the menu (OPT_W in gba/match.c).
PMO_NAMES = ("HARD DROP", "FAST DOWN", "FAST SHIFT", "GHOST", "SWAP A-B", "HOLD")
PMO_H = len(PMO_NAMES) + 3
PMO_W = 16
PMO_TY = (SCREEN_H // TILE - PMO_H) // 2
PMO_TX = (30 - PMO_W) // 2
PMO_NAME = PMO_TX + 3        # the name's first column
PMO_BOX = PMO_TX + PMO_W - 3  # the box's left half
PMO_HEAD = PMO_TY + 1        # OPTIONS
PMO_FIRST = PMO_TY + 2       # HARD DROP, and the rest in PMO_NAMES' order
# The tunes' window: the heading MUSIC and a line per tune, eight under the
# chord (PMENU_H_MAX in gba/port.h), centred like the menu.
PMT_H = 11
PMT_TY = (SCREEN_H // TILE - PMT_H) // 2
PMT_HEAD = PMT_TY + 1        # MUSIC
PMT_FIRST = PMT_TY + 2       # NO MUSIC, and the rest in kMusicNames' order
PM_ASK = PMENU_TY + 1        # the question, "SURE?"
PM_SURE = PM_ASK
PM_ANSWER = PMENU_TY + 2     # YES, with NO under it
# The box's own columns, which is all a check about the box should read: the
# rest of the row is the HUD, and the braid decodes as stray letters. The box
# is as wide as its lines need (pmenu_width in gba/match.c), so these are the
# WIDEST it gets and pmenu_span reads the one actually on screen.
PMENU_W_T = 18                # PMENU_W_MAX in gba/port.h
PMENU_TX = (SCREEN_TW_TILES - PMENU_W_T) // 2
PM_L = PMENU_TX + 1
PM_R = PMENU_TX + PMENU_W_T - 1


def pmenu_span(core):
    """The interior columns of the pause box on screen now, found by its top
    corners ($29 and $2B, the game-over plaque's), or the widest if none."""
    top = [c for c in range(SCREEN_TW_TILES)
           if (core.memory.u16[SCREENBLOCK_ADDR + (PMENU_TY * 32 + c) * 2]
               & 0x3FF) in (0x29, 0x2B)]
    return (top[0] + 1, top[-1]) if len(top) >= 2 else (PM_L, PM_R)


# ---------------------------------------------------------------------------
# THE CREDITS, which this port had been dropping.
#
# The cartridge prints a credit in the bottom corner of each of its four
# front-end screens — six lines in all, in the PPU patch chains at $A19B,
# $A20E, $A280 and $A2DF. The port folded three of those screens into one, so
# five of the six had nowhere to go and the sixth was never drawn; what stood
# in their place was a line the port made up. They roll at the bottom of GAME
# SELECT now, one at a time.
#
# The words are the cartridge's and this check holds them to that: all six,
# spelled its way, PAZHITNOV and all.
# ---------------------------------------------------------------------------
# MIRRORSOFT'S LINE IS DELIBERATELY NOT HERE. The cartridge spends its GAME
# SELECT corner on a licensing notice for a company that folded in 1991; this
# port spends it on whoever made the port. The other five are the people who
# made the game and are held to the cartridge's own spelling.
# ...WITHOUT THE CHORD. Under it the port's own line takes Mirrorsoft's place
# (CREDIT_PORT); without it the six are the cartridge's, Mirrorsoft's too.
CREDIT_PORT = ("PORTED WITH CLAUDE", "BY EDUARDO MARTINEZ")
CREDITS = [
    ("LICENSED BY", "MIRRORSOFT LTD."),
    ("CONCEPT BY", "ALEXEY PAZHITNOV"),
    ("DESIGN BY", "VADIM GERASIMOV"),
    ("PROGRAMMED BY", "ED LOGG"),
    ("VIDEO GRAPHICS BY", "KRIS MOSER"),
    ("AUDIO BY", "BRAD FULLER"),
    # ...and the title screen's two copyright lines, which the port's title
    # has no room for, with the cartridge's own copyright sign (T_COPYRIGHT).
    ("\u00a91987 ACADEMYSOFT-ELORG.", "\u00a91988 TENGEN"),
]
CREDIT_FRAMES = 240     # each credit's time on screen (gba/port.h)
CREDIT_TY = 16
