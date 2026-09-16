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

/* The two pads read the same generator from different starts, which is how
 * trace_match.py makes player 2 independent without a second one. */
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
    bool coop = argc > 3 && argv[3][0] == 'c';

    TengenGame game;
    tengen_new_game(&game, (uint16_t)seed, 0, coop, coop, false);
    printf("seed %04X\n", seed);

    /* FRAME 0 IS THE CARTRIDGE'S SPAWN FRAME, and tengen_new_game has just
     * done that frame's work — piece dealt, fall timer loaded, nothing
     * decremented. So the script starts one entry in and the loop at one. */
    long s1 = 12345, s2 = 999983;
    if (!coop) {
        (void)script_button(&s1);
        (void)script_button(&s2);
    }
    /* IN COOPERATIVE THE CARTRIDGE SPENDS ONE FRAME MORE ON THE DEAL than it
     * does in 1 PLAYER — its deal frame leaves both fall timers at 47 where
     * 1 PLAYER's leaves one at 48 — and that frame eats a button too. So the
     * script is NOT wound on here: this loop's step k takes the entry the
     * cartridge's frame k-1 took, and trace_match.py drops the first line to
     * put the two back on the same frame. See its note. */

    for (int f = 1; f < frames; f++) {
        uint8_t b1 = script_button(&s1);
        uint8_t b2 = script_button(&s2);
        const TengenPlayerState *p = &game.player[0];
        const TengenPlayerState *q = &game.player[1];
        /* mainLoop runs player 1 and then player 2, in that order
         * (main.asm.txt:71-74). */
        tengen_step(&game, TENGEN_PLAYER_1, b1);
        if (coop) tengen_step(&game, TENGEN_PLAYER_2, b2);

        /* BOTH SIDES REPORT WHAT THE FRAME LEFT BEHIND, which is the only way
         * the two can be compared: the cartridge's gameState is read after
         * its frame, so the frame that ends a game reads F9 there, and this
         * has to do the same or the last frame of every match looks like a
         * divergence. It is not one — both consoles top out on it. */
        if (!p->game_active) { printf("%d estado F9\n", f); continue; }
        /* AND THE CLEAR ANIMATION IS SKIPPED ON PURPOSE. While it runs the
         * ROM marks the completed rows in the playfield with $FE and the port
         * keeps them standing with a bitmask beside them, so the two boards
         * legitimately read differently for those few frames. What has to
         * match is the board the collapse leaves, and it is compared again
         * the frame after. */
        if (p->line_clear_timer || (coop && q->line_clear_timer)) {
            printf("%d estado 03\n", f);
            continue;
        }
        printf("%d p%d o%d y%d x%d n%d t%d", f,
               p->piece.current, p->piece.orientation, (int)p->piece.y,
               (int)p->piece.x, p->piece.next, p->fall_timer);
        if (coop)
            printf(" q%d o%d y%d x%d n%d t%d",
                   q->piece.current, q->piece.orientation, (int)q->piece.y,
                   (int)q->piece.x, q->piece.next, q->fall_timer);
        printf(" lvl%d L%d S%d ", p->level, (int)p->lines, (int)p->score);
        for (int r = 0; r < TENGEN_PF_HEIGHT; r++)
            for (int c = (coop ? 0 : 1); c <= (coop ? 11 : 10); c++)
                printf("%X", game.field[0].cell[r][c]);
        printf("\n");
    }
    return 0;
}
