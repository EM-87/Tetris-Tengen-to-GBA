/* tengen_cfi — the reading of a flash chip's query table. See the header.
 *
 * The table is addressed in words; each word carries one byte of it in its
 * low half. The places below are the standard's (JEDEC JESD68.01), and the
 * same ones the flasher reads (FlashGBX, Flashcart.py: it indexes bytes, so
 * its 0x4E is word 0x27 here):
 *
 *   0x10-0x12  'Q' 'R' 'Y'
 *   0x13-0x14  the command set
 *   0x15-0x16  where the vendor's own table is
 *   0x27       the device's size, as a power of two
 *   0x2C       how many erase regions
 *   0x2D...    four words a region: sectors less one, then bytes over 256
 */
#include "tengen_cfi.h"

#include <string.h>

uint8_t cfi_wire(uint8_t wiring, uint8_t value) {
    if (wiring == CFI_WIRE_D0D1 || wiring == CFI_WIRE_D0D1_D6D7) {
        if (((value >> 0) ^ (value >> 1)) & 1u) value ^= 0x03u;
    }
    if (wiring == CFI_WIRE_D0D1_D6D7) {
        if (((value >> 6) ^ (value >> 7)) & 1u) value ^= 0xC0u;
    }
    return value;
}

int cfi_magic(const uint16_t *query) {
    for (int w = 0; w < CFI_WIRE_COUNT; w++) {
        if (cfi_wire((uint8_t)w, (uint8_t)query[0]) == 'Q' &&
            cfi_wire((uint8_t)w, (uint8_t)query[1]) == 'R' &&
            cfi_wire((uint8_t)w, (uint8_t)query[2]) == 'Y')
            return w;
    }
    return -1;
}

/* The table's byte at word `at`, as the chip meant it. */
static uint8_t byte_at(const uint16_t *query, uint8_t wiring, unsigned at) {
    return cfi_wire(wiring, (uint8_t)query[at - CFI_FIRST]);
}

static bool power_of_two(uint32_t v) {
    return v && !(v & (v - 1u));
}

bool cfi_parse(const uint16_t *query, CfiInfo *info) {
    memset(info, 0, sizeof *info);
    int wiring = cfi_magic(query);
    if (wiring < 0) return false;
    uint8_t w = (uint8_t)wiring;
    info->wiring = w;
    info->command_set = (uint16_t)(byte_at(query, w, 0x13) | (byte_at(query, w, 0x14) << 8));

    /* 64KB to 256MB. Outside that the byte is not a size. */
    unsigned n = byte_at(query, w, 0x27);
    if (n < 16 || n > 28) return false;
    info->device_bytes = 1u << n;

    unsigned regions = byte_at(query, w, 0x2C);
    if (regions < 1 || regions > CFI_MAX_REGIONS) return false;
    info->regions = (uint8_t)regions;
    for (unsigned i = 0; i < regions; i++) {
        unsigned at = 0x2D + 4 * i;
        uint32_t blocks = (uint32_t)(byte_at(query, w, at) | (byte_at(query, w, at + 1) << 8)) + 1u;
        uint32_t bytes = (uint32_t)(byte_at(query, w, at + 2) | (byte_at(query, w, at + 3) << 8)) * 256u;
        /* A sector is a power of two, and a region's are no more than the
         * device holds: a table that says otherwise is not believed. */
        if (!power_of_two(bytes)) return false;
        if (blocks > info->device_bytes / bytes) return false;
        info->region[i].blocks = blocks;
        info->region[i].bytes = bytes;
    }

    /* The vendor's table, if it is where it says and is one: the word that
     * tells which end the small sectors are at. */
    unsigned pri = (unsigned)(byte_at(query, w, 0x15) | (byte_at(query, w, 0x16) << 8));
    if (pri >= CFI_FIRST && pri + 0x0F < CFI_FIRST + CFI_WORDS &&
        byte_at(query, w, pri) == 'P' && byte_at(query, w, pri + 1) == 'R' &&
        byte_at(query, w, pri + 2) == 'I')
        info->boot = byte_at(query, w, pri + 0x0F);
    return true;
}

/* The sector beginning at `addr`, with the regions laid from the bottom of
 * the chip in the order listed, or in the opposite order. */
static uint32_t block_one_way(const CfiInfo *info, uint32_t addr, bool reversed) {
    uint32_t base = 0;
    for (int k = 0; k < info->regions; k++) {
        int i = reversed ? info->regions - 1 - k : k;
        uint32_t bytes = info->region[i].bytes;
        uint32_t span = info->region[i].blocks * bytes;
        if (addr - base < span)
            return ((addr - base) & (bytes - 1u)) ? 0 : bytes;
        base += span;
    }
    return 0;
}

uint32_t cfi_block_at(const CfiInfo *info, uint32_t addr) {
    uint32_t total = 0;
    for (int i = 0; i < info->regions; i++)
        total += info->region[i].blocks * info->region[i].bytes;
    /* Regions that do not add up to the device leave it unsaid where the
     * second begins — unless there is only the one. */
    if (info->regions > 1 && total != info->device_bytes) return 0;
    uint32_t up = block_one_way(info, addr, false);
    uint32_t down = block_one_way(info, addr, true);
    return up == down ? up : 0;
}

bool cfi_place(const CfiInfo *info, uint32_t first, uint32_t slot,
               uint32_t least, uint32_t limit, uint32_t *block, uint32_t *half) {
    /* ONLY THE COMMAND SET THE CARTRIDGE'S ROUTINES SPEAK. Intel's chips
     * answer the same query and take different commands; they are told
     * apart here and left alone. */
    if (info->command_set != CFI_SET_AMD) return false;
    uint32_t b = cfi_block_at(info, first);
    if (!b) return false;
    uint32_t h = b > least ? b : least;
    if (h > slot || (h & (b - 1u))) return false;
    if (first + slot + h > limit) return false;
    /* Every sector of both halves, each beginning where the last ended and
     * all of the one size — which a half past the chip's end has none of. */
    for (int s = 0; s < 2; s++)
        for (uint32_t at = 0; at < h; at += b)
            if (cfi_block_at(info, first + (uint32_t)s * slot + at) != b) return false;
    *block = b;
    *half = h;
    return true;
}
