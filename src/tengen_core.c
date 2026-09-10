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

/* main.asm.txt:1473-1478, bonusLinesTable — ASCII digit pairs, and WHICH TWO
 * DIGITS decides everything about this game's pacing.
 *
 * The bytes are $30,$33 / $30,$36 / $30,$39 / $31,$32 / $31,$35 / $32,$30 …
 * $39,$35: "03", "06", "09", "12", "15", "20" … "95". Read as tens-and-ones
 * that is 3, 6, 9, 12, 15, 20 … 95 lines, and this port had exactly that —
 * a level every three lines, which is not the game anybody remembers.
 *
 * They are not tens and ones. Both places that use the table compare them
 * against player1LinesHUNDREDS and player1LinesTENS (:1482-1487 for the show,
 * :3145-3151 for the level itself) — the hundreds and tens digits of the line
 * counter, as a two-digit number, with the ones digit never entering it. So
 * "03" is 03X, the first line total whose tens digit is 3: THIRTY. The
 * cartridge's own comment above the table says so in as many words ("first
 * check at X03X, then every 30 lines until X150 at which point it's every 50
 * lines until X95X"), and the decoded table below is that sentence:
 *
 *     30 60 90 120 150, then 200 250 300 … 950
 *
 * Twenty-one entries, which is where the $2A (42 bytes) the ROM wraps its
 * cursor at comes from. */
const uint8_t TENGEN_LEVEL_LINE_TENS[21] = {
     3,  6,  9, 12, 15, 20, 25, 30, 35, 40,
    45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95
};
const uint16_t TENGEN_LEVEL_LINE_THRESHOLDS[21] = {
     30,  60,  90, 120, 150, 200, 250, 300, 350, 400,
    450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 950
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
/* Shared by tengen_position_valid and the lock path. When a collision is
 * found, `lowest_hit_row` (if non-NULL) receives the ROM row of the LOWEST
 * colliding cell — the ROM's scan writes its hit index to $2D as it goes, so
 * the last write wins, and that value is what the scoring routine consumes
 * (main.asm.txt:1029-1064). */
static bool position_valid_ex(const TengenGame *game, TengenPlayerSlot slot,
                               int *lowest_hit_row) {
    const TengenPlayerState *p = &game->player[slot];
    const TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
    const TengenPiece *piece = &p->piece;
    bool ok = true;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(piece->current, piece->orientation, r, c)) continue;

            int rom_row = piece->y + r;
            int storage_col = piece->x + c - TENGEN_ROM_COL_ORIGIN;
            bool hit = false;

            if (rom_row >= TENGEN_ROM_FLOOR_ROW) {
                hit = true; /* the ROM's solid floor rows */
            } else if (storage_col < 0 || storage_col >= TENGEN_PF_WIDTH) {
                hit = true; /* past the buffer's padding nibbles entirely */
            } else {
                int visible_row = rom_row - TENGEN_ROM_ROW_ORIGIN;
                if (visible_row >= 0) {
                    if (field->cell[visible_row][storage_col] != TT_NONE) {
                        hit = true; /* a locked block, or a TT_WALL sentinel */
                    }
                } else if (!game->coop &&
                           (storage_col == 0 ||
                            storage_col == TENGEN_PF_WIDTH - 1)) {
                    /* THE WALLS DO NOT STOP AT THE TOP OF THE VISIBLE FIELD.
                     *
                     * The ROM's playfield is one flat array of 8-byte rows
                     * starting at row 0, and `L89C3` (main.asm.txt:1481-1503)
                     * writes the $F0/$0F wall nibbles into EVERY row it
                     * builds — the two rows a piece spawns in included. Only
                     * coop leaves them clear, which is the `bit playMode`
                     * there and is what widens its field to twelve.
                     *
                     * This core only stores the twenty VISIBLE rows, so the
                     * spawn rows have to say so themselves. Treating them as
                     * open space instead was a real bug and a nasty one: a
                     * piece could be walked sideways into the wall column
                     * while it was still above the field, and then the first
                     * row it descended into blocked it — resting at y=5, one
                     * short of TENGEN_TOPOUT_ROW, which ends the game. Four
                     * pieces into an empty board, GAME OVER, and nothing on
                     * screen to explain it. */
                    hit = true;
                }
            }

            if (hit) {
                ok = false;
                if (lowest_hit_row == NULL) return false;
                if (rom_row > *lowest_hit_row) *lowest_hit_row = rom_row;
            }
        }
    }
    return ok;
}

bool tengen_position_valid(const TengenGame *game, TengenPlayerSlot slot) {
    return position_valid_ex(game, slot, NULL);
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

uint32_t tengen_find_full_rows(const TengenPlayfield *field) {
    uint32_t mask = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        bool full = true;
        /* Scanning all 12 stored columns works for both modes without a
         * special case: in 1P/2P the two wall columns hold TT_WALL and are
         * therefore always "occupied", so the test reduces to the ten
         * playable cells; in coop there are no walls and all twelve count. */
        for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
            if (field->cell[row][col] == TT_NONE) { full = false; break; }
        }
        if (full) mask |= (1u << row);
    }
    return mask;
}

uint32_t tengen_collapse_rows(TengenPlayfield *field, uint32_t mask) {
    if (mask == 0) return 0;

    /* Collapse: build a fresh field skipping cleared rows, matching the
     * ROM's plant-then-drop-remaining-rows behavior (main.asm.txt:856-908,
     * L8565, though re-implemented cleanly rather than nibble-by-nibble).
     * Rows vacated at the top are refilled from row 0's wall pattern so the
     * frame survives a clear. */
    uint8_t empty_row[TENGEN_PF_WIDTH];
    for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
        empty_row[col] = (field->cell[0][col] == TT_WALL) ? (uint8_t)TT_WALL
                                                          : (uint8_t)TT_NONE;
    }

    TengenPlayfield collapsed;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        memcpy(collapsed.cell[row], empty_row, sizeof(empty_row));
    }

    int dst = TENGEN_PF_HEIGHT - 1;
    for (int row = TENGEN_PF_HEIGHT - 1; row >= 0; row--) {
        if (mask & (1u << row)) continue;
        memcpy(collapsed.cell[dst], field->cell[row], sizeof(collapsed.cell[dst]));
        dst--;
    }
    *field = collapsed;
    return mask;
}

uint32_t tengen_clear_full_rows(TengenPlayfield *field) {
    return tengen_collapse_rows(field, tengen_find_full_rows(field));
}

/* ----------------------------------------------------------------------- *
 * Cheat codes (VERIFIED, checkCodeInput at main.asm.txt:7025-7182)
 *
 * The three codes are ONE array here, in the ROM's own order and with its
 * terminators, because the matcher's behaviour depends on that: it walks a
 * single cursor through this table (`codeInputYPlayer1`) and tells the codes
 * apart purely by where the cursor is when it hits a 0. Splitting them into
 * three separate arrays would look tidier and would quietly change how a
 * half-entered code behaves.
 *
 * Offsets, straight from the ROM's addresses ($B5A8 / $B5B2 / $B5BB):
 *   levelUpCode     0
 *   getLongbarCode 10 ($0A), terminator at 18 ($12)
 *   undoCode       19 ($13), terminator at 28 ($1C)
 * ----------------------------------------------------------------------- */
#define CODE_LONGBAR_START 0x0A
#define CODE_LONGBAR_END   0x12
#define CODE_UNDO_START    0x13
#define CODE_UNDO_END      0x1C

static const uint8_t kCheatCodes[] = {
    /* levelUpCode: up, down, up, down, left, right, b, b, a */
    TENGEN_BTN_UP, TENGEN_BTN_DOWN, TENGEN_BTN_UP, TENGEN_BTN_DOWN,
    TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT, TENGEN_BTN_B, TENGEN_BTN_B,
    TENGEN_BTN_A, 0,
    /* getLongbarCode: down, down, left, right, left, right, b, a */
    TENGEN_BTN_DOWN, TENGEN_BTN_DOWN, TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT,
    TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT, TENGEN_BTN_B, TENGEN_BTN_A, 0,
    /* undoCode: left, down, right, up, left, down, right, b, a */
    TENGEN_BTN_LEFT, TENGEN_BTN_DOWN, TENGEN_BTN_RIGHT, TENGEN_BTN_UP,
    TENGEN_BTN_LEFT, TENGEN_BTN_DOWN, TENGEN_BTN_RIGHT, TENGEN_BTN_B,
    TENGEN_BTN_A, 0,
};

/* The level-up code's own level bump. Separate from the one checkLevelUp
 * does, and deliberately different: it does NOT refresh the long bar
 * (main.asm.txt:7051-7070 vs :3186-3191). */
static void cheat_level_up(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    /* main.asm.txt:7059-7067 refuses to store a level of 18, digit by digit;
     * with the cap at 17 that is exactly this. */
    if (p->level >= TENGEN_MAX_LEVEL) return;
    p->level++;
    if (game->coop) game->player[slot ^ 1].level = p->level;
    p->fall_timer = 60; /* main.asm.txt:7075, `lda #$3C` */
}

/* @spawnReplacementTetromino (main.asm.txt:7113-7128): both the long bar and
 * the undo end here, dropping `current` in at the top with a fresh 60-frame
 * fall timer. It does NOT roll a new piece or touch the statistics. */
static void cheat_respawn_current(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    p->piece.y = TENGEN_SPAWN_Y;
    p->piece.x = game->coop ? TENGEN_SPAWN_X[slot] : TENGEN_SPAWN_X[2];
    p->piece.orientation = 0;
    p->fall_timer = 60;
}

/* Takes the last locked piece back out of the field, using the snapshot of
 * where it came to rest. That is what the ROM's L84D8/L8607/L8565/L8426
 * sequence does (main.asm.txt:7166-7169): it re-reads the window around the
 * restored position, zeroes the piece's own cells and writes the result back
 * into the playfield. */
static void erase_snapshot_piece(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            if (!tengen_piece_occupies(p->last_piece, p->last_orientation, row, col))
                continue;
            int f_row = p->last_y + row - TENGEN_ROM_ROW_ORIGIN;
            int f_col = p->last_x + col - TENGEN_ROM_COL_ORIGIN;
            if (f_row < 0 || f_row >= TENGEN_PF_HEIGHT) continue;
            if (f_col < 0 || f_col >= TENGEN_PF_WIDTH) continue;
            field->cell[f_row][f_col] = TENGEN_CELL_EMPTY;
        }
    }
}

/* @applyUndoCode, main.asm.txt:7131-7170. */
static bool cheat_undo(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    if (p->undo_code_used) return false;
    if (p->last_piece == TT_NONE) return false;  /* a line clear wiped it */
    p->undo_code_used++;

    /* The piece you were holding becomes the next one. The ROM guards this
     * with `beq` and leaves a "todo: look into why current piece id might be
     * 0" comment at :7154 — reproduced as the same guard, not as a fix. */
    if (p->piece.current != TT_NONE) p->piece.next = p->piece.current;

    p->piece.orientation = p->last_orientation;
    p->piece.x = p->last_x;
    p->piece.y = p->last_y;
    p->piece.current = p->last_piece;
    p->rng = p->last_rng;   /* rewinds the lookahead too */

    erase_snapshot_piece(game, slot);
    cheat_respawn_current(game, slot);
    return true;
}

/* @applyLongbarCode, main.asm.txt:7107-7112. */
static bool cheat_long_bar(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    if (p->long_bar_code_used) return false;
    p->long_bar_code_used = 1;
    p->piece.current = TT_I;    /* `sta player1TetrominoCurrent,x` with A = 1 */
    cheat_respawn_current(game, slot);
    return true;
}

static TengenCheat check_code_input(TengenGame *game, TengenPlayerSlot slot,
                                     uint8_t new_presses) {
    TengenPlayerState *p = &game->player[slot];
    if (!p->game_active) return TENGEN_CHEAT_NONE;
    if (new_presses == 0) return TENGEN_CHEAT_NONE;

    uint8_t y = p->code_input_y;
    if (y == 0) {
        /* From a standing start the first press is tested against all three
         * codes; a match jumps the cursor straight to that code's second
         * byte (main.asm.txt:7033-7047). */
        if (new_presses == kCheatCodes[CODE_UNDO_START]) {
            p->code_input_y = CODE_UNDO_START + 1;
            return TENGEN_CHEAT_NONE;
        }
        if (new_presses == kCheatCodes[CODE_LONGBAR_START]) {
            p->code_input_y = CODE_LONGBAR_START + 1;
            return TENGEN_CHEAT_NONE;
        }
    }

    if (new_presses != kCheatCodes[y]) {
        /* The press that breaks a sequence is swallowed: the ROM stores 0 and
         * returns without re-testing it against the code starts. */
        p->code_input_y = 0;
        return TENGEN_CHEAT_NONE;
    }

    y++;
    if (kCheatCodes[y] != 0) {
        p->code_input_y = y;
        return TENGEN_CHEAT_NONE;
    }

    /* Complete. Note what is NOT done here: the cursor is left where it was,
     * so the code's last button on its own re-triggers it (main.asm.txt:7098
     * is only reached from the partial-match paths). */
    if (y == CODE_LONGBAR_END)
        return cheat_long_bar(game, slot) ? TENGEN_CHEAT_LONG_BAR : TENGEN_CHEAT_NONE;
    if (y == CODE_UNDO_END)
        return cheat_undo(game, slot) ? TENGEN_CHEAT_UNDO : TENGEN_CHEAT_NONE;
    cheat_level_up(game, slot);
    return TENGEN_CHEAT_LEVEL_UP;
}

void tengen_pause_input(TengenGame *game, const uint8_t new_presses[2],
                         TengenCheat out_cheat[2]) {
    TengenCheat fired[2] = { TENGEN_CHEAT_NONE, TENGEN_CHEAT_NONE };

    /* Codes first, then the Start check — the ROM's order, and it matters:
     * the Start press that unpauses also runs through the matcher, where it
     * matches nothing and resets the cursor. */
    if (game->paused) {
        for (int i = 0; i < 2; i++)
            fired[i] = check_code_input(game, (TengenPlayerSlot)i, new_presses[i]);
    }

    if ((new_presses[0] | new_presses[1]) & TENGEN_BTN_START)
        game->paused = !game->paused;

    if (out_cheat) { out_cheat[0] = fired[0]; out_cheat[1] = fired[1]; }
}

uint8_t tengen_line_clear_step(const TengenGame *game, TengenPlayerSlot slot) {
    const TengenPlayerState *p = &game->player[slot];
    if (p->line_clear_timer == 0) return 0;
    /* main.asm.txt:1279-1283: the timer is decremented every frame but the
     * sweep only moves when what's left is odd, so it advances once per two
     * frames. Counting from the full timer gives the same column the ROM's
     * sprite would be on. */
    uint8_t total = game->coop ? TENGEN_LINE_CLEAR_FRAMES_COOP
                                : TENGEN_LINE_CLEAR_FRAMES;
    return (uint8_t)((total - p->line_clear_timer) / 2);
}

int tengen_active_piece_cells(const TengenGame *game, TengenPlayerSlot slot,
                               TengenCell out[4]) {
    const TengenPiece *piece = &game->player[slot].piece;
    if (piece->current <= TT_NONE || piece->current >= TENGEN_TETROMINO_COUNT) return 0;

    int written = 0;
    for (int r = 0; r < 4 && written < 4; r++) {
        for (int c = 0; c < 4 && written < 4; c++) {
            if (!tengen_piece_occupies(piece->current, piece->orientation, r, c)) continue;
            out[written].row = (int8_t)(piece->y + r - TENGEN_ROM_ROW_ORIGIN);
            out[written].col = (int8_t)(piece->x + c - TENGEN_ROM_COL_ORIGIN);
            written++;
        }
    }
    return written;
}

/* Plants the piece into the field, storing each cell's BLOCK TILE ID rather
 * than the piece id — see the cell-value note in tengen_core.h for why that
 * matters. Mirrors L85B3/L8565 (main.asm.txt:911-935, 856-908), which walks
 * the same scan order pulling ids out of the piece's tile table. */
static void lock_piece(TengenGame *game, TengenPlayerSlot slot) {
    TengenPlayerState *p = &game->player[slot];
    TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
    int occupied_index = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(p->piece.current, p->piece.orientation, r, c)) continue;
            uint8_t tile = tengen_tile_id_for_cell(p->piece.current, p->piece.orientation,
                                                    occupied_index);
            occupied_index++;

            int visible_row = p->piece.y + r - TENGEN_ROM_ROW_ORIGIN;
            int storage_col = p->piece.x + c - TENGEN_ROM_COL_ORIGIN;
            /* Cells still above the field simply aren't stored — they're off
             * the top of the buffer, which is what makes a high lock a
             * top-out rather than a write out of bounds. */
            if (visible_row >= 0 && visible_row < TENGEN_PF_HEIGHT &&
                storage_col >= 0 && storage_col < TENGEN_PF_WIDTH) {
                field->cell[visible_row][storage_col] = tile;
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
    /* main.asm.txt:3716-3720: 1P and 2P both spawn centred at entry [2];
     * only coop indexes the table by player so the two share a wide field. */
    p->piece.x = game->coop ? TENGEN_SPAWN_X[slot] : TENGEN_SPAWN_X[2];
    /* main.asm.txt:3689-3691: both the fall timer and the soft-drop threshold
     * start at 20 on spawn, before the level's own gravity value takes over
     * on the first reload. */
    p->fall_timer = TENGEN_DROP_RATE_AT_SPAWN;
    p->drop_repeat = 0;
    p->drop_rate_possible = TENGEN_DROP_RATE_AT_SPAWN;

    /* Piece statistics (main.asm.txt:3730-3797, the tail of getNextTetromino).
     * Counted as the piece is DEALT, not as it locks, and only in 1P: the ROM
     * checks `menuGameMode` and skips the whole routine for 2P/coop/vs, which
     * is why only the 1P screen has a stats panel. The count saturates rather
     * than wrapping. */
    if (!game->two_player && !game->coop &&
        p->piece.current > TT_NONE && p->piece.current < TENGEN_TETROMINO_COUNT) {
        if (p->piece_stats[p->piece.current] < TENGEN_PIECE_STAT_MAX) {
            p->piece_stats[p->piece.current]++;
        }
    }
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

/* Level, VERIFIED against main.asm.txt:3140-3186.
 *
 * The ROM does not increment the level a step at a time — it recomputes it
 * from scratch on every line clear as `start_level + (number of
 * bonusLinesTable thresholds the running line total has reached)`, walking
 * the table until the first threshold it hasn't reached, then only commits
 * the result if it's higher than the current level. Recomputing rather than
 * incrementing matters: a clear that crosses two thresholds at once (say a
 * tetris taking the total from 2 to 6) advances two levels, which a
 * one-step-per-clear implementation would silently get wrong.
 *
 * The ones digit is clamped at '7' (main.asm.txt:3168-3170), i.e. level 17.
 *
 * Not modelled: in demo/title states the ROM substitutes a flat +10 for the
 * start level (main.asm.txt:3157-3160). That path never runs during play. */
static uint8_t level_for_lines(uint32_t lines, uint8_t start_level) {
    const unsigned count = sizeof(TENGEN_LEVEL_LINE_THRESHOLDS) /
                            sizeof(TENGEN_LEVEL_LINE_THRESHOLDS[0]);
    /* The ROM's compare is on the hundreds and tens digits only (:3145-3151),
     * so `lines >= T` with T already in lines is the same test as its
     * `lines/10 >= T/10` — the ones digit cannot change the answer, because
     * every threshold is a multiple of ten. */
    unsigned passed = 0;
    while (passed < count && lines >= TENGEN_LEVEL_LINE_THRESHOLDS[passed]) {
        passed++;
    }
    unsigned level = (unsigned)start_level + passed;
    if (level > TENGEN_MAX_LEVEL) level = TENGEN_MAX_LEVEL;
    return (uint8_t)level;
}

/* Scoring, VERIFIED against L9A47 (main.asm.txt:3874-3893), its shift-add
 * multiply L98D7 (main.asm.txt:3632-3685), the doubling pass L9A17
 * (main.asm.txt:3843-3871) and the digit-wise accumulate L9A6A
 * (main.asm.txt:3894-3948).
 *
 * Three things about this differ from what "Tetris scoring" usually means,
 * and all three are real:
 *
 *  1. Points are awarded **per piece locked**, not per line cleared. Clearing
 *     lines is worth nothing directly; it's worth something because it keeps
 *     you alive to lock more pieces.
 *  2. The reward grows the HIGHER the piece comes to rest. $2D is the number
 *     of rows between the obstruction and the floor, so building tall is what
 *     pays — the opposite of a drop-distance bonus.
 *  3. A fully-accelerated soft drop (threshold down to 1) doubles the award.
 *
 * The award is computed as (level+1) * ((level+1) + rows_above_floor). The
 * level term reads oddly in the ROM — ones digit + 1, plus a flat 10 when the
 * tens digit is set — but since the level caps at 17 the tens digit is only
 * ever 0 or 1, so it works out to exactly level + 1 across the whole range. */
static uint32_t add_lock_score(uint32_t score, uint8_t level, int lowest_hit_row,
                                uint8_t drop_rate_possible) {
    if (lowest_hit_row < 0) return score;

    int rows_above_floor = TENGEN_ROM_FLOOR_ROW - lowest_hit_row;
    if (rows_above_floor < 0) rows_above_floor = 0;

    uint32_t level_term = (uint32_t)level + 1;
    uint32_t award = level_term * (level_term + (uint32_t)rows_above_floor);

    /* L98D7 renders the product as three decimal digits, and L9A17's doubling
     * saturates them at 9,9,9. The product itself can't exceed 999 at the
     * ROM's level cap (18 * 38 = 684), but the doubled value can. */
    if (award > 999) award = 999;
    if (drop_rate_possible < 2) {
        award *= 2;
        if (award > 999) award = 999;
    }

    score += award;

    /* main.asm.txt:3942-3946: the hundred-thousands digit is replaced by '1'
     * rather than carrying when it would pass '9', so the score wraps to
     * 100000 instead of rolling over to 0 or sticking at 999999. */
    if (score > 999999) score = 100000 + (score % 100000);
    return score;
}

void tengen_new_game(TengenGame *game, uint16_t seed, uint8_t start_level, bool two_player, bool coop) {
    memset(game, 0, sizeof(*game));
    game->two_player = two_player;
    game->coop = coop;

    /* initPlayer1orCoopPlayfield (main.asm.txt:3468-3495): the wall columns
     * are written as solid nibbles in 1P/2P and left open in coop, which is
     * what makes coop a 12-wide game. */
    if (!coop) {
        for (int f = 0; f < 2; f++) {
            for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
                game->field[f].cell[row][0] = TT_WALL;
                game->field[f].cell[row][TENGEN_PF_WIDTH - 1] = TT_WALL;
            }
        }
    }

    TengenRng shared;
    tengen_rng_seed(&shared, seed);
    /* initHandicapGarbage's own source, seeded the same way and stepped only
     * by it (main.asm.txt:3556-3562). */
    game->garbage_rng = shared;

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

/* initHandicapGarbage (main.asm.txt:3545-3603) and its per-byte filler L98AD
 * (:3602-3629). See the header for the shape of it; what follows is the loop
 * itself, kept in the ROM's own terms.
 *
 * The nibble walk matters. The ROM works a BYTE at a time — high nibble then
 * low, which is left to right — and only draws for a nibble that is currently
 * EMPTY, so the wall columns cost no random numbers and coop, whose walls are
 * open, gets twelve draws a row where 1P and 2P get ten. Reproducing that
 * exactly is what keeps a seed producing the same field it produces on the
 * cartridge. */
static uint8_t rng_times(TengenRng *rng, int n) {
    uint8_t v = 0;
    while (n-- > 0) v = tengen_rng_step(rng);
    return v;
}

void tengen_apply_handicap(TengenGame *game, TengenPlayerSlot slot,
                            uint8_t handicap) {
    if (handicap == 0 || handicap > TENGEN_HANDICAP_MAX) return;
    TengenPlayfield *field = &game->field[game->coop ? 0 : (int)slot];
    TengenRng *rng = &game->garbage_rng;

    int rows = handicap * TENGEN_HANDICAP_ROWS_PER_STEP;
    for (int row = TENGEN_PF_HEIGHT - rows; row < TENGEN_PF_HEIGHT; row++) {
        int filled = 0;
        /* Bytes 1..7 of the ROM's eight-byte row; byte 0 is wall either way
         * and byte 7 is the far wall, so neither ever draws. */
        for (int byte = 1; byte <= 7; byte++) {
            for (int half = 0; half < 2; half++) {
                int col = byte * 2 + half - TENGEN_ROM_COL_ORIGIN;
                if (col < 0 || col >= TENGEN_PF_WIDTH) continue;
                if (field->cell[row][col] != TENGEN_CELL_EMPTY) continue;
                /* genNextPseudoRandom3x / and #$07: empty one time in eight. */
                if ((rng_times(rng, 3) & 7) == 0) continue;
                field->cell[row][col] = TENGEN_CELL_WALL;   /* the ROM's $F */
                filled++;
            }
        }
        /* `cmp #$07 / bcc`: a row that came out seven or more full gets one
         * hole punched into bytes 2-5 — the middle eight columns, never
         * against a wall. $F0 keeps the high nibble and clears the low one. */
        if (filled >= 7) {
            int byte = 2 + (rng_times(rng, 2) & 3);
            int high = tengen_rng_step(rng) & 1;
            int col = byte * 2 + (high ? 0 : 1) - TENGEN_ROM_COL_ORIGIN;
            if (col >= 0 && col < TENGEN_PF_WIDTH)
                field->cell[row][col] = TENGEN_CELL_EMPTY;
        }
    }
}

/* L8D8B (main.asm.txt:2050-2082). See the header for what this is.
 *
 * The ROM's shape, kept: sum the two players' (tetrises*2 + triples) with the
 * carry already set — that `sec` is the +1 that puts one cossack on stage even
 * for a level cleared entirely with singles — cap at 8, and cap again at 6
 * unless playMode says coop. */
int tengen_dancer_count(const TengenGame *game) {
    unsigned n = 1;
    for (int i = 0; i < 2; i++) {
        const TengenPlayerState *p = &game->player[i];
        if (!p->game_active) continue;
        n += (unsigned)p->clear_counts[3] * 2u + p->clear_counts[2];
    }
    if (n > 8) n = 8;
    if (!game->coop && n > 6) n = 6;
    return (int)n;
}

/* finishLevelUpAnimation's `ldx #$07 / sta $6C,x / dex / bpl`
 * (main.asm.txt:2476-2482): the tally is this level's, so the show empties it
 * on its way out. */
void tengen_clear_bonus_counts(TengenGame *game) {
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 4; j++) game->player[i].clear_counts[j] = 0;
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

    /* While the line-clear animation runs the game is held still and the
     * completed rows are still standing, so a renderer can animate them. When
     * the timer expires they collapse, the counters catch up, and the next
     * piece is dealt. */
    if (p->line_clear_timer > 0) {
        p->line_clear_timer--;
        if (p->line_clear_timer == 0) {
            TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
            uint32_t cleared = p->clearing_rows;
            p->clearing_rows = 0;

            tengen_collapse_rows(field, cleared);

            int count = 0;
            for (int i = 0; i < TENGEN_PF_HEIGHT; i++) if (cleared & (1u << i)) count++;
            p->lines += (uint32_t)count;
            /* The level's bonus tally, $6C-$73. See clear_counts. */
            if (count >= 1 && count <= 4 && p->clear_counts[count - 1] < 255)
                p->clear_counts[count - 1]++;
            result.lines_collapsed = true;
            result.rows_cleared_mask = cleared;

            /* L94E4 (main.asm.txt:3087-3092) clears lastCurrentBlock as the
             * rows come down, which disarms the undo code: there is no longer
             * a coherent board to put the piece back into. */
            p->last_piece = TT_NONE;
            if (game->coop) game->player[slot ^ 1].last_piece = TT_NONE;

            /* No score is awarded here on purpose: this game pays per piece
             * locked, not per line cleared (see add_lock_score). */
            uint8_t new_level = level_for_lines(p->lines, p->start_level);
            if (new_level > p->level) {
                p->level = new_level;
                result.leveled_up = true;
                /* main.asm.txt:3189-3190: reaching a level by PLAY hands back
                 * the long bar. The cheat level-up does not. */
                p->long_bar_code_used = 0;
            }

            spawn_piece(game, slot);
            if (!tengen_position_valid(game, slot)) {
                p->game_active = false;
                result.topped_out = true;
            }
        }
        p->held_last_frame = held_buttons;
        return result;
    }

    /* Paused: gameplay stops here. The ROM gates it on gameState being 0
     * (branchOnActiveDemoOrGameOver, main.asm.txt:444-446) — but note the
     * line-clear animation above is deliberately NOT gated, because
     * stageLineClearAnimation is called from the main loop unconditionally
     * (main.asm.txt:66-70), so a clear started before pausing still finishes.
     * Cheat-code entry happens in tengen_pause_input, not here. */
    if (game->paused) {
        p->held_last_frame = held_buttons;
        return result;
    }

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
        int lowest_hit_row = -1;
        if (!position_valid_ex(game, slot, &lowest_hit_row)) {
            p->piece.y--;

            /* Snapshot for the undo code, taken here and nowhere else: the
             * ROM calls L85B3 between backing the piece out of the collision
             * and scoring it (main.asm.txt:583-584), so it records where the
             * piece came to rest and the RNG state before the next is dealt. */
            p->last_piece = p->piece.current;
            p->last_orientation = p->piece.orientation;
            p->last_x = p->piece.x;
            p->last_y = p->piece.y;
            p->last_rng = p->rng;

            /* Score first: the ROM awards points at the moment of the failed
             * move, from the collision it just recorded, and does so BEFORE
             * deciding whether this was a normal lock or a top-out
             * (main.asm.txt:582-590). */
            p->score = add_lock_score(p->score, p->level, lowest_hit_row,
                                       p->drop_rate_possible);

            /* main.asm.txt:588-590: resting with the box top still above the
             * visible field ends the game — the piece is not planted. */
            if (p->piece.y < TENGEN_TOPOUT_ROW) {
                p->game_active = false;
                result.topped_out = true;
                p->held_last_frame = held_buttons;
                return result;
            }

            lock_piece(game, slot);
            result.piece_locked = true;

            /* Completed rows are found now but do NOT vanish yet: the ROM
             * holds the game for lineClearTimerP1 frames while they animate
             * (main.asm.txt:1192-1197, 1274-1335), and only then collapses
             * them and deals the next piece. */
            TengenPlayfield *field = &game->field[game->coop ? 0 : slot];
            uint32_t cleared = tengen_find_full_rows(field);
            if (cleared) {
                p->clearing_rows = cleared;
                p->line_clear_timer = game->coop ? TENGEN_LINE_CLEAR_FRAMES_COOP
                                                  : TENGEN_LINE_CLEAR_FRAMES;
                result.lines_cleared = true;
                result.rows_cleared_mask = cleared;
                p->held_last_frame = held_buttons;
                return result;
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
