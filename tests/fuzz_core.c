/*
 * fuzz_core.c — thousands of games of every mode, played by the computer
 * and by random hands, with the rules' invariants checked after every
 * frame. Built under AddressSanitizer and UBSan by `make fuzz`.
 *
 * What the host tests cannot reach is the combination nobody thought to
 * write down: a cheat code entered on the frame a clear ends, an A+B
 * restart under a handicap at level 19 with the prototypes' rules, two
 * coop pieces pressed together while a row goes. This plays enough of
 * them that the rare ones come up, and asks after every frame:
 *
 *   - every cell is a tile id (0-15), and the walls of a 1P/2P board are
 *     still walls;
 *   - level, score and lines are in range, and lines never go down except
 *     through a restart;
 *   - a falling piece is a piece (1-7, orientation 0-3), and did not MOVE
 *     into anything: a shift, a turn or a fall that lands in a settled cell,
 *     or into the coop partner's falling piece, is a broken collision rule.
 *
 * Two things that look like the last and are the cartridge's own are left
 * alone, because the port must do them too:
 *   - getNextTetromino deals the coop piece at its entry column without
 *     asking where the partner's is (main.asm.txt:3688-3725, no collision
 *     call), so a new piece can come up overlapping one still near the top;
 *   - a coop partner's rows collapsing move settled blocks, and a falling
 *     piece is not in the field to be moved: blocks can come down into it
 *     (INFERRED from the same split — the playfield buffer holds only
 *     settled cells; seen once in sixty million random frames, with the
 *     prototypes' rules).
 *
 *     fuzz_core [games] [seed]
 */
#include <stdio.h>
#include <stdlib.h>

#include "../src/tengen_core.h"
#include "../src/tengen_ai.h"

static unsigned long long g_rs;

static unsigned rnd(void) {
    g_rs ^= g_rs << 13;
    g_rs ^= g_rs >> 7;
    g_rs ^= g_rs << 17;
    return (unsigned)(g_rs >> 11);
}

static int g_fails;
static int g_game;
static long g_frame;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            if (g_fails++ < 20) {                                     \
                printf("FAIL game %d frame %ld: ", g_game, g_frame);  \
                printf(__VA_ARGS__);                                  \
                printf("\n");                                         \
            }                                                         \
        }                                                             \
    } while (0)

static bool piece_in_play(const TengenGame *g, int p) {
    const TengenPlayerState *ps = &g->player[p];
    return ps->game_active && ps->piece.current != TT_NONE &&
           !ps->line_clear_timer;
}

static bool same_place(const TengenPiece *a, const TengenPiece *b) {
    return a->current == b->current && a->x == b->x && a->y == b->y &&
           a->orientation == b->orientation;
}

int main(int argc, char **argv) {
    int games = argc > 1 ? atoi(argv[1]) : 200;
    g_rs = argc > 2 ? strtoull(argv[2], NULL, 0) : 88172645463325252ULL;
    if (!g_rs) g_rs = 1;
    long total = 0;
    int top_level = 0;
    unsigned long top_lines = 0;

    for (g_game = 0; g_game < games && g_fails <= 20; g_game++) {
        TengenGame g;
        int mode = (int)(rnd() % 5);   /* 1P, 2P, COOP, VERSUS AI, WITH AI */
        bool two = mode != 0;
        bool coop = mode == 2 || mode == 4;
        bool xe = rnd() & 1;
        uint8_t start = (uint8_t)(rnd() % (xe ? 20 : 18));
        tengen_new_game(&g, (uint16_t)rnd(), start, two, coop, xe);
        if (!(rnd() % 4)) g.proto_rules = true;
        if (!(rnd() % 4)) g.piece_id_cells = true;
        tengen_apply_handicap(&g, TENGEN_PLAYER_1, (uint8_t)(rnd() % 5));
        if (two && !coop)
            tengen_apply_handicap(&g, TENGEN_PLAYER_2, (uint8_t)(rnd() % 5));

        TengenAi ai[2];
        tengen_ai_reset(&ai[0]);
        tengen_ai_reset(&ai[1]);
        bool use_ai[2] = { (rnd() % 3) != 0, mode >= 3 || (rnd() & 1) };
        TengenTetromino seen[2] = { TT_NONE, TT_NONE };
        uint8_t held[2] = { 0, 0 }, prev[2] = { 0, 0 };
        uint32_t lines_before[2] = { 0, 0 };
        long frames = 20000 + (long)(rnd() % 60000);
        int players = two ? 2 : 1;
        int max_level = xe ? TENGEN_MAX_LEVEL_XE : TENGEN_MAX_LEVEL;
        uint8_t clock = 0;

        for (g_frame = 0; g_frame < frames && g_fails <= 20; g_frame++) {
            for (int p = 0; p < players; p++) {
                if (use_ai[p] && g.player[p].game_active) {
                    TengenTetromino now = g.player[p].piece.current;
                    if (now != seen[p])
                        tengen_ai_choose(&ai[p], &g, (TengenPlayerSlot)p);
                    seen[p] = now;
                    held[p] = tengen_ai_buttons(&ai[p], &g, (TengenPlayerSlot)p,
                                                clock);
                    if (!(rnd() % 50)) held[p] |= TENGEN_BTN_DOWN;
                } else if (!(rnd() % 6)) {
                    held[p] = (uint8_t)rnd();
                    /* START now and then, so games are mostly played and
                     * the codes get typed on the plaque sometimes. */
                    if (rnd() % 20) held[p] &= (uint8_t)~TENGEN_BTN_START;
                }
            }
            if (g.paused && !(rnd() % 30)) held[0] |= TENGEN_BTN_START;
            uint8_t presses[2] = { (uint8_t)(held[0] & ~prev[0]),
                                   (uint8_t)(held[1] & ~prev[1]) };
            tengen_pause_input(&g, presses, NULL);

            for (int p = 0; p < players; p++) {
                TengenGame before = g;
                TengenStepResult r = tengen_step(&g, (TengenPlayerSlot)p, held[p]);
                if (r.restarted) lines_before[p] = 0;
                /* WHAT THIS STEP MOVED, and only that: the stepping
                 * player's piece, if it was in play before and after and
                 * is the same piece. */
                const TengenPlayerState *was = &before.player[p];
                const TengenPlayerState *is = &g.player[p];
                bool moved = piece_in_play(&before, p) && piece_in_play(&g, p) &&
                             was->piece.current == is->piece.current &&
                             !same_place(&was->piece, &is->piece) &&
                             !r.restarted && !before.paused;
                if (moved)
                    CHECK(tengen_position_valid(&g, (TengenPlayerSlot)p),
                          "p%d moved into the field: piece %d (%d,%d o%d)",
                          p, is->piece.current, is->piece.x, is->piece.y,
                          is->piece.orientation);
                if (moved && coop && piece_in_play(&g, p ^ 1) &&
                    !tengen_coop_pieces_overlap(&before, (TengenPlayerSlot)p))
                    CHECK(!tengen_coop_pieces_overlap(&g, (TengenPlayerSlot)p),
                          "p%d moved into its partner's piece", p);
            }
            prev[0] = held[0];
            prev[1] = held[1];
            clock++;

            int boards = two && !coop ? 2 : 1;
            for (int b = 0; b < boards; b++)
                for (int y = 0; y < TENGEN_PF_HEIGHT; y++)
                    for (int x = 0; x < TENGEN_PF_WIDTH; x++) {
                        uint8_t c = g.field[b].cell[y][x];
                        CHECK(c <= 15, "board %d cell (%d,%d) = %d", b, y, x, c);
                        if (!coop && (x == 0 || x == TENGEN_PF_WIDTH - 1))
                            CHECK(c == TT_WALL, "board %d wall (%d,%d) = %d",
                                  b, y, x, c);
                    }
            for (int p = 0; p < players; p++) {
                const TengenPlayerState *ps = &g.player[p];
                if (ps->level > top_level) top_level = ps->level;
                if (ps->lines > top_lines) top_lines = ps->lines;
                CHECK(ps->level <= max_level, "p%d level %d", p, ps->level);
                CHECK(ps->score <= 999999, "p%d score %u", p, (unsigned)ps->score);
                CHECK(ps->lines >= lines_before[p], "p%d lines %u -> %u", p,
                      (unsigned)lines_before[p], (unsigned)ps->lines);
                lines_before[p] = ps->lines;
                CHECK(ps->piece.orientation < 4, "p%d orientation %d", p,
                      ps->piece.orientation);
                if (ps->game_active && ps->piece.current != TT_NONE) {
                    CHECK(ps->piece.current <= TT_Z, "p%d piece %d", p,
                          ps->piece.current);
                    CHECK(ps->piece.next >= TT_I && ps->piece.next <= TT_Z,
                          "p%d next %d", p, ps->piece.next);
                }
            }
            bool alive = g.player[0].game_active ||
                         (two && g.player[1].game_active);
            if (!alive && (mode == 0 || coop)) break;
        }
        total += g_frame;
    }
    printf("fuzz_core: %d games, %ld frames, top level %d, top lines %lu, "
           "%d failures\n", g_game, total, top_level, top_lines, g_fails);
    return g_fails != 0;
}
