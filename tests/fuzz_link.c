/*
 * fuzz_link.c — the lobby and the records swap, two consoles' worth each,
 * over a link that loses transfers. Built under AddressSanitizer and UBSan
 * by `make fuzz`.
 *
 * The host tests walk the handshakes through the cases somebody wrote
 * down; two real SPs found the ones nobody had. This runs a hundred
 * thousand of each with what can be varied varied — who chose what, which
 * skins each has, when the master lets the lobby go, when each player
 * finishes typing — and a link that drops up to half its transfers, on
 * both consoles at once (a cable) or on one alone (a transfer one
 * interrupt missed). What must hold:
 *
 *   LOBBY  two consoles that both start the match start it with the same
 *          seed, level, tune, handicaps, mode, XE and skin; neither starts
 *          while the other never will; and they do link, given time.
 *   NAMES  a name that arrives is the one that was typed, and both
 *          consoles get each other's or neither does.
 *
 *     fuzz_link [runs] [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tengen_link.h"

static unsigned long long g_rs;

static unsigned rnd(void) {
    g_rs ^= g_rs << 13;
    g_rs ^= g_rs >> 7;
    g_rs ^= g_rs << 17;
    return (unsigned)(g_rs >> 11);
}

#define LOBBY_TURNS 20000
#define NAME_TURNS  3000
#define LEADER_LETTERS 27    /* gba/port.h: the name entry's alphabet */

static bool lobby_run(int run) {
    TengenLobby m, s;
    uint16_t seed = (uint16_t)rnd();
    tengen_lobby_start_held(&m, seed);
    tengen_lobby_start_held(&s, (uint16_t)rnd());
    bool coop = rnd() & 1;
    tengen_lobby_mode(&m, coop);
    if (rnd() % 4) tengen_lobby_mode(&s, coop);
    else tengen_lobby_mode_any(&s);           /* the Single-Pak copy */
    static const uint16_t prints[3] = { 0x123, 0x456, 0x789 };
    int m_count = (int)(rnd() % 4);
    int s_count = (int)(rnd() % 3);
    int offer = m_count ? (int)(rnd() % (unsigned)(m_count + 1)) - 1 : -1;
    tengen_lobby_skins(&m, prints, m_count, offer);
    tengen_lobby_skins(&s, prints + (rnd() % 2), s_count, -1);

    unsigned loss = rnd() % 60;               /* percent, per console */
    bool one_sided = rnd() & 1;
    long release_at = (long)(rnd() % 300);
    uint8_t handicap[2] = { (uint8_t)(rnd() % 5), (uint8_t)(rnd() % 5) };
    uint8_t level = (uint8_t)(rnd() % 20), music = (uint8_t)(rnd() % 8);
    bool xe = rnd() & 1;

    for (long t = 0; t < LOBBY_TURNS && !(m.ready && s.ready); t++) {
        if (t == release_at)
            tengen_lobby_release(&m, seed, level, music, handicap, coop, xe);
        /* A console that has gone to the match sends its first move. */
        uint16_t mw = m.ready ? tengen_link_pack(0, 0) : tengen_lobby_word(&m, true);
        uint16_t sw = s.ready ? tengen_link_pack(0, 0) : tengen_lobby_word(&s, false);
        bool m_got = (rnd() % 100) >= loss;
        bool s_got = one_sided ? (rnd() % 100) >= loss : m_got;
        if (!m.ready) tengen_lobby_apply(&m, true, m_got, mw, sw);
        if (!s.ready) tengen_lobby_apply(&s, false, s_got, mw, sw);
    }
    if (m.ready != s.ready) {
        printf("FAIL lobby %d (loss %u%%%s): only the %s went to the match\n",
               run, loss, one_sided ? ", one-sided" : "",
               m.ready ? "master" : "slave");
        return false;
    }
    if (!m.ready) {
        printf("FAIL lobby %d (loss %u%%): never linked\n", run, loss);
        return false;
    }
    bool same = m.seed == s.seed && m.start_level == s.start_level &&
                m.music == s.music && m.handicap[0] == s.handicap[0] &&
                m.handicap[1] == s.handicap[1] && m.coop == s.coop &&
                m.xe == s.xe && (m.skin < 0) == (s.skin < 0) &&
                (m.skin < 0 || m.skins[m.skin] == s.skins[s.skin]);
    if (!same) {
        printf("FAIL lobby %d (loss %u%%): different games — seed %04x/%04x "
               "level %d/%d music %d/%d coop %d/%d skin %d/%d\n",
               run, loss, m.seed, s.seed, m.start_level, s.start_level,
               m.music, s.music, m.coop, s.coop, m.skin, s.skin);
        return false;
    }
    return true;
}

static bool names_run(int run) {
    TengenNameSwap a, b;
    tengen_name_start(&a);
    tengen_name_start(&b);
    uint8_t name_a[3], name_b[3];
    for (int i = 0; i < 3; i++) {
        name_a[i] = (uint8_t)(rnd() % LEADER_LETTERS);
        name_b[i] = (uint8_t)(rnd() % LEADER_LETTERS);
    }
    long typed_a = (long)(rnd() % 400), typed_b = (long)(rnd() % 400);
    unsigned loss = rnd() % 50;
    bool one_sided = rnd() & 1;
    for (long t = 0; t < NAME_TURNS; t++) {
        if (t == typed_a) tengen_name_send(&a, name_a);
        if (t == typed_b) tengen_name_send(&b, name_b);
        bool a_on = !a.complete && !a.failed, b_on = !b.complete && !b.failed;
        if (!a_on && !b_on) break;
        uint16_t wa = tengen_name_word(&a), wb = tengen_name_word(&b);
        /* A console that is done has put the link away: nothing comes. */
        bool both = a_on && b_on;
        bool a_got = both && (rnd() % 100) >= loss;
        bool b_got = one_sided ? both && (rnd() % 100) >= loss : a_got;
        if (a_on) tengen_name_apply(&a, a_got, wb);
        if (b_on) tengen_name_apply(&b, b_got, wa);
    }
    bool has_a = tengen_name_have(&a), has_b = tengen_name_have(&b);
    if ((has_a && memcmp(a.theirs, name_b, 3)) ||
        (has_b && memcmp(b.theirs, name_a, 3))) {
        printf("FAIL names %d (loss %u%%): a name arrived wrong\n", run, loss);
        return false;
    }
    if (has_a != has_b) {
        printf("FAIL names %d (loss %u%%%s): only one console got a name\n",
               run, loss, one_sided ? ", one-sided" : "");
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 20000;
    g_rs = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x9E3779B97F4A7C15ULL;
    if (!g_rs) g_rs = 1;
    int fails = 0;
    for (int r = 0; r < runs && fails < 20; r++) {
        if (!lobby_run(r)) fails++;
        if (!names_run(r)) fails++;
    }
    printf("fuzz_link: %d lobbies and %d name swaps over a lossy link, "
           "%d failures\n", runs, runs, fails);
    return fails != 0;
}
