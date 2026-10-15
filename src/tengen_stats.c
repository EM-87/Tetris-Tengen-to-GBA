/*
 * tengen_stats.c — the end-of-game figures. See tengen_stats.h.
 */
#include "tengen_stats.h"

#include <string.h>

static uint16_t add16(uint16_t a, uint32_t b) {
    uint32_t sum = (uint32_t)a + b;
    return (uint16_t)(sum > 0xFFFF ? 0xFFFF : sum);
}

void tengen_stats_begin(TengenStats *s) {
    memset(s, 0, sizeof *s);
}

void tengen_stats_step(TengenStats *s, const TengenGame *game,
                       TengenPlayerSlot slot, const TengenStepResult *r) {
    if (r->restarted) tengen_stats_begin(s);
    const TengenPlayerState *p = &game->player[slot];
    if (!game->paused && (p->game_active || r->topped_out) && s->frames != 0xFFFFFFFFu)
        s->frames++;
    if (r->piece_locked) s->pieces = add16(s->pieces, 1);
    if (r->lines_cleared) {
        int rows = 0;
        for (uint32_t m = r->rows_cleared_mask; m; m &= m - 1) rows++;
        if (rows >= 1 && rows <= 4) s->clears[rows - 1] = add16(s->clears[rows - 1], 1);
    }

    /* The deals since last frame, off the port's own count of them. An I
     * among them ends the run; any dealt with it are taken to have come
     * after it (one a frame is all a step deals). */
    uint32_t dealt = 0;
    for (int t = TT_I; t <= TT_Z; t++) dealt += p->piece_total[t];
    uint16_t dealt_i = p->piece_total[TT_I];
    if (dealt > s->dealt) {
        uint32_t fresh = dealt - s->dealt;
        uint32_t fresh_i = dealt_i >= s->dealt_i ? (uint32_t)(dealt_i - s->dealt_i) : 0;
        if (fresh_i > fresh) fresh_i = fresh;
        s->drought = fresh_i ? add16(0, fresh - fresh_i) : add16(s->drought, fresh);
        if (s->drought > s->drought_max) s->drought_max = s->drought;
    }
    s->dealt = dealt;
    s->dealt_i = dealt_i;
}

unsigned tengen_stats_lines(const TengenStats *s) {
    return s->clears[0] + 2u * s->clears[1] + 3u * s->clears[2] + 4u * s->clears[3];
}

unsigned tengen_stats_tetris_rate(const TengenStats *s) {
    unsigned lines = tengen_stats_lines(s);
    return lines ? (unsigned)(400ul * s->clears[3] / lines) : 0;
}

TengenStats tengen_stats_sum(const TengenStats *a, const TengenStats *b) {
    TengenStats s = *a;
    s.frames = a->frames > b->frames ? a->frames : b->frames;
    s.pieces = add16(a->pieces, b->pieces);
    for (int i = 0; i < 4; i++) s.clears[i] = add16(a->clears[i], b->clears[i]);
    s.drought = a->drought > b->drought ? a->drought : b->drought;
    s.drought_max = a->drought_max > b->drought_max ? a->drought_max : b->drought_max;
    return s;
}
