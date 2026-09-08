/*
 * tengen_core.h — Platform-independent core logic for the Tengen NES Tetris → GBA port.
 *
 * This header has ZERO dependency on GBA hardware (no <gba.h>, no memory-mapped
 * registers). It compiles standalone with any C99 compiler so the game rules can
 * be unit tested on the host machine (see tests/test_tengen.c) before ever being
 * wired to real GBA video/audio/input. The GBA-specific layer (rendering the
 * playfield to a tile background, reading REG_KEYINPUT, etc.) lives elsewhere and
 * only calls into this API.
 *
 * Every rule implemented here is traced to the disassembly in
 * ../reference/disasm/main.asm.txt (the Tengen Tetris (NES) disassembly
 * project, see ../reference/disasm/README.md.txt) and cited at its point of
 * use. ../reference/NOTES.md is the index, with ROM line numbers; its
 * PLACEHOLDER section is empty, and the rule for keeping it that way is in
 * ../CLAUDE.md.
 *
 * Some highlights, because they are what make this Tengen rather than Tetris
 * in general: a piece selector with no anti-repeat (hence the notorious
 * droughts), a rotation that only ever wall-kicks one column LEFT, an
 * auto-rotate that spins every frame once a button is held ~15, DAS that
 * charges 11 frames then repeats every 6, gravity that goes fractional above
 * level 10, and scoring that pays per piece locked — more the HIGHER it
 * lands, not the further it falls.
 */
#ifndef TENGEN_CORE_H
#define TENGEN_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------- *
 * Playfield
 *
 * The ROM packs the playfield as one nibble per cell, with sentinel wall
 * columns and floor rows baked right into the same buffer the collision
 * check walks (notes.txt: "playfield is stored with 1 tile per nibble ...
 * walls are built with F0 on left, 0F on right. Floor is FF"). This core
 * drops the nibble packing (one byte per cell instead) but KEEPS the
 * sentinel idea, because that's what makes both collision and full-row
 * detection fall out for free in either play mode — see below.
 *
 * Layout verified against initPlayer1orCoopPlayfield (main.asm.txt:3468-3503):
 * each ROM row is 8 bytes / 16 nibbles, of which nibbles 3..12 are the ten
 * playable cells, nibbles 2 and 13 are the walls, and 0,1,14,15 are padding.
 * The buffer runs 26 open rows (0..25) followed by two solid floor rows, so
 * the floor sits at row 26 — which is exactly the constant the scoring
 * routine subtracts against (main.asm.txt:1062), an independent confirmation
 * of the geometry.
 *
 * This core stores 12 columns per row, mapping storage column = ROM nibble
 * - 2. So:
 *   - 1P / 2P: columns 1..10 are playable, columns 0 and 11 hold TT_WALL.
 *   - Coop:    all 12 columns are playable. The ROM writes $00 instead of
 *              the wall nibbles in coop specifically to widen the field by
 *              one column on each side (its own comment says so at
 *              main.asm.txt:3483), so coop really is a 12-wide game.
 * ----------------------------------------------------------------------- */
#define TENGEN_PF_WIDTH  12 /* storage width; see the mode note above */
#define TENGEN_PF_HEIGHT 20 /* visible rows, = ROM rows 6..25 */

/* Piece coordinates are kept in the ROM's own space rather than remapped, so
 * they can be compared against the disassembly directly. Translate with:
 *   storage_column = piece.x + local_col - TENGEN_ROM_COL_ORIGIN
 *   visible_row    = piece.y + local_row - TENGEN_ROM_ROW_ORIGIN
 * A visible_row below 0 is the hidden area above the field, where pieces
 * spawn; it is legal to occupy and simply isn't drawn. */
#define TENGEN_ROM_COL_ORIGIN  2
#define TENGEN_ROM_ROW_ORIGIN  6  /* ROM row of the top visible playfield row */
#define TENGEN_ROM_FLOOR_ROW  26  /* first solid floor row (main.asm.txt:3496-3502) */

/* Piece ids match player1TetrominoCurrent's values 0-7 exactly
 * (tetris-ram.asm.txt:80; notes.txt lines 11-18). */
typedef enum {
    TT_NONE = 0,
    TT_I = 1,
    TT_T = 2,
    TT_O = 3,
    TT_J = 4,
    TT_L = 5,
    TT_S = 6,
    TT_Z = 7,
    TENGEN_TETROMINO_COUNT = 8,
    /* Not a piece: the sentinel the ROM keeps in the wall columns, and it
     * really is $F there — see the cell-value note below. Stored in the field
     * so collision and full-row checks need no mode-specific bounds logic,
     * exactly why the ROM does it that way. Renderers should treat it as
     * frame, not as a block. */
    TT_WALL = 15
} TengenTetromino;

/* WHAT A PLAYFIELD CELL HOLDS
 *
 * The same thing the ROM's nibble holds (notes.txt.txt:35, "nibble aligns
 * with tile index"): 0 for empty, 1..14 for one of the fourteen block tiles,
 * and 15 for a wall. It is NOT the piece id.
 *
 * That distinction is what makes settled blocks look right. The ROM's block
 * artwork is directional — a cell drawn with the tile for "top-left corner
 * of a piece" joins onto its neighbours, and a piece that locked as an
 * L-shape leaves a different set of tiles behind than the same cells would
 * as part of an S. Storing piece ids and picking a tile at draw time cannot
 * reproduce that, because by then the piece boundary is gone.
 *
 * The ROM discards the piece id at lock time and so does this core; nothing
 * needs it afterwards, since settled blocks are all drawn in one level-wide
 * palette (see reference/NOTES.md). */
#define TENGEN_CELL_EMPTY 0
#define TENGEN_CELL_WALL  15
#define TENGEN_CELL_IS_BLOCK(v) ((v) >= 1 && (v) <= 14)

/* Button bits match constants.asm.txt:1-8 exactly, so a GBA REG_KEYINPUT
 * read can be remapped to this bitmask with a single small lookup instead
 * of guessing button semantics. */
typedef enum {
    TENGEN_BTN_A      = 0x01,
    TENGEN_BTN_B      = 0x02,
    TENGEN_BTN_SELECT = 0x04,
    TENGEN_BTN_START  = 0x08,
    TENGEN_BTN_UP     = 0x10,
    TENGEN_BTN_DOWN   = 0x20,
    TENGEN_BTN_LEFT   = 0x40,
    TENGEN_BTN_RIGHT  = 0x80
} TengenButton;

/* Which player's own timing state to use for input DAS/auto-rotate/gravity.
 * Coop uses both; 1P/2P vs. use just PLAYER_1. Kept explicit rather than
 * inferring from mode, since that's how the ROM keys its per-player arrays
 * (tetris-ram.asm.txt: dasLeftPlayer1/2, autoRotateCounterP1/2, ...). */
typedef enum {
    TENGEN_PLAYER_1 = 0,
    TENGEN_PLAYER_2 = 1
} TengenPlayerSlot;

/*
 * 16-bit LFSR-style RNG state, split into two bytes to mirror rngSeed's
 * on-ROM layout (tetris-ram.asm.txt:37, $0034/$0035). Splitting it like
 * this (rather than a plain uint16_t) makes tengen_rng_step's translation
 * of the disassembled algorithm (main.asm.txt:3819, genNextPseudoRandom)
 * a direct, checkable transcription instead of a from-scratch reimplementation.
 */
typedef struct {
    uint8_t lo; /* mirrors rngSeed+0 */
    uint8_t hi; /* mirrors rngSeed+1 */
} TengenRng;

typedef struct {
    uint8_t cell[TENGEN_PF_HEIGHT][TENGEN_PF_WIDTH]; /* 0 = empty, else TengenTetromino id */
} TengenPlayfield;

typedef struct {
    TengenTetromino current;
    TengenTetromino next;
    uint8_t orientation; /* 0..3, matches player1TetrominoOrientation */
    int8_t x;            /* column of the piece's 4x4 bounding box, top-left */
    int8_t y;            /* row of the piece's 4x4 bounding box, top-left */
} TengenPiece;

/* Per-player input/timing state that the ROM keeps in the $01AA-$01B5
 * range (tetris-ram.asm.txt:109-120). */
typedef struct {
    uint8_t held_last_frame;
    uint8_t das_left;          /* dasLeftPlayer1/2 */
    uint8_t das_right;         /* dasRightPlayer1/2 */
    /* Names below reflect the *effect* each counter produces (CW/CCW), not
     * the ROM's own (confusingly reversed) variable names for them:
     * BTN_B increments orientation ("CW" here) via autoRotateCounterP1/2
     * (main.asm.txt:155); BTN_A decrements it ("CCW" here) via
     * autoRotateClockwiseP1/2 (main.asm.txt:171). */
    uint8_t auto_rotate_counter_b; /* BTN_B -> clockwise (orientation+1) */
    uint8_t auto_rotate_counter_a; /* BTN_A -> counter-clockwise (orientation-1) */
    uint8_t drop_repeat;       /* dropRepeatP1/2 */
    uint8_t drop_rate_possible;/* dropRatePossibleP1/2 */
    uint8_t fall_timer;        /* player1FallTimer/2 */
    TengenPiece piece;
    TengenRng rng;             /* player1RNGSeed/2 (own lookahead sequence) */
    uint32_t score;
    uint32_t lines;
    uint8_t level;
    uint8_t start_level;
    bool game_active;
    /* How many of each piece have been dealt, indexed by TengenTetromino
     * (slot 0 unused). The ROM keeps these at pieceStatsI..pieceStatsZ
     * ($0053-$0059) and drives the 1P screen's bar chart from them. Counted
     * on spawn, capped, and only tracked in 1P — see tengen_core.c. */
    uint8_t piece_stats[TENGEN_TETROMINO_COUNT];

    /* Line-clear animation state, mirroring lineClearTimerP1/2 ($01CE/$01CF).
     * While the timer runs the game is held still and the completed rows are
     * still standing in the field, so a renderer can animate them; the rows
     * collapse when it reaches zero. */
    uint8_t line_clear_timer;
    uint32_t clearing_rows;   /* bit i set => row i is completed and waiting */

    /* Cheat-code entry, mirroring $01B6-$01BB. See TENGEN_CHEAT_* below. */
    uint8_t code_input_y;       /* codeInputYPlayer1: offset into the code table */
    uint8_t long_bar_code_used; /* longBarCodeUsedP1: cleared on every level-up */
    uint8_t undo_code_used;     /* undoCodeUsedP1: cleared only on a new game */

    /* The snapshot the undo code restores, taken the instant a piece comes to
     * rest and before the next is dealt (L85B3, main.asm.txt:911-925). A line
     * clear wipes it (L94E4, main.asm.txt:3087-3092), which is what stops an
     * undo from putting back a piece whose row has already gone. */
    TengenTetromino last_piece;
    uint8_t last_orientation;
    int8_t last_x;
    int8_t last_y;
    TengenRng last_rng;
} TengenPlayerState;

/* main.asm.txt:1192-1197: the timer starts at $1D in 1P/2P and $21 in coop. */
#define TENGEN_LINE_CLEAR_FRAMES      29
#define TENGEN_LINE_CLEAR_FRAMES_COOP 33

/* THE SWEEP
 *
 * What the ROM does while that timer runs is not a placeholder pause: a puff
 * of smoke crosses each completed row from left to right, erasing it, and
 * leaves the word SINGLE / DOUBLE / TRIPLE / TETRIS written where the blocks
 * were. Drawing it is the renderer's job, but its timing is a rule, so it is
 * defined and tested here.
 *
 * stageLineClearAnimation (main.asm.txt:1274-1338) decrements the timer every
 * frame and then acts only on ODD values (`lsr a / bcc`), so the sweep
 * advances one column every OTHER frame — 14 steps for the 29-frame timer,
 * 16 for coop's 33.
 *
 * The puff is five 8x8 sprites (tiles $5B..$5F, drawn in piecePaletteIndexA,
 * which is flat black — main.asm.txt:5394-5396). L87FB (main.asm.txt:1230-1273)
 * stages the head at the row's leftmost column on the frame the row completes,
 * and each step the sprite still sitting at that column clones itself one
 * slot back with the next tile down, so the five build into a trail behind
 * the head. The trailing sprite is the one that writes a character into the
 * row it is passing over (L89E9, main.asm.txt:1508-1546). */
#define TENGEN_CLEAR_SPARKS 5  /* sprites in the trail, head included */
#define TENGEN_CLEAR_TRAIL  4  /* columns the writing tail lags the head by */

/* The ROM stops counting a piece at 144 (main.asm.txt:3755), which is where
 * its eight-tall bar chart runs out of room. */
#define TENGEN_PIECE_STAT_MAX 144

typedef struct {
    TengenPlayfield field[2];       /* player1Playfield / player2Playfield; coop shares [0] */
    TengenPlayerState player[2];
    bool coop;
    bool two_player;
    /* gameState == GAMESTATE_PAUSED. Start toggles it (pauseOrUnpause,
     * main.asm.txt:7184-7215) and it is where the cheat codes are entered. */
    bool paused;
} TengenGame;

/* ----------------------------------------------------------------------- *
 * The cheat codes (VERIFIED, checkCodeInput at main.asm.txt:7025-7182)
 *
 * Tengen's three famous button codes, entered WHILE PAUSED, one button per
 * frame:
 *
 *   level up:  Up Down Up Down Left Right B B A
 *   long bar:  Down Down Left Right Left Right B A
 *   undo:      Left Down Right Up Left Down Right B A
 *
 * Each has its own limit and its own quirk, all of them reproduced:
 *   - Level up adds one level (capped at 17) and is unlimited.
 *   - The long bar hands you an I piece and can be used once per level. Only
 *     levelling up by PLAY clears its "used" flag (main.asm.txt:3190); the
 *     level-up code above deliberately doesn't, so the two codes can't be
 *     alternated for an endless supply of long bars.
 *   - Undo takes back the last piece you dropped, ONCE per game, and only if
 *     no line has been cleared since.
 *   - After a code completes, the ROM leaves the match cursor on its last
 *     byte rather than resetting it, so pressing A again re-triggers the same
 *     code. That is why the level-up code repeats on a single button.
 * ----------------------------------------------------------------------- */
typedef enum {
    TENGEN_CHEAT_NONE = 0,
    TENGEN_CHEAT_LEVEL_UP,
    TENGEN_CHEAT_LONG_BAR,
    TENGEN_CHEAT_UNDO
} TengenCheat;

typedef struct {
    bool piece_locked;
    /* Set on the frame the completed rows are FOUND, which is when the
     * animation starts — not when they collapse. `rows_cleared_mask` names
     * them, and they stay in the field until `lines_collapsed`. */
    bool lines_cleared;
    uint32_t rows_cleared_mask; /* bit i set => row i is/was completed (needs >16 bits: TENGEN_PF_HEIGHT is 20) */
    bool lines_collapsed;       /* set on the frame the rows actually vanish */
    bool leveled_up;
    bool topped_out;
} TengenStepResult;

/* ----------------------------------------------------------------------- *
 * RNG (VERIFIED, main.asm.txt:3805-3840)
 * ----------------------------------------------------------------------- */
void tengen_rng_seed(TengenRng *rng, uint16_t seed);
/* Advances the LFSR by one step (one call = one genNextPseudoRandom). Returns
 * the new low byte, same as what the ROM leaves in A. */
uint8_t tengen_rng_step(TengenRng *rng);

/* ----------------------------------------------------------------------- *
 * Shape tables (VERIFIED, main.asm.txt:1104-1150)
 * ----------------------------------------------------------------------- */
/* True if tetromino `piece` in `orientation` (0..3) occupies local cell
 * (row, col) of its 4x4 bounding box (row/col each 0..3). */
bool tengen_piece_occupies(TengenTetromino piece, uint8_t orientation, int row, int col);

/* Sub-tile graphic id (1 of ~15 distinct joined-block tiles the ROM uses so
 * adjacent blocks of the same piece render as one smooth shape instead of
 * four separate squares) for the Nth occupied cell (0..3, in top-to-bottom,
 * left-to-right scan order) of `piece` in `orientation`. Used by the
 * renderer, not by collision logic. main.asm.txt:1125-1150. */
uint8_t tengen_tile_id_for_cell(TengenTetromino piece, uint8_t orientation, int occupied_index);

/* tetrominoXSpawnTable = {3, 9, 7} (main.asm.txt:3802). Read carefully, the
 * indexing is not what the layout suggests: getNextTetromino
 * (main.asm.txt:3716-3720) loads entry [2] = 7 for 1P *and* 2P, and only
 * falls back to indexing by player — 3 for P1, 9 for P2 — in coop, where
 * both players share one 12-wide field and need to start on opposite sides.
 * So single-field play always spawns dead centre at 7.
 *
 * Spawn row is always 4 (constants.asm.txt:63, TETROMINO_Y_INIT), which sits
 * two rows ABOVE the visible field (TENGEN_ROM_ROW_ORIGIN is 6) — pieces
 * genuinely slide in from off-screen in this game. */
extern const int8_t TENGEN_SPAWN_X[3];
#define TENGEN_SPAWN_Y 4

/* A piece that comes to rest with its bounding box still starting above the
 * visible field is a top-out (main.asm.txt:588-590, `cmp #$06`). */
#define TENGEN_TOPOUT_ROW TENGEN_ROM_ROW_ORIGIN

/* Cumulative lines needed to be AT level (start_level + i) for i in 0..20.
 * VERIFIED, main.asm.txt:1473-1478 (bonusLinesTable, decoded from ASCII digit
 * pairs "03","06","09","12","15","20","25",...,"95"). Beyond the last entry
 * the ROM's own start-level-relative indexing hasn't been traced yet (see
 * reference/NOTES.md); this core keeps adding +5 lines per level past 95,
 * which matches the visible pattern but is NOT yet confirmed against the ROM. */
extern const uint8_t TENGEN_LEVEL_LINE_THRESHOLDS[21];

/* ----------------------------------------------------------------------- *
 * DAS / auto-rotate timing constants (VERIFIED, main.asm.txt:96-183)
 * ----------------------------------------------------------------------- */
#define TENGEN_DAS_CHARGE_FIRST  11 /* frames held before the first auto-repeat shift */
#define TENGEN_DAS_CHARGE_REPEAT 6  /* frames between subsequent auto-repeat shifts */
#define TENGEN_AUTOROTATE_CHARGE 15 /* frames held before auto-rotate kicks in (fires every frame after) */

/* ----------------------------------------------------------------------- *
 * Gravity and soft drop (VERIFIED, main.asm.txt:184-216, 3970-4025)
 * ----------------------------------------------------------------------- */
/* The ROM's level counter tops out here — the level-up code clamps the
 * displayed level to "17" (main.asm.txt:3168-3170), which is exactly the
 * length of the fall-timer table. */
#define TENGEN_MAX_LEVEL 17

/* Soft-drop repeat threshold at piece spawn, and the (lower) value it resets
 * to whenever Down stops being held alone. main.asm.txt:3689-3691, 213-216. */
#define TENGEN_DROP_RATE_AT_SPAWN 20
#define TENGEN_DROP_RATE_AFTER_RELEASE 5

/* Frames the piece waits before gravity pulls it down one row, for a given
 * level. `piece_y` matters because levels 10-17 alternate between two table
 * entries based on the piece's row, giving effectively fractional gravity
 * (e.g. level 15 averages 3.5 frames/row); pass the piece's row BEFORE the
 * move, which is when the ROM reads it. Coop mode uses a separate, gentler
 * table. */
uint8_t tengen_frames_per_row(uint8_t level, int8_t piece_y, bool coop);

/* ----------------------------------------------------------------------- *
 * Game lifecycle
 * ----------------------------------------------------------------------- */
void tengen_new_game(TengenGame *game, uint16_t seed, uint8_t start_level, bool two_player, bool coop);

/* pauseOrUnpause (main.asm.txt:7184-7215) and the cheat-code entry it wraps
 * (checkCodeInput, :7025-7182). Call ONCE per frame, BEFORE stepping the
 * players, with each player's newly-pressed buttons (held & ~held_last_frame).
 * That mirrors the ROM: one routine reads both controllers, feeds each
 * player's own presses to its own code matcher, and then ORs the two for the
 * Start check — which is why either player can pause.
 *
 * `out_cheat` (may be NULL) receives what fired for each player this frame.
 * A game that isn't paused ignores the code input entirely, exactly as the
 * ROM does. */
void tengen_pause_input(TengenGame *game, const uint8_t new_presses[2],
                         TengenCheat out_cheat[2]);

/* Advances one player's piece by exactly one game frame (call at ~60Hz to
 * match the NES's ~60.1Hz / the GBA's ~59.7Hz — close enough that no frame
 * compensation is implemented; see reference/NOTES.md). `held_buttons` is the
 * full TengenButton bitmask currently held by that player. */
TengenStepResult tengen_step(TengenGame *game, TengenPlayerSlot slot, uint8_t held_buttons);

/* True if the piece's current position is legal (in bounds, not overlapping
 * a locked cell). Exposed for tests and for renderer ghost-piece previews. */
bool tengen_position_valid(const TengenGame *game, TengenPlayerSlot slot);

/* Attempts to shift the active piece by `dx` columns; reverts and returns
 * false if that would be an invalid position. */
bool tengen_try_move(TengenGame *game, TengenPlayerSlot slot, int dx);

/* Attempts to rotate the active piece using the verified single-left-kick
 * rule (main.asm.txt:538-575): try the new orientation in place; on failure
 * try it shifted one column left; on failure revert entirely. */
bool tengen_try_rotate(TengenGame *game, TengenPlayerSlot slot, bool clockwise);

/* Returns a bitmask of the field's completed rows without touching it. The
 * animation phase needs them named while they are still standing. */
uint32_t tengen_find_full_rows(const TengenPlayfield *field);

/* Removes the named rows and drops everything above them down, returning the
 * mask it acted on. Vacated rows at the top keep the frame's wall columns. */
uint32_t tengen_collapse_rows(TengenPlayfield *field, uint32_t mask);

/* Find and collapse in one step. Kept for callers that don't care about the
 * animation phase; tengen_step uses the two halves separately. */
uint32_t tengen_clear_full_rows(TengenPlayfield *field);

/* How many columns the line-clear sweep has advanced, 0 on the frame the rows
 * are found. See the SWEEP note above for what a renderer does with it. Zero
 * when no clear is running. */
uint8_t tengen_line_clear_step(const TengenGame *game, TengenPlayerSlot slot);

/* One cell of the active piece, already translated out of the ROM's
 * coordinate space into field storage indices. `row` is a visible row and
 * CAN BE NEGATIVE, meaning the cell is still in the hidden area above the
 * field — renderers should skip those rather than clamp them. */
typedef struct {
    int8_t row; /* visible row; < 0 means above the field */
    int8_t col; /* storage column, 0..TENGEN_PF_WIDTH-1 */
} TengenCell;

/* Fills `out` with the active piece's four occupied cells and returns how
 * many were written (always 4 for a real piece, 0 if there is none). Exists
 * so renderers don't have to re-derive the ROM-to-storage coordinate
 * mapping — getting that subtly wrong is exactly the kind of bug that only
 * shows up as "the piece draws one column off". */
int tengen_active_piece_cells(const TengenGame *game, TengenPlayerSlot slot,
                               TengenCell out[4]);

#ifdef __cplusplus
}
#endif

#endif /* TENGEN_CORE_H */
