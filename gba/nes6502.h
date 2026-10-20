/*
 * nes6502.h — just enough 6502 to run the cartridge's sound engine.
 *
 * Not a NES emulator: there is no PPU, no mapper, no interrupt handling and
 * no cycle timing, because the sound engine needs none of them. What it needs
 * is 2KB of RAM, a slice of program ROM, and somewhere for its APU writes to
 * go — and tools/extract_assets.py proves that claim every time the assets
 * are built, by running every track and asserting nothing else is touched.
 *
 * Only the documented NMOS opcodes exist here. An undocumented one stops the
 * CPU rather than doing something plausible; see nes6502_faulted().
 */
#ifndef NES6502_H
#define NES6502_H

#include <stdint.h>
#include <stdbool.h>

/* The half of the address space the program is in, and the view of it. */
#define NES6502_CODE_BASE 0x8000u
#define NES6502_CODE_SIZE 0x8000u

/* Memory the interpreter needs, supplied by the caller so this file stays
 * free of any decision about where things live. */
typedef struct {
    uint8_t *ram;          /* 2KB, mirrored across $0000-$1FFF */
    /* THE PROGRAM'S HALF OF THE ADDRESS SPACE, AS THE CODE SEES IT: the
     * program bytes at their own address and zero everywhere else, $8000 to
     * $FFFF. An instruction fetch is then one load, `code[pc]`, with no range
     * check in front of it — and a fetch is the one thing the interpreter
     * does more of than anything else. The pointer is the view's less
     * $8000, so it is indexed by the 6502's address; PC never leaves the
     * half (nes6502_call stops it as a runaway). Reads of anything that is
     * not RAM or the APU come from here too, or are zero below it, which is
     * the same answer the range checks used to give.
     *
     * IT WAS THE WHOLE 64KB, half of it zeros nothing could fetch, and on a
     * Single-Pak copy the program and its buffers share 256KB: the copy was
     * down to thirty-two bytes spare. Supplied by the caller, 32KB, filled
     * by nes6502_init. */
    const uint8_t *code;
    uint8_t apu[0x18];     /* $4000-$4017, last value written (also read back) */
    /* One bit per APU register, set by every write to it and cleared by
     * whoever reads it (nes_audio_frame). The sweep needs WRITES, not values:
     * a write to a period byte replaces that byte of the period the sweep
     * has already moved, even with the value it held before. */
    uint32_t apu_written;
    /* Called on every write to a sound channel's registers ($4000-$400F),
     * with the channel (0-3) and the RAM as the write found it: the program
     * writing them can be asked, there and then, which of its voices it is
     * writing for. NULL for nobody. */
    void (*channel_written)(const uint8_t *ram, unsigned channel);
} Nes6502Bus;

typedef struct {
    Nes6502Bus bus;
    uint8_t a, x, y, sp, p;
    uint16_t pc;
    bool faulted;          /* hit an undocumented opcode, a BRK, or ran away */
} Nes6502;

/* `code_view` is the caller's NES6502_CODE_SIZE, standing for $8000-$FFFF;
 * `prg` is copied into it at `prg_base` (an address in that half) and the
 * rest is zeroed. */
void nes6502_init(Nes6502 *cpu, uint8_t *ram, uint8_t *code_view,
                   const uint8_t *prg, uint16_t prg_base, uint32_t prg_size);

/* Runs a subroutine to its RTS, the way a JSR from nowhere would, with A/X/Y
 * preloaded. Returns false if it faulted or ran past `max_steps` — a hang
 * guard, not a timing model. */
bool nes6502_call(Nes6502 *cpu, uint16_t addr, uint8_t a, uint32_t max_steps);

static inline bool nes6502_faulted(const Nes6502 *cpu) { return cpu->faulted; }

#endif /* NES6502_H */
