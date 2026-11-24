/* cartsave.c — the save in the save RAM and, on a board that has nothing
 * else to keep it, in the cartridge's own flash. cartsave.h says what and
 * why; this is how a chip is written by a program that lives in it.
 *
 * A FLASH CHIP THAT IS BEING TOLD SOMETHING IS NOT A ROM. From the first
 * write of a command until it is done — a few bus cycles for a question,
 * some tens of microseconds for a word programmed, a second for a sector
 * erased — every read of the chip, anywhere in it, returns what it has to
 * say about the command and not what is stored there. The program is
 * stored there. So nothing that runs in that time may come from the
 * cartridge:
 *
 *   * the routines that talk to the chip are copied to external WRAM at
 *     power-on and run from it (RAM_CODE: their own section, .ramcode, in
 *     gba.ld). They call nothing, read no table and no constant from the
 *     ROM — what they need is handed to them in RAM (ChipWords) — and
 *     `--cartsave` takes them apart to see that this stays true
 *     (ramcode_faults, tools/romcheck/flashchip.py): no branch leaves the
 *     section;
 *   * interrupts are off around every call (the handler is in internal
 *     WRAM, but what it calls is not all);
 *   * and none of them returns until the chip reads as the ROM again
 *     (back_to_rom), because the instruction after the return is in it.
 *
 * THE COMMANDS are AMD's, the set every board of this family takes, as the
 * flasher that wrote the game onto it sends them (FlashGBX,
 * config/fc_AGB_MSP55LV128M.txt and its neighbours) and as the tools that
 * give other games a save on these boards do (the batteryless patchers:
 * metroid-maniac/gba-auto-batteryless-patcher, payload.c; PocketNES's
 * flash_write): the two unlock writes, 0xAA to word 0x555 and 0x55 to word
 * 0x2AA, then 0x90 to identify, 0xA0 and the word to program one, 0x80,
 * the unlock again and 0x30 to the sector to erase it, 0xF0 to reset —
 * each byte through the board's wiring (cfi_wire). One word at a time, and
 * waited for by reading it back: the write buffer is quicker and one more
 * thing to go wrong.
 *
 * NOTHING HERE HAS RUN ON SUCH A BOARD AS THIS IS WRITTEN (INFERRED from
 * those sources and the chip family's datasheets; checked against a model
 * of the chip in the emulator, tools/romcheck/flashchip.py). What it is
 * built to do if the chip is not what it expects is nothing: every step
 * that can be checked is, and a chip that fails one is left alone until
 * the power is cycled, with the save RAM carrying on as before.
 */
#include "cartsave.h"

#ifdef TENGEN_MULTIBOOT
typedef int cartsave_is_not_in_the_single_pak_copy;
#else

#include <string.h>

#include "port.h"
#include "../src/tengen_cfi.h"
#include "../src/tengen_nvlog.h"

#define ROM_WORDS ((volatile uint16_t *)0x08000000)
#define ROM_BYTES ((const uint8_t *)0x08000000)
/* How many words are programmed with the interrupts off before they are
 * let in again: some four milliseconds' worth. */
#define PROGRAM_AT_ONCE 64u
/* The three keys that, down at power-on, ask for the page (cartsave_page). */
#define PAGE_KEYS (KEY_L | KEY_R | KEY_A)

/* ----------------------------------------------------------------------- *
 * The part that runs from RAM
 * ----------------------------------------------------------------------- */
/* noinline and noclone: link-time optimisation would otherwise fold a
 * routine into its caller in the ROM, or make itself a copy with the
 * arguments built in — in the right section, as it happens, and under a
 * name the checks could not find. */
#define RAM_CODE   __attribute__((section(".ramcode"), long_call, noinline, noclone))
#define RAM_INLINE static inline __attribute__((always_inline))

/* Everything the RAM routines need to know, in RAM: the commands as this
 * board is wired, what the ROM reads as where reads_as_rom looks, and how
 * long to wait. */
typedef struct {
    uint16_t unlock1, unlock2;      /* 0xAA and 0x55 */
    uint16_t id, erase_setup, erase, program, reset;
    uint16_t known[5];
    uint32_t settle, program_patience, erase_patience;
} ChipWords;

enum {
    CHIP_OK = 0,
    CHIP_TIMEOUT,       /* it never read back as it should, for as long as was waited */
    CHIP_WONT_TAKE,     /* the word there has a 0 where a 1 is wanted */
};

/* The words reads_as_rom compares: the entry's branch, the three places a
 * query table's letters appear, and the word the query is written to. */
#define KNOWN_AT { 0x00, 0x10, 0x11, 0x12, 0x55 }
#define PROBE_WORD 0x55             /* CFI's: the query goes to word 0x55 */
/* How long back_to_rom gives a reset before sending another, in looks at
 * the ROM, and how many of those before it says so on the screen. */
#define SETTLE_LOOKS  2000u
#define STUCK_ROUNDS  100u

/* One write to the chip, and a breath: the patcher's routines put a nop
 * after each, and they are the ones known to work. */
RAM_INLINE void bus_write(volatile uint16_t *at, uint16_t value) {
    *at = value;
    __asm__ volatile ("nop");
}

RAM_INLINE bool reads_as_rom(volatile uint16_t *rom, const ChipWords *c) {
    return rom[0x00] == c->known[0] && rom[0x10] == c->known[1] &&
           rom[0x11] == c->known[2] && rom[0x12] == c->known[3] &&
           rom[PROBE_WORD] == c->known[4];
}

/* THE CHIP BACK TO BEING THE ROM, or this does not return: the next
 * instruction after the caller's is in it. A reset, a wait for the ROM's
 * own words to read as they did at power-on, and again if they do not. A
 * chip that will not come back at all leaves a red screen where there
 * would otherwise be a program running into whatever the chip was saying,
 * and it goes on asking — the power switch is what is left. */
RAM_INLINE void back_to_rom(volatile uint16_t *rom, const ChipWords *c) {
    uint32_t rounds = 0;
    uint16_t dispcnt = 0, backdrop = 0;
    for (;;) {
        bus_write(rom, c->reset);
        for (uint32_t n = c->settle; n; n--) {
            if (!reads_as_rom(rom, c)) continue;
            if (rounds >= STUCK_ROUNDS) {
                *(vu16 *)0x05000000 = backdrop;
                REG_DISPCNT = dispcnt;
            }
            return;
        }
        if (++rounds == STUCK_ROUNDS) {
            dispcnt = REG_DISPCNT;
            backdrop = *(vu16 *)0x05000000;
            REG_DISPCNT = 0;                    /* nothing but the backdrop */
            *(vu16 *)0x05000000 = 0x001F;       /* red */
        }
    }
}

/* THE QUESTION: the query command to word 0x55, the table read, and out
 * again. This is the one thing sent to a cartridge nothing is known about
 * yet, so it is the least there is: three writes, all to the one word, of
 * values no chip takes for an order to change anything (0x98 or 0x58, then
 * the two resets, AMD's 0xF0 and Intel's 0xFF, which no wiring alters).
 *
 * out[0..CFI_WORDS-1] is what the table's place read as, then the probe
 * word during the query, the same after the resets, and whether the ROM
 * read as the ROM at the end.
 *
 * A ROM THAT IS REALLY RAM keeps what is written to it — a flash cart that
 * loads the game into memory and leaves it unlocked. It is known by the
 * probe word having changed for good, and what was there is put back. */
RAM_CODE void cartsave_ram_query(volatile uint16_t *rom, const ChipWords *c,
                                 uint32_t command, uint16_t *out) {
    bus_write(rom + PROBE_WORD, (uint16_t)command);
    for (uint32_t i = 0; i < CFI_WORDS; i++) out[i] = rom[CFI_FIRST + i];
    out[CFI_WORDS] = rom[PROBE_WORD];
    bus_write(rom + PROBE_WORD, 0xF0);
    bus_write(rom + PROBE_WORD, 0xFF);
    uint32_t n = c->settle;
    while (n && !reads_as_rom(rom, c)) n--;
    out[CFI_WORDS + 1] = rom[PROBE_WORD];
    if (!n) {
        rom[PROBE_WORD] = c->known[4];
        n = c->settle;
        while (n && !reads_as_rom(rom, c)) n--;
    }
    out[CFI_WORDS + 2] = n != 0;
}

/* WHO THE CHIP SAYS IT IS, which is also the first three writes of every
 * command there is: if the unlock sequence as this board is thought to be
 * wired does not put the chip into its identify mode, no erase or program
 * built on it is ever sent. */
RAM_CODE void cartsave_ram_ident(volatile uint16_t *rom, const ChipWords *c,
                                 uint16_t *out) {
    bus_write(rom + 0x555, c->unlock1);
    bus_write(rom + 0x2AA, c->unlock2);
    bus_write(rom + 0x555, c->id);
    out[0] = rom[0];
    out[1] = rom[1];
    back_to_rom(rom, c);
}

/* ONE SECTOR ERASED: every bit of it back to 1. The chip is done when the
 * sector's first word reads 0xFFFF; until then it reads as status, whose
 * top data bit is 0.
 *
 * THAT IS ALL THE STATUS IS READ FOR, here and in programming: is the word
 * what it should be yet. The chip's status has more to say — bit 5 comes
 * up when its own time limit runs out — and the datasheets' algorithm
 * stops on it. The tools that are known to work on these boards do not
 * look at it (the patchers' loops, FlashGBX's "wait for 0xFFFF"), and a
 * clone that put anything in that bit would be refused here while it
 * worked for them. So the wait is theirs, with an end to it: this file's
 * own patience, after which the chip is reset and left. */
RAM_CODE uint32_t cartsave_ram_erase(volatile uint16_t *rom, const ChipWords *c,
                                     uint32_t word) {
    volatile uint16_t *at = rom + word;
    uint32_t result = CHIP_TIMEOUT;
    bus_write(at, c->reset);
    bus_write(rom + 0x555, c->unlock1);
    bus_write(rom + 0x2AA, c->unlock2);
    bus_write(rom + 0x555, c->erase_setup);
    bus_write(rom + 0x555, c->unlock1);
    bus_write(rom + 0x2AA, c->unlock2);
    bus_write(at, c->erase);
    for (uint32_t n = c->erase_patience; n; n--) {
        if (*at == 0xFFFF) {
            result = CHIP_OK;
            break;
        }
    }
    back_to_rom(rom, c);
    return result;
}

/* WORDS PROGRAMMED, one command each: bits go from 1 to 0 and never back,
 * so a word that would need one to is refused before the chip is asked. A
 * word is done when it reads back as written; it reads as status until
 * then, which can never be the word (its top data bit is the opposite). */
RAM_CODE uint32_t cartsave_ram_program(volatile uint16_t *rom, const ChipWords *c,
                                       uint32_t word, const uint8_t *src,
                                       uint32_t words) {
    volatile uint16_t *at = rom + word;
    uint32_t result = CHIP_OK;
    for (; words; words--, at++, src += 2) {
        uint16_t want = (uint16_t)(src[0] | (src[1] << 8));
        uint16_t have = *at;
        if (have == want) continue;
        if ((have & want) != want) {
            result = CHIP_WONT_TAKE;
            break;
        }
        bus_write(rom + 0x555, c->unlock1);
        bus_write(rom + 0x2AA, c->unlock2);
        bus_write(rom + 0x555, c->program);
        bus_write(at, want);
        uint32_t n = c->program_patience;
        while (*at != want) {
            if (!--n) {
                result = CHIP_TIMEOUT;
                break;
            }
        }
        if (result != CHIP_OK) break;
    }
    back_to_rom(rom, c);
    return result;
}

/* ----------------------------------------------------------------------- *
 * The rest runs from the cartridge like everything else.
 * ----------------------------------------------------------------------- */
#define CS_BLOCK_SHIFT 4            /* what changed is kept in blocks of 16 bytes */
#define CS_BLOCKS      ((CARTSAVE_IMAGE + 15u) >> CS_BLOCK_SHIFT)
/* Runs of changed blocks: at most every other block, so there is always
 * room to list them all. */
#define CS_RANGES      ((CS_BLOCKS + 1) / 2)

typedef struct {
    CartSaveReport report;
    NvLog log;
    ChipWords chip;
    uint32_t hash;                  /* the image's, kept up as it is written */
    uint32_t marked;                /* ...and as the save RAM's mark has it */
    bool flash;                     /* the journal is in use: reads come from the image */
    bool dirty;
    bool dead;
    bool waiting;
    bool page_asked;                /* L+R+A were down at power-on */
    bool hold;                      /* ...and the page is not left yet: nothing is written */
    bool no_erase;                  /* a sector outlasted all patience: no more are tried */
    uint8_t changed[(CS_BLOCKS + 7) / 8];
    NvRange ranges[CS_RANGES];
    uint16_t query[CFI_WORDS + 3];
} CartSave;

/* In external WRAM, all of it: internal WRAM has the stack to think of
 * (gba.ld). Nothing clears that memory, so cartsave_init does. (Not
 * static: the checks find them by name, and link-time optimisation renames
 * what is.) */
__attribute__((section(".ewram"))) CartSave g_cartsave;
__attribute__((section(".ewram"), aligned(4))) uint8_t g_cartsave_image[CARTSAVE_IMAGE];
#define g_cs    g_cartsave
#define g_image g_cartsave_image

/* Where things are in g_cartsave, for tools/romcheck/cartsave.py: it reads
 * the report out of the console's memory, and a field moved here would
 * otherwise be a check quietly reading its neighbour. */
#define AT(field) (uint16_t)offsetof(CartSaveReport, field)
__attribute__((used)) const uint16_t kCartSaveProbe[] = {
    (uint16_t)offsetof(CartSave, report), (uint16_t)sizeof(CartSaveReport),
    AT(probe), AT(why), AT(boot), AT(flash), AT(wiring), AT(ram_kept), AT(dead), AT(error),
    AT(command_set), AT(id), AT(magic), AT(probe_word),
    AT(device_bytes), AT(block_bytes), AT(half_bytes), AT(gen), AT(used), AT(seq),
    AT(commits), AT(fresh), AT(erases), AT(refused), AT(waits), AT(afresh), AT(off),
    (uint16_t)offsetof(CartSave, dirty), (uint16_t)offsetof(CartSave, waiting),
    (uint16_t)(offsetof(CartSave, chip) + offsetof(ChipWords, erase_patience)),
};
#undef AT

static const char kMark[4] = { 'T', 'N', 'V', 'S' };

extern const uint8_t __ramcode_lma[];
extern uint8_t __ramcode_start[], __ramcode_end[];

/* The interrupts off around a call into the RAM routines, and back as
 * they were. Before irq_init they are off already. */
#define WITH_CHIP(call) \
    do { \
        uint16_t ime_ = REG_IME; \
        REG_IME = 0; \
        call; \
        REG_IME = ime_; \
    } while (0)

/* ONLY THE JOURNAL'S OWN SECTORS ARE EVER WRITTEN OR ERASED, whatever asks
 * and whatever it asks for: the program is in the same chip. */
static bool in_journal(uint32_t at, uint32_t bytes) {
    for (uint32_t h = 0; h < 2; h++) {
        uint32_t base = CARTSAVE_FIRST + h * CARTSAVE_SLOT;
        if (at >= base && bytes <= g_cs.report.half_bytes &&
            at - base <= g_cs.report.half_bytes - bytes)
            return true;
    }
    return false;
}

static bool from_cartridge(const uint8_t *p) {
    uint32_t region = (uint32_t)(uintptr_t)p >> 24;
    return region >= 0x08 && region <= 0x0D;
}

static bool dev_program(void *ctx, int half, uint32_t at, const uint8_t *src, uint32_t n) {
    (void)ctx;
    uint32_t base = CARTSAVE_FIRST + (uint32_t)half * CARTSAVE_SLOT + at;
    if (((at | n) & 1u) || !in_journal(base, n) || from_cartridge(src)) return false;
    /* A FEW MILLISECONDS AT A TIME. A paused game is seven hundred words
     * and three frames of programming; in one call that was three frames
     * with the interrupts off, and the serial port's is how the cable, the
     * adapter and a Game Boy Player are answered. Between two of these the
     * chip is a ROM again and whatever was waiting is taken. */
    for (uint32_t done = 0; done < n; done += PROGRAM_AT_ONCE * 2u) {
        uint32_t words = (n - done) >> 1;
        if (words > PROGRAM_AT_ONCE) words = PROGRAM_AT_ONCE;
        uint32_t result;
        WITH_CHIP(result = cartsave_ram_program(ROM_WORDS, &g_cs.chip, (base + done) >> 1,
                                                src + done, words));
        if (result != CHIP_OK) return false;
    }
    /* ...and read again now that the chip is a ROM once more. */
    const volatile uint8_t *got = (const volatile uint8_t *)ROM_BYTES + base;
    for (uint32_t i = 0; i < n; i++)
        if (got[i] != src[i]) return false;
    return true;
}

static bool dev_erase(void *ctx, int half) {
    (void)ctx;
    uint32_t base = CARTSAVE_FIRST + (uint32_t)half * CARTSAVE_SLOT;
    uint32_t bytes = g_cs.report.half_bytes, block = g_cs.report.block_bytes;
    /* A SECTOR THAT OUTLASTED ALL PATIENCE IS THE LAST ONE TRIED until the
     * power is cycled. The wait is twelve seconds with the console frozen,
     * and a chip that would not erase one sector is not going to erase the
     * next: the journal's other half is tried as it stands, and if it
     * needs erasing too that is the end of it, at once. */
    if (!block || !in_journal(base, bytes) || g_cs.no_erase) return false;
    for (uint32_t at = 0; at < bytes; at += block) {
        uint32_t result;
        WITH_CHIP(result = cartsave_ram_erase(ROM_WORDS, &g_cs.chip, (base + at) >> 1));
        if (result != CHIP_OK) {
            g_cs.no_erase = true;
            return false;
        }
    }
    const volatile uint16_t *got = (const volatile uint16_t *)(ROM_WORDS + (base >> 1));
    for (uint32_t i = 0; i < bytes / 2; i++)
        if (got[i] != 0xFFFF) return false;
    return true;
}

/* Whether part of a half reads as erased, a word at a time: the journal
 * asks this of all that is left of a sector at every power-on, and a byte
 * at a time over the cartridge bus that was a sixth of a second. */
static bool dev_blank(void *ctx, int half, uint32_t from, uint32_t to) {
    (void)ctx;
    const volatile uint8_t *mem =
        (const volatile uint8_t *)ROM_BYTES + CARTSAVE_FIRST + (uint32_t)half * CARTSAVE_SLOT;
    while (from < to && (from & 3u))
        if (mem[from++] != NV_FILL) return false;
    for (; from + 4 <= to; from += 4)
        if (*(const volatile uint32_t *)(mem + from) != 0xFFFFFFFFu) return false;
    while (from < to)
        if (mem[from++] != NV_FILL) return false;
    return true;
}

/* The journal's figures into the report, after anything that moves them. */
static void report_journal(void) {
    CartSaveReport *r = &g_cs.report;
    r->dead = g_cs.dead;
    r->error = g_cs.log.error;
    r->gen = g_cs.log.gen;
    r->used = nvlog_used(&g_cs.log);
    r->seq = g_cs.log.seq;
    r->commits = g_cs.log.commits;
    r->fresh = g_cs.log.compactions;
    r->erases = g_cs.log.erases;
}

/* THE SAVE RAM'S MARK: four letters, the image's hash and the hash again
 * upside down, past the end of what the journal covers. RAM that has been
 * without power does not add up to it, and RAM that never had one (an
 * older build's save, another game's) has none. */
static void mark_write(void) {
    uint32_t h = g_cs.hash;
    for (unsigned i = 0; i < 4; i++) {
        MEM_SRAM[CARTSAVE_MARK + i] = (unsigned char)kMark[i];
        MEM_SRAM[CARTSAVE_MARK + 4 + i] = (unsigned char)(h >> (8 * i));
        MEM_SRAM[CARTSAVE_MARK + 8 + i] = (unsigned char)(~h >> (8 * i));
    }
    g_cs.marked = h;
}

/* The hash the mark carries, if there is a mark there at all. */
static bool mark_read(uint32_t *hash) {
    uint32_t h = 0, inv = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (MEM_SRAM[CARTSAVE_MARK + i] != (unsigned char)kMark[i]) return false;
        h |= (uint32_t)MEM_SRAM[CARTSAVE_MARK + 4 + i] << (8 * i);
        inv |= (uint32_t)MEM_SRAM[CARTSAVE_MARK + 8 + i] << (8 * i);
    }
    *hash = h;
    return h == ~inv;
}

/* The image into the save RAM, all of it. */
static void ram_fill(void) {
    for (unsigned i = 0; i < CARTSAVE_IMAGE; i++) MEM_SRAM[i] = g_image[i];
}

/* WHETHER ANOTHER GAME FOLLOWS THIS ONE IN THE CHIP. The megabyte past
 * the program is the port's to keep a journal in on a board made for one
 * game. A flash cart that keeps several in the one chip, each mapped in
 * turn, may have the next where the journal would go — and what is there
 * that is not a journal gets erased. Every GBA ROM begins with the same
 * 156 bytes, the boot logo the BIOS checks, four bytes in; this program's
 * own header has them to compare with. Looked for every 32KB from the end
 * of the program to the end of the journal: a game that starts anywhere
 * there is in the way or runs into it. */
#define ROM_STEP     0x8000u
#define LOGO_AT      4u
#define LOGO_BYTES   156u

static bool another_rom_follows(uint32_t end) {
    const volatile uint8_t *rom = (const volatile uint8_t *)ROM_BYTES;
    uint32_t from = (uint32_t)(uintptr_t)__ramcode_lma - 0x08000000u +
                    (uint32_t)(__ramcode_end - __ramcode_start);
    from = (from + ROM_STEP - 1u) & ~(ROM_STEP - 1u);
    for (uint32_t at = from; at < end; at += ROM_STEP) {
        uint32_t i = 0;
        while (i < LOGO_BYTES && rom[at + LOGO_AT + i] == rom[LOGO_AT + i]) i++;
        if (i == LOGO_BYTES) return true;
    }
    return false;
}

/* What the cartridge is, asked once. True if there is a flash chip the
 * journal can live in; the report says what was found either way. */
static bool chip_found(void) {
    CartSaveReport *r = &g_cs.report;
    ChipWords *c = &g_cs.chip;
    volatile uint16_t *rom = ROM_WORDS;
    static const uint8_t at[5] = KNOWN_AT;
    for (int i = 0; i < 5; i++) c->known[i] = rom[at[i]];
    c->settle = SETTLE_LOOKS;
    c->reset = 0xF0;
    r->why = CS_WHY_NO_CHIP;

    uint16_t *q = g_cs.query;
    WITH_CHIP(cartsave_ram_query(rom, c, CFI_QUERY, q));
    bool is_ram = q[CFI_WORDS + 1] != c->known[4];
    /* D6 and D7 crossed turn the query itself into something else: asked
     * again as such a board needs it, if the first went unanswered. */
    if (cfi_magic(q) < 0 && !is_ram)
        WITH_CHIP(cartsave_ram_query(rom, c, CFI_QUERY_D6D7, q));
    for (int i = 0; i < 3; i++) r->magic[i] = q[i];
    r->probe_word[0] = c->known[4];
    r->probe_word[1] = q[CFI_WORDS];
    r->probe_word[2] = q[CFI_WORDS + 1];
    if (cfi_magic(q) < 0) {
        r->probe = is_ram || q[CFI_WORDS + 1] != c->known[4] ? CS_PROBE_RAM : CS_PROBE_SILENT;
        return false;
    }
    r->probe = CS_PROBE_TABLE;

    CfiInfo info;
    if (!cfi_parse(q, &info)) {
        r->why = CS_WHY_TABLE;
        return false;
    }
    r->wiring = info.wiring;
    r->command_set = info.command_set;
    r->device_bytes = info.device_bytes;
    r->regions = info.regions;
    r->region_blocks = (uint16_t)info.region[0].blocks;
    r->region_bytes = info.region[0].bytes;
    if (info.command_set != CFI_SET_AMD) {
        r->why = CS_WHY_COMMANDS;
        return false;
    }
    uint32_t block, half;
    /* Before the cartridge's address space ends, and clear of the program
     * (the link has already refused one that reaches the journal). */
    if ((uint32_t)(uintptr_t)__ramcode_lma + (uint32_t)(__ramcode_end - __ramcode_start) >
            0x08000000u + CARTSAVE_FIRST ||
        !cfi_place(&info, CARTSAVE_FIRST, CARTSAVE_SLOT, CARTSAVE_LEAST, 0x02000000u,
                   &block, &half)) {
        r->why = CS_WHY_PLACE;
        return false;
    }
    if (another_rom_follows(CARTSAVE_FIRST + CARTSAVE_SLOT + half)) {
        r->why = CS_WHY_TAKEN;
        return false;
    }
    r->block_bytes = block;
    r->half_bytes = half;

    c->unlock1 = cfi_wire(info.wiring, 0xAA);
    c->unlock2 = cfi_wire(info.wiring, 0x55);
    c->id = cfi_wire(info.wiring, 0x90);
    c->erase_setup = cfi_wire(info.wiring, 0x80);
    c->erase = cfi_wire(info.wiring, 0x30);
    c->program = cfi_wire(info.wiring, 0xA0);
    c->reset = cfi_wire(info.wiring, 0xF0);
    /* A look at the chip is 1.7 microseconds from here (measured). A word
     * takes some sixty to program and a sector half a second to erase, a
     * few seconds at the worst the datasheets admit to; these give a word
     * twenty-eight milliseconds and a sector twelve seconds. */
    c->program_patience = 0x4000;
    c->erase_patience = 0x700000;

    uint16_t id[2];
    WITH_CHIP(cartsave_ram_ident(rom, c, id));
    r->id[0] = id[0];
    r->id[1] = id[1];
    if (id[0] == c->known[0] && id[1] == rom[1]) {
        r->why = CS_WHY_NO_ID;
        return false;
    }
    r->why = CS_WHY_USED;
    return true;
}

void cartsave_init(void) {
    memset(&g_cs, 0, sizeof g_cs);
    g_cs.page_asked = (uint16_t)(~REG_KEYINPUT & KEY_MASK) == PAGE_KEYS;
    const uint8_t *from = __ramcode_lma;
    for (uint8_t *to = __ramcode_start; to < __ramcode_end;) *to++ = *from++;
    if (!chip_found()) return;

    CartSaveReport *r = &g_cs.report;
    NvDevice dev = {
        .mem = { ROM_BYTES + CARTSAVE_FIRST, ROM_BYTES + CARTSAVE_FIRST + CARTSAVE_SLOT },
        .size = r->half_bytes,
        .program = dev_program,
        .erase = dev_erase,
        .blank = dev_blank,
        .ctx = 0,
    };
    nvlog_mount(&g_cs.log, &dev, CARTSAVE_IMAGE);

    /* THE SAVE RAM IS READ AND ADDED UP only if there is something to hold
     * it to — a mark — or nothing else to go by. With a journal and no
     * mark, which is a board with no battery at every power-on, what the
     * RAM holds is of no interest, and reading it all is a twentieth of a
     * second saved. */
    uint32_t hash = 0, marked = 0;
    bool has_mark = mark_read(&marked), kept = false;
    if (has_mark || !g_cs.log.have_commit) {
        for (unsigned i = 0; i < CARTSAVE_IMAGE; i++) g_image[i] = MEM_SRAM[i];
        hash = nv_hash(g_image, CARTSAVE_IMAGE);
        kept = has_mark && hash == marked;
    }
    r->ram_kept = kept;
    /* From here the save is read from the image (sram_read). */
    g_cs.flash = true;
    r->flash = true;

    bool level = false;
    if (g_cs.log.have_commit) {
        if (kept && hash == g_cs.log.hash) {
            r->boot = CS_BOOT_LEVEL;
            level = true;
        } else if (kept) {
            /* THE RAM ADDS UP AND THE JOURNAL IS BEHIND IT: a save that
             * was waiting for its moment when the power went (a match
             * over the cable), on a board whose RAM outlived it. */
            r->boot = CS_BOOT_RAM_NEWER;
        } else {
            r->boot = CS_BOOT_FROM_FLASH;
            nvlog_replay(&g_cs.log, g_image);
            hash = nv_hash(g_image, CARTSAVE_IMAGE);
            ram_fill();
            level = true;
        }
    } else if (kept || leader_saved_before() || lifetime_saved_before()) {
        /* NO JOURNAL YET AND A SAVE IN THE RAM: a board with a battery
         * that an older build has been saving to. It is where the journal
         * starts from. */
        r->boot = CS_BOOT_FIRST_KEPT;
    } else {
        /* ...and nothing there that this game wrote: what RAM holds at
         * power-on with no battery. An empty save, not thirty-two
         * kilobytes of noise carried in the journal for good. */
        r->boot = CS_BOOT_FIRST_EMPTY;
        memset(g_image, NV_FILL, CARTSAVE_IMAGE);
        hash = nv_hash(g_image, CARTSAVE_IMAGE);
        ram_fill();
    }
    g_cs.hash = hash;
    mark_write();

    /* THE SLOW WORK IS FOR THE FIRST FRAME, with nothing on the screen yet
     * and nothing playing: a journal that is not level with the image, one
     * that takes no more records, and one with less than a quarter of its
     * sector left are written afresh in the other sector — an erase, a
     * second or so — so that a save in the middle of a game is a few
     * words added and never that.
     *
     * NOT HERE, though: everything up to this line only READ the chip, and
     * the page behind L+R+A is shown on that alone (cartsave_page). A chip
     * that does not take to being written has said what it is by then. */
    r->afresh = !level || !g_cs.log.appendable || nvlog_room(&g_cs.log) < r->half_bytes / 4;
    g_cs.hold = g_cs.page_asked;
    report_journal();
}

unsigned char cartsave_read(unsigned offset) {
    if (g_cs.flash && offset < CARTSAVE_IMAGE) return g_image[offset];
    return MEM_SRAM[offset];
}

void cartsave_write(unsigned offset, unsigned char value) {
    if (!g_cs.flash || offset >= CARTSAVE_IMAGE) {
        MEM_SRAM[offset] = value;
        return;
    }
    unsigned char was = g_image[offset];
    if (was == value) return;
    g_image[offset] = value;
    g_cs.hash += nv_hash_step(offset, was, value);
    MEM_SRAM[offset] = value;
    unsigned block = offset >> CS_BLOCK_SHIFT;
    g_cs.changed[block >> 3] |= (uint8_t)(1u << (block & 7));
    g_cs.dirty = true;
}

/* What changed since the last commit, as runs of the image. */
static int changed_runs(NvRange *out) {
    int n = 0;
    for (unsigned b = 0; b < CS_BLOCKS; b++) {
        if (!(g_cs.changed[b >> 3] & (1u << (b & 7)))) continue;
        unsigned lo = b << CS_BLOCK_SHIFT;
        while (b + 1 < CS_BLOCKS && (g_cs.changed[(b + 1) >> 3] & (1u << ((b + 1) & 7)))) b++;
        unsigned hi = (b + 1) << CS_BLOCK_SHIFT;
        if (hi > CARTSAVE_IMAGE) hi = CARTSAVE_IMAGE;
        out[n].lo = (uint16_t)lo;
        out[n].hi = (uint16_t)hi;
        n++;
    }
    return n;
}

/* WHAT WAS WRITTEN GOES TO THE FLASH. Mostly that is a few words after the
 * journal's last and a millisecond or two with the interrupts off. Two
 * things wait for a better moment unless `now` says there will not be one:
 * anything at all while a match is running over the cable, whose serial
 * interrupt is the match; and a journal that has to be written afresh
 * while there is anybody on the other end of the cable or the adapter,
 * since a second of silence is a partner gone. The save RAM has it all
 * meanwhile. */
static void commit(bool now) {
    int n = changed_runs(g_cs.ranges);
    bool adds = nvlog_appends(&g_cs.log, g_cs.ranges, n);
    if (!now && (g_linked || (!adds && (link_connected() || wireless_linked())))) {
        if (!g_cs.waiting) g_cs.report.waits++;
        g_cs.waiting = true;
        return;
    }
    g_cs.waiting = false;
    /* A second of the last note held is worse than a second of none. */
    if (!adds) psg_mute(true);
    bool ok = nvlog_commit(&g_cs.log, g_image, g_cs.ranges, n, g_cs.hash);
    if (!adds) psg_mute(false);
    memset(g_cs.changed, 0, sizeof g_cs.changed);
    g_cs.dirty = false;
    if (!ok) {
        /* Both sectors refused. The chip is left alone until the power is
         * cycled; the save RAM carries on, as it would have alone. */
        g_cs.report.refused++;
        g_cs.dead = true;
    }
    report_journal();
}

/* What is owed to the chip, paid: the journal written afresh if power-on
 * found it wanting, and otherwise whatever has changed. Nothing while the
 * page is up, and nothing ever again once the chip has refused. */
static void to_the_chip(bool now) {
    if (g_cs.hold || g_cs.dead) return;
    if (g_cs.report.afresh) {
        g_cs.report.afresh = false;
        if (!nvlog_sync(&g_cs.log, g_image, g_cs.hash)) {
            g_cs.report.refused++;
            g_cs.dead = true;
        }
        /* The whole image went: nothing of it is still to go. */
        memset(g_cs.changed, 0, sizeof g_cs.changed);
        g_cs.dirty = false;
        report_journal();
        return;
    }
    if (g_cs.dirty) commit(now);
}

void cartsave_tick(void) {
    if (!g_cs.flash) return;
    /* The save RAM first: its mark, level with what was written to it. */
    if (g_cs.hash != g_cs.marked) mark_write();
    to_the_chip(false);
}

void cartsave_flush(void) {
    if (!g_cs.flash) return;
    if (g_cs.hash != g_cs.marked) mark_write();
    to_the_chip(true);
}

const CartSaveReport *cartsave_report(void) {
    return &g_cs.report;
}

/* ----------------------------------------------------------------------- *
 * What the cartridge turned out to be, on the screen
 * ----------------------------------------------------------------------- */
bool cartsave_page_asked(void) {
    return g_cs.page_asked;
}

static char *put_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_hex(char *p, uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        unsigned d = (v >> (4 * i)) & 15u;
        *p++ = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    *p = 0;
    return p;
}

static char *put_dec(char *p, uint32_t v) {
    char digits[10];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) *p++ = digits[--n];
    *p = 0;
    return p;
}

#define PAGE_NAME_TX   1
#define PAGE_VALUE_TX  12

static void page_line(int row, const char *name, const char *value) {
    draw_text(PAGE_NAME_TX, row, name, BANK_LABEL);
    draw_text(PAGE_VALUE_TX, row, value, BANK_NOTE);
}

void cartsave_page(void) {
    static const char *const kProbe[] = { "SILENT", "ROM IS RAM", "ANSWERS" };
    static const char *const kWiring[] = { "STRAIGHT", "D0 D1", "D0 D1 D6 D7" };
    static const char *const kWhy[] = {
        "IN USE", "NO CHIP", "BAD TABLE", "NOT AMD", "NO ROOM", "NO ID",
        "ANOTHER GAME",
    };
    static const char *const kBoot[] = {
        "SAVE RAM ONLY", "RAM AND FLASH", "RAM WAS AHEAD", "FROM FLASH",
        "FIRST: KEPT RAM", "FIRST: EMPTY",
    };
    const CartSaveReport *r = &g_cs.report;
    char v[32], *p;
    int row = 1;
    clear_screen();
    draw_text((SCREEN_TW - 14) / 2, row, "CARTRIDGE SAVE", BANK_NOTE);
    row += 2;
#ifdef TENGEN_VERSION
    page_line(row++, "VERSION", TENGEN_VERSION);
#endif
    page_line(row++, "FLASH CHIP", kProbe[r->probe]);
    p = put_hex(v, r->magic[0], 4);
    *p++ = ' ';
    p = put_hex(p, r->magic[1], 4);
    *p++ = ' ';
    put_hex(p, r->magic[2], 4);
    page_line(row++, "QUERY", v);
    if (r->probe == CS_PROBE_TABLE) {
        page_line(row++, "WIRING", kWiring[r->wiring]);
        p = put_str(v, r->command_set == CFI_SET_AMD ? "AMD "
                       : r->command_set == CFI_SET_INTEL ||
                         r->command_set == CFI_SET_INTEL_EXT ? "INTEL " : "SET ");
        put_hex(p, r->command_set, 4);
        page_line(row++, "COMMANDS", v);
        /* The chip's size, and its sectors as its table lists them: so
         * many of so many kilobytes, and how many such regions if more
         * than the one. */
        p = put_dec(v, r->device_bytes >> 10);
        p = put_str(p, "K ");
        if (r->why != CS_WHY_TABLE) {
            p = put_dec(p, r->region_blocks);
            p = put_str(p, "X");
            p = put_dec(p, r->region_bytes >> 10);
            p = put_str(p, "K");
            if (r->regions > 1) {
                p = put_str(p, " R");
                put_dec(p, r->regions);
            }
        }
        page_line(row++, "CHIP", v);
    }
    if (r->why == CS_WHY_USED || r->why == CS_WHY_NO_ID) {
        p = put_hex(v, r->id[0], 4);
        *p++ = ' ';
        put_hex(p, r->id[1], 4);
        page_line(row++, "CHIP ID", v);
    }
    if (r->probe == CS_PROBE_TABLE && r->why != CS_WHY_USED) {
        /* A chip that answered and is not used: the bytes of its table
         * that decided it, as they were read — its size, how many regions
         * and the first region's four. */
        static const uint8_t at[6] = { 0x27, 0x2C, 0x2D, 0x2E, 0x2F, 0x30 };
        p = v;
        for (int i = 0; i < 6; i++) {
            p = put_hex(p, g_cs.query[at[i] - CFI_FIRST] & 0xFFu, 2);
            *p++ = ' ';
        }
        p[-1] = 0;
        page_line(row++, "TABLE", v);
    }
    page_line(row++, "JOURNAL", r->dead ? "GIVEN UP" : kWhy[r->why]);
    page_line(row++, "STARTED", kBoot[r->boot]);
    if (r->flash) {
        page_line(row++, "SAVE RAM", r->ram_kept ? "KEPT" : "LOST");
        p = put_dec(v, r->gen);
        p = put_str(p, " SAVE ");
        put_dec(p, r->seq);
        page_line(row++, "GENERATION", v);
        p = put_dec(v, r->used);
        p = put_str(p, " OF ");
        p = put_dec(p, r->half_bytes >> 10);
        put_str(p, "K");
        page_line(row++, "USED", v);
        /* What START will have the chip do first: add to the journal as
         * it stands, or write it out afresh (an erase, if the sector it
         * goes to is not blank). */
        page_line(row++, "NEXT", r->afresh ? "WRITE AFRESH" : "ADD TO IT");
        draw_text((SCREEN_TW - 12) / 2, SCREEN_TH - 3, "START: GO ON", BANK_NOTE);
        draw_text((SCREEN_TW - 26) / 2, SCREEN_TH - 2, "SELECT: PLAY WITHOUT FLASH", BANK_NOTE);
    } else {
        draw_text((SCREEN_TW - 11) / 2, SCREEN_TH - 2, "PRESS START", BANK_NOTE);
    }
    /* The keys that asked let go of; START, or SELECT where there is a
     * chip to go on without; and that let go of too: the title takes a
     * START still down for its own. */
    uint16_t keys, leave = (uint16_t)(r->flash ? KEY_START | KEY_SELECT : KEY_START);
    while ((uint16_t)(~REG_KEYINPUT & KEY_MASK)) vsync();
    while (!(keys = (uint16_t)(~REG_KEYINPUT & leave))) vsync();
    while ((uint16_t)(~REG_KEYINPUT & KEY_MASK)) vsync();
    if (r->flash && (keys & KEY_SELECT) && !(keys & KEY_START)) {
        /* WITHOUT THE CHIP, THIS ONCE. Nothing has been written to it and
         * nothing will be until the power is cycled; what is saved goes
         * to the save RAM, which is all a board with a battery needs and
         * all there is to be had from one whose chip is the trouble. */
        g_cs.dead = true;
        g_cs.report.off = true;
        report_journal();
    }
    g_cs.hold = false;
    clear_screen();
}

#endif /* !TENGEN_MULTIBOOT */
