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
 *   RECORDS a copy's records that a console with a save takes are the ones
 *          sent, byte for byte; a copy never hears "kept" unless they were
 *          merged (it would forget them); and both finish, given time.
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

#define RECORD_TURNS 6000

static bool records_run(int run) {
    TengenRecordSync s[2];
    TengenRecord mine[2][TENGEN_RECORDS_MAX];
    bool storage[2] = { rnd() & 1, rnd() & 1 };
    int n[2];
    for (int c = 0; c < 2; c++) {
        n[c] = (int)(rnd() % (TENGEN_RECORDS_MAX + 1));
        for (int i = 0; i < n[c]; i++) {
            mine[c][i].table = (uint8_t)(rnd() % 5);
            mine[c][i].score = rnd() % 1000000;
            mine[c][i].lines = (uint16_t)(rnd() % 1000);
            for (int k = 0; k < 3; k++)
                mine[c][i].initials[k] = (uint8_t)(rnd() % LEADER_LETTERS);
        }
        tengen_records_start(&s[c], storage[c], mine[c], n[c]);
    }
    bool merged[2] = { false, false };
    uint8_t kept[2] = { 0, 0 };
    unsigned loss = rnd() % 50;
    bool one_sided = rnd() & 1;
    for (long t = 0; t < RECORD_TURNS; t++) {
        bool on[2] = { !s[0].complete && !s[0].failed, !s[1].complete && !s[1].failed };
        if (!on[0] && !on[1]) break;
        /* One that has finished is in the match: match words. */
        uint16_t w[2];
        for (int c = 0; c < 2; c++)
            w[c] = on[c] ? tengen_records_word(&s[c]) : tengen_link_pack(0, (uint8_t)t);
        bool got0 = (rnd() % 100) >= loss;
        bool got1 = one_sided ? (rnd() % 100) >= loss : got0;
        if (on[0]) tengen_records_apply(&s[0], got0, w[1]);
        if (on[1]) tengen_records_apply(&s[1], got1, w[0]);
        for (int c = 0; c < 2; c++)
            if (s[c].theirs_ready && !merged[c]) {
                merged[c] = true;
                kept[c] = (uint8_t)(rnd() % (s[c].theirs_n + 1));
                tengen_records_saved(&s[c], kept[c]);
            }
    }
    for (int c = 0; c < 2; c++) {
        int o = c ^ 1;
        if (s[c].theirs_ready) {
            if (s[c].theirs_n != n[o] || !storage[c] || storage[o]) {
                printf("FAIL records %d: took %d records, %d were sent\n",
                       run, s[c].theirs_n, n[o]);
                return false;
            }
            for (int i = 0; i < n[o]; i++)
                if (memcmp(&s[c].theirs[i].initials, mine[o][i].initials, 3) ||
                    s[c].theirs[i].score != mine[o][i].score ||
                    s[c].theirs[i].lines != mine[o][i].lines ||
                    s[c].theirs[i].table != mine[o][i].table) {
                    printf("FAIL records %d: record %d arrived wrong\n", run, i);
                    return false;
                }
        }
        /* A copy that finished believing a save has them: it did merge. */
        if (!storage[c] && s[c].complete && s[c].partner_storage &&
            (!merged[o] || s[c].saved != kept[o])) {
            printf("FAIL records %d (loss %u%%): the copy heard %d kept, "
                   "merged=%d kept=%d\n", run, loss, s[c].saved, merged[o], kept[o]);
            return false;
        }
        if (!s[c].complete) {
            printf("FAIL records %d (loss %u%%%s): console %d never finished\n",
                   run, loss, one_sided ? ", one-sided" : "", c);
            return false;
        }
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
        if (!records_run(r)) fails++;
    }
    printf("fuzz_link: %d lobbies, %d name swaps and %d records dumps over a "
           "lossy link, %d failures\n", runs, runs, runs, fails);
    return fails != 0;
}
