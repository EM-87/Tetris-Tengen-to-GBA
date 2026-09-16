/*
 * trace_core.c — the port's side of the match trace. See tools/trace_match.py.
 *
 * One line per frame, in the same shape the Python side writes for the
 * cartridge, so `diff` is the whole comparison. Nothing here interprets: it
 * seeds the core with the seed the cartridge reported, feeds it the same
 * button script, and prints what the core says.
 *
 * THE SCRIPT GENERATOR IS DUPLICATED IN trace_match.py ON PURPOSE. Two copies
 * of nine lines are cheaper than a file the two sides have to agree about,
 * and if they ever drift the trace diverges on frame one rather than
 * silently comparing two different matches.
 */
#include "../src/tengen_core.h"

#include <stdio.h>
#include <stdlib.h>

static uint8_t script_button(long *s) {
    *s = (*s * 1103515245 + 12345) & 0x7FFFFFFF;
    switch ((int)((*s >> 16) % 10)) {
        case 0: return TENGEN_BTN_LEFT;
        case 1: return TENGEN_BTN_RIGHT;
        case 2: return TENGEN_BTN_A;
        case 3: return TENGEN_BTN_B;
        default: return TENGEN_BTN_DOWN;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: trace_core <seed-hex> <frames>\n");
        return 2;
    }
    unsigned seed = (unsigned)strtoul(argv[1], NULL, 16);
    int frames = atoi(argv[2]);

    TengenGame game;
    tengen_new_game(&game, (uint16_t)seed, 0, false, false, false);
    printf("seed %04X\n", seed);

    /* FRAME 0 IS THE CARTRIDGE'S SPAWN FRAME, and tengen_new_game has just
     * done that frame's work — piece dealt, fall timer loaded, nothing
     * decremented. So the script starts one entry in and the loop at one. */
    long s = 12345;
    (void)script_button(&s);

    for (int f = 1; f < frames; f++) {
        uint8_t b = script_button(&s);
        const TengenPlayerState *p = &game.player[0];
        if (!p->game_active) { printf("%d estado F9\n", f); continue; }
        if (p->line_clear_timer) {
            tengen_step(&game, TENGEN_PLAYER_1, b);
            printf("%d estado 03\n", f);
            continue;
        }
        tengen_step(&game, TENGEN_PLAYER_1, b);
        printf("%d p%d o%d y%d x%d n%d t%d lvl%d L%d S%d ", f,
               p->piece.current, p->piece.orientation, (int)p->piece.y,
               (int)p->piece.x, p->piece.next, p->fall_timer, p->level,
               (int)p->lines, (int)p->score);
        for (int r = 0; r < TENGEN_PF_HEIGHT; r++)
            for (int c = 1; c <= 10; c++)
                printf("%X", game.field[0].cell[r][c]);
        printf("\n");
    }
    return 0;
}
