/*
 * tengen_core.c — see tengen_core.h for the file-level explanation of what's
 * verified against the disassembly vs. placeholder.
 */
#include "tengen_core.h"
#include <string.h>

/* ----------------------------------------------------------------------- *
 * Shape tables — transcribed verbatim from main.asm.txt:1104-1150.
 *
 * Each orientation is a 4x4 grid packed into 2 bytes, 1 bit per cell,
 * row-major, MSB-first (notes.txt:52-63 spells this out with a worked
 * example for T-down: 0000/1110/0100/0000 == $0E,$40).
 *
 * Index: [piece][orientation][0..1]. TT_NONE has no shape (never rendered).
 * ----------------------------------------------------------------------- */
static const uint8_t kOrientationBitmap[TENGEN_TETROMINO_COUNT][4][2] = {
    /* TT_NONE */ { {0x00,0x00}, {0x00,0x00}, {0x00,0x00}, {0x00,0x00} },
    /* TT_I    */ { {0xF0,0x00}, {0x44,0x44}, {0xF0,0x00}, {0x44,0x44} },
    /* TT_T    */ { {0xE4,0x00}, {0x8C,0x80}, {0x4E,0x00}, {0x4C,0x40} },
    /* TT_O    */ { {0xCC,0x00}, {0xCC,0x00}, {0xCC,0x00}, {0xCC,0x00} },
    /* TT_J    */ { {0xE2,0x00}, {0xC8,0x80}, {0x8E,0x00}, {0x44,0xC0} },
    /* TT_L    */ { {0xE8,0x00}, {0x88,0xC0}, {0x2E,0x00}, {0xC4,0x40} },
    /* TT_S    */ { {0x6C,0x00}, {0x8C,0x40}, {0x6C,0x00}, {0x8C,0x40} },
    /* TT_Z    */ { {0xC6,0x00}, {0x4C,0x80}, {0xC6,0x00}, {0x4C,0x80} },
};

/* Sub-tile ids per occupied cell (scan order top-to-bottom, left-to-right),
 * verbatim from main.asm.txt:1125-1150 (tilesForI/T/O/J/L/S/Z). Purely
 * cosmetic — picks which of the ~15 joined-block graphics to draw so a
 * locked/falling piece reads as one smooth shape. Not used by collision. */
static const uint8_t kTileIds[TENGEN_TETROMINO_COUNT][4][4] = {
    /* TT_NONE */ {{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}},
    /* TT_I */ {{0x01,0x02,0x02,0x03},{0x04,0x05,0x05,0x06},{0x01,0x02,0x02,0x03},{0x04,0x05,0x05,0x06}},
    /* TT_T */ {{0x01,0x09,0x03,0x06},{0x04,0x08,0x03,0x06},{0x04,0x01,0x07,0x03},{0x04,0x01,0x0A,0x06}},
    /* TT_O */ {{0x0B,0x0E,0x0D,0x0C},{0x0B,0x0E,0x0D,0x0C},{0x0B,0x0E,0x0D,0x0C},{0x0B,0x0E,0x0D,0x0C}},
    /* TT_J */ {{0x01,0x02,0x0E,0x06},{0x0B,0x03,0x05,0x06},{0x04,0x0D,0x02,0x03},{0x04,0x05,0x01,0x0C}},
    /* TT_L */ {{0x0B,0x02,0x03,0x06},{0x04,0x05,0x0D,0x03},{0x04,0x01,0x02,0x0C},{0x01,0x0E,0x05,0x06}},
    /* TT_S */ {{0x0B,0x03,0x01,0x0C},{0x04,0x0D,0x0E,0x06},{0x0B,0x03,0x01,0x0C},{0x04,0x0D,0x0E,0x06}},
    /* TT_Z */ {{0x01,0x0E,0x0D,0x03},{0x04,0x0B,0x0C,0x06},{0x01,0x0E,0x0D,0x03},{0x04,0x0B,0x0C,0x06}},
};

const int8_t TENGEN_SPAWN_X[3] = { 3, 9, 7 }; /* main.asm.txt:3802 tetrominoXSpawnTable */

/* main.asm.txt:1473-1478, bonusLinesTable decoded from ASCII digit pairs. */
const uint8_t TENGEN_LEVEL_LINE_THRESHOLDS[21] = {
    3, 6, 9, 12, 15, 20, 25, 30, 35, 40,
    45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95
};

bool tengen_piece_occupies(TengenTetromino piece, uint8_t orientation, int row, int col) {
    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return false;
    if (row < 0 || row > 3 || col < 0 || col > 3) return false;
    orientation &= 3;
    int bit_index = row * 4 + col;           /* 0 = top-left, MSB-first row-major */
    const uint8_t *bytes = kOrientationBitmap[piece][orientation];
    uint16_t bits = ((uint16_t)bytes[0] << 8) | bytes[1];
    return (bits & (0x8000u >> bit_index)) != 0;
}

uint8_t tengen_tile_id_for_cell(TengenTetromino piece, uint8_t orientation, int occupied_index) {
    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return 0;
    if (occupied_index < 0 || occupied_index > 3) return 0;
    return kTileIds[piece][orientation & 3][occupied_index];
}

/* ----------------------------------------------------------------------- *
 * RNG — transcribed from the pseudocode comment directly above
 * genNextPseudoRandom (main.asm.txt:3812-3819) and cross-checked against
 * the 6502 that follows it. Byte1 == rng->lo (aliases ppuControl,x in the
 * ROM), byte2 == rng->hi (aliases ppuMask,x).
 * ----------------------------------------------------------------------- */
void tengen_rng_seed(TengenRng *rng, uint16_t seed) {
    rng->lo = (uint8_t)(seed & 0xFF);
    rng->hi = (uint8_t)((seed >> 8) & 0xFF);
}

uint8_t tengen_rng_step(TengenRng *rng) {
    uint8_t b1 = rng->lo;
    uint8_t b2 = rng->hi;
    uint8_t eor = (uint8_t)(b1 ^ b2);
    uint8_t new_bit;
    if (eor != 0) {
        new_bit = (uint8_t)((eor >> 6) & 1);
    } else {
        uint8_t diff = (uint8_t)(eor - b2); /* == -b2 since eor == 0 here */
        new_bit = (diff == 0) ? 1 : 0;      /* "if not eor and not diff: newbit = 1" */
    }
    uint8_t carry_out_of_b1 = (uint8_t)((b1 >> 7) & 1);
    rng->lo = (uint8_t)((b1 << 1) | new_bit);
    rng->hi = (uint8_t)((b2 << 1) | carry_out_of_b1);
    return rng->lo;
}

/* getNextTetromino (main.asm.txt:3688-3721): step the RNG 5 times
 * (genNextPseudoRandom5x), mask the result to 0..7, and reroll on 0 — a
 * plain 1-in-7 draw with NO anti-repeat logic. This is a deliberate,
 * well-documented Tengen quirk (unlike the Nintendo-published NES Tetris,
 * which rerolls on a repeat of the last piece too), and it's the reason
 * Tengen games can deal noticeably long same-piece or S/Z droughts. */
static TengenTetromino roll_next_piece(TengenRng *rng) {
    uint8_t v;
    do {
        for (int i = 0; i < 5; i++) {
            v = tengen_rng_step(rng);
        }
        v &= 0x07;
    } while (v == 0);
    return (TengenTetromino)v;
}

/* ----------------------------------------------------------------------- *
 * Collision / movement / rotation
 * ----------------------------------------------------------------------- */
bool tengen_position_valid(const TengenGame *game, TengenPlayerSlot slot) {
    const TengenPlayerState *p = &game->player[slot];
    const TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
    const TengenPiece *piece = &p->piece;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(piece->current, piece->orientation, r, c)) continue;
            int row = piece->y + r;
            int col = piece->x + c;
            if (col < 0 || col >= TENGEN_PF_WIDTH) return false;
            if (row >= TENGEN_PF_HEIGHT) return false;
            if (row >= 0 && field->cell[row][col] != TT_NONE) return false;
        }
    }
    return true;
}

bool tengen_try_move(TengenGame *game, TengenPlayerSlot slot, int dx) {
    TengenPiece *piece = &game->player[slot].piece;
    int8_t old_x = piece->x;
    piece->x = (int8_t)(piece->x + dx);
    if (tengen_position_valid(game, slot)) return true;
    piece->x = old_x;
    return false;
}

/* Verified against main.asm.txt:538-575 using checkPositionAndClearFlagsOnCarrySet's
 * actual carry convention (carry SET = valid position, traced via the $2D
 * sentinel logic at main.asm.txt:1053-1073: $2D stays negative/$FF — "no
 * collision found" — only when nothing overlapped, and that path is the one
 * that returns via `sec` i.e. carry set). With that convention the shipped
 * routine reads as:
 *   1. rotate in place; if valid, done (kept)
 *   2. else shift one column LEFT and try the same new orientation; if
 *      valid, done (kept, both the new orientation and the shift)
 *   3. else revert the orientation entirely (position untouched)
 * It never kicks right. This matches the documented Tengen behavior quoted
 * in notes.txt: "this game will wallkick one square to the left if basic
 * rotation fails" — including the (real, faithfully reproduced here) oddity
 * that it still only ever tries left, even flush against the left wall. */
bool tengen_try_rotate(TengenGame *game, TengenPlayerSlot slot, bool clockwise) {
    TengenPiece *piece = &game->player[slot].piece;
    uint8_t old_orientation = piece->orientation;
    uint8_t new_orientation = clockwise ? (uint8_t)((old_orientation + 1) & 3)
                                         : (uint8_t)((old_orientation - 1) & 3);

    piece->orientation = new_orientation;
    if (tengen_position_valid(game, slot)) return true;

    piece->x = (int8_t)(piece->x - 1);
    if (tengen_position_valid(game, slot)) return true;

    piece->x = (int8_t)(piece->x + 1);
    piece->orientation = old_orientation;
    return false;
}

uint32_t tengen_clear_full_rows(TengenPlayfield *field) {
    uint32_t mask = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        bool full = true;
        for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
            if (field->cell[row][col] == TT_NONE) { full = false; break; }
        }
        if (full) mask |= (1u << row);
    }
    if (mask == 0) return 0;

    /* Collapse: build a fresh field skipping cleared rows, matching the
     * ROM's plant-then-drop-remaining-rows behavior (main.asm.txt:856-908,
     * L8565, though re-implemented cleanly rather than nibble-by-nibble). */
    TengenPlayfield collapsed;
    memset(&collapsed, 0, sizeof(collapsed));
    int dst = TENGEN_PF_HEIGHT - 1;
    for (int row = TENGEN_PF_HEIGHT - 1; row >= 0; row--) {
        if (mask & (1u << row)) continue;
        memcpy(collapsed.cell[dst], field->cell[row], sizeof(collapsed.cell[dst]));
        dst--;
    }
    *field = collapsed;
    return mask;
}

static void lock_piece(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(p->piece.current, p->piece.orientation, r, c)) continue;
            int row = p->piece.y + r;
            int col = p->piece.x + c;
            if (row >= 0 && row < TENGEN_PF_HEIGHT && col >= 0 && col < TENGEN_PF_WIDTH) {
                field->cell[row][col] = (uint8_t)p->piece.current;
            }
        }
    }
}

static void spawn_piece(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    p->piece.current = p->piece.next;
    p->piece.next = roll_next_piece(&p->rng);
    p->piece.orientation = 0;
    p->piece.y = TENGEN_SPAWN_Y;
    int spawn_index = game->coop ? 2 : (int)slot;
    p->piece.x = TENGEN_SPAWN_X[spawn_index];
    /* main.asm.txt:3689-3691: both the fall timer and the soft-drop threshold
     * start at 20 on spawn, before the level's own gravity value takes over
     * on the first reload. */
    p->fall_timer = TENGEN_DROP_RATE_AT_SPAWN;
    p->drop_repeat = 0;
    p->drop_rate_possible = TENGEN_DROP_RATE_AT_SPAWN;
}

/* Gravity, VERIFIED against L9AEE (main.asm.txt:3970-4025).
 *
 * possibleFallTimerTable ($9B36) holds 18 entries, one per level 0..17 — and
 * 17 really is the ceiling: the level-up code clamps the displayed level to
 * '1','7' (main.asm.txt:3168-3170), which is exactly the length of this table.
 *
 * Note entry 15 (4 frames) is SLOWER than entry 14 (3 frames). That is not a
 * transcription slip — the ROM's bytes really do bump back up there, and the
 * level 14/15/16 masks below lean on it to produce their averages. Kept
 * as-is; "fixing" it would make the port less faithful, not more. */
static const uint8_t kFallTimerTable[18] = {
    0x21, 0x1C, 0x18, 0x14, 0x11, 0x0E, 0x0B, 0x09, /* 33 28 24 20 17 14 11 9 */
    0x07, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x04, /*  7  6  5  5  4  4  3 4 */
    0x03, 0x03                                       /*  3  3 */
};

/* Coop uses its own, gentler table (L9B48, $9B48). Same 18 levels, and it
 * stays strictly monotonic. */
static const uint8_t kFallTimerTableCoop[18] = {
    0x21, 0x1C, 0x18, 0x14, 0x12, 0x11, 0x10, 0x0F, /* 33 28 24 20 18 17 16 15 */
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, /* 14 13 12 11 10  9  8  7 */
    0x06, 0x05                                       /*  6  5 */
};

/* Fractional-gravity masks (L9B50, $9B50), indexed by level, only consulted
 * for levels >= 10. The ROM ANDs the piece's current Y with the mask and, on
 * the result, either uses table[level] or drops back to table[level-1] — so a
 * level can average a non-integer number of frames per row (e.g. level 15
 * alternates 4 and 3 for an effective 3.5). Note these bytes physically
 * overlap the tail of the coop fall-timer table above; that's the ROM
 * reusing the same bytes for two purposes, not an error here.
 *
 * Only indices 10..17 are ever read; 0..9 are filler so the index math
 * matches the ROM's without an offset. */
static const uint8_t kFractionalGravityMask[18] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x01, 0x00, 0x01, 0x00, 0x03, 0x01, 0x03, 0x00
};

/* Reproduces L9AEE: pick the fall-timer reload value for this level, taking
 * the piece's current row into account for the fractional levels. The ROM
 * calls this BEFORE moving the piece down, so callers must pass the piece's
 * pre-move Y to stay frame-accurate. */
uint8_t tengen_frames_per_row(uint8_t level, int8_t piece_y, bool coop) {
    if (level > TENGEN_MAX_LEVEL) level = TENGEN_MAX_LEVEL;
    const uint8_t *table = coop ? kFallTimerTableCoop : kFallTimerTable;

    uint8_t index = level;
    if (level >= 10 && !coop) {
        /* main.asm.txt:3985-4000. The polarity of the test flips between the
         * 10-15 band and the 16+ band, which is why this reads as two cases
         * rather than one. */
        uint8_t masked = (uint8_t)piece_y & kFractionalGravityMask[level];
        if (level >= 16) {
            if (masked == 0 && index > 0) index--;
        } else {
            if (masked != 0 && index > 0) index--;
        }
    }
    return table[index];
}

void tengen_new_game(TengenGame *game, uint16_t seed, uint8_t start_level, bool two_player, bool coop) {
    memset(game, 0, sizeof(*game));
    game->two_player = two_player;
    game->coop = coop;

    TengenRng shared;
    tengen_rng_seed(&shared, seed);

    for (int i = 0; i < 2; i++) {
        TengenPlayerState *p = &game->player[i];
        p->rng = shared; /* main.asm.txt:3319-3326: both players' lookahead RNGs start from the same seed */
        p->start_level = start_level;
        p->level = start_level;
        p->game_active = (i == 0) || two_player || coop;
        p->piece.next = roll_next_piece(&p->rng); /* pre-roll so spawn_piece's first "current = next" is meaningful */
        spawn_piece(game, (TengenPlayerSlot)i);
    }
}

/* main.asm.txt:108-150. Two things worth spelling out, because both are easy
 * to get subtly wrong:
 *
 *  - A shift fires from two independent sources OR'd together: the fresh
 *    press (edge), and the DAS repeat. The caller passes the already-filtered
 *    edge bits in `new_presses`.
 *  - DAS only charges while the direction is held AND Down is NOT
 *    (`and #BUTTON_DOWN+BUTTON_LEFT; cmp #BUTTON_LEFT`). Holding Down+Left
 *    zeroes the counter outright. The counter also increments on the press
 *    frame itself, which is why the first repeat lands on the 11th frame of
 *    the hold rather than the 11th frame after it. */
static void apply_das(uint8_t held, uint8_t new_presses, TengenButton dir_btn,
                       uint8_t *das_counter, bool *fire) {
    *fire = (new_presses & dir_btn) != 0;
    if ((held & dir_btn) && !(held & TENGEN_BTN_DOWN)) {
        (*das_counter)++;
        if (*das_counter >= TENGEN_DAS_CHARGE_FIRST) {
            *fire = true;
            /* Reloads to 5, not 0 (main.asm.txt:124) — that's what makes the
             * repeat interval 6 frames while the initial charge is 11. */
            *das_counter = TENGEN_DAS_CHARGE_FIRST - TENGEN_DAS_CHARGE_REPEAT;
        }
    } else {
        *das_counter = 0;
    }
}

static void apply_autorotate(uint8_t held, uint8_t new_presses, TengenButton btn,
                              uint8_t *counter, bool *fire) {
    *fire = false;
    if (!(held & btn)) { *counter = 0; return; }
    if (new_presses & btn) *fire = true;
    (*counter)++;
    if (*counter >= TENGEN_AUTOROTATE_CHARGE) *fire = true; /* verified: no reload, fires every frame once charged */
}

TengenStepResult tengen_step(TengenGame *game, TengenPlayerSlot slot, uint8_t held_buttons) {
    TengenStepResult result;
    memset(&result, 0, sizeof(result));

    TengenPlayerState *p = &game->player[slot];
    if (!p->game_active) return result;

    uint8_t new_presses = (uint8_t)(held_buttons & ~p->held_last_frame);

    /* Down is never edge-triggered — it only ever acts through the soft-drop
     * repeat counter (main.asm.txt:82-83 masks it out of the new-press set). */
    new_presses &= (uint8_t)~TENGEN_BTN_DOWN;

    /* main.asm.txt:98-107: a fresh Left/Right press is discarded outright if
     * Down was held on the previous frame. In practice this means you cannot
     * start a horizontal move on the frame you stop soft-dropping — a real
     * quirk of Tengen's input handling, not an emulation artifact. */
    if ((new_presses & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) &&
        (p->held_last_frame & TENGEN_BTN_DOWN)) {
        new_presses &= (uint8_t)~(TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT);
    }

    /* Order mirrors doSomethingWithInputDuringGameplay: left, right, then
     * B (cw) rotate, then A (ccw) rotate (main.asm.txt:96-183, 538-575). */
    bool move_left, move_right, rotate_cw, rotate_ccw;
    apply_das(held_buttons, new_presses, TENGEN_BTN_LEFT, &p->das_left, &move_left);
    apply_das(held_buttons, new_presses, TENGEN_BTN_RIGHT, &p->das_right, &move_right);
    apply_autorotate(held_buttons, new_presses, TENGEN_BTN_B, &p->auto_rotate_counter_b, &rotate_cw);
    apply_autorotate(held_buttons, new_presses, TENGEN_BTN_A, &p->auto_rotate_counter_a, &rotate_ccw);

    if (move_left) tengen_try_move(game, slot, -1);
    if (move_right) tengen_try_move(game, slot, 1);
    if (rotate_cw) tengen_try_rotate(game, slot, true);
    if (rotate_ccw) tengen_try_rotate(game, slot, false);

    /* Soft drop (main.asm.txt:184-216) and natural gravity
     * (main.asm.txt:502-513 + L9AEE) are two separate paths that can each
     * pull the piece down one row.
     *
     * Soft drop engages ONLY while Down is held with neither Left nor Right
     * (`and #DOWN+LEFT+RIGHT; cmp #DOWN`). Every time it fires it also
     * tightens its own threshold by one (floored at 1), so holding Down
     * accelerates: the first step waits 20 frames, then 19, 18, and so on.
     * Letting go — or pressing a direction — resets the threshold to 5,
     * which is why a second soft drop on the same piece bites much faster
     * than the first. */
    bool gravity_tick = false;
    bool soft_dropping = (held_buttons & (TENGEN_BTN_DOWN | TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT))
                          == TENGEN_BTN_DOWN;

    if (soft_dropping) {
        p->drop_repeat++;
        if (p->drop_repeat >= p->drop_rate_possible) {
            gravity_tick = true;
            if (p->drop_rate_possible >= 2) p->drop_rate_possible--;
            p->drop_repeat = 0;
        }
    } else {
        p->drop_repeat = 0;
        p->drop_rate_possible = TENGEN_DROP_RATE_AFTER_RELEASE;
    }

    /* Natural gravity runs in parallel with soft drop rather than instead of
     * it: the ROM decrements this counter every frame in L8320 and OR's its
     * result into the same "move down" bit the soft drop sets, so a frame
     * where both fire still moves the piece exactly one row. */
    if (p->fall_timer > 0) p->fall_timer--;
    if (p->fall_timer == 0) gravity_tick = true;

    if (gravity_tick) {
        /* L9AEE reloads the fall timer from the level's gravity table using
         * the piece's row BEFORE it moves, and clamps the soft-drop
         * threshold so soft dropping is never slower than plain gravity
         * (main.asm.txt:4008-4011). */
        uint8_t reload = tengen_frames_per_row(p->level, p->piece.y, game->coop);
        p->fall_timer = reload;
        if (reload < p->drop_rate_possible) p->drop_rate_possible = reload;
    }

    if (gravity_tick) {
        p->piece.y++;
        if (!tengen_position_valid(game, slot)) {
            p->piece.y--;
            lock_piece(game, slot);
            result.piece_locked = true;

            TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
            uint32_t cleared = tengen_clear_full_rows(field);
            if (cleared) {
                int count = 0;
                for (int i = 0; i < TENGEN_PF_HEIGHT; i++) if (cleared & (1u << i)) count++;
                p->lines += (uint32_t)count;
                result.lines_cleared = true;
                result.rows_cleared_mask = cleared;

                /* TODO(verify): placeholder scoring — see reference/NOTES.md.
                 * Uses the classic 40/100/300/1200 * (level+1) shape as a
                 * reasonable stand-in until main.asm.txt's score-add routine
                 * (around line 3900, near player1ScoreOnes) is traced. */
                static const uint16_t base_score[5] = {0, 40, 100, 300, 1200};
                p->score += (uint32_t)base_score[count] * (p->level + 1);

                uint8_t idx = (uint8_t)(p->level - p->start_level);
                if (idx < sizeof(TENGEN_LEVEL_LINE_THRESHOLDS) / sizeof(TENGEN_LEVEL_LINE_THRESHOLDS[0]) &&
                    p->lines >= TENGEN_LEVEL_LINE_THRESHOLDS[idx] &&
                    p->level < TENGEN_MAX_LEVEL) { /* the ROM clamps at 17, main.asm.txt:3168-3170 */
                    p->level++;
                    result.leveled_up = true;
                }
            }

            spawn_piece(game, slot);
            if (!tengen_position_valid(game, slot)) {
                p->game_active = false;
                result.topped_out = true;
            }
        }
    }

    p->held_last_frame = held_buttons;
    return result;
}
