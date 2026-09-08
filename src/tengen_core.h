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
 * Every rule implemented here is cross-referenced against the disassembly in
 * ../reference/disasm/main.asm.txt (the Tengen Tetris (NES) disassembly project,
 * see ../reference/disasm/README.md.txt). See ../reference/NOTES.md for the full
 * verified-vs-placeholder breakdown with ROM address citations. In short:
 *
 *   VERIFIED against the ROM (byte-exact or algorithm-exact):
 *     - Playfield size (10x20) and piece spawn position/orientation
 *     - All 7 tetromino orientation bitmaps + sub-tile id tables
 *     - The RNG algorithm and the "reroll on 0 of 8" piece selector
 *       (this is why Tengen can deal long S/Z droughts/streaks - there is no
 *       anti-repeat logic, unlike the Nintendo-published NES Tetris)
 *     - The DAS timing (11 frames to first repeat, 6 frames between repeats)
 *     - The "auto-rotate" feature bound to A/B (hold ~15 frames -> spins every
 *       frame while held)
 *     - The single-column-left-only wall kick (a real, well-documented Tengen
 *       quirk: rotation kicks one column left on failure, even when that makes
 *       no sense against the left wall; it never kicks right)
 *     - The cumulative lines-per-level table (3,6,9,12,15, then every 5)
 *
 *   PLACEHOLDER pending further disassembly work (flagged in this file and in
 *   tengen_core.c with TODO(verify) and a pointer to where to keep digging):
 *     - Exact gravity (frames-per-row) curve per level
 *     - Exact score awarded per line clear / soft drop
 *     - The finer points of soft-drop rate ramping (dropRatePossibleP1)
 *
 * This is intentional: shipping a wrong "verified" number is worse than a
 * clearly labeled placeholder. Tighten these as reference/disasm/main.asm.txt
 * gets fully traced (see reference/NOTES.md's TODO list).
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
    /* Not a piece: the sentinel the ROM keeps in the wall columns. Stored in
     * the field so collision and full-row checks need no mode-specific
     * bounds logic — exactly why the ROM does it that way. Renderers should
     * treat it as frame, not as a block. */
    TT_WALL = 8
} TengenTetromino;

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
} TengenPlayerState;

typedef struct {
    TengenPlayfield field[2];       /* player1Playfield / player2Playfield; coop shares [0] */
    TengenPlayerState player[2];
    bool coop;
    bool two_player;
} TengenGame;

typedef struct {
    bool piece_locked;
    bool lines_cleared;
    uint32_t rows_cleared_mask; /* bit i set => row i was cleared this step (needs >16 bits: TENGEN_PF_HEIGHT is 20) */
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

/* Scans the field for full rows, clears+collapses them, and returns a
 * bitmask of which rows (pre-collapse indices) were cleared. Does not by
 * itself update score/lines/level — tengen_step does that. */
uint32_t tengen_clear_full_rows(TengenPlayfield *field);

#ifdef __cplusplus
}
#endif

#endif /* TENGEN_CORE_H */
