/*
 * tengen_stats.h — THE GAME'S OWN FIGURES, for the port's end-of-game page.
 *
 * Not the cartridge's. The cartridge keeps this level's clears (for the
 * BONUS show) and a capped count of the pieces dealt (for the histogram),
 * and forgets both; the port, under the chord, puts a whole game's worth on
 * the board once it is over (gba/hud.c, draw_end_stats). What it counts:
 *
 *   frames       the game running for this player and not paused, the
 *                frame it topped out included
 *   pieces       pieces that came to rest
 *   clears[n]    clears of n+1 rows: singles, doubles, triples, tetrises
 *   drought_max  the longest run of pieces dealt with no I among them
 *
 * Fed once a frame per player with that player's step result, after the
 * step (tengen_stats_step); the rules never read any of it. Every count
 * saturates rather than wraps.
 */
#ifndef TENGEN_STATS_H
#define TENGEN_STATS_H

#include "tengen_core.h"

typedef struct {
    uint32_t frames;
    uint16_t pieces;
    uint16_t clears[4];
    uint16_t drought;       /* pieces dealt since the last I */
    uint16_t drought_max;
    /* What piece_total said last frame, all seven and the I's alone: the
     * pieces dealt since are the difference. */
    uint32_t dealt;
    uint16_t dealt_i;
} TengenStats;

/* A game starting: nothing counted. The first step counts the pieces the
 * new game was dealt to begin with. */
void tengen_stats_begin(TengenStats *s);

/* One frame of `slot`'s game, after its step. A board put back on its feet
 * (`restarted`) is a new game and starts the count again. */
void tengen_stats_step(TengenStats *s, const TengenGame *game,
                       TengenPlayerSlot slot, const TengenStepResult *r);

/* The lines the clears made, and how many in a hundred of them came four at
 * a time (rounded down; 0 with no lines). */
unsigned tengen_stats_lines(const TengenStats *s);
unsigned tengen_stats_tetris_rate(const TengenStats *s);

/* ONE BOARD, ONE PAGE: a coop board's figures are both players' together —
 * the pieces and the clears added, the time and the drought the longer of
 * the two (the BONUS show counts a coop board's clears the same way). */
TengenStats tengen_stats_sum(const TengenStats *a, const TengenStats *b);

#endif /* TENGEN_STATS_H */
