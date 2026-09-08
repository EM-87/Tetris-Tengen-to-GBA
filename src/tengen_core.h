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
 * The ROM packs the playfield as one nibble per cell with sentinel F0/0F
 * wall columns and an FF floor row baked right into the same buffer the
 * collision check walks (notes.txt: "playfield is stored with 1 tile per
 * nibble ... walls are built with F0 on left, 0F on right. Floor is FF").
 * That trick exists purely to make the 6502 collision routine cheap; a
 * clean C port has no reason to replicate the bit-packing, so this core
 * uses one byte per cell and does explicit bounds checks instead. The
 * *behavior* (10x20 visible field, values 0=empty/1..7=locked piece id)
 * matches the ROM exactly.
 * ----------------------------------------------------------------------- */
#define TENGEN_PF_WIDTH  10
#define TENGEN_PF_HEIGHT 20

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
    TENGEN_TETROMINO_COUNT = 8
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

/* Spawn column by mode: index 0 = player 1 / 1P, 1 = player 2, 2 = coop.
 * VERIFIED, main.asm.txt:3802 (tetrominoXSpawnTable). Spawn row is always 4
 * (constants.asm.txt:63, TETROMINO_Y_INIT). */
extern const int8_t TENGEN_SPAWN_X[3];
#define TENGEN_SPAWN_Y 4

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
