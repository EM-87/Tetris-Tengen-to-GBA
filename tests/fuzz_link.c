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
#include "records_v1.h"

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
    /* Two titles whose demos share a board (tengen_lobby_demo) — or one of
     * them a console with a player in its lobby, and then they never meet. */
    unsigned demo = rnd() % 8;
    bool m_demo = demo == 0 || demo == 1, s_demo = demo == 0 || demo == 2;
    if (m_demo) tengen_lobby_demo(&m);
    if (s_demo) tengen_lobby_demo(&s);
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
    if (m_demo != s_demo) {
        if (m.ready || m.linked || s.linked) {
            printf("FAIL lobby %d: a demo and a player's lobby met (master's "
                   "a demo %d, slave's %d)\n", run, m_demo, s_demo);
            return false;
        }
        return true;
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

static void random_records(TengenRecord *r, int n) {
    for (int i = 0; i < n; i++) {
        r[i].table = (uint8_t)(rnd() % 5);
        r[i].score = rnd() % 1000000;
        r[i].lines = (uint16_t)(rnd() % 1000);
        for (int k = 0; k < 3; k++)
            r[i].initials[k] = (uint8_t)(rnd() % LEADER_LETTERS);
    }
}

static bool same_records(const TengenRecord *got, const TengenRecord *sent, int n) {
    for (int i = 0; i < n; i++)
        if (memcmp(got[i].initials, sent[i].initials, 3) ||
            got[i].score != sent[i].score || got[i].lines != sent[i].lines ||
            got[i].table != sent[i].table)
            return false;
    return true;
}

/* Two consoles of this version: a copy's rows to a save, two saves' offers
 * to each other unless they are the same, nothing to a copy. */
static bool records_run(int run) {
    TengenRecordSync s[2];
    TengenRecord mine[2][TENGEN_RECORDS_MAX];
    bool storage[2] = { rnd() & 1, rnd() & 1 };
    int n[2];
    for (int c = 0; c < 2; c++) {
        n[c] = (int)(rnd() % (TENGEN_RECORDS_MAX + 1));
        random_records(mine[c], n[c]);
    }
    /* Now and then, two saves with the same rows: nothing should cross. */
    if (storage[0] && storage[1] && (rnd() % 4) == 0) {
        n[1] = n[0];
        for (int i = 0; i < n[0]; i++) mine[1][i] = mine[0][n[0] - 1 - i];
    }
    for (int c = 0; c < 2; c++) tengen_records_start(&s[c], storage[c], mine[c], n[c]);
    bool merged[2] = { false, false };
    uint8_t kept[2] = { 0, 0 };
    /* What each player says if asked, and how long they take to say it. */
    bool take[2] = { rnd() & 1, rnd() & 1 };
    long think[2] = { (long)(rnd() % 300), (long)(rnd() % 300) };
    bool asked[2] = { false, false };
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
        for (int c = 0; c < 2; c++) {
            asked[c] = asked[c] || s[c].asking;
            if (s[c].asking && t >= think[c]) tengen_records_answer(&s[c], take[c]);
        }
        for (int c = 0; c < 2; c++)
            if (s[c].theirs_ready && !merged[c]) {
                merged[c] = true;
                kept[c] = (uint8_t)(rnd() % (s[c].theirs_n + 1));
                tengen_records_saved(&s[c], kept[c]);
            }
    }
    for (int c = 0; c < 2; c++) {
        int o = c ^ 1;
        /* What should have crossed from o to c: a copy's rows to a save; a
         * save's to a save with another digest whose player said yes — and
         * that one, and only that one, was asked. */
        bool differ = s[0].digest != s[1].digest;
        if (asked[c] != (storage[c] && storage[o] && differ)) {
            printf("FAIL records %d: console %d was %sasked (saves %d %d, "
                   "digests %s)\n", run, c, asked[c] ? "" : "not ",
                   storage[0], storage[1], differ ? "differ" : "the same");
            return false;
        }
        bool due = storage[c] && n[o] > 0 &&
                   (!storage[o] || (differ && take[c]));
        if (!s[c].complete) {
            printf("FAIL records %d (loss %u%%%s): console %d never finished\n",
                   run, loss, one_sided ? ", one-sided" : "", c);
            return false;
        }
        if (s[c].theirs_ready != due) {
            printf("FAIL records %d: console %d %s the other's %d records "
                   "(saves %d %d)\n", run, c,
                   due ? "did not take" : "took", n[o], storage[0], storage[1]);
            return false;
        }
        if (due && (s[c].theirs_n != n[o] || !same_records(s[c].theirs, mine[o], n[o]))) {
            printf("FAIL records %d: the records arrived wrong\n", run);
            return false;
        }
        /* ...and what each heard of its own: what the other kept, if the
         * other has a save and merged; nothing otherwise. Only where this
         * one finished by its own count and not by the other's first move. */
        bool gave = storage[o] && n[c] > 0 &&
                    (!storage[c] || (differ && take[o]));
        if (tengen_records_done(&s[c]) && s[c].saved != (gave ? kept[o] : 0)) {
            printf("FAIL records %d (loss %u%%): console %d heard %d kept, "
                   "the other kept %d (merged %d)\n", run, loss, c, s[c].saved,
                   gave ? kept[o] : 0, merged[o]);
            return false;
        }
    }
    return true;
}

/* A console of this version and one of the last (records_v1.h), either as
 * either: the older form both ways. */
static bool records_mixed_run(int run) {
    TengenRecordSync now;
    V1RecordSync old;
    TengenRecord mine[2][TENGEN_RECORDS_MAX];
    bool storage[2] = { rnd() & 1, rnd() & 1 };     /* now, old */
    int n[2];
    for (int c = 0; c < 2; c++) {
        n[c] = (int)(rnd() % (TENGEN_RECORDS_MAX + 1));
        random_records(mine[c], n[c]);
    }
    tengen_records_start(&now, storage[0], mine[0], n[0]);
    v1_records_start(&old, storage[1], storage[1] ? NULL : mine[1], storage[1] ? 0 : n[1]);
    bool merged[2] = { false, false };
    uint8_t kept[2] = { 0, 0 };
    unsigned loss = rnd() % 50;
    bool one_sided = rnd() & 1;
    for (long t = 0; t < RECORD_TURNS; t++) {
        bool on[2] = { !now.complete && !now.failed, !old.complete && !old.failed };
        if (!on[0] && !on[1]) break;
        uint16_t w0 = on[0] ? tengen_records_word(&now) : tengen_link_pack(0, (uint8_t)t);
        uint16_t w1 = on[1] ? v1_records_word(&old) : tengen_link_pack(0, (uint8_t)t);
        bool got0 = (rnd() % 100) >= loss;
        bool got1 = one_sided ? (rnd() % 100) >= loss : got0;
        if (on[0]) tengen_records_apply(&now, got0, w1);
        if (on[1]) v1_records_apply(&old, got1, w0);
        if (now.theirs_ready && !merged[0]) {
            merged[0] = true;
            kept[0] = (uint8_t)(rnd() % (now.theirs_n + 1));
            tengen_records_saved(&now, kept[0]);
        }
        if (old.theirs_ready && !merged[1]) {
            merged[1] = true;
            kept[1] = (uint8_t)(rnd() % (old.theirs_n + 1));
            v1_records_saved(&old, kept[1]);
        }
    }
    if (!now.complete || !old.complete) {
        printf("FAIL mixed records %d (loss %u%%%s, saves %d %d): %s never finished\n",
               run, loss, one_sided ? ", one-sided" : "", storage[0], storage[1],
               now.complete ? "the older one" : "this one");
        return false;
    }
    /* A copy's rows reach a save of the other version, whole. */
    if (storage[0] && !storage[1] &&
        (!now.theirs_ready || now.theirs_n != n[1] ||
         !same_records(now.theirs, mine[1], n[1]))) {
        printf("FAIL mixed records %d: the older copy's rows did not arrive\n", run);
        return false;
    }
    if (!storage[0] && storage[1] &&
        (!old.theirs_ready || old.theirs_n != n[0] ||
         !same_records(old.theirs, mine[0], n[0]))) {
        printf("FAIL mixed records %d: this copy's rows did not reach the "
               "older cartridge\n", run);
        return false;
    }
    /* Two saves of two versions, or two copies: nothing is taken. */
    if (storage[0] == storage[1] && (now.theirs_ready || old.theirs_ready) &&
        storage[0]) {
        printf("FAIL mixed records %d: two saves of two versions exchanged rows\n", run);
        return false;
    }
    return true;
}

/* A CONSOLE AND WHATEVER COMES DOWN THE CABLE: words of the exchange with
 * every other bit at random, now and then no word at all or one of another
 * kind — a console of some build to come, a cable with a bad pin. Nothing
 * it hears may make it write outside its own state, take more rows than a
 * message holds, take a save's rows its player did not say yes to, or give
 * a copy anything to keep. */
#define NOISE_TURNS 1500

static bool records_noise_run(int run) {
    struct { uint32_t before; TengenRecordSync s; uint32_t after; } box;
    TengenRecord mine[TENGEN_RECORDS_MAX];
    int n = (int)(rnd() % (TENGEN_RECORDS_MAX + 1));
    bool storage = rnd() & 1;
    bool marked = rnd() % 4 != 0;       /* the other speaks this form, mostly */
    bool answered = false, said_yes = false;
    random_records(mine, n);
    box.before = box.after = 0xC0FFEE11u;
    tengen_records_start(&box.s, storage, mine, n);
    TengenRecordSync *s = &box.s;
    for (int turn = 0; turn < NOISE_TURNS && !s->complete && !s->failed; turn++) {
        uint16_t word = (uint16_t)((TENGEN_LOBBY_RECORDS << TENGEN_LOBBY_TAG_SHIFT) |
                                   (rnd() & 0x7FFu) | (marked ? 0x800u : 0));
        unsigned kind = rnd() % 64;
        (void)tengen_records_word(s);
        if (kind == 0) tengen_records_apply(s, false, 0xFFFF);
        else if (kind == 1) tengen_records_apply(s, true, (uint16_t)(rnd() & 0xBFFF));
        else tengen_records_apply(s, true, word);
        if (s->asking && rnd() % 16 == 0) {
            said_yes = rnd() & 1;
            answered = true;
            tengen_records_answer(s, said_yes);
        }
        const char *bad = NULL;
        if (box.before != 0xC0FFEE11u || box.after != 0xC0FFEE11u)
            bad = "wrote outside its state";
        else if (s->theirs_n > TENGEN_RECORDS_MAX) bad = "more rows than a message holds";
        else if (s->in_pos > TENGEN_RECORDS_MSG || s->out_pos > TENGEN_RECORDS_MSG ||
                 s->out_len > TENGEN_RECORDS_MSG || s->out_pos > s->out_len)
            bad = "a position past the end of a message";
        else if (s->theirs_ready && !storage) bad = "a copy with rows to keep";
        else if (s->theirs_ready && s->partner_storage && !(answered && said_yes))
            bad = "a save's rows taken with nobody having said yes";
        if (bad) {
            printf("records noise run %d, turn %d: %s (storage %d, marked %d)\n",
                   run, turn, bad, storage, marked);
            return false;
        }
        if (s->theirs_ready && s->out_held)
            tengen_records_saved(s, (uint8_t)(rnd() % (s->theirs_n + 1)));
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
        if (!records_mixed_run(r)) fails++;
        if (!records_noise_run(r)) fails++;
    }
    printf("fuzz_link: %d lobbies, %d name swaps and %d records exchanges, as "
           "many between two versions and as many against noise, over a lossy "
           "link, %d failures\n",
           runs, runs, runs, fails);
    return fails != 0;
}
