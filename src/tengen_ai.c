/* tengen_ai.c — computerMove, transcribed. See tengen_ai.h. */
#include "tengen_ai.h"

#include <string.h>

/* THE PIECE TABLE, and why only a quarter of it is here.
 *
 * `computerMoveSelectTableOffsetBy18` ($A0D9) is four bytes per entry, indexed
 * piece*16 + orientation*4: a signed bonus, then the piece's bottom profile,
 * $80-terminated. The PROFILE is not transcribed, because it does not have to
 * be — it is each column's bottom relative to the piece's leftmost occupied
 * column, times eight, and generating that from the core's own
 * kOrientationBitmap reproduces all 28 entries of the ROM byte for byte,
 * terminator included. tengen_ai_profile does the generating and
 * test_the_computers_piece_table_derives_from_the_bitmaps checks it.
 *
 * The BONUS cannot be derived from anything: it is the cartridge's taste, and
 * these are its bytes. An I standing on end is -8 and -10, a T flat is +9, an
 * L in its third orientation is +15. */
static const uint8_t kAiBonus[TENGEN_TETROMINO_COUNT][4] = {
    /* TT_NONE */ { 0x00, 0x00, 0x00, 0x00 },
    /* TT_I    */ { 0x00, 0xF8, 0xFE, 0xF6 },
    /* TT_T    */ { 0x09, 0x00, 0x02, 0x08 },
    /* TT_O    */ { 0x00, 0x00, 0x00, 0x00 },
    /* TT_J    */ { 0x08, 0xFF, 0x01, 0x00 },
    /* TT_L    */ { 0x00, 0x00, 0x01, 0x0F },
    /* TT_S    */ { 0x01, 0x08, 0xFF, 0x06 },
    /* TT_Z    */ { 0x09, 0x00, 0x07, 0xFE },
};

/* What the ROM's height scan reads off a column that is solid at the first
 * row it looks at: ROM row 6, which is 6*8. The walls hold it, and so do the
 * $FF padding bytes either side of every row (initPlayer1orCoopPlayfield
 * writes bytes 0 and 7 as $FF at main.asm.txt:3471-3473). */
#define AI_TOP 0x30
/* The two callers' arguments to the well term: the flush one passes $0C, the
 * bumpy one $00 (main.asm.txt:4348, 4402). */
#define AI_BIAS_FLUSH 0x0C
#define AI_BIAS_BUMPY 0x00
/* The tie-break's thumb on the scale, `adc #$0B` at $9D17. */
#define AI_FLUSH_BIAS 0x0B

int8_t tengen_ai_bonus(TengenTetromino piece, uint8_t orientation) {
    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return 0;
    return (int8_t)kAiBonus[piece][orientation & 3];
}

int tengen_ai_profile(TengenTetromino piece, uint8_t orientation,
                       uint8_t out[3]) {
    int bottom[4];
    int first = -1, n = 0;

    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return 0;
    orientation &= 3;

    for (int c = 0; c < 4; c++) {
        bottom[c] = -1;
        for (int r = 0; r < 4; r++)
            if (tengen_piece_occupies(piece, orientation, r, c)) bottom[c] = r;
        if (bottom[c] >= 0 && first < 0) first = c;
    }
    if (first < 0) return 0;

    for (int c = first + 1; c < 4 && n < 3; c++) {
        if (bottom[c] < 0) continue;
        out[n++] = (uint8_t)((bottom[c] - bottom[first]) * 8);
    }
    return n;
}

/* computerMove's first job (main.asm.txt:4224-4256): for each of the sixteen
 * nibble columns, the byte offset of the topmost cell that is not empty,
 * found by stepping down in eights from $28 — so the first row it actually
 * reads is $30, ROM row 6, the first visible one. Everything downstream is in
 * these units, which is why every number in this file is a multiple of eight.
 *
 * A column that is empty all the way down stops on the solid floor the ROM
 * lays at rows 26-27, giving $D0. The core stores only the twenty visible
 * rows, so that case is the loop falling off the end. */
void tengen_ai_heights(const TengenGame *game, TengenPlayerSlot slot,
                        uint8_t out[TENGEN_AI_SCRATCH_A]) {
    const TengenPlayfield *field = &game->field[game->coop ? 0 : (int)slot];

    memset(out, 0, TENGEN_AI_SCRATCH_A);
    for (int nibble = 0; nibble < TENGEN_AI_COLUMNS; nibble++) {
        int col = nibble - TENGEN_ROM_COL_ORIGIN;
        int row;
        if (col < 0 || col >= TENGEN_PF_WIDTH) {
            /* The padding nibbles, which are $FF in every row the ROM builds
             * and therefore solid from the first one it looks at. */
            out[nibble] = AI_TOP;
            continue;
        }
        for (row = 0; row < TENGEN_PF_HEIGHT; row++)
            if (field->cell[row][col] != TT_NONE) break;
        out[nibble] = (uint8_t)((row + TENGEN_ROM_ROW_ORIGIN) * 8);
    }
}

/* L9E31 (main.asm.txt:4433-4462): the well term, and the only place either
 * scorer looks at the ground BESIDE the piece rather than under it.
 *
 * `left` is the column just left of the placement and `right` the one just
 * right of it; $30 in either means a wall (or the screen's own padding, which
 * reads the same). The shape of it:
 *
 *   right is a wall  -> average the LEFT neighbour with the piece's own
 *                       column, less the caller's bias
 *   left is a wall   -> the same with the RIGHT neighbour
 *   neither          -> average the two neighbours, no bias
 *
 * and then a shift of two. So a placement tucked against a wall is measured
 * against the ground it is leaning on, and one out in the open against the
 * ground either side of it.
 *
 * THE INDEX CAN RUN PAST THE SIXTEEN COLUMNS, and that is the ROM's doing:
 * `computerScratchA+1,y` with y the placement's rightmost column reaches
 * scratchA[17] when a four-wide piece is tried at column 13, which is the
 * byte the routine's own caller just saved the table index into. It is
 * reachable and it is harmless — the comparison it feeds cannot match — so
 * the scratch here is sized to let it happen exactly as it does there rather
 * than clamped into something the cartridge never computes. */
static uint8_t ai_well(const uint8_t *a, int x, int y, uint8_t bias) {
    uint8_t left = a[x - 1];
    uint8_t right = a[y + 1];
    uint16_t sum;
    uint8_t value;

    if (right == AI_TOP || left == AI_TOP) {
        /* `ror` after `adc` is a nine-bit average: the carry the addition
         * produced comes back in as the top bit. */
        uint8_t other = (right == AI_TOP) ? left : right;
        sum = (uint16_t)other + a[x];
        value = (uint8_t)(((sum & 0xFF) >> 1) | ((sum & 0x100) ? 0x80 : 0));
        value = (uint8_t)(value - bias);
    } else {
        sum = (uint16_t)left + right;
        value = (uint8_t)(((sum & 0xFF) >> 1) | ((sum & 0x100) ? 0x80 : 0));
    }
    return (uint8_t)(value >> 2);
}

/* L9DA3 (main.asm.txt:4361-4429): the placement that does NOT sit flush.
 *
 * It drops the piece column by column — for each profile byte, if the piece
 * would reach the terrain there, the whole piece is lifted so it rests on it
 * instead — and scores where it comes to rest. The accumulator is the resting
 * height of the piece's own leftmost column, so the mismatch it picks up on
 * the way is exactly how far the piece is held off the ground.
 *
 * It refuses two things the flush scorer does not: a placement whose score
 * borrows (the well term came out bigger than the resting height) and one
 * below $20. And it will not start at all from column $0C or beyond, which
 * the flush scorer is happy to do — an asymmetry, and the cartridge's. */
static void ai_score_bumpy(TengenAi *ai, uint8_t *a, int x,
                            TengenTetromino piece, uint8_t orientation) {
    uint8_t profile[3];
    int count = tengen_ai_profile(piece, orientation, profile);
    uint8_t fit = a[x];
    int cover = x;

    for (int k = 0; k < count; k++) {
        if (x >= 0x0C) return;
        cover++;
        uint8_t reach = (uint8_t)(profile[k] + fit);
        uint8_t over = (uint8_t)(reach - a[x + 1 + k]);
        if (reach >= a[x + 1 + k]) {
            /* `eor #$FF` then `adc` with the carry the compare left set:
             * fit - over, in one byte. */
            fit = (uint8_t)(fit - over);
        }
    }

    uint8_t well = ai_well(a, x, cover, AI_BIAS_BUMPY);
    if (fit < well) return;                 /* the subtraction borrowed */
    uint8_t score = (uint8_t)(fit - well);
    if (score < 0x20) return;
    score = (uint8_t)(score + kAiBonus[piece][orientation & 3]);
    if (score < ai->scratch[5]) return;
    ai->scratch[5] = score;
    ai->scratch[1] = (uint8_t)x;
    ai->scratch[3] = orientation;
}

/* possibleComputerChoosingMove (main.asm.txt:4311-4357): the placement that
 * sits FLUSH, every column of the piece's bottom landing exactly on the
 * terrain under it. The moment one does not, the whole thing is handed to the
 * bumpy scorer instead — the two are alternatives, not a pair. */
static void ai_score(TengenAi *ai, uint8_t *a, int x,
                      TengenTetromino piece, uint8_t orientation) {
    uint8_t profile[3];
    int count = tengen_ai_profile(piece, orientation, profile);
    int cover = x;

    for (int k = 0; k < count; k++) {
        cover++;
        if ((uint8_t)(profile[k] + a[x]) != a[x + 1 + k]) {
            ai_score_bumpy(ai, a, x, piece, orientation);
            return;
        }
    }

    uint8_t well = ai_well(a, x, cover, AI_BIAS_FLUSH);
    uint8_t score = (uint8_t)(a[x] - well + kAiBonus[piece][orientation & 3]);
    if (score < ai->scratch[4]) return;
    ai->scratch[4] = score;
    ai->scratch[0] = (uint8_t)x;
    ai->scratch[2] = orientation;
}

void tengen_ai_reset(TengenAi *ai) {
    memset(ai, 0, sizeof(*ai));
}

void tengen_ai_choose(TengenAi *ai, const TengenGame *game,
                       TengenPlayerSlot slot) {
    uint8_t a[TENGEN_AI_SCRATCH_A];
    TengenTetromino piece = game->player[slot].piece.current;

    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return;
    tengen_ai_heights(game, slot, a);

    /* Only the two SCORES are cleared; see the note on TengenAi. */
    ai->scratch[4] = 0;
    ai->scratch[5] = 0;

    /* Columns 2 to 13 — nibble columns, so 2 and 13 are the walls in a
     * ten-wide game and playable in a twelve-wide one — and all four
     * orientations of each. */
    for (int x = 2; x < 14; x++)
        for (uint8_t o = 0; o < 4; o++)
            ai_score(ai, a, x, piece, o);

    /* THE TIE-BREAK, and the thumb on the scale. The flush candidate wins
     * unless the bumpy one beats it by more than eleven: `adc #$0B` then a
     * carry test ($9D13-$9D23), which is also why a flush candidate that was
     * never found — score zero — still holds the choice against any bumpy one
     * scoring eleven or less. */
    uint8_t x = ai->scratch[2], y = ai->scratch[0];
    if ((uint8_t)(ai->scratch[4] + AI_FLUSH_BIAS) < ai->scratch[5]) {
        x = ai->scratch[3];
        y = ai->scratch[1];
    }
    ai->target_orientation = x;

    /* Everything above works in the piece's LEFTMOST OCCUPIED column, and the
     * driver below compares against player1TetrominoX, which is its bitmap's
     * left edge. Those are the same column for every piece and orientation
     * but one: the I standing on end occupies its bitmap's second column. So
     * the I, and only the I, gets a column back ($9D27-$9D35). */
    if (piece == TT_I && (x == 1 || x == 3)) y--;
    ai->target_x = y;
}

uint8_t tengen_ai_buttons(const TengenAi *ai, const TengenGame *game,
                           TengenPlayerSlot slot, uint8_t frame_counter) {
    const TengenPlayerState *p = &game->player[slot];
    uint8_t buttons = 0;

    /* "shifting occurs every 8 frames; rotation every 16" — the cartridge's
     * own comment, at main.asm.txt:4170. */
    if ((frame_counter & 0x07) == 0) {
        uint8_t delta = (uint8_t)(ai->target_x - (uint8_t)p->piece.x);
        if (delta != 0)
            buttons |= (ai->target_x >= (uint8_t)p->piece.x)
                ? TENGEN_BTN_RIGHT : TENGEN_BTN_LEFT;
    }
    if ((frame_counter & 0x0F) == 0) {
        uint8_t delta = (uint8_t)(ai->target_orientation - p->piece.orientation);
        if (delta != 0) {
            /* Three steps one way is one step the other, and the ROM says so
             * with a single compare: B for one or two, A for three. */
            buttons |= ((delta & 3) < 3) ? TENGEN_BTN_B : TENGEN_BTN_A;
        }
    }
    return buttons;
}
