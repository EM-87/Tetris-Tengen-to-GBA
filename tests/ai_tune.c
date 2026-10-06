/*
 * ai_tune.c — the port's computer's coop weights, looked for by evolution.
 *
 * The weights the port's computer (`smart`, tengen_ai.c) scores a placement
 * with were El-Tetris's: tuned for a board of one's own, with all the time
 * in the world to get a piece anywhere. On a shared board, at the
 * cartridge's pace, with a partner in the way, nothing says they are still
 * the right ones. So this plays, and keeps what plays better.
 *
 * THE METHOD is the cross-entropy one, which needs nothing but playing:
 * each generation draws `POP` sets of weights around a mean (each weight
 * the base's times exp(N(0, sigma))) — the landing weight held at its base,
 * since a placement's ranking is the same under any common scale — plays
 * every one of them on the same games, keeps the best `ELITE`, and moves
 * the mean and the spreads to them. PAR at a time, one process each.
 *
 * THE GAMES are coop, with three kinds of partner: the cartridge's computer
 * at its own pace and with its soft drop on (two stand-ins for a person,
 * who does not say where they are going), and the port's own. Each at two
 * starting levels. The fitness is the board's lines in a fixed number of
 * frames (or until it tops out), each kind of game divided by what the
 * whole generation made of it, so all six count alike.
 *
 * WHAT THE FIRST RUN TAUGHT: a game with the cartridge's computer for a
 * partner lasts fifteen lines or so, and how long is mostly the deal. On
 * the same eight seeds every generation, the evolution learned those eight
 * deals (1.36 there, 0.96 on others). So every generation gets seeds it has
 * never played, the answer is the distribution's MEAN — never the best
 * single draw, which is the luckiest — and it is only worth anything if it
 * wins on seeds no generation saw, game for game against the base (the
 * same seed for both), by more than its standard error.
 *
 *     ai_tune [generations] [games per scenario] [frames] [validation games]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/tengen_core.h"
#include "../src/tengen_ai.h"

#define NW 8
#define POP 16
#define ELITE 4
#define PAR 4
#define MAX_GAMES 128

typedef struct { int partner; int level; } Scenario;    /* 0 cart, 1 cart fast, 2 port */
static const Scenario kScen[] = {
    { 0, 5 }, { 0, 12 }, { 1, 8 }, { 1, 15 }, { 2, 10 }, { 2, 16 },
};
#define NSCEN ((int)(sizeof kScen / sizeof kScen[0]))

static long play(const Scenario *sc, uint16_t seed, long frames) {
    TengenGame game;
    TengenAi ai[2];
    TengenTetromino last[2] = { TT_NONE, TT_NONE }, partner[2] = { TT_NONE, TT_NONE };
    tengen_new_game(&game, seed, (uint8_t)sc->level, true, true, false);
    for (int s = 0; s < 2; s++) {
        tengen_ai_reset(&ai[s]);
        ai[s].coop_aware = true;
    }
    ai[1].smart = true;                       /* the port's, as player 2 */
    ai[1].adaptive = true;
    if (sc->partner == 2) { ai[0].smart = true; ai[0].adaptive = true; }
    if (sc->partner == 1) ai[0].soft_drop = true;
    long lines = 0;
    for (long f = 0; f < frames; f++) {
        for (int s = 0; s < 2; s++) {
            TengenPlayerSlot slot = (TengenPlayerSlot)s;
            if (!game.player[s].game_active) return lines;
            TengenTetromino mine = game.player[s].piece.current;
            TengenTetromino theirs = game.player[s ^ 1].piece.current;
            ai[s].partner_known = ai[s ^ 1].smart && ai[s].smart && ai[s ^ 1].plan_have;
            ai[s].partner_x = ai[s ^ 1].target_x;
            ai[s].partner_o = ai[s ^ 1].target_orientation;
            if (mine != last[s]) tengen_ai_choose(&ai[s], &game, slot);
            else if (theirs != partner[s] && theirs != TT_NONE && mine != TT_NONE)
                tengen_ai_rechoose(&ai[s], &game, slot);
            last[s] = mine;
            partner[s] = theirs;
            TengenStepResult r = tengen_step(&game, slot,
                tengen_ai_buttons(&ai[s], &game, slot, (uint8_t)f));
            if (r.lines_collapsed)
                for (int i = 0; i < TENGEN_PF_HEIGHT; i++)
                    if (r.rows_cleared_mask & (1u << i)) lines++;
            if (r.topped_out) return lines;
        }
    }
    return lines;
}

static const char *kNames[NW] = { "landing", "cleared", "row_trans", "col_trans",
                                  "holes", "wells", "side", "cross" };

static void set_weights(const double *w) {
    TengenAiWeights *c = &tengen_ai_weights[1];
    c->landing = (int32_t)lround(w[0]); c->cleared = (int32_t)lround(w[1]);
    c->row_trans = (int32_t)lround(w[2]); c->col_trans = (int32_t)lround(w[3]);
    c->holes = (int32_t)lround(w[4]); c->wells = (int32_t)lround(w[5]);
    c->side = (int32_t)lround(w[6]); c->cross = (int32_t)lround(w[7]);
}

static uint16_t seed_of(unsigned seed0, int s, int g) {
    return (uint16_t)(seed0 * 0x9E37u + (unsigned)g * 0x2F1Bu + (unsigned)s * 0x51u);
}

/* Lines per scenario and game, `games` seeds from `seed0`. */
typedef long Lines[NSCEN][MAX_GAMES];

static void evaluate(const double *w, int games, long frames, unsigned seed0, Lines out) {
    set_weights(w);
    for (int s = 0; s < NSCEN; s++)
        for (int g = 0; g < games; g++)
            out[s][g] = play(&kScen[s], seed_of(seed0, s, g), frames);
}

/* The same, for several weight sets at once, PAR processes at a time. */
static void evaluate_all(double w[][NW], int n, int games, long frames,
                         unsigned seed0, Lines *out) {
    for (int i = 0; i < n; i += PAR) {
        int fds[PAR][2], kids = 0;
        pid_t pids[PAR];
        for (int k = 0; k < PAR && i + k < n; k++, kids++) {
            if (pipe(fds[k])) { perror("pipe"); exit(1); }
            pids[k] = fork();
            if (pids[k] == 0) {
                static Lines r;
                close(fds[k][0]);
                evaluate(w[i + k], games, frames, seed0, r);
                const char *p = (const char *)r;
                size_t left = sizeof r;
                while (left) {
                    ssize_t got = write(fds[k][1], p, left);
                    if (got <= 0) _exit(1);
                    p += got; left -= (size_t)got;
                }
                _exit(0);
            }
            close(fds[k][1]);
        }
        for (int k = 0; k < kids; k++) {
            char *p = (char *)out[i + k];
            size_t left = sizeof out[0];
            while (left) {
                ssize_t got = read(fds[k][0], p, left);
                if (got <= 0) { memset(out[i + k], 0, sizeof out[0]); break; }
                p += got; left -= (size_t)got;
            }
            close(fds[k][0]);
            waitpid(pids[k], NULL, 0);
        }
    }
}

static double total(const Lines r, int s, int games) {
    double t = 0;
    for (int g = 0; g < games; g++) t += (double)r[s][g];
    return t;
}

static unsigned long long g_rs = 0x2545F4914F6CDD1DULL;
static double gauss(void) {
    double u, v, s;
    do {
        g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17;
        u = (double)(g_rs >> 11) / 9007199254740992.0 * 2 - 1;
        g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17;
        v = (double)(g_rs >> 11) / 9007199254740992.0 * 2 - 1;
        s = u * u + v * v;
    } while (s >= 1 || s == 0);
    return u * sqrt(-2 * log(s) / s);
}

int main(int argc, char **argv) {
    int gens = argc > 1 ? atoi(argv[1]) : 30;
    int games = argc > 2 ? atoi(argv[2]) : 24;
    long frames = argc > 3 ? atol(argv[3]) : 20000;
    int vgames = argc > 4 ? atoi(argv[4]) : 96;
    if (games > MAX_GAMES) games = MAX_GAMES;
    if (vgames > MAX_GAMES) vgames = MAX_GAMES;
    const unsigned VALID = 0xBEEF;
    const TengenAiWeights *b0 = &tengen_ai_weights[1];
    double base[NW] = { b0->landing, b0->cleared, b0->row_trans, b0->col_trans,
                        b0->holes, b0->wells, b0->side, b0->cross };
    double mean[NW], sigma[NW];
    for (int i = 0; i < NW; i++) { mean[i] = 0; sigma[i] = i == 0 ? 0 : 0.4; }

    static Lines r[POP];
    for (int gen = 0; gen < gens; gen++) {
        double x[POP][NW], w[POP][NW], f[POP];
        for (int p = 0; p < POP; p++)
            for (int i = 0; i < NW; i++) {
                x[p][i] = p == 0 ? mean[i] : mean[i] + sigma[i] * gauss();
                w[p][i] = base[i] * exp(x[p][i]);
            }
        /* Seeds no generation has played: 1 + gen, never VALID. */
        evaluate_all(w, POP, games, frames, 1u + (unsigned)gen, r);
        double norm[NSCEN];
        for (int s = 0; s < NSCEN; s++) {
            norm[s] = 0;
            for (int p = 0; p < POP; p++) norm[s] += total(r[p], s, games);
            norm[s] = norm[s] / POP;
            if (norm[s] < 1) norm[s] = 1;
        }
        int order[POP];
        for (int p = 0; p < POP; p++) {
            f[p] = 0;
            for (int s = 0; s < NSCEN; s++) f[p] += total(r[p], s, games) / norm[s];
            f[p] /= NSCEN;
            order[p] = p;
        }
        for (int a = 0; a < POP; a++)
            for (int c = a + 1; c < POP; c++)
                if (f[order[c]] > f[order[a]]) { int t = order[a]; order[a] = order[c]; order[c] = t; }
        for (int i = 0; i < NW; i++) {
            double m = 0, v = 0;
            for (int e = 0; e < ELITE; e++) m += x[order[e]][i];
            m /= ELITE;
            for (int e = 0; e < ELITE; e++) v += (x[order[e]][i] - m) * (x[order[e]][i] - m);
            /* Halfway to the elite, not all the way: one generation's luck
             * moves the mean half as far. */
            mean[i] = 0.5 * mean[i] + 0.5 * m;
            if (i) sigma[i] = fmax(0.5 * sigma[i] + 0.5 * sqrt(v / ELITE), 0.08);
        }
        printf("gen %2d: best %.3f mean-cand %.3f  mean:", gen, f[order[0]], f[0]);
        for (int i = 1; i < NW; i++) printf(" %+.2f", mean[i]);
        printf("\n");
        fflush(stdout);
    }

    /* Validation: the base against the mean, the same seeds game for game. */
    double cand[2][NW];
    static Lines vr[2];
    memcpy(cand[0], base, sizeof base);
    for (int i = 0; i < NW; i++) cand[1][i] = base[i] * exp(mean[i]);
    evaluate_all(cand, 2, vgames, frames, VALID, vr);
    printf("validation, %d games a scenario (lines a game, base -> mean, diff +- s.e.):\n",
           vgames);
    double fit = 0;
    for (int s = 0; s < NSCEN; s++) {
        double d = 0, d2 = 0;
        for (int g = 0; g < vgames; g++) {
            double x = (double)(vr[1][s][g] - vr[0][s][g]);
            d += x; d2 += x * x;
        }
        double md = d / vgames;
        double se = vgames > 1 ? sqrt((d2 / vgames - md * md) / (vgames - 1)) : 0;
        double b = total(vr[0], s, vgames) / vgames, m = total(vr[1], s, vgames) / vgames;
        fit += m / (b > 0 ? b : 1);
        printf("  partner %d level %2d: %6.1f -> %6.1f  %+6.1f +- %.1f\n",
               kScen[s].partner, kScen[s].level, b, m, md, se);
    }
    printf("  fitness %.3f\nweights:", fit / NSCEN);
    for (int i = 0; i < NW; i++) printf(" %s=%ld", kNames[i], lround(cand[1][i]));
    printf("\n");
    return 0;
}
