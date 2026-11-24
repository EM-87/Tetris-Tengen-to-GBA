/* test_cfi — the reading of a flash chip's query table (src/tengen_cfi.c),
 * against tables shaped like the ones the datasheets print. The shapes, not
 * anybody's dump: a uniform chip of 128 sectors of 128KB, which is what the
 * boards this was written for carry; chips with small sectors at one end;
 * one that takes Intel's commands; and things that are not tables at all.
 */
#include "../src/tengen_cfi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed++; \
        } \
    } while (0)

typedef struct {
    uint32_t blocks, bytes;
} Region;

/* A table as a chip wired `wiring` would give it to the bus. */
static void make(uint16_t *q, uint8_t wiring, uint16_t set, unsigned size_log2,
                 const Region *r, int regions, int boot) {
    uint8_t t[CFI_FIRST + CFI_WORDS];
    memset(t, 0, sizeof t);
    t[0x10] = 'Q';
    t[0x11] = 'R';
    t[0x12] = 'Y';
    t[0x13] = (uint8_t)set;
    t[0x14] = (uint8_t)(set >> 8);
    t[0x15] = 0x40;
    t[0x1B] = 0x27;             /* 2.7 V */
    t[0x1C] = 0x36;             /* 3.6 V */
    t[0x1F] = 0x06;             /* a word in 64 microseconds */
    t[0x21] = 0x09;             /* a sector in half a second */
    t[0x27] = (uint8_t)size_log2;
    t[0x28] = 0x02;
    t[0x2A] = 0x06;
    t[0x2C] = (uint8_t)regions;
    for (int i = 0; i < regions && i < 12; i++) {
        t[0x2D + 4 * i] = (uint8_t)(r[i].blocks - 1);
        t[0x2E + 4 * i] = (uint8_t)((r[i].blocks - 1) >> 8);
        t[0x2F + 4 * i] = (uint8_t)(r[i].bytes / 256);
        t[0x30 + 4 * i] = (uint8_t)(r[i].bytes / 256 >> 8);
    }
    if (boot >= 0 && regions <= 4) {
        t[0x40] = 'P';
        t[0x41] = 'R';
        t[0x42] = 'I';
        t[0x43] = '1';
        t[0x44] = '3';
        t[0x4F] = (uint8_t)boot;
    }
    for (int i = 0; i < CFI_WORDS; i++) q[i] = cfi_wire(wiring, t[CFI_FIRST + i]);
}

static const Region kUniform128[] = { { 128, 0x20000 } };
static const Region kUniform64[] = { { 256, 0x10000 } };
static const Region kBoot8M[] = { { 8, 0x2000 }, { 127, 0x10000 } };

static void test_the_crossing_is_the_flashers(void) {
    printf("the crossing of the data lines is the one the flasher uses\n");
    /* FlashGBX's commands for these boards, config/fc_AGB_MSP55LV128M.txt
     * (D0 and D1) and fc_AGB_MSP54LV256.txt (D6 and D7 as well). */
    CHECK(cfi_wire(CFI_WIRE_D0D1, 0xAA) == 0xA9);
    CHECK(cfi_wire(CFI_WIRE_D0D1, 0x55) == 0x56);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0xAA) == 0x69);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0x55) == 0x96);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0x90) == 0x50);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0xA0) == 0x60);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0x80) == 0x40);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, CFI_QUERY) == CFI_QUERY_D6D7);
    /* What does not change on any of them: the commands a console that
     * does not yet know the wiring can send. */
    static const uint8_t same[] = { 0x90, 0xA0, 0x80, 0x30, 0xF0, 0xFF, CFI_QUERY };
    for (unsigned i = 0; i < sizeof same; i++)
        CHECK(cfi_wire(CFI_WIRE_D0D1, same[i]) == same[i]);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0x30) == 0x30);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0xF0) == 0xF0);
    CHECK(cfi_wire(CFI_WIRE_D0D1_D6D7, 0xFF) == 0xFF);
    /* ...and it is its own undoing, and nothing at all when straight. */
    for (int w = 0; w < CFI_WIRE_COUNT; w++)
        for (int v = 0; v < 256; v++) {
            CHECK(cfi_wire((uint8_t)w, cfi_wire((uint8_t)w, (uint8_t)v)) == v);
            if (w == CFI_WIRE_STRAIGHT) CHECK(cfi_wire((uint8_t)w, (uint8_t)v) == v);
        }
}

static void test_a_table_is_told_by_its_three_letters(void) {
    printf("a table is told, and its wiring, by its three letters\n");
    uint16_t q[CFI_WORDS];
    for (int w = 0; w < CFI_WIRE_COUNT; w++) {
        make(q, (uint8_t)w, CFI_SET_AMD, 24, kUniform128, 1, 5);
        CHECK(cfi_magic(q) == w);
    }
    make(q, CFI_WIRE_D0D1, CFI_SET_AMD, 24, kUniform128, 1, 5);
    CHECK(q[0] == 'R' && q[1] == 'Q' && q[2] == 'Z');
    make(q, CFI_WIRE_D0D1_D6D7, CFI_SET_AMD, 24, kUniform128, 1, 5);
    CHECK(q[0] == 0x92 && q[1] == 0x91 && q[2] == 0x9A);

    /* What a cartridge that is not in query mode reads there: its own
     * header, where those three words are part of the boot logo. */
    static const uint16_t logo[3] = { 0x0993, 0x20CE, 0x4610 };
    memset(q, 0, sizeof q);
    memcpy(q, logo, sizeof logo);
    CHECK(cfi_magic(q) == -1);
    CfiInfo info;
    CHECK(!cfi_parse(q, &info));
    memset(q, 0, sizeof q);
    CHECK(cfi_magic(q) == -1);
    memset(q, 0xFF, sizeof q);
    CHECK(cfi_magic(q) == -1);
    /* Two letters of three are not a table. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, 5);
    q[2] = 'Z';
    CHECK(cfi_magic(q) == -1);
    q[2] = 'Y';
    q[1] = 'Q';
    CHECK(cfi_magic(q) == -1);
    q[1] = 'R';
    q[0] = 'R';
    CHECK(cfi_magic(q) == -1);
}

static void test_what_the_table_says(void) {
    printf("what the table says is what is read, whichever way it is wired\n");
    uint16_t q[CFI_WORDS];
    for (int w = 0; w < CFI_WIRE_COUNT; w++) {
        CfiInfo info;
        make(q, (uint8_t)w, CFI_SET_AMD, 24, kUniform128, 1, 5);
        CHECK(cfi_parse(q, &info));
        CHECK(info.wiring == w);
        CHECK(info.command_set == CFI_SET_AMD);
        CHECK(info.device_bytes == 0x1000000);
        CHECK(info.regions == 1);
        CHECK(info.region[0].blocks == 128 && info.region[0].bytes == 0x20000);
        CHECK(info.boot == 5);
        /* The upper half of each word is not the table's: a chip may leave
         * anything there, and the flasher never looks either. */
        for (int i = 0; i < CFI_WORDS; i++) q[i] |= 0xA500;
        CfiInfo again;
        CHECK(cfi_parse(q, &again));
        CHECK(memcmp(&info, &again, sizeof info) == 0);
    }
    CfiInfo info;
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_INTEL_EXT, 25, kUniform128, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(info.command_set == CFI_SET_INTEL_EXT && info.boot == 0);
    CHECK(info.device_bytes == 0x2000000);
    make(q, CFI_WIRE_STRAIGHT, 0x0200, 24, kUniform128, 1, -1);
    CHECK(cfi_parse(q, &info) && info.command_set == 0x0200);
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 23, kBoot8M, 2, 2);
    CHECK(cfi_parse(q, &info));
    CHECK(info.regions == 2 && info.boot == 2);
    CHECK(info.region[0].blocks == 8 && info.region[0].bytes == 0x2000);
    CHECK(info.region[1].blocks == 127 && info.region[1].bytes == 0x10000);

    /* A vendor table that is somewhere this did not read says nothing. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, 3);
    q[0x15 - CFI_FIRST] = 0x58;
    CHECK(cfi_parse(q, &info) && info.boot == 0);
    q[0x15 - CFI_FIRST] = 0x08;
    CHECK(cfi_parse(q, &info) && info.boot == 0);
    /* The last place it can be and still be read whole. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, -1);
    q[0x15 - CFI_FIRST] = 0x50;
    q[0x50 - CFI_FIRST] = 'P';
    q[0x51 - CFI_FIRST] = 'R';
    q[0x52 - CFI_FIRST] = 'I';
    q[0x5F - CFI_FIRST] = 3;
    CHECK(cfi_parse(q, &info) && info.boot == 3);
    q[0x15 - CFI_FIRST] = 0x51;
    CHECK(cfi_parse(q, &info) && info.boot == 0);
    /* Its three letters where its last word would be past what was read:
     * not read past. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, -1);
    q[0x15 - CFI_FIRST] = 0x58;
    q[0x58 - CFI_FIRST] = 'P';
    q[0x59 - CFI_FIRST] = 'R';
    q[0x5A - CFI_FIRST] = 'I';
    CHECK(cfi_parse(q, &info) && info.boot == 0);
    /* ...and all three letters, not the first of them. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, 3);
    q[0x41 - CFI_FIRST] = 'Q';
    CHECK(cfi_parse(q, &info) && info.boot == 0);
    q[0x41 - CFI_FIRST] = 'R';
    q[0x42 - CFI_FIRST] = 'Y';
    CHECK(cfi_parse(q, &info) && info.boot == 0);
    q[0x42 - CFI_FIRST] = 'I';
    CHECK(cfi_parse(q, &info) && info.boot == 3);
}

static void test_a_table_that_makes_no_sense_is_not_believed(void) {
    printf("a table that makes no sense is not believed\n");
    uint16_t q[CFI_WORDS];
    CfiInfo info;
    /* The size: 64KB to 256MB and nothing else. */
    static const struct { unsigned n; bool ok; } sizes[] = {
        { 0, false }, { 15, false }, { 16, true }, { 28, true }, { 29, false }, { 255, false },
    };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        static const Region one[] = { { 1, 0x10000 } };
        make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, sizes[i].n, one, 1, -1);
        CHECK(cfi_parse(q, &info) == sizes[i].ok);
        if (sizes[i].ok) CHECK(info.device_bytes == 1u << sizes[i].n);
    }
    /* ...whatever its sectors say: 32KB of one 32KB sector is consistent,
     * and still not a chip a game is in. */
    static const Region lone[] = { { 1, 0x8000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 15, lone, 1, -1);
    CHECK(!cfi_parse(q, &info));
    /* No regions, and more than there is room to hold. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, -1);
    q[0x2C - CFI_FIRST] = 0;
    CHECK(!cfi_parse(q, &info));
    q[0x2C - CFI_FIRST] = CFI_MAX_REGIONS + 1;
    CHECK(!cfi_parse(q, &info));
    Region many[CFI_MAX_REGIONS + 1];
    for (int i = 0; i <= CFI_MAX_REGIONS; i++) many[i] = (Region){ 2, 0x10000 };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, many, CFI_MAX_REGIONS, -1);
    CHECK(cfi_parse(q, &info) && info.regions == CFI_MAX_REGIONS);
    CHECK(info.region[CFI_MAX_REGIONS - 1].blocks == 2);
    /* One more than that, every one of them in order: still too many to
     * hold, and not to be cut down to what fits. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, many, CFI_MAX_REGIONS + 1, -1);
    CHECK(!cfi_parse(q, &info));
    /* A sector that is not a power of two; one of no bytes; one bigger
     * than the chip; more of them than the chip holds. */
    static const Region odd[] = { { 128, 0x18000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, odd, 1, -1);
    CHECK(!cfi_parse(q, &info));
    static const Region none[] = { { 128, 0 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, none, 1, -1);
    CHECK(!cfi_parse(q, &info));
    static const Region big[] = { { 1, 0x800000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 22, big, 1, -1);
    CHECK(!cfi_parse(q, &info));
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 23, big, 1, -1);
    CHECK(cfi_parse(q, &info));
    static const Region crowd[] = { { 129, 0x20000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, crowd, 1, -1);
    CHECK(!cfi_parse(q, &info));
}

static void test_where_a_sector_begins(void) {
    printf("where a sector begins, and how big it is\n");
    uint16_t q[CFI_WORDS];
    CfiInfo info;
    make(q, CFI_WIRE_D0D1, CFI_SET_AMD, 24, kUniform128, 1, 5);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_block_at(&info, 0) == 0x20000);
    CHECK(cfi_block_at(&info, 0x100000) == 0x20000);
    CHECK(cfi_block_at(&info, 0xFE0000) == 0x20000);
    CHECK(cfi_block_at(&info, 0x1000000) == 0);        /* past the chip */
    CHECK(cfi_block_at(&info, 0x100002) == 0);         /* inside a sector */
    CHECK(cfi_block_at(&info, 0x110000) == 0);
    CHECK(cfi_block_at(&info, 0x11FFFE) == 0);

    /* SMALL SECTORS AT ONE END, and the table does not say which in a way
     * to be trusted: where the two readings differ, nothing is said. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 23, kBoot8M, 2, 2);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_block_at(&info, 0) == 0);                /* 8KB or 64KB */
    CHECK(cfi_block_at(&info, 0x2000) == 0);
    CHECK(cfi_block_at(&info, 0x10000) == 0x10000);    /* 64KB either way */
    CHECK(cfi_block_at(&info, 0x100000) == 0x10000);
    CHECK(cfi_block_at(&info, 0x7E0000) == 0x10000);
    CHECK(cfi_block_at(&info, 0x7F0000) == 0);         /* 64KB or 8KB */
    CHECK(cfi_block_at(&info, 0x7FE000) == 0);
    CHECK(cfi_block_at(&info, 0x108000) == 0);

    /* Regions that do not add up to the chip. One alone is still clear
     * about what it covers; two leave it open where the second starts. */
    static const Region half[] = { { 64, 0x20000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, half, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_block_at(&info, 0x100000) == 0x20000);
    CHECK(cfi_block_at(&info, 0x7E0000) == 0x20000);
    CHECK(cfi_block_at(&info, 0x800000) == 0);
    static const Region shy[] = { { 8, 0x2000 }, { 100, 0x10000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 23, shy, 2, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_block_at(&info, 0x100000) == 0);
    /* Two regions of the same size are one, whichever way round. */
    static const Region twin[] = { { 64, 0x20000 }, { 64, 0x20000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, twin, 2, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_block_at(&info, 0x100000) == 0x20000);
    CHECK(cfi_block_at(&info, 0x800000) == 0x20000);
}

#define FIRST  0x100000u
#define SLOT   0x40000u
#define LEAST  0x10000u
#define LIMIT  0x2000000u

static void test_where_the_journal_goes(void) {
    printf("where the journal's two halves go\n");
    uint16_t q[CFI_WORDS];
    CfiInfo info;
    uint32_t block = 1, half = 1;

    make(q, CFI_WIRE_D0D1, CFI_SET_AMD, 24, kUniform128, 1, 5);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x20000 && half == 0x20000);

    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform64, 1, 5);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x10000 && half == 0x10000);

    /* Sectors as big as the slot are the most there is room for... */
    static const Region quarter[] = { { 64, 0x40000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, quarter, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x40000 && half == 0x40000);
    static const Region halfmeg[] = { { 32, 0x80000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, halfmeg, 1, -1);
    CHECK(cfi_parse(q, &info));
    block = half = 1;
    CHECK(!cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 1 && half == 1);

    /* ...and small ones are taken as many at a time as make the least. */
    static const Region small[] = { { 512, 0x8000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, small, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x8000 && half == 0x10000);
    static const Region tiny[] = { { 4096, 0x1000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, tiny, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x1000 && half == 0x10000);

    /* INTEL'S COMMANDS ARE NOT SPOKEN: the same table from such a chip is
     * read and then left alone. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_INTEL_EXT, 24, kUniform128, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(!cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_INTEL, 24, kUniform128, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(!cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    make(q, CFI_WIRE_STRAIGHT, 0x0200, 24, kUniform128, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(!cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));

    /* A chip the halves do not fit in: 1MB has no room past the program,
     * and 1.25MB has room for one. One of 1.5MB holds both, to the byte. */
    static const Region meg[] = { { 16, 0x10000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 20, meg, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(!cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    static const Region two[] = { { 32, 0x10000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 21, two, 1, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(!cfi_place(&info, 0x1C0000, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(cfi_place(&info, 0x1B0000, SLOT, LEAST, LIMIT, &block, &half));
    /* ...and the limit asked for is one as well. */
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, FIRST + SLOT + 0x10000, &block, &half));
    CHECK(!cfi_place(&info, FIRST, SLOT, LEAST, FIRST + SLOT + 0xFFFF, &block, &half));

    /* Halves that would not start on a sector, or would take in sectors
     * of two sizes. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform128, 1, 5);
    CHECK(cfi_parse(q, &info));
    CHECK(!cfi_place(&info, FIRST + 0x10000, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(!cfi_place(&info, FIRST, SLOT + 0x10000, LEAST, LIMIT, &block, &half));
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 23, kBoot8M, 2, 2);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_place(&info, FIRST, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x10000 && half == 0x10000);
    CHECK(!cfi_place(&info, 0, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(!cfi_place(&info, 0x7B0000, SLOT, LEAST, LIMIT, &block, &half));
    CHECK(cfi_place(&info, 0x7A0000, SLOT, LEAST, LIMIT, &block, &half));
    /* A HALF OF TWO SECTORS WHOSE SECOND IS ANOTHER SIZE. Small sectors at
     * both ends and big ones between, the same whichever way round it is
     * read: the last small one before the big ones starts a half that
     * would run on into them. */
    static const Region both_ends[] = { { 34, 0x8000 }, { 30, 0x10000 }, { 34, 0x8000 } };
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 22, both_ends, 3, -1);
    CHECK(cfi_parse(q, &info));
    CHECK(cfi_block_at(&info, 0x108000) == 0x8000 && cfi_block_at(&info, 0x110000) == 0x10000);
    CHECK(cfi_block_at(&info, 0x2F0000) == 0x8000 && cfi_block_at(&info, 0x2F8000) == 0x8000);
    CHECK(cfi_place(&info, 0x100000, 0x1F0000, LEAST, LIMIT, &block, &half));
    CHECK(block == 0x8000 && half == 0x10000);
    CHECK(!cfi_place(&info, 0x108000, 0x1E8000, LEAST, LIMIT, &block, &half));
    /* The least asked for is more than a slot: nothing fits. */
    make(q, CFI_WIRE_STRAIGHT, CFI_SET_AMD, 24, kUniform64, 1, 5);
    CHECK(cfi_parse(q, &info));
    CHECK(!cfi_place(&info, FIRST, SLOT, SLOT + 0x10000, LIMIT, &block, &half));
    CHECK(cfi_place(&info, FIRST, SLOT, SLOT, LIMIT, &block, &half));
    CHECK(block == 0x10000 && half == SLOT);
}

int main(void) {
    test_the_crossing_is_the_flashers();
    test_a_table_is_told_by_its_three_letters();
    test_what_the_table_says();
    test_a_table_that_makes_no_sense_is_not_believed();
    test_where_a_sector_begins();
    test_where_the_journal_goes();
    if (g_failed) {
        printf("%d check(s) FAILED\n", g_failed);
        return 1;
    }
    printf("All query-table tests passed.\n");
    return 0;
}
