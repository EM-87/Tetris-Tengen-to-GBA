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
#include "../src/tengen_ai.h"
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
        fprintf(stderr, "usage: trace_core <seed-hex> <frames> "
                        "[coop | vs|with|demo <frame-counter> [pad1]] "
                        "[h=<handicap>] [l=<level>]\n");
        return 2;
    }
    unsigned seed = (unsigned)strtoul(argv[1], NULL, 16);
    int frames = atoi(argv[2]);
    bool coop = argc > 3 && argv[3][0] == 'c';
    /* VERSUS COMPUTER: two boards, the script on player 1 and computerMove
     * on player 2, with every knob TengenAi adds left off — this is the
     * cartridge's computer, not the one the GBA plays (see main.c). Its
     * cadence runs off frameCounterLow, so the cartridge's value on the
     * first compared frame comes in as an argument; from there it counts. */
    bool vs = argc > 4 && argv[3][0] == 'v';
    /* WITH COMPUTER is the same computer on COOPERATIVE's shared board. */
    bool with = argc > 4 && argv[3][0] == 'w';
    bool computer = vs || with;
    coop = coop || with;
    /* THE ATTRACT DEMO: 1 PLAYER with computerMove on player 1's pad
     * (compInputForDemo, main.asm.txt:4178), planning on its own spawns
     * (getNextTetromino, :3731-3736). Same clock as the other two. */
    bool demo = argc > 4 && argv[3][0] == 'd';
    uint8_t clock = (computer || demo) ? (uint8_t)atoi(argv[4]) : 0;
    /* "l=N": the level the game starts on — the demo's is whatever the level
     * menu was last left on. */
    uint8_t start_level = 0;
    for (int i = 3; i < argc; i++)
        if (argv[i][0] == 'l' && argv[i][1] == '=')
            start_level = (uint8_t)atoi(argv[i] + 2);
    TengenAi ai;
    tengen_ai_reset(&ai);
    TengenTetromino ai_piece = TT_NONE, partner_piece = TT_NONE;
    /* "pad1": player 1 played by the port's own computer instead of the
     * random script, and each frame's buttons printed as the line's last
     * field, for trace_match.py to press on the cartridge. The random
     * script tops the shared board out before WITH clears a single row;
     * this plays long enough to clear them. It is only a source of buttons:
     * what gets compared is still what both consoles do with them. */
    bool pad1 = argc > 5 && argv[5][0] == 'p';
    TengenAi ai1;
    tengen_ai_reset(&ai1);
    ai1.soft_drop = !demo;          /* the demo's is the cartridge's, as is */
    ai1.coop_aware = !demo;
    pad1 = pad1 || demo;

    TengenGame game;
    tengen_new_game(&game, (uint16_t)seed, start_level, coop || vs, coop, false);
    /* "h=N": player 1's starting handicap, laid the way endPlayfieldInit
     * lays it, right after the playfield is set up (main.asm.txt:3536). */
    /* Against the computer BOTH boards take player 1's (`bcs
     * @computerIsPlaying` skips the per-player read, :3539-3542); a shared
     * board takes it once. */
    for (int i = 3; i < argc; i++)
        if (argv[i][0] == 'h' && argv[i][1] == '=') {
            uint8_t h = (uint8_t)atoi(argv[i] + 2);
            tengen_apply_handicap(&game, TENGEN_PLAYER_1, h);
            if (vs) tengen_apply_handicap(&game, TENGEN_PLAYER_2, h);
        }
    printf("seed %04X\n", seed);
    /* The deal frame's getNextTetromino has already called computerMove. */
    if (computer) {
        tengen_ai_choose(&ai, &game, TENGEN_PLAYER_2);
        ai_piece = game.player[1].piece.current;
        partner_piece = game.player[0].piece.current;
        if (pad1) tengen_ai_choose(&ai1, &game, TENGEN_PLAYER_1);
    }
    if (demo) {
        tengen_ai_choose(&ai1, &game, TENGEN_PLAYER_1);
        partner_piece = game.player[0].piece.current;
    }

    /* FRAME 0 IS THE CARTRIDGE'S DEAL, and tengen_new_game has just done
     * that frame's work — piece dealt, fall timer loaded, nothing
     * decremented. So the script starts one entry in and the loop at one:
     * step f takes the entry the cartridge's iteration f took. */
    long s1 = 12345, s2 = 999983;
    (void)script_button(&s1);
    (void)script_button(&s2);

    for (int f = 1; f < frames; f++) {
        uint8_t b1 = script_button(&s1);
        uint8_t b2 = script_button(&s2);
        const TengenPlayerState *p = &game.player[0];
        const TengenPlayerState *q = &game.player[1];
        if (pad1)
            b1 = p->game_active
                ? tengen_ai_buttons(&ai1, &game, TENGEN_PLAYER_1, clock) : 0;
        /* mainLoop runs player 1 and then player 2, in that order
         * (main.asm.txt:71-74). */
        if (computer) {
            /* The ROM reads the computer's pad before either player moves
             * (compInputForGameplay, main.asm.txt:4164). */
            b2 = q->game_active
                ? tengen_ai_buttons(&ai, &game, TENGEN_PLAYER_2, clock) : 0;
            clock++;
        } else if (demo) {
            clock++;
        }
        tengen_step(&game, TENGEN_PLAYER_1, b1);
        /* ...and plans inside getNextTetromino, on the frame a piece appears
         * (main.asm.txt:3740-3749): in VERSUS only its own, in WITH player
         * 1's as well — which happens before player 2's step does. */
        if (with && q->game_active && p->piece.current != partner_piece &&
            p->piece.current != TT_NONE)
            tengen_ai_choose(&ai, &game, TENGEN_PLAYER_2);
        if (pad1 && p->piece.current != partner_piece &&
            p->piece.current != TT_NONE)
            tengen_ai_choose(&ai1, &game, TENGEN_PLAYER_1);
        partner_piece = p->piece.current;
        if (coop || vs) tengen_step(&game, TENGEN_PLAYER_2, b2);
        if (computer && q->game_active && q->piece.current != ai_piece &&
            q->piece.current != TT_NONE)
            tengen_ai_choose(&ai, &game, TENGEN_PLAYER_2);
        ai_piece = q->piece.current;

        if (computer || demo) {
            /* Two boards, each its own game: one ending or clearing does not
             * stop the other being compared. */
            printf("%d", f);
            for (int i = 0; i < 2; i++) {
                const TengenPlayerState *r = &game.player[i];
                if (!r->game_active) { printf(" | fin"); continue; }
                if (r->line_clear_timer) { printf(" | limpia"); continue; }
                printf(" | p%d o%d y%d x%d n%d t%d lvl%d L%d S%d ",
                       r->piece.current, r->piece.orientation,
                       (int)r->piece.y, (int)r->piece.x, r->piece.next,
                       r->fall_timer, r->level, (int)r->lines, (int)r->score);
                /* WITH's one board is twelve wide and printed under both. */
                for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
                    for (int c = with ? 0 : 1; c <= (with ? 11 : 10); c++)
                        printf("%X", game.field[with ? 0 : i].cell[row][c]);
            }
            const TengenAi *shown = demo ? &ai1 : &ai;   /* whose compTarget */
            printf(" | T%d,%d", shown->target_x, shown->target_orientation);
            if (pad1) printf(" | B%d", b1);
            printf("\n");
            continue;
        }
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
