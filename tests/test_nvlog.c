/* The save journal (src/tengen_nvlog.c) against a flash chip that behaves
 * like one: bits only go from 1 to 0 when programmed, a sector erases
 * whole, and the power can go at any word. What is asked of the journal is
 * one thing, said many ways: what is read back is the save as some commit
 * left it — the last, unless the power went during it — and never anything
 * else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tengen_nvlog.h"

static int g_failures;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- the chip ---------------------------------------------------------- */
#define MAX_SECTOR 0x20000u

typedef struct {
    uint8_t mem[2][MAX_SECTOR];
    uint32_t size;
    long fuse;              /* words and erasures left before the power goes; <0: never */
    bool clean_cut;         /* ...between two words, not in the middle of one */
    bool dead;              /* it went */
    bool bad[2];            /* a sector that will not take a word */
    unsigned erased[2], words;
    uint32_t rng;
} Nor;

static uint32_t nor_rand(Nor *n) {
    n->rng = n->rng * 1664525u + 1013904223u;
    return n->rng >> 8;
}

static bool nor_blow(Nor *n) {
    if (n->fuse < 0) return false;
    if (n->fuse-- > 0) return false;
    n->dead = true;
    return true;
}

static bool nor_program(void *ctx, int half, uint32_t at, const uint8_t *src, uint32_t len) {
    Nor *n = ctx;
    if (n->dead) return false;
    if ((at & 1) || (len & 1) || at + len > n->size) {
        printf("FAIL: a program outside the rules: at %u, %u bytes\n", (unsigned)at, (unsigned)len);
        g_failures++;
        return false;
    }
    if (src >= n->mem[0] && src < n->mem[1] + MAX_SECTOR) {
        printf("FAIL: programmed from the flash itself\n");
        g_failures++;
        return false;
    }
    for (uint32_t i = 0; i < len; i += 2) {
        if (n->bad[half]) return false;
        if (nor_blow(n)) {
            /* The power goes as the word is being programmed: some of its
             * bits made it and some did not — or just before it. */
            if (n->clean_cut) return false;
            uint8_t keep0 = (uint8_t)nor_rand(n), keep1 = (uint8_t)nor_rand(n);
            n->mem[half][at + i] &= (uint8_t)(src[i] | keep0);
            n->mem[half][at + i + 1] &= (uint8_t)(src[i + 1] | keep1);
            return false;
        }
        n->words++;
        n->mem[half][at + i] &= src[i];
        n->mem[half][at + i + 1] &= src[i + 1];
        /* A bit that was already 0 and should be 1 does not come back. */
        if (n->mem[half][at + i] != src[i] || n->mem[half][at + i + 1] != src[i + 1])
            return false;
    }
    return true;
}

static bool nor_erase(void *ctx, int half) {
    Nor *n = ctx;
    if (n->dead) return false;
    if (n->bad[half]) return false;
    if (nor_blow(n)) {
        /* An erasure cut short leaves anything at all. */
        for (uint32_t i = 0; i < n->size; i++) n->mem[half][i] = (uint8_t)nor_rand(n);
        return false;
    }
    n->erased[half]++;
    memset(n->mem[half], 0xFF, n->size);
    return true;
}

static Nor *nor_new(uint32_t size, bool garbage, uint32_t seed) {
    Nor *n = calloc(1, sizeof *n);
    n->size = size;
    n->fuse = -1;
    n->rng = seed * 2654435761u + 1;
    for (int h = 0; h < 2; h++)
        for (uint32_t i = 0; i < size; i++)
            n->mem[h][i] = garbage ? (uint8_t)nor_rand(n) : 0xFF;
    return n;
}

static NvDevice nor_device(Nor *n) {
    NvDevice d;
    d.mem[0] = n->mem[0];
    d.mem[1] = n->mem[1];
    d.size = n->size;
    d.program = nor_program;
    d.erase = nor_erase;
    d.blank = NULL;
    d.ctx = n;
    return d;
}

/* The power back on: the same chip, read with fresh eyes. */
static void power_on(Nor *n, NvLog *log, uint32_t image_size) {
    n->dead = false;
    n->fuse = -1;
    NvDevice d = nor_device(n);
    nvlog_mount(log, &d, image_size);
    /* The generation it goes by is the one written in the sector it took —
     * not the other's, which may be a newer one the power went off on. */
    if (log->active >= 0) {
        const uint8_t *h = n->mem[log->active];
        uint32_t gen = (uint32_t)h[4] | ((uint32_t)h[5] << 8) | ((uint32_t)h[6] << 16) |
                       ((uint32_t)h[7] << 24);
        CHECK(log->gen == gen);
    }
}

/* ---- the save's own doings, for the tests to repeat --------------------- */
#define IMG 0x7FE0u
static uint8_t g_image[IMG], g_back[IMG];

/* A few runs of the image changed, as the game changes it. */
static int scribble(uint8_t *image, uint32_t size, uint32_t *rng, NvRange *out, int max) {
    int count = 1 + (int)((*rng = *rng * 1664525u + 1013904223u) >> 28) % max;
    for (int i = 0; i < count; i++) {
        *rng = *rng * 1664525u + 1013904223u;
        uint32_t len = 1 + (*rng >> 9) % (size / 6);
        *rng = *rng * 1664525u + 1013904223u;
        uint32_t lo = (*rng >> 9) % (size - len + 1);      /* up to the image's last byte */
        for (uint32_t b = 0; b < len; b++) {
            *rng = *rng * 1664525u + 1013904223u;
            image[lo + b] = (uint8_t)(*rng >> 16);
        }
        out[i].lo = (uint16_t)lo;
        out[i].hi = (uint16_t)(lo + len);
    }
    return count;
}

static void test_a_first_save_makes_the_journal(void) {
    for (int garbage = 0; garbage < 2; garbage++) {
        Nor *n = nor_new(0x10000, garbage != 0, 7);
        NvLog log;
        power_on(n, &log, IMG);
        CHECK(log.active < 0 && !log.have_commit);
        CHECK(!nvlog_replay(&log, g_back));
        for (uint32_t i = 0; i < IMG; i++) CHECK(g_back[i] == NV_FILL);
        /* Nothing was touched by looking. */
        CHECK(n->words == 0 && n->erased[0] == 0 && n->erased[1] == 0);

        memset(g_image, NV_FILL, IMG);
        memcpy(g_image + 0x406, "\x08\xFB", 2);
        NvRange r = { 0x406, 0x408 };
        CHECK(nvlog_commit(&log, g_image, &r, 1, 0x1234));
        CHECK(log.active == 0 && log.have_commit && log.seq == 1 && log.hash == 0x1234);
        /* A sector that was not erased had to be; one that was, not. */
        CHECK(n->erased[0] == (unsigned)garbage && n->erased[1] == 0);

        NvLog again;
        power_on(n, &again, IMG);
        CHECK(again.active == 0 && again.seq == 1 && again.hash == 0x1234);
        CHECK(again.appendable);
        CHECK(nvlog_replay(&again, g_back));
        CHECK(memcmp(g_back, g_image, IMG) == 0);
        free(n);
    }
}

/* A SECTOR IS ERASED BEFORE IT IS WRITTEN IF ANYTHING AT ALL IS IN IT, its
 * first bytes included: where the header goes is as much the sector as the
 * rest. */
static void test_a_sector_with_anything_in_it_is_erased_first(void) {
    static const uint32_t where[] = { 0, 3, 9, 15, 16, 0x7FF };
    for (unsigned i = 0; i < sizeof where / sizeof where[0]; i++) {
        Nor *n = nor_new(0x800, false, 4);
        NvLog log;
        n->mem[0][where[i]] = 0;
        power_on(n, &log, 0x200);
        memset(g_image, NV_FILL, 0x200);
        g_image[1] = 2;
        NvRange r = { 1, 2 };
        CHECK(nvlog_commit(&log, g_image, &r, 1, 5));
        CHECK(log.active == 0 && n->erased[0] == 1 && n->erased[1] == 0);
        power_on(n, &log, 0x200);
        CHECK(log.active == 0 && log.appendable);
        CHECK(nvlog_replay(&log, g_back) && memcmp(g_back, g_image, 0x200) == 0);
        free(n);
    }
}

/* Thousands of saves, each read back with the power cycled: the image as
 * it was left, through as many full sectors as it takes. */
static void test_what_is_read_back_is_what_was_saved(void) {
    static const struct { uint32_t image, sector; int saves; } kShape[] = {
        { 0x200, 0x800, 1500 },         /* a sector full every few saves */
        { 0x7FE0, 0x10000, 300 },       /* the cartridge's, 64 KB sectors */
        { 0x7FE0, 0x20000, 300 },       /* ...and 128 KB */
    };
    for (unsigned s = 0; s < sizeof kShape / sizeof kShape[0]; s++) {
        Nor *n = nor_new(kShape[s].sector, s == 1, 11 + s);
        NvLog log;
        uint32_t rng = 99 + s, size = kShape[s].image;
        power_on(n, &log, size);
        memset(g_image, NV_FILL, size);
        for (int i = 0; i < kShape[s].saves; i++) {
            NvRange r[4];
            int count = scribble(g_image, size, &rng, r, 4);
            uint32_t hash = nv_hash(g_image, size);
            CHECK(nvlog_commit(&log, g_image, r, count, hash));
            CHECK(log.error == NV_OK);
            if (i % 7 == 0) power_on(n, &log, size);
            CHECK(log.hash == hash && log.seq == (uint32_t)i + 1);
            CHECK(nvlog_replay(&log, g_back));
            if (memcmp(g_back, g_image, size) != 0) {
                CHECK(!"the image read back is not the image saved");
                break;
            }
        }
        /* It went round: both sectors were erased, and about as often. */
        CHECK(log.gen > 2);
        CHECK(n->erased[0] > 0 && n->erased[1] > 0);
        CHECK(n->erased[0] <= n->erased[1] + 2 && n->erased[1] <= n->erased[0] + 2);
        free(n);
    }
}

/* THE POWER GOES AT EVERY WORD THERE IS. A run of saves that fills the
 * sector twice over, cut at each word programmed and each erasure in turn:
 * with the power back, the save is the one the last finished commit left,
 * or the one that was being made if every bit of it got there — and the
 * saves that follow go on as if nothing had happened. */
static void test_the_power_can_go_at_any_word(void) {
    enum { SIZE = 0x180, SECTOR = 0x600, SAVES = 14 };
    static uint8_t snap[SAVES + 1][SIZE];
    static NvRange ranges[SAVES][3];
    static int counts[SAVES];
    /* The saves, worked out once: the same for every cut. */
    uint32_t rng = 4242;
    memset(g_image, NV_FILL, SIZE);
    memcpy(snap[0], g_image, SIZE);
    for (int i = 0; i < SAVES; i++) {
        counts[i] = scribble(g_image, SIZE, &rng, ranges[i], 3);
        memcpy(snap[i + 1], g_image, SIZE);
    }
    long total = 0;
    int landed_early = 0, compactions_cut = 0;
    for (int clean = 0; clean < 2; clean++)
    for (long cut = 0;; cut++) {
        Nor *n = nor_new(SECTOR, true, 5);
        NvLog log;
        power_on(n, &log, SIZE);
        n->fuse = cut;
        n->clean_cut = clean != 0;
        int done = 0;
        for (; done < SAVES; done++) {
            unsigned before = log.compactions;
            if (!nvlog_commit(&log, snap[done + 1], ranges[done], counts[done],
                              nv_hash(snap[done + 1], SIZE))) {
                if (log.compactions != before || !log.appendable) compactions_cut++;
                break;
            }
        }
        if (!n->dead) {             /* the fuse outlasted the saves: done */
            total = cut;
            CHECK(done == SAVES);
            free(n);
            break;
        }
        power_on(n, &log, SIZE);
        nvlog_replay(&log, g_back);
        bool last = memcmp(g_back, snap[done], SIZE) == 0;
        bool next = done < SAVES && memcmp(g_back, snap[done + 1], SIZE) == 0;
        if (!last && !next) {
            printf("FAIL: %s cut at word %ld, %d saves done: the save read back is neither\n",
                   clean ? "clean" : "ragged", cut, done);
            g_failures++;
            free(n);
            break;
        }
        /* A cut between two words leaves nothing half done: the save in
         * hand got there only if its last word did, and then it was done. */
        if (clean) CHECK(last);
        if (next && !last) landed_early++;
        if (log.have_commit) CHECK(log.hash == nv_hash(g_back, SIZE));
        /* ...and on from wherever it was left. */
        int from = (next && !last) ? done + 1 : done;
        for (int i = from; i < SAVES; i++)
            CHECK(nvlog_commit(&log, snap[i + 1], ranges[i], counts[i],
                               nv_hash(snap[i + 1], SIZE)));
        NvLog after;
        power_on(n, &after, SIZE);
        nvlog_replay(&after, g_back);
        CHECK(memcmp(g_back, snap[SAVES], SIZE) == 0);
        free(n);
    }
    CHECK(total > 400);             /* it was a real run, cut in many places */
    CHECK(compactions_cut > 20);    /* ...the rewriting of a full sector among them */
    (void)landed_early;
}

/* RECORDS THE POWER LEFT WITHOUT THEIR COMMIT ARE NOT ADOPTED LATER. Two
 * runs saved together, the power gone after the first is whole and before
 * the commit: the next save must not make that first run part of the save
 * by putting its own commit after it. */
static void test_half_a_commit_is_never_made_whole(void) {
    enum { SIZE = 0x100, SECTOR = 0x1000 };
    Nor *n = nor_new(SECTOR, false, 3);
    NvLog log;
    power_on(n, &log, SIZE);
    uint8_t a[SIZE], b[SIZE];
    memset(a, NV_FILL, SIZE);
    memset(a + 0x10, 0x11, 8);
    memset(a + 0x80, 0x22, 8);
    NvRange first[2] = { { 0x10, 0x18 }, { 0x80, 0x88 } };
    CHECK(nvlog_commit(&log, a, first, 2, 1));
    /* The second save changes both runs; the power goes after the first
     * run's record (head 3 words, 4 of data, 1 of seal) is all there, and
     * before a bit of the second: the sector ends in a record that adds
     * up, and erased bytes after it. */
    memcpy(b, a, SIZE);
    memset(b + 0x10, 0x33, 8);
    memset(b + 0x80, 0x44, 8);
    n->fuse = 8;
    n->clean_cut = true;
    uint32_t end_before = log.end;
    CHECK(!nvlog_commit(&log, b, first, 2, 2));
    CHECK(memcmp(n->mem[0] + end_before + 6, b + 0x10, 8) == 0);   /* it is there */
    for (uint32_t i = end_before + 16; i < SECTOR; i++) CHECK(n->mem[0][i] == 0xFF);
    power_on(n, &log, SIZE);
    CHECK(log.seq == 1 && !log.appendable);
    nvlog_replay(&log, g_back);
    CHECK(memcmp(g_back, a, SIZE) == 0);
    /* A third save that touches neither run: the first run's record must
     * stay out of it. */
    uint8_t c[SIZE];
    memcpy(c, a, SIZE);
    c[0xF0] = 0x55;
    NvRange other = { 0xF0, 0xF1 };
    unsigned before = log.compactions;
    CHECK(nvlog_commit(&log, c, &other, 1, 3));
    CHECK(log.compactions == before + 1);   /* written afresh, not appended */
    power_on(n, &log, SIZE);
    nvlog_replay(&log, g_back);
    CHECK(memcmp(g_back, c, SIZE) == 0);
    CHECK(g_back[0x10] == 0x11 && g_back[0x80] == 0x22);
    free(n);
}

/* A SECTOR THAT WILL NOT TAKE A WORD: the save goes into the other one,
 * whole, and when neither will the journal says so and what it held is
 * still what is read. */
static void test_a_sector_that_fails_is_left_for_the_other(void) {
    enum { SIZE = 0x100, SECTOR = 0x1000 };
    Nor *n = nor_new(SECTOR, false, 9);
    NvLog log;
    power_on(n, &log, SIZE);
    uint8_t img[SIZE];
    memset(img, NV_FILL, SIZE);
    img[5] = 1;
    NvRange r = { 5, 6 };
    CHECK(nvlog_commit(&log, img, &r, 1, 1) && log.active == 0);
    n->bad[0] = true;
    img[5] = 2;
    CHECK(nvlog_commit(&log, img, &r, 1, 2));
    CHECK(log.active == 1 && log.error == NV_OK);
    power_on(n, &log, SIZE);
    n->bad[0] = true;
    CHECK(log.active == 1);
    nvlog_replay(&log, g_back);
    CHECK(g_back[5] == 2);
    /* Both gone: the save is refused, and says why. */
    n->bad[1] = true;
    img[5] = 3;
    for (int i = 0; i < 400 && log.error == NV_OK; i++)
        (void)nvlog_commit(&log, img, &r, 1, 3);
    CHECK(log.error == NV_ERR_PROGRAM || log.error == NV_ERR_ERASE);
    n->bad[0] = n->bad[1] = false;
    power_on(n, &log, SIZE);
    nvlog_replay(&log, g_back);
    CHECK(g_back[5] == 2);
    free(n);
}

/* WHAT A SECTOR REPLACED IS STILL THERE TO GO BACK TO. Once the image has
 * been written afresh the old sector is left alone until its turn comes
 * round: lose the new one and the save is the old one's, not nothing. */
static void test_the_sector_before_is_what_is_left_if_this_one_goes(void) {
    enum { SIZE = 0x100, SECTOR = 0x400 };
    Nor *n = nor_new(SECTOR, false, 2);
    NvLog log;
    power_on(n, &log, SIZE);
    uint8_t img[SIZE], old[SIZE];
    memset(img, NV_FILL, SIZE);
    NvRange r = { 0x20, 0x60 };
    uint32_t rng = 5;
    int saves = 0;
    while (log.gen < 2 && saves < 200) {         /* until it has gone round once */
        if (log.gen == 1) memcpy(old, img, SIZE);
        for (int i = 0x20; i < 0x60; i++) img[i] = (uint8_t)((rng = rng * 69069u + 1) >> 16);
        CHECK(nvlog_commit(&log, img, &r, 1, (uint32_t)saves));
        saves++;
    }
    CHECK(log.gen == 2 && log.active == 1);
    memset(n->mem[1], 0xFF, SECTOR);             /* the new one, gone */
    power_on(n, &log, SIZE);
    CHECK(log.active == 0 && log.gen == 1);
    nvlog_replay(&log, g_back);
    CHECK(memcmp(g_back, old, SIZE) == 0);
    free(n);
}

/* A RECORD WHOSE HEAD DOES NOT ADD UP IS NOT A RECORD, whatever the rest of
 * it says: one bit gone from the head's own seal, and the commit that
 * record belongs to was never made. */
static void test_a_damaged_head_ends_the_journal_there(void) {
    enum { SIZE = 0x100, SECTOR = 0x1000 };
    Nor *n = nor_new(SECTOR, false, 6);
    NvLog log;
    power_on(n, &log, SIZE);
    uint8_t img[SIZE];
    memset(img, NV_FILL, SIZE);
    img[0x40] = 0xA1;
    NvRange r = { 0x40, 0x41 };
    CHECK(nvlog_commit(&log, img, &r, 1, 1));
    uint32_t second = log.end;
    img[0x40] = 0xB2;
    CHECK(nvlog_commit(&log, img, &r, 1, 2));
    /* The second save's record: off, len, and its head's seal at +4. */
    uint8_t *seal = n->mem[0] + second + 4;
    int bit = 0;
    while (!(seal[0] & (1 << bit))) bit++;
    seal[0] &= (uint8_t)~(1 << bit);
    power_on(n, &log, SIZE);
    CHECK(log.seq == 1 && !log.appendable);
    nvlog_replay(&log, g_back);
    CHECK(g_back[0x40] == 0xA1);
    free(n);
}

/* NOTHING WRITTEN CAN PASS FOR NOTHING. A seal is never the erased word:
 * of the 65536 two-byte saves there are, exactly one has a checksum of
 * 0xFFFF, and it is stored as something else and still read back. */
static void test_no_seal_is_the_erased_word(void) {
    enum { SIZE = 0x40, SECTOR = 0x20000 };
    Nor *n = nor_new(SECTOR, false, 8);
    NvLog log;
    power_on(n, &log, SIZE);
    uint8_t img[SIZE];
    memset(img, NV_FILL, SIZE);
    NvRange r = { 0x10, 0x12 };
    int blank_seals = 0, nearly = 0;
    for (unsigned v = 0; v < 0x10000; v++) {
        img[0x10] = (uint8_t)v;
        img[0x11] = (uint8_t)(v >> 8);
        unsigned compactions = log.compactions;
        CHECK(nvlog_commit(&log, img, &r, 1, v));
        if (log.compactions != compactions) continue;   /* written afresh: other records */
        /* The data record's seal: before the commit's sixteen bytes. */
        const uint8_t *seal = n->mem[log.active] + log.end - 16 - 2;
        unsigned stored = seal[0] | (seal[1] << 8);
        if (stored == 0xFFFF) blank_seals++;
        if (stored == 0xFFFE) nearly++;
        if (stored == 0xFFFE || stored == 0xFFFF) {
            NvLog again;
            power_on(n, &again, SIZE);
            nvlog_replay(&again, g_back);
            CHECK(g_back[0x10] == (uint8_t)v && g_back[0x11] == (uint8_t)(v >> 8));
        }
    }
    CHECK(blank_seals == 0);
    CHECK(nearly >= 1);
    free(n);
}

/* An image a sector cannot hold is refused before anything is erased —
 * to the byte: the sector's header and the commit are part of what has to
 * fit, and an image eight bytes over is as much too big as one twice the
 * size. */
static void test_an_image_too_big_for_the_sector_touches_nothing(void) {
    enum { SECTOR = 0x200 };
    /* A run of L bytes costs L + 8, the header 16 and the commit 16. */
    static const struct { uint32_t len; bool fits; } kCase[] = {
        { 0x400, false }, { SECTOR - 40 + 2, false }, { SECTOR - 40, true },
    };
    for (unsigned c = 0; c < sizeof kCase / sizeof kCase[0]; c++) {
        Nor *n = nor_new(SECTOR, false, 1);
        NvLog log;
        uint32_t size = kCase[c].len;
        power_on(n, &log, size);
        memset(g_image, 0x5A, size);
        NvRange r = { 0, (uint16_t)size };
        CHECK(nvlog_commit(&log, g_image, &r, 1, 0) == kCase[c].fits);
        if (kCase[c].fits) {
            CHECK(log.end == SECTOR);       /* to the last byte of it */
            nvlog_replay(&log, g_back);
            CHECK(memcmp(g_back, g_image, size) == 0);
        } else {
            CHECK(log.error == NV_ERR_SPACE);
            CHECK(n->words == 0 && n->erased[0] == 0 && n->erased[1] == 0);
        }
        free(n);
    }
}

/* A JOURNAL IS READ BY THE RULES OF THE SAVE IT IS READ FOR. A record that
 * adds up and says it goes past the end of the image — one written by a
 * build whose save was bigger — is not played (it would be played over
 * whatever comes after the image), and the journal ends before it; and a
 * commit that is not a commit's length is not a commit. */
static uint16_t test_crc16(uint16_t crc, const uint8_t *p, uint32_t n) {
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static void test_records_that_do_not_fit_the_save_are_not_played(void) {
    enum { BIG = 0x200, SMALL = 0x100, SECTOR = 0x1000 };
    Nor *n = nor_new(SECTOR, false, 4);
    NvLog log;
    power_on(n, &log, BIG);
    uint8_t img[BIG];
    memset(img, NV_FILL, BIG);
    img[0x10] = 0x77;
    NvRange low = { 0x10, 0x11 };
    CHECK(nvlog_commit(&log, img, &low, 1, 1));
    memset(img + 0x180, 0x99, 0x40);
    NvRange high = { 0x180, 0x1C0 };
    CHECK(nvlog_commit(&log, img, &high, 1, 2));
    /* Read for a save half the size, into a buffer with a guard after it. */
    uint8_t *small = malloc(SMALL);
    power_on(n, &log, SMALL);
    CHECK(log.seq == 1 && !log.appendable);
    nvlog_replay(&log, small);
    CHECK(small[0x10] == 0x77);
    free(small);

    /* A record at the journal's end that says it is a commit, sealed as a
     * record is, two bytes long. */
    power_on(n, &log, BIG);
    CHECK(log.seq == 2 && log.appendable);
    uint8_t fake[10] = { 0xDE, 0xC0, 2, 0, 0, 0, 0x34, 0x12, 0, 0 };
    uint16_t head = test_crc16(0x2A5F, fake, 4);
    fake[4] = (uint8_t)head;
    fake[5] = (uint8_t)(head >> 8);
    uint16_t all = test_crc16(head, fake + 6, 2);
    fake[8] = (uint8_t)all;
    fake[9] = (uint8_t)(all >> 8);
    CHECK(head != 0xFFFF && all != 0xFFFF);
    memcpy(n->mem[log.active] + log.end, fake, sizeof fake);
    power_on(n, &log, BIG);
    CHECK(log.seq == 2);                /* not 0x....1234 */
    CHECK(!log.appendable);             /* ...and there is something after it */
    free(n);
}

/* The hash is the sum it says it is, kept up a byte at a time; and one
 * byte changed anywhere changes it. */
static void test_the_hash_follows_the_image_a_byte_at_a_time(void) {
    uint32_t rng = 77;
    memset(g_image, NV_FILL, IMG);
    uint32_t h = nv_hash(g_image, IMG);
    for (int i = 0; i < 20000; i++) {
        rng = rng * 1664525u + 1013904223u;
        uint32_t off = (rng >> 8) % IMG;
        uint8_t now = (uint8_t)(rng >> 24);
        uint32_t stepped = h + nv_hash_step(off, g_image[off], now);
        if (now != g_image[off]) CHECK(stepped != h);
        h = stepped;
        g_image[off] = now;
    }
    CHECK(h == nv_hash(g_image, IMG));
    /* EVERY PLACE'S NUMBER IS ODD, which is what makes one byte changed
     * anywhere, by anything, change the sum (an odd number times a byte's
     * difference is never a multiple of 2^32) — and no two neighbours
     * share one, or two bytes changing places would not show. */
    for (uint32_t off = 0; off < 0x8000; off++) {
        CHECK(nv_hash_step(off, 0, 1) & 1u);
        CHECK(nv_hash_step(off, 0, 1) != nv_hash_step(off + 1, 0, 1));
    }
}

/* IT SAYS BEFOREHAND WHICH A SAVE WILL BE: a few words added, or the whole
 * image written afresh after an erase — a second or more with nothing else
 * running, which the cartridge keeps for a moment that can spare it. What
 * it says is what then happens, to the byte. */
static void test_it_says_beforehand_what_a_save_will_cost(void) {
    const uint32_t size = 0x200, sector = 0x200;
    Nor *n = nor_new(sector, false, 5);
    NvLog log;
    power_on(n, &log, size);
    NvRange r = { 0x10, 0x12 };
    /* No journal: nothing can be added to nothing. */
    CHECK(nvlog_room(&log) == 0);
    CHECK(!nvlog_appends(&log, &r, 1) && !nvlog_appends(&log, &r, 0));

    memset(g_image, NV_FILL, size);
    g_image[0x10] = 1;
    g_image[0x11] = 2;
    CHECK(nvlog_commit(&log, g_image, &r, 1, 1));
    CHECK(nvlog_room(&log) == sector - nvlog_used(&log));
    CHECK(nvlog_room(&log) > 0 && nvlog_room(&log) < sector);

    /* A record is its bytes, padded to a word, and eight more; a commit
     * sixteen. The longest run that still goes in, and one byte longer. */
    uint32_t room = nvlog_room(&log);
    uint32_t most = room - 16 - 8;
    CHECK(most < size);
    NvRange fits = { 0, (uint16_t)most }, over = { 0, (uint16_t)(most + 1) };
    CHECK(nvlog_appends(&log, &fits, 1));
    CHECK(!nvlog_appends(&log, &over, 1));
    /* Runs that are not runs cost nothing, and do not make room either. */
    NvRange junk[3] = { { 9, 9 }, { 12, 4 }, { (uint16_t)(size - 1), (uint16_t)(size + 1) } };
    CHECK(nvlog_appends(&log, junk, 3));
    NvRange both[2] = { { 9, 9 }, { 0, (uint16_t)(most + 1) } };
    CHECK(!nvlog_appends(&log, both, 2));
    /* ...the longest that fits still fits with them beside it. */
    NvRange beside[3] = { { 12, 4 }, { 0, (uint16_t)most },
                          { (uint16_t)(size - 1), (uint16_t)(size + 1) } };
    CHECK(nvlog_appends(&log, beside, 3));
    /* Two runs are two records. */
    NvRange two[2] = { { 0, 222 }, { 222, (uint16_t)most } };
    CHECK(!nvlog_appends(&log, two, 2));
    two[1].hi = (uint16_t)(most - 8);
    CHECK(nvlog_appends(&log, two, 2));

    /* ...and so it goes. The one that fits is added where the journal is: */
    uint32_t gen = log.gen;
    for (uint32_t i = 0; i < most; i++) g_image[i] = (uint8_t)(i * 7 + 1);
    CHECK(nvlog_commit(&log, g_image, &fits, 1, 2));
    CHECK(log.gen == gen && n->erased[0] + n->erased[1] == 0);
    CHECK(nvlog_room(&log) == 0);
    /* nothing more does, not even a commit of nothing — */
    CHECK(!nvlog_appends(&log, &r, 0));
    /* — and the next is written afresh, in the other sector. */
    g_image[0x10] = 9;
    CHECK(nvlog_commit(&log, g_image, &r, 1, 3));
    CHECK(log.gen == gen + 1 && log.active == 1);
    CHECK(nvlog_replay(&log, g_back) && memcmp(g_back, g_image, size) == 0);

    /* A tail the power left dirty takes nothing either, whatever is free. */
    n->fuse = 2;
    n->clean_cut = true;
    g_image[0x20] = 4;
    NvRange one = { 0x20, 0x21 };
    CHECK(!nvlog_commit(&log, g_image, &one, 1, 4));
    power_on(n, &log, size);
    CHECK(log.have_commit && !log.appendable);
    CHECK(nvlog_room(&log) == 0 && !nvlog_appends(&log, &one, 1));
    free(n);
}

/* A DEVICE THAT CAN LOOK OVER ITS OWN SECTORS IS ASKED TO, and what it
 * says is what is done: the cartridge reads its flash a word at a time,
 * four times as fast as the journal can a byte at a time, and the journal
 * looks over all that is left of a sector at every power-on. */
static unsigned g_blank_asked;
static bool g_blank_never;

static bool nor_blank(void *ctx, int half, uint32_t from, uint32_t to) {
    Nor *n = ctx;
    g_blank_asked++;
    if (from > to || to > n->size || half < 0 || half > 1) {
        printf("FAIL: asked about bytes that are not a sector's: %d, %u to %u\n",
               half, (unsigned)from, (unsigned)to);
        g_failures++;
        return false;
    }
    if (g_blank_never) return false;
    for (uint32_t i = from; i < to; i++)
        if (n->mem[half][i] != 0xFF) return false;
    return true;
}

static void test_a_device_that_reads_its_own_sectors_is_asked(void) {
    const uint32_t size = 0x200;
    Nor *n = nor_new(0x800, false, 3);
    NvDevice d = nor_device(n);
    d.blank = nor_blank;
    NvLog log;
    g_blank_asked = 0;
    g_blank_never = false;
    /* Something in the OTHER sector, where a journal that asked about the
     * wrong one would find it. */
    n->mem[1][0x100] = 0;
    nvlog_mount(&log, &d, size);
    CHECK(g_blank_asked == 0);          /* no journal: nothing to look over */

    memset(g_image, NV_FILL, size);
    g_image[5] = 1;
    NvRange r = { 5, 6 };
    CHECK(nvlog_commit(&log, g_image, &r, 1, 1));
    /* The sector about to be written: blank, so it is not erased. */
    CHECK(g_blank_asked == 1 && n->erased[0] == 0);
    nvlog_mount(&log, &d, size);
    /* What is left of it after the last record: clean, so records go on. */
    CHECK(g_blank_asked == 2 && log.appendable);

    /* Told the rest of the sector is not clean, nothing is added to it;
     * told the other is not blank, it is erased before it is written. */
    g_blank_never = true;
    nvlog_mount(&log, &d, size);
    CHECK(log.have_commit && !log.appendable);
    g_image[6] = 2;
    r.hi = 7;
    CHECK(nvlog_commit(&log, g_image, &r, 1, 2));
    CHECK(log.active == 1 && n->erased[1] == 1 && n->erased[0] == 0);
    CHECK(n->mem[1][0x100] != 0);
    g_blank_never = false;
    nvlog_mount(&log, &d, size);
    CHECK(log.active == 1 && log.appendable && log.seq == 2);
    CHECK(nvlog_replay(&log, g_back) && memcmp(g_back, g_image, size) == 0);
    free(n);
}

/* A FIRST SAVE THAT FINDS THE FIRST SECTOR DEAD GOES TO THE SECOND, and
 * the journal lives there from then on for as long as it has room: one bad
 * sector of the two is not a cartridge that cannot save. */
static void test_a_first_save_takes_whichever_sector_works(void) {
    const uint32_t size = 0x200;
    for (int garbage = 0; garbage < 2; garbage++) {
        Nor *n = nor_new(0x800, garbage != 0, 9);
        NvLog log;
        n->bad[0] = true;
        power_on(n, &log, size);
        memset(g_image, NV_FILL, size);
        g_image[9] = 7;
        NvRange r = { 9, 10 };
        CHECK(nvlog_commit(&log, g_image, &r, 1, 1));
        CHECK(log.active == 1 && log.error == NV_OK && log.gen == 1);
        power_on(n, &log, size);
        n->bad[0] = true;
        CHECK(log.active == 1 && log.appendable);
        CHECK(nvlog_replay(&log, g_back) && memcmp(g_back, g_image, size) == 0);
        /* ...and it goes on taking records there. */
        g_image[10] = 8;
        r.hi = 11;
        CHECK(nvlog_commit(&log, g_image, &r, 1, 2));
        CHECK(log.active == 1 && log.seq == 2);
        free(n);
    }
    /* Both dead: no journal, and it says so. */
    Nor *n = nor_new(0x800, false, 9);
    NvLog log;
    n->bad[0] = n->bad[1] = true;
    power_on(n, &log, size);
    n->bad[0] = n->bad[1] = true;
    NvRange r = { 9, 10 };
    CHECK(!nvlog_commit(&log, g_image, &r, 1, 1));
    CHECK(log.active < 0 && log.error == NV_ERR_PROGRAM);
    /* ...and which of the two it could not do: with something in the
     * sectors it is the erase that fails, before a word is tried. */
    n->mem[0][40] = n->mem[1][40] = 0;
    unsigned words = n->words;
    CHECK(!nvlog_commit(&log, g_image, &r, 1, 1));
    CHECK(log.active < 0 && log.error == NV_ERR_ERASE && n->words == words);
    /* An image too big for a sector is not a reason to try the other. */
    n->bad[0] = n->bad[1] = false;
    Nor *small = nor_new(0x40, false, 9);
    power_on(small, &log, size);
    for (uint32_t i = 0; i < size; i++) g_image[i] = (uint8_t)(i | 1);
    CHECK(!nvlog_sync(&log, g_image, 3));
    CHECK(log.error == NV_ERR_SPACE && small->words == 0);
    free(small);
    free(n);
}

int main(void) {
    test_a_first_save_makes_the_journal();
    test_a_sector_with_anything_in_it_is_erased_first();
    test_what_is_read_back_is_what_was_saved();
    test_the_power_can_go_at_any_word();
    test_half_a_commit_is_never_made_whole();
    test_a_sector_that_fails_is_left_for_the_other();
    test_the_sector_before_is_what_is_left_if_this_one_goes();
    test_a_damaged_head_ends_the_journal_there();
    test_no_seal_is_the_erased_word();
    test_an_image_too_big_for_the_sector_touches_nothing();
    test_records_that_do_not_fit_the_save_are_not_played();
    test_the_hash_follows_the_image_a_byte_at_a_time();
    test_it_says_beforehand_what_a_save_will_cost();
    test_a_device_that_reads_its_own_sectors_is_asked();
    test_a_first_save_takes_whichever_sector_works();
    if (g_failures) {
        printf("%d check(s) failed.\n", g_failures);
        return 1;
    }
    printf("All journal tests passed.\n");
    return 0;
}
