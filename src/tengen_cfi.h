/* tengen_cfi — WHAT A FLASH CHIP SAYS OF ITSELF.
 *
 * The port can keep its save in the flash chip its own program is in
 * (tengen_nvlog.h), on the boards that have nothing else to keep it in. To
 * do that it has to know three things about a chip it has never seen: which
 * commands it takes, how big it is, and where its sectors begin and end —
 * an erase takes a whole sector, and one that took part of the program with
 * it would be the end of the cartridge.
 *
 * Chips answer that themselves. The Common Flash Interface is a table every
 * NOR flash of the last twenty-five years returns when it is asked (one
 * write: 0x98 to word 0x55), and it is what the flasher that wrote the game
 * onto the board trusted to do so. This file is the reading of that table
 * and nothing else: no bus, no chip, only the words as they were read. The
 * cartridge's side asks (gba/cartsave.c) and tests/test_cfi.c gives it
 * tables shaped like the ones the datasheets print.
 *
 * THE BOARD CAN HAVE THE DATA LINES CROSSED. Reproduction boards are routed
 * for cost, and on most of them the chip's D0 and D1 are swapped on the way
 * to the cartridge edge; on a few D6 and D7 as well. What is STORED does
 * not notice — it goes in crossed and comes out crossed back — but what the
 * chip is TOLD and what it SAYS of itself are read by the chip and by this
 * file, so both have to be crossed by hand: the unlock byte 0xAA goes out
 * as 0xA9, and the table's "QRY" comes back as "RQZ". That is how the
 * wiring is told (cfi_parse), and cfi_wire is the crossing, which is its
 * own undoing.
 */
#ifndef TENGEN_CFI_H
#define TENGEN_CFI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The part of the table that is read: CFI_WORDS words from word CFI_FIRST,
 * which takes in the vendor's own table where the datasheets put it (word
 * 0x40). The words are passed as the bus gave them, sixteen bits each. */
#define CFI_FIRST        0x10
#define CFI_WORDS        0x50
#define CFI_MAX_REGIONS  8

/* The query command, as the two kinds of wiring that change it need it: a
 * board with D6 and D7 crossed has to be asked with the second. */
#define CFI_QUERY        0x98
#define CFI_QUERY_D6D7   0x58

/* How a board has the chip's data lines wired. */
enum {
    CFI_WIRE_STRAIGHT = 0,
    CFI_WIRE_D0D1,          /* D0 and D1 crossed */
    CFI_WIRE_D0D1_D6D7,     /* ...and D6 and D7 too */
    CFI_WIRE_COUNT,
};

/* The command sets (the table's words 0x13-0x14). */
#define CFI_SET_INTEL_EXT  0x0001
#define CFI_SET_AMD        0x0002
#define CFI_SET_INTEL      0x0003

typedef struct {
    uint8_t wiring;                 /* CFI_WIRE_* */
    uint8_t regions;
    uint8_t boot;                   /* the vendor table's word 0x0F: 2 the
                                     * regions as listed, 3 the other way
                                     * round, 4/5 uniform; 0 if not said */
    uint16_t command_set;
    uint32_t device_bytes;
    struct {
        uint32_t blocks, bytes;     /* so many sectors of so many bytes */
    } region[CFI_MAX_REGIONS];
} CfiInfo;

/* A byte through a board's wiring, either way: what to write for the chip
 * to read `value`, and what the chip meant by a byte that read `value`. */
uint8_t cfi_wire(uint8_t wiring, uint8_t value);

/* The wiring the first three words of a table show ("QRY" straight or
 * crossed), or -1 if they are not a table's at all. */
int cfi_magic(const uint16_t *query);

/* The table read: false if it is not one, or says something this file will
 * not act on (no size, no sectors, a sector that is not a power of two). */
bool cfi_parse(const uint16_t *query, CfiInfo *info);

/* The size of the sector that begins at byte `addr` of the chip — 0 if none
 * begins there, or if it depends on which way round the regions are (chips
 * with small sectors at one end list them the same whichever end it is,
 * and say which in a word not all of them have). */
uint32_t cfi_block_at(const CfiInfo *info, uint32_t addr);

/* WHERE A JOURNAL'S TWO HALVES GO: one at `first` and one `slot` bytes on,
 * each of at least `least` bytes and at most `slot`, made of whole sectors
 * all of one size. Gives the sector size and the size of a half; false if
 * the chip is not one whose commands the cartridge knows, the halves do
 * not start on sectors, or they do not fit before `limit` and the chip's
 * own end. */
bool cfi_place(const CfiInfo *info, uint32_t first, uint32_t slot,
               uint32_t least, uint32_t limit, uint32_t *block, uint32_t *half);

#ifdef __cplusplus
}
#endif

#endif /* TENGEN_CFI_H */
