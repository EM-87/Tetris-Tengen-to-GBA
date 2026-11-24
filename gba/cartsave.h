/* cartsave — WHERE THE SAVE IS KEPT, WHICHEVER CARTRIDGE THIS IS.
 *
 * The port saves into the cartridge's save RAM, and for a long time that
 * was the whole story: a battery keeps the RAM alive, an emulator or a
 * flash cart keeps a file. The boards reproductions are built on today
 * have the RAM and NO BATTERY (or no RAM at all). On one of those the game
 * ran perfectly and forgot everything at the power switch.
 *
 * What those boards do have is the flash chip the game itself is written
 * in, sixteen megabytes of it for a program of half of one. So at power-on
 * the cartridge is asked what it is (cartsave_init), and nobody has to tell
 * it or patch anything:
 *
 *   * a chip that answers the flash query (CFI) and takes the commands
 *     this file speaks: the save is kept in two of its sectors as well, a
 *     megabyte in, well clear of the program — a journal of what changed
 *     (../src/tengen_nvlog.h) — and the save RAM is still written too;
 *   * anything else — an emulator, an EZ-Flash, an EverDrive, a SuperCard,
 *     a board with a battery and a chip that does not answer: the save RAM
 *     alone, exactly as before. Their ROM does not take writes, so the
 *     question goes nowhere and nothing changes.
 *
 * WHICH OF THE TWO IS BELIEVED AT POWER-ON, when both are there. The save
 * RAM carries a mark of what was last written to it; RAM that lost its
 * power does not add up to its mark. If it adds up, it is the newest there
 * is (it is always written first) and the flash is brought level with it;
 * if it does not, the save is read back from the flash and the RAM filled
 * from that. A board with a battery and a board without come to the same
 * save, each by the half that works.
 *
 * See cartsave.c for how a chip is written from a program that lives in it.
 */
#ifndef CARTSAVE_H
#define CARTSAVE_H

#include <stdbool.h>
#include <stdint.h>

/* The journal's two halves in the cartridge, as offsets into the ROM: one
 * megabyte in and a quarter more. The program has to stay below the first
 * (gba.ld refuses a link that does not), and a chip has to reach past the
 * second or the save RAM is all there is. */
#define CARTSAVE_FIRST  0x100000u
#define CARTSAVE_SLOT   0x040000u
#define CARTSAVE_LEAST  0x010000u       /* a half is at least this big */
/* How much of the save RAM the journal covers: all but its last 32 bytes,
 * where the mark is. */
#define CARTSAVE_IMAGE  0x7FE0u
#define CARTSAVE_MARK   CARTSAVE_IMAGE

/* What the question at power-on came to. */
enum {
    CS_PROBE_SILENT = 0,    /* nothing answered: a ROM that is a ROM */
    CS_PROBE_RAM,           /* the ROM took the write: a cart that keeps it in RAM */
    CS_PROBE_TABLE,         /* a flash chip answered */
};
/* ...and why a chip that answered is not being used, if it is not. */
enum {
    CS_WHY_USED = 0,
    CS_WHY_NO_CHIP,
    CS_WHY_TABLE,           /* its table makes no sense */
    CS_WHY_COMMANDS,        /* it takes commands this does not speak (Intel's) */
    CS_WHY_PLACE,           /* no room for the journal, or sectors of two sizes there */
    CS_WHY_NO_ID,           /* it did not answer its own unlock sequence */
    CS_WHY_TAKEN,           /* another game's ROM is in the chip where the journal would go */
};
/* Which of the two was believed. */
enum {
    CS_BOOT_PLAIN = 0,      /* no flash: the save RAM as it is */
    CS_BOOT_LEVEL,          /* RAM and journal agreed */
    CS_BOOT_RAM_NEWER,      /* the RAM added up and was ahead of the journal */
    CS_BOOT_FROM_FLASH,     /* the RAM did not add up: read back from the journal */
    CS_BOOT_FIRST_KEPT,     /* no journal yet; the RAM held a save, which starts one */
    CS_BOOT_FIRST_EMPTY,    /* no journal and nothing in the RAM: an empty one */
};

typedef struct {
    uint8_t probe;          /* CS_PROBE_* */
    uint8_t why;            /* CS_WHY_* */
    uint8_t boot;           /* CS_BOOT_* */
    uint8_t flash;          /* the journal is in use */
    uint8_t wiring;         /* CFI_WIRE_* */
    uint8_t ram_kept;       /* the save RAM added up at power-on */
    uint8_t dead;           /* the chip refused, and is left alone until the next power-on */
    uint8_t error;          /* the journal's last (NV_*) */
    uint16_t command_set;
    uint16_t id[2];         /* maker and device, as the bus gave them */
    uint16_t magic[3];      /* the table's first three words, the same */
    uint16_t probe_word[3]; /* ROM word 0x55 before, during and after the query */
    uint32_t device_bytes, block_bytes, half_bytes;
    uint32_t gen, used, seq;            /* the journal: generation, bytes, commits ever */
    uint16_t commits, fresh, erases;    /* since power-on */
    uint16_t refused, waits;            /* saves the chip refused; saves that waited */
    /* The table's sectors, for the page: how many regions, and the first's
     * count and size. */
    uint16_t regions, region_blocks;
    uint32_t region_bytes;
    uint8_t afresh;         /* the journal is to be written afresh before anything is added */
    uint8_t off;            /* the chip was switched off for this session, from the page */
} CartSaveReport;

#ifdef TENGEN_MULTIBOOT
/* The Single-Pak copy has no save of its own (gba_hw.h). */
static inline void cartsave_init(void) { }
static inline void cartsave_tick(void) { }
static inline void cartsave_flush(void) { }
static inline bool cartsave_page_asked(void) { return false; }
static inline void cartsave_page(void) { }
#else
/* At power-on and after a soft reset, before anything reads the save. */
void cartsave_init(void);
/* The save's bytes: sram_read and sram_write (gba_hw.h) are these. */
unsigned char cartsave_read(unsigned offset);
void cartsave_write(unsigned offset, unsigned char value);
/* Once a frame, from vsync: what was written since the last goes to the
 * flash, if there is one and the moment can spare the milliseconds. */
void cartsave_tick(void);
/* ...and now, whatever the moment: before a restart or a sleep. */
void cartsave_flush(void);
const CartSaveReport *cartsave_report(void);
/* L+R+A HELD AT POWER-ON: a page of what the cartridge turned out to be —
 * whether its save RAM kept its contents, what the flash chip answered,
 * where the save is going. For the one question a board nobody here has
 * seen raises ("it does not save": does not save where?), answered by a
 * photograph of the screen. cartsave_page_asked is whether the three were
 * down when the console came on; cartsave_page draws it, once the screen
 * is laid out, and waits.
 *
 * NOTHING HAS BEEN WRITTEN TO THE CHIP WHEN THE PAGE COMES UP, and nothing
 * is while it is up: what it shows was read. So a board whose chip does
 * not take to being written still gets as far as saying what it is. START
 * goes on, and what was waiting is written; SELECT goes on WITHOUT the
 * chip for this session — the save RAM alone, as on any other cartridge —
 * which is the way round a board that hangs on its first write. */
bool cartsave_page_asked(void);
void cartsave_page(void);
#endif

#endif /* CARTSAVE_H */
