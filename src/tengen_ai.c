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

/* THE PARTNER'S SHADOW — the port's own, and it has no line in the ROM.
 *
 * `computerMove` reads the settled board. On the shared twelve-wide field of
 * WITH COMPUTER that leaves out the one thing that matters most: the other
 * player's FALLING piece, which is solid to this one (checkCoopCollision,
 * main.asm.txt:1113-1175) and which both scorers are blind to. Both players
 * therefore score the same columns with the same routine and pick the same
 * one, and the two pieces spend the whole descent shouldering each other.
 *
 * WHAT IT MARKS IS WHERE THE PARTNER IS GOING, not where it is, and that is
 * the whole difference between this reading the board and this lying about
 * it: the piece is dropped straight down onto the settled field first and
 * the shadow is what it covers WHERE IT COMES TO REST. Marking the corridor
 * from the partner's own row down was the first cut, and a column the
 * partner is merely passing through is not full — a piece stacked against
 * that phantom wall leaves a hole the moment the partner lands lower.
 *
 * So the shadow is future terrain: the computer stacks flush on top of what
 * the partner is about to put down, which is what a partner does. */
void tengen_ai_shadow(const TengenGame *game, TengenPlayerSlot slot,
                       uint8_t out[TENGEN_AI_SCRATCH_A]) {
    const TengenPlayfield *field = &game->field[0];
    TengenCell cells[4];
    int n;

    if (!game->coop) return;
    n = tengen_active_piece_cells(game, (TengenPlayerSlot)(slot ^ 1), cells);
    if (n == 0) return;

    /* Gravity, and nothing else: the partner may still shift out of it. */
    int drop = 0;
    for (;;) {
        int next = drop + 1;
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            int row = cells[i].row + next;
            int col = cells[i].col;
            if (row >= TENGEN_PF_HEIGHT) ok = false;
            else if (col < 0 || col >= TENGEN_PF_WIDTH) ok = false;
            else if (row >= 0 && field->cell[row][col] != TT_NONE) ok = false;
        }
        if (!ok) break;
        drop = next;
    }

    for (int i = 0; i < n; i++) {
        int nibble = cells[i].col + TENGEN_ROM_COL_ORIGIN;
        int row = cells[i].row + drop;
        uint8_t top;
        if (nibble < 0 || nibble >= TENGEN_AI_COLUMNS) continue;
        if (row < 0) row = 0;             /* the spawn rows, above the field */
        top = (uint8_t)((row + TENGEN_ROM_ROW_ORIGIN) * 8);
        if (top < out[nibble]) out[nibble] = top;   /* smaller is taller */
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


/* ======================================================================= *
 * THE PORT'S OWN COMPUTER (`smart`, behind the chord)
 *
 * Everything above is computerMove, and it stays the computer the cartridge
 * ships. This is a second one, for a player who rang the chord and wants a
 * partner (or a rival) that plays well, built the way Tetris programs have
 * been built since: a board read as bits, every placement scored by a
 * weighted sum of the board's features, a look one piece further ahead with
 * NEXT, and nothing tried that the pad could not actually do in time.
 *
 * THE BOARD AS BITS. One sixteen-bit word a row, bit c the storage column c,
 * the playable columns in `full` (ten in a race, twelve in coop). A
 * placement is a few shifts and ORs; a hole or a transition is an AND and a
 * population count. Cheap enough for the GBA to score the lot in a few
 * frames, which matters: see the budget below.
 *
 * THE SCORE is El-Tetris's (Islam El-Ashi, 2011), Pierre Dellacherie's six
 * features with weights found by a particle swarm: the piece's landing
 * height, the rows it clears, the row and column transitions, the holes,
 * and the wells (each well's cells summed 1+2+...+depth). Scaled by a
 * thousand and kept in integers.
 *
 * REACHABLE, AT THE CARTRIDGE'S PACE. The pad is still computerMove's — a
 * shift every eighth frame, a turn every sixteenth (tengen_ai_buttons) —
 * and it never presses Down (the computer plays at the cartridge's pace),
 * so a placement across the board at level 15 is one the hands cannot get
 * to. Each candidate's path is walked frame by frame as the core will play
 * it (ai_reachable): fall timer, fractional gravity, shifts and turns on
 * the driver's clock, the left kick. One out of reach is not dropped, it
 * goes to the back of the queue (AI_UNREACHABLE): with nothing in reach,
 * the least bad aim beats none, which is the piece dropped where it came
 * in. A first version walked the turns and then the shifts at one gravity
 * for the whole fall; at level 18 that was optimistic (fractional gravity
 * is the faster of two rates on some rows) and the computer lost 16 games
 * in 16 where the cartridge's lost 9.
 *
 * ON A SHARED BOARD it reads its partner: where the partner's falling piece
 * is now is solid for the path; the columns it is going to land in cost
 * W_CROSS each (whoever gets there second lands on the other); and every
 * column past the middle costs W_SIDE while the partner has a piece. The
 * partner's landing is NOT read as ground for the score (AI_SHADOW 0): it
 * made the computer plan rows around cells that were not there yet, and
 * the holes were its own. A computer partner says where it is going
 * (`partner_known`); a person is guessed straight down from where they are.
 *
 * MEASURED (`make ai-bench`, sixteen games of up to 30000 frames each,
 * the cartridge's pace for both, the budget below), lines cleared and
 * games lost:
 *
 *                        level 0       level 10      level 18
 *   SOLO  cartridge      234   1      1445   5      1483   9
 *   SOLO  port's         263   0      1505   0      2152   0
 *   COOP  cart + cart     78  16        43  16        48  16
 *   COOP  port + port    464   1      1052   9       715  16
 *   COOP+ port + cart    375  10       366  16       225  16
 *
 * At level 0 a solo board does not fill in 30000 frames either way; the
 * difference there is pace. COOP+ is the port's computer with the
 * cartridge's for a partner: one that does not say where it is going and
 * plays badly, the hard case of a human. W_SIDE was tried at 0, -2000,
 * -5000, -10000 and -20000; W_CROSS earlier at the soft drop's pace.
 *
 * THE BUDGET. Planning starts at the spawn (tengen_ai_choose) and is done
 * a slice a frame from tengen_ai_buttons: first every placement of the
 * piece in hand, each a walk and a score, charged two (a target as soon as
 * they are all in, five frames or so), then, for the best few, every
 * placement of NEXT on the board they leave — and a better total moves the
 * target, while there is still time to get there. MEASURED on the ROM
 * (`run_rom.py --aiframe`): sixteen a slice, charged one each, made 140
 * turns of 3000 in WITH COMPUTER take two frames; eight, charged as above,
 * none, the latest turn reaching draw_match at line 105 of 160. The shapes
 * come from a table (ai_shape) and the wells are counted by set bit: a
 * third of the cost, the same choices.
 * ======================================================================= */

#define AI_H TENGEN_PF_HEIGHT
#ifndef TENGEN_AI_SMART_BUDGET
#define TENGEN_AI_SMART_BUDGET 8
#endif

/* El-Tetris's weights, times a thousand. */
#define W_LANDING   (-4500)
#define W_CLEARED     3418
#define W_ROW_TRANS (-3218)
#define W_COL_TRANS (-9349)
#define W_HOLES     (-7899)
#define W_WELLS     (-3386)
/* The port's: a column on the partner's side of a shared board. */
#ifndef W_SIDE
#define W_SIDE      (-5000)
#endif
/* ...and one under where the partner's falling piece is going to land:
 * whichever of the two gets there second lands on the other. */
#ifndef W_CROSS
#define W_CROSS     (-15000)
#endif
/* Knobs for tests/ai_bench.c to measure each part by taking it away. */
#ifndef AI_REACH
#define AI_REACH 1
#endif
#ifndef AI_LOOKAHEAD
#define AI_LOOKAHEAD 1
#endif
#define AI_DEAD     (-0x3FFFFFFF)
#define AI_UNREACHABLE (-100000000)

typedef struct {
    uint16_t row[AI_H];
    uint16_t full;
} AiBoard;

static int ai_popcount(uint32_t v) {
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

static uint16_t ai_shift(uint16_t bits, int left) {
    return (uint16_t)(left >= 0 ? bits << left : bits >> -left);
}

/* A piece's four rows as bits, bitmap column c at bit c. Worked out once
 * from the core's own table, the first time it is asked for: the planner
 * asks thousands of times a piece, and sixteen calls into the core each
 * time was a fifth of what it cost on the GBA. */
static uint16_t g_ai_shapes[TENGEN_TETROMINO_COUNT][4][4];
static bool g_ai_shapes_ready;

static void ai_shape(TengenTetromino piece, uint8_t o, uint16_t sh[4]) {
    if (!g_ai_shapes_ready) {
        for (int p = 0; p < TENGEN_TETROMINO_COUNT; p++)
            for (int q = 0; q < 4; q++)
                for (int r = 0; r < 4; r++) {
                    uint16_t bits = 0;
                    for (int c = 0; c < 4; c++)
                        if (p > TT_NONE &&
                            tengen_piece_occupies((TengenTetromino)p, (uint8_t)q, r, c))
                            bits |= (uint16_t)(1u << c);
                    g_ai_shapes[p][q][r] = bits;
                }
        g_ai_shapes_ready = true;
    }
    memcpy(sh, g_ai_shapes[piece][o & 3], 4 * sizeof(uint16_t));
}

static void ai_board(const TengenGame *g, TengenPlayerSlot slot, AiBoard *b) {
    const TengenPlayfield *f = &g->field[g->coop ? 0 : slot];
    b->full = g->coop ? 0x0FFF : 0x07FE;
    for (int y = 0; y < AI_H; y++) {
        uint16_t m = 0;
        for (int c = 0; c < TENGEN_PF_WIDTH; c++)
            if (f->cell[y][c]) m |= (uint16_t)(1u << c);
        b->row[y] = (uint16_t)(m & b->full);
    }
}

/* Does the shape at left column `l`, top row `t` hit anything? Rows above
 * the field are open; below it and outside the playable columns are not. */
static bool ai_hits(const AiBoard *b, const uint16_t sh[4], int l, int t) {
    for (int r = 0; r < 4; r++) {
        if (!sh[r]) continue;
        uint16_t cols = ai_shift(sh[r], l);
        if (l < 0 && (sh[r] & ((1u << -l) - 1))) return true;   /* off the left */
        if (cols & ~b->full) return true;
        int y = t + r;
        if (y >= AI_H) return true;
        if (y >= 0 && (b->row[y] & cols)) return true;
    }
    return false;
}

static int ai_drop(const AiBoard *b, const uint16_t sh[4], int l, int t) {
    while (!ai_hits(b, sh, l, t + 1)) t++;
    return t;
}

/* Puts it there and takes the full rows away; how many, or -1 if any of it
 * is above the field (a top-out). */
static int ai_place(AiBoard *b, const uint16_t sh[4], int l, int t) {
    for (int r = 0; r < 4; r++) {
        if (!sh[r]) continue;
        if (t + r < 0 || t + r >= AI_H) return -1;
        b->row[t + r] |= ai_shift(sh[r], l);
    }
    int cleared = 0, to = AI_H - 1;
    for (int y = AI_H - 1; y >= 0; y--) {
        if ((b->row[y] & b->full) == b->full) { cleared++; continue; }
        b->row[to--] = b->row[y];
    }
    while (to >= 0) b->row[to--] = 0;
    return cleared;
}

/* The board's four El-Tetris terms (landing height and rows cleared are the
 * placement's, added by the caller). */
static int32_t ai_board_terms(const AiBoard *b) {
    int row_trans = 0, col_trans = 0, holes = 0, wells = 0;
    uint16_t covered = 0, prev = 0;
    uint8_t run[16] = { 0 };
    uint16_t in_well = 0;
    for (int y = 0; y < AI_H; y++) {
        uint16_t r = b->row[y];
        /* Outside the playable columns reads as filled, one either side. */
        uint16_t x = (uint16_t)(((r & b->full) << 1) | (uint16_t)~(b->full << 1));
        x &= 0x3FFF;
        row_trans += ai_popcount((uint16_t)(x ^ (x >> 1)) & 0x1FFF);
        col_trans += ai_popcount((uint16_t)(r ^ prev) & b->full);
        holes += ai_popcount((uint16_t)(covered & ~r) & b->full);
        covered |= r;
        prev = r;
        /* A well cell: empty, both sides filled. */
        uint16_t well = (uint16_t)(~x & (x << 1) & (x >> 1)) >> 1;
        well &= b->full;
        /* Only the columns that are wells now, or were a row up: the rest
         * have nothing to add and nothing to reset. */
        for (uint16_t m = (uint16_t)(well | in_well); m; m &= (uint16_t)(m - 1)) {
            int c = 0;
            while (!(m & (1u << c))) c++;
            if (well & (1u << c)) { run[c]++; wells += run[c]; }
            else run[c] = 0;
        }
        in_well = well;
    }
    col_trans += ai_popcount((uint16_t)~prev & b->full);   /* the floor */
    return (int32_t)W_ROW_TRANS * row_trans + (int32_t)W_COL_TRANS * col_trans +
           (int32_t)W_HOLES * holes + (int32_t)W_WELLS * wells;
}

static int ai_shape_rows(const uint16_t sh[4], int *top) {
    int first = -1, last = -1;
    for (int r = 0; r < 4; r++)
        if (sh[r]) { if (first < 0) first = r; last = r; }
    *top = first;
    return last - first + 1;
}

/* Score of the shape dropped at `l` from row `t` on `b` (which is changed),
 * El-Tetris in full; AI_DEAD for a top-out. */
static int32_t ai_score_drop(AiBoard *b, const uint16_t sh[4], int l, int t,
                             int *cleared_out) {
    /* A shape that does not fit where it would start from — a piece lying
     * near the floor, asked about standing up — cannot be put there. */
    if (ai_hits(b, sh, l, t)) return AI_DEAD;
    int land = ai_drop(b, sh, l, t);
    int top, rows = ai_shape_rows(sh, &top);
    int cleared = ai_place(b, sh, l, land);
    if (cleared < 0) return AI_DEAD;
    if (cleared_out) *cleared_out = cleared;
    /* The middle of the piece, counted up from the floor, in halves. */
    int height2 = 2 * AI_H - (2 * (land + top) + rows - 1);
    return (int32_t)W_LANDING * height2 / 2 + (int32_t)W_CLEARED * cleared +
           ai_board_terms(b);
}

/* The partner's piece, if it has one: where it is (`now`) and where it will
 * land (`ahead`), both as cells added to an otherwise empty board. */
static void ai_partner(const TengenGame *g, TengenPlayerSlot slot,
                       const TengenAi *ai, const AiBoard *base, AiBoard *now,
                       AiBoard *ahead, uint16_t *cols_out) {
    *cols_out = 0;
    memset(now, 0, sizeof(*now));
    memset(ahead, 0, sizeof(*ahead));
    now->full = ahead->full = base->full;
    if (!g->coop) return;
    const TengenPlayerState *q = &g->player[slot ^ 1];
    if (!q->game_active || q->piece.current <= TT_NONE ||
        q->piece.current >= TENGEN_TETROMINO_COUNT)
        return;
    uint16_t sh[4];
    ai_shape(q->piece.current, q->piece.orientation, sh);
    int l = q->piece.x - TENGEN_ROM_COL_ORIGIN;
    int t = q->piece.y - TENGEN_ROM_ROW_ORIGIN;
    for (int r = 0; r < 4; r++)
        if (sh[r] && t + r >= 0 && t + r < AI_H)
            now->row[t + r] |= (uint16_t)(ai_shift(sh[r], l) & base->full);
    /* Where it is going: a computer's own target if the caller passed it
     * on, otherwise straight down from where it is. */
    if (ai->partner_known) {
        ai_shape(q->piece.current, ai->partner_o, sh);
        l = ai->partner_x - TENGEN_ROM_COL_ORIGIN;
    }
    int land = ai_drop(base, sh, l, t);
    for (int r = 0; r < 4; r++) {
        if (!sh[r]) continue;
        uint16_t cols = (uint16_t)(ai_shift(sh[r], l) & base->full);
        *cols_out |= cols;
        if (land + r >= 0 && land + r < AI_H) ahead->row[land + r] |= cols;
    }
}

static void ai_or(AiBoard *dst, const AiBoard *a, const AiBoard *b) {
    dst->full = a->full;
    for (int y = 0; y < AI_H; y++) dst->row[y] = (uint16_t)(a->row[y] | b->row[y]);
}

/* CAN THE PAD GET IT THERE IN TIME? Walked frame by frame the way the core
 * will play it: the fall timer ticks first, reloading from the row the piece
 * is on (tengen_frames_per_row, fractional gravity and all), then the
 * driver's shift on every eighth frame of the clock and its turn on every
 * sixteenth — together, as tengen_ai_buttons presses them — with the
 * release's one-column left kick, and then the row gravity owes. Reachable
 * if the piece is over its column, turned, before it comes to rest.
 * INFERRED as close rather than exact: the partner's piece is taken to
 * stand still meanwhile. */
typedef struct {
    uint8_t level, timer, clock;
    bool coop, xe, kick;
} AiPace;

static bool ai_reachable(const AiBoard *obstacles, TengenTetromino piece,
                         uint8_t o0, int l0, int t0, uint8_t o1, int l1,
                         const AiPace *pace) {
    uint16_t sh[4];
    uint8_t o = o0;
    int l = l0, t = t0;
    uint8_t timer = pace->timer, clock = pace->clock;
    ai_shape(piece, o, sh);
    for (int f = 0; f < 600; f++, clock++) {
        if (l == l1 && o == o1) return true;
        bool fall = false;
        if (timer > 0) timer--;
        if (timer == 0) {
            fall = true;
            timer = tengen_frames_per_row(pace->level,
                                          (int8_t)(t + TENGEN_ROM_ROW_ORIGIN),
                                          pace->coop, pace->xe);
        }
        if ((clock & 0x07) == 0 && l != l1) {
            int d = l1 > l ? 1 : -1;
            if (!ai_hits(obstacles, sh, l + d, t)) l += d;
        }
        if ((clock & 0x0F) == 0 && o != o1) {
            uint8_t delta = (uint8_t)((o1 - o) & 3);
            uint8_t no = (uint8_t)((o + (delta < 3 ? 1 : 3)) & 3);
            uint16_t ns[4];
            ai_shape(piece, no, ns);
            if (!ai_hits(obstacles, ns, l, t)) {
                o = no; memcpy(sh, ns, sizeof(sh));
            } else if (pace->kick && !ai_hits(obstacles, ns, l - 1, t)) {
                o = no; l--; memcpy(sh, ns, sizeof(sh));
            }
        }
        if (fall) {
            if (ai_hits(obstacles, sh, l, t + 1))
                return l == l1 && o == o1;      /* came to rest */
            t++;
        }
    }
    return false;
}

/* The working boards for this frame's slice: the settled board, what is in
 * the way (that and the partner now), and what to score against (that and
 * the partner where it will land). */
typedef struct {
    AiBoard base, obstacles, scored;
    int l0, t0;
    AiPace pace;
    uint8_t o0;
    TengenTetromino piece, next;
    int side_mid;                /* coop: the column past which it is "theirs" */
    int side_dir;                /* +1: mine is the left; -1: the right */
    uint16_t partner_cols;       /* coop: the columns its piece is landing in */
} AiView;

static bool ai_view(const TengenGame *g, TengenPlayerSlot slot,
                    const TengenAi *ai, AiView *v) {
    const TengenPlayerState *p = &g->player[slot];
    v->piece = p->piece.current;
    v->next = p->piece.next;
    if (v->piece <= TT_NONE || v->piece >= TENGEN_TETROMINO_COUNT) return false;
    ai_board(g, slot, &v->base);
    AiBoard now, ahead;
    ai_partner(g, slot, ai, &v->base, &now, &ahead, &v->partner_cols);
    ai_or(&v->obstacles, &v->base, &now);
#ifndef AI_SHADOW
#define AI_SHADOW 0
#endif
    if (AI_SHADOW) ai_or(&v->scored, &v->base, &ahead);
    else v->scored = v->base;
    v->l0 = p->piece.x - TENGEN_ROM_COL_ORIGIN;
    v->t0 = p->piece.y - TENGEN_ROM_ROW_ORIGIN;
    v->o0 = p->piece.orientation;
    v->pace.level = p->level;
    v->pace.timer = p->fall_timer;
    v->pace.clock = ai->clock;
    v->pace.coop = g->coop;
    v->pace.xe = g->xe;
    v->pace.kick = !g->proto_rules;
    v->side_mid = 0;
    v->side_dir = 0;
    if (g->coop && g->player[slot ^ 1].game_active &&
        g->player[slot ^ 1].piece.current != TT_NONE) {
        v->side_mid = TENGEN_PF_WIDTH / 2;
        v->side_dir = slot == TENGEN_PLAYER_1 ? 1 : -1;
    }
    return true;
}

/* What the side of a shared board costs this placement: columns past the
 * middle (W_SIDE, off by default) and columns the partner's piece is landing
 * in (W_CROSS). */
static int32_t ai_trespass(const AiView *v, const uint16_t sh[4], int l) {
    if (!v->side_dir) return 0;
    uint16_t mine = 0;
    for (int r = 0; r < 4; r++) mine |= ai_shift(sh[r], l);
    int32_t cost = (int32_t)W_CROSS * ai_popcount(mine & v->partner_cols);
    if (!W_SIDE) return cost;
    int lo = 99, hi = -99;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (sh[r] & (1u << c)) {
                if (l + c < lo) lo = l + c;
                if (l + c > hi) hi = l + c;
            }
    int past = v->side_dir > 0 ? (hi >= v->side_mid ? hi - v->side_mid + 1 : 0)
                               : (lo < v->side_mid ? v->side_mid - lo : 0);
    return cost + (int32_t)W_SIDE * past;
}

static void ai_set_target(TengenAi *ai, int l, uint8_t o) {
    ai->target_x = (uint8_t)(l + TENGEN_ROM_COL_ORIGIN);
    ai->target_orientation = o;
    ai->plan_have = true;
}

static void ai_smart_start(TengenAi *ai, const TengenGame *g,
                           TengenPlayerSlot slot) {
    AiView v;
    ai->plan_stage = 0;
    ai->plan_count = 0;
    ai->plan_cursor = 0;
    ai->plan_inner = 0;
    ai->plan_top_count = 0;
    ai->plan_have = false;
    if (!ai_view(g, slot, ai, &v)) return;
    /* Every orientation once (the O's four are one, the I's S's and Z's
     * two), at every left column it fits. */
    uint16_t seen[4][4];
    int distinct = 0;
    for (uint8_t o = 0; o < 4; o++) {
        uint16_t sh[4];
        ai_shape(v.piece, o, sh);
        bool dup = false;
        for (int d = 0; d < distinct; d++)
            if (!memcmp(seen[d], sh, sizeof(sh))) dup = true;
        if (dup) continue;
        memcpy(seen[distinct++], sh, sizeof(sh));
        for (int l = -3; l < TENGEN_PF_WIDTH && ai->plan_count < TENGEN_AI_SMART_MAX; l++) {
            if (ai_hits(&v.base, sh, l, v.t0 < 0 ? v.t0 : 0) &&
                ai_hits(&v.base, sh, l, v.t0))
                continue;
            ai->plan_cand_l[ai->plan_count] = (uint8_t)(l + 3);
            ai->plan_cand_o[ai->plan_count] = o;
            ai->plan_count++;
        }
    }
    ai->plan_best = AI_DEAD;
    ai->plan_stage = 1;
}

/* One slice: up to `budget` placements scored. */
static void ai_smart_think(TengenAi *ai, const TengenGame *g,
                           TengenPlayerSlot slot, int budget) {
    if (ai->plan_stage == 0 || ai->plan_stage == 3) return;
    AiView v;
    if (!ai_view(g, slot, ai, &v)) { ai->plan_stage = 3; return; }

    while (budget > 0 && ai->plan_stage == 1) {
        if (ai->plan_cursor >= ai->plan_count) {
            /* Every placement of the piece in hand is in: aim at the best
             * reachable one now, and pick the few to look ahead from. */
            int best = -1;
            for (int k = 0; k < ai->plan_count; k++) {
                if (ai->plan_cand_score[k] == AI_DEAD) continue;
                if (best < 0 || ai->plan_cand_score[k] > ai->plan_cand_score[best])
                    best = k;
            }
            if (best >= 0) {
                ai_set_target(ai, ai->plan_cand_l[best] - 3, ai->plan_cand_o[best]);
                ai->plan_best = AI_DEAD;
            }
            for (int pick = 0; pick < TENGEN_AI_SMART_KEEP; pick++) {
                int top = -1;
                for (int k = 0; k < ai->plan_count; k++) {
                    if (ai->plan_cand_score[k] == AI_DEAD) continue;
                    bool taken = false;
                    for (int j = 0; j < ai->plan_top_count; j++)
                        if (ai->plan_top[j] == k) taken = true;
                    if (taken) continue;
                    if (top < 0 || ai->plan_cand_score[k] > ai->plan_cand_score[top])
                        top = k;
                }
                if (top < 0) break;
                ai->plan_top[ai->plan_top_count++] = (uint8_t)top;
            }
            ai->plan_cursor = 0;
            ai->plan_inner = 0;
            ai->plan_reply_best = AI_DEAD;
            ai->plan_stage = (AI_LOOKAHEAD && ai->plan_top_count && v.next > TT_NONE &&
                              v.next < TENGEN_TETROMINO_COUNT) ? 2 : 3;
            break;
        }
        int k = ai->plan_cursor++;
        int l = ai->plan_cand_l[k] - 3;
        uint8_t o = ai->plan_cand_o[k];
        uint16_t sh[4];
        ai_shape(v.piece, o, sh);
        budget -= 2;                 /* a score and a walk: two of the rest */
        AiBoard b = v.scored;
        int32_t score = ai_score_drop(&b, sh, l, v.t0, 0);
        if (score != AI_DEAD) {
            score += ai_trespass(&v, sh, l);
            /* Out of reach is not out of the running: if nothing is in
             * reach, the least bad of the rest is still a better aim than
             * none, which is a piece dropped where it spawned. */
            if (AI_REACH && !ai_reachable(&v.obstacles, v.piece, v.o0, v.l0,
                                          v.t0, o, l, &v.pace))
                score += AI_UNREACHABLE;
        }
        ai->plan_cand_score[k] = score;
    }

    while (budget > 0 && ai->plan_stage == 2) {
        if (ai->plan_cursor >= ai->plan_top_count) { ai->plan_stage = 3; break; }
        int k = ai->plan_top[ai->plan_cursor];
        int l = ai->plan_cand_l[k] - 3;
        uint8_t o = ai->plan_cand_o[k];
        uint16_t sh[4];
        ai_shape(v.piece, o, sh);
        /* The board this one leaves, and its first piece's own terms. */
        AiBoard after = v.scored;
        int cleared = 0;
        if (ai_hits(&after, sh, l, v.t0)) { ai->plan_cursor++; ai->plan_inner = 0; continue; }
        int land = ai_drop(&after, sh, l, v.t0);
        int top, rows = ai_shape_rows(sh, &top);
        cleared = ai_place(&after, sh, l, land);
        if (cleared < 0) { ai->plan_cursor++; ai->plan_inner = 0; continue; }
        int height2 = 2 * AI_H - (2 * (land + top) + rows - 1);
        int32_t first = (int32_t)W_LANDING * height2 / 2 +
                        (int32_t)W_CLEARED * cleared +
                        ai_trespass(&v, sh, l);
        /* NEXT, every orientation and column, dropped from the top. */
        while (budget > 0 && ai->plan_inner < 4 * 16) {
            uint8_t o2 = (uint8_t)(ai->plan_inner / 16);
            int l2 = ai->plan_inner % 16 - 3;
            ai->plan_inner++;
            uint16_t sh2[4];
            ai_shape(v.next, o2, sh2);
            if (ai_hits(&after, sh2, l2, -2)) continue;
            budget--;
            AiBoard b2 = after;
            int32_t s2 = ai_score_drop(&b2, sh2, l2, -2, 0);
            if (s2 > ai->plan_reply_best) ai->plan_reply_best = s2;
        }
        if (ai->plan_inner < 4 * 16) break;      /* more next frame */
        int32_t total = ai->plan_reply_best == AI_DEAD
                        ? AI_DEAD : first + ai->plan_reply_best;
        if (total > ai->plan_best) {
            ai->plan_best = total;
            /* Still reachable from where the piece is NOW? */
            if (ai_reachable(&v.obstacles, v.piece, v.o0, v.l0, v.t0, o, l,
                             &v.pace))
                ai_set_target(ai, l, o);
        }
        ai->plan_cursor++;
        ai->plan_inner = 0;
        ai->plan_reply_best = AI_DEAD;
    }
}

void tengen_ai_think(TengenAi *ai, const TengenGame *game, TengenPlayerSlot slot) {
    if (ai->smart) ai_smart_think(ai, game, slot, TENGEN_AI_SMART_BUDGET);
}

void tengen_ai_reset(TengenAi *ai) {
    memset(ai, 0, sizeof(*ai));
}

void tengen_ai_choose(TengenAi *ai, const TengenGame *game,
                       TengenPlayerSlot slot) {
    uint8_t a[TENGEN_AI_SCRATCH_A];
    TengenTetromino piece = game->player[slot].piece.current;

    /* A new piece is a new hand on the pad, as far as `settle` is concerned. */
    ai->since_spawn = 0;

    /* The port's own computer plans over the next few frames instead. */
    if (ai->smart) {
        ai_smart_start(ai, game, slot);
        return;
    }

    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return;
    tengen_ai_heights(game, slot, a);
    /* The port's own, off by default and coop's only. See tengen_ai_shadow. */
    if (ai->coop_aware) tengen_ai_shadow(game, slot, a);

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

/* THE SAME CHOICE, MADE AGAIN FOR A PIECE ALREADY ON ITS WAY DOWN.
 *
 * WITH COMPUTER is one board with two pieces falling into it, and the
 * cartridge calls computerMove for player 2 on EVERY spawn, the human's
 * included (main.asm.txt:3740-3749 — the `txa`/`beq` that makes VERSUS ignore
 * player 1's spawn is skipped for this mode). So when a human piece lands in
 * the hole the computer was aiming at, the computer's next look at the board
 * is the one that notices.
 *
 * The only difference from a spawn's own call is `settle`, which is this
 * port's and not the ROM's: a piece halfway down that suddenly stops dead for
 * a quarter of a second reads as a hang, not as a decision. So the clock
 * carries on. */
void tengen_ai_rechoose(TengenAi *ai, const TengenGame *game,
                         TengenPlayerSlot slot) {
    uint8_t elapsed = ai->since_spawn;
    tengen_ai_choose(ai, game, slot);
    ai->since_spawn = elapsed;
}

uint8_t tengen_ai_buttons(TengenAi *ai, const TengenGame *game,
                           TengenPlayerSlot slot, uint8_t frame_counter) {
    const TengenPlayerState *p = &game->player[slot];
    uint8_t buttons = 0;

    /* The port's own computer thinks a slice a frame, and keeps its hands
     * off the pad until it has a target. */
    if (ai->smart) {
        ai->clock = frame_counter;
        ai_smart_think(ai, game, slot, TENGEN_AI_SMART_BUDGET);
        if (!ai->plan_have) {
            if (ai->since_spawn < 0xFF) ai->since_spawn++;
            return 0;
        }
    }

    /* Look at it before touching it. See `settle` on TengenAi. */
    if (ai->since_spawn < ai->settle) {
        ai->since_spawn++;
        return 0;
    }
    if (ai->since_spawn < 0xFF) ai->since_spawn++;

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

    /* AND DOWN, ON SEVEN FRAMES IN EVERY EIGHT. See `soft_drop`. Three things
     * decide the shape of this and the third one is the whole reason it is
     * written as a mask on the frame counter:
     *
     * IT HAS TO START AT ONCE. A piece spawns two rows above the field and
     * level-0 gravity is fifty-three frames a row, so a computer that waits
     * until it is lined up before dropping spends its first hundred frames
     * off the top of the screen and then falls through the whole board in
     * twenty. Which is what "no veo la caida de sus piezas" was: the piece
     * was drawn, there was just nothing to see for most of its life.
     *
     * IT HAS TO BREATHE. The soft drop TIGHTENS while Down is held — 20 frames
     * for the first step, then 19, 18, down to 1 — and resets to 5 the moment
     * it is let go (main.asm.txt:184-216). Held flat out, a piece crosses the
     * board in half a second; letting go keeps the ramp near its start.
     *
     * AND IT MUST LET GO ON THE FRAME BEFORE IT SHIFTS. This is the one that
     * broke it. The core discards a FRESH Left or Right outright if Down was
     * held on the previous frame (main.asm.txt:98-107) — a real quirk of the
     * cartridge's input handling, faithfully reproduced — and the driver
     * shifts on frames where the counter is a multiple of eight. Holding Down
     * through frame seven ate every shift the computer ever tried: it dropped
     * each piece straight down its spawn column, buried itself in a couple of
     * minutes, and then sat there dead. Both "ha vuelto a desaparecer la CPU"
     * and "Rival no sube de puntuacion" were that.
     *
     * So the gap goes at counter & 7 == 7: one frame off before every shift
     * frame, which both frees the shift and resets the ramp. A row roughly
     * every eight frames — a board in under three seconds, watchable, and
     * about seven times what waiting for gravity gives.
     *
     * ...EXCEPT THAT ON A SHARED BOARD IT WAITS ITS TURN, which is the other
     * half of `coop_aware` and worth as much as the shadow. Down is dropped
     * while the piece is not yet over the column it wants. On a board of its
     * own that would only be slower; on the shared one it is the difference
     * between crossing in front of the partner and shouldering it all the
     * way down, because a shift the partner refuses is retried eight frames
     * later and by then a piece that kept dropping is a row lower and out of
     * position. MEASURED over twenty-four playouts with the computer on both
     * pads: 1304 pieces and 120 lines with the shadow alone, 1637 and 197
     * with this as well, and refused shifts from 35% of all of them to 27%.
     * It costs pace — about eighty frames a piece instead of sixty-six — and
     * it cannot hang, because gravity runs whether Down is pressed or not.
     * Capping the wait was tried at 32, 48, 64 and 96 frames and every cap
     * was worse than no cap at all. */
    /* The port's own computer waits on any board, for its column and its
     * turn both: its plan assumed gravity only on the way across. */
    bool waiting = (ai->coop_aware && game->coop &&
                     ai->target_x != (uint8_t)p->piece.x) ||
                   (ai->smart && (ai->target_x != (uint8_t)p->piece.x ||
                                  ai->target_orientation != p->piece.orientation));
    if (ai->soft_drop && !buttons && !waiting && (frame_counter & 0x07) != 0x07)
        buttons |= TENGEN_BTN_DOWN;
    return buttons;
}
