/*
 * ai_bench.c — the computers, measured by playing.
 *
 * The cartridge's computer (computerMove, with the port's `coop_aware` on
 * the shared board, as the chord gives it) against the port's own (`smart`,
 * tengen_ai.c), on the same seeds, at the cartridge's pace for both (no soft drop,
 * no settle: what the GBA plays). Lines cleared and pieces placed until the
 * board tops out or the frames run out.
 *
 *   SOLO     one computer, a ten-wide board (VERSUS's board, alone)
 *   COOP     two computers on the twelve-wide board (WITH COMPUTER, with a
 *            computer for a human)
 *   COOP+    the port's computer with the CARTRIDGE'S for a partner: a
 *            partner that does not read it back, which is what a person is
 *            to it
 *
 *     ai_bench [games] [frames] [start_level]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tengen_core.h"
#include "../src/tengen_ai.h"

typedef struct { long pieces, lines, holes_end, dead, lines_p[2], frames; } Tally;

static bool g_adaptive;     /* the port's computer adapts to its partner */
static bool g_fast_partner; /* ...whose partner, the cartridge's, drops */

static void setup(TengenAi *ai, bool smart, bool coop) {
    tengen_ai_reset(ai);
    ai->coop_aware = coop;
    ai->smart = smart;
    ai->adaptive = smart && g_adaptive;
}

static void play(int games, long frames, int level, bool coop,
                 const bool smart[2], Tally *t) {
    memset(t, 0, sizeof(*t));
    for (int g = 0; g < games; g++) {
        TengenGame game;
        TengenAi ai[2];
        TengenTetromino last[2] = { TT_NONE, TT_NONE };
        TengenTetromino partner[2] = { TT_NONE, TT_NONE };
        int players = coop ? 2 : 1;
        bool dead = false;
        tengen_new_game(&game, (uint16_t)(0x1234 + g * 0x2F1B), (uint8_t)level,
                        coop, coop, false);
        for (int s = 0; s < 2; s++) setup(&ai[s], smart[s], coop);
        /* A partner that drops its pieces: the cartridge's computer with
         * its soft drop on, standing in for a quick human. */
        if (g_fast_partner)
            for (int s = 0; s < 2; s++) if (!smart[s]) ai[s].soft_drop = true;
        for (long f = 0; f < frames && !dead; f++) {
            for (int s = 0; s < players && !dead; s++) {
                TengenPlayerSlot slot = (TengenPlayerSlot)s;
                TengenPlayerState *p = &game.player[s];
                if (!p->game_active) { dead = true; break; }
                TengenTetromino mine = p->piece.current;
                TengenTetromino theirs = game.player[s ^ 1].piece.current;
                /* Both computers: each knows where the other is going. */
                ai[s].partner_known = coop && ai[s ^ 1].smart && ai[s ^ 1].plan_have;
                ai[s].partner_x = ai[s ^ 1].target_x;
                ai[s].partner_o = ai[s ^ 1].target_orientation;
                if (mine != last[s])
                    tengen_ai_choose(&ai[s], &game, slot);
                else if (coop && theirs != partner[s] &&
                         theirs != TT_NONE && mine != TT_NONE)
                    tengen_ai_rechoose(&ai[s], &game, slot);
                last[s] = mine;
                partner[s] = theirs;
                TengenStepResult r = tengen_step(
                    &game, slot, tengen_ai_buttons(&ai[s], &game, slot, (uint8_t)f));
                if (r.piece_locked) t->pieces++;
                if (r.lines_collapsed)
                    for (int i = 0; i < TENGEN_PF_HEIGHT; i++)
                        if (r.rows_cleared_mask & (1u << i)) t->lines++;
                if (r.topped_out) dead = true;
            }
            t->frames++;
        }
        for (int s = 0; s < 2; s++) t->lines_p[s] += game.player[s].lines;
        if (dead) t->dead++;
    }
}

int main(int argc, char **argv) {
    int games = argc > 1 ? atoi(argv[1]) : 8;
    long frames = argc > 2 ? atol(argv[2]) : 20000;
    int level = argc > 3 ? atoi(argv[3]) : 0;
    /* A fourth argument: "fast" gives the cartridge's computer a soft drop,
     * "adapt" turns on the port's manners, "both" does both. */
    if (argc > 4) {
        g_fast_partner = !strcmp(argv[4], "fast") || !strcmp(argv[4], "both");
        g_adaptive = !strcmp(argv[4], "adapt") || !strcmp(argv[4], "both");
    }
    static const struct { const char *name; bool coop; bool smart[2]; } kRuns[] = {
        { "SOLO   cartucho        ", false, { false, false } },
        { "SOLO   del port        ", false, { true, false } },
        { "COOP   cartucho+cartucho", true, { false, false } },
        { "COOP   port+port       ", true, { true, true } },
        { "COOP+  port+cartucho   ", true, { true, false } },
    };
    printf("%d partidas, %ld frames como mucho, nivel %d\n", games, frames, level);
    for (unsigned i = 0; i < sizeof kRuns / sizeof kRuns[0]; i++) {
        Tally t;
        play(games, frames, level, kRuns[i].coop, kRuns[i].smart, &t);
        printf("  %s  piezas %6ld  lineas %6ld  enterrados %ld de %d"
               "  (lineas j1 %ld j2 %ld, frames %ld)\n",
               kRuns[i].name, t.pieces, t.lines, t.dead, games,
               t.lines_p[0], t.lines_p[1], t.frames);
    }
    return 0;
}
