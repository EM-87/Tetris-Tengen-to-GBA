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

/* Memory the interpreter needs, supplied by the caller so this file stays
 * free of any decision about where things live. */
typedef struct {
    uint8_t *ram;          /* 2KB, mirrored across $0000-$1FFF */
    const uint8_t *prg;    /* program bytes */
    uint16_t prg_base;     /* the 6502 address prg[0] sits at */
    uint32_t prg_size;
    uint8_t apu[0x18];     /* $4000-$4017, last value written (also read back) */
    /* Set for each APU register written since the last frame, so the caller
     * only has to look at what actually changed. */
    uint32_t apu_dirty;
} Nes6502Bus;

typedef struct {
    Nes6502Bus bus;
    uint8_t a, x, y, sp, p;
    uint16_t pc;
    bool faulted;          /* hit an undocumented opcode, a BRK, or ran away */
} Nes6502;

void nes6502_init(Nes6502 *cpu, uint8_t *ram, const uint8_t *prg,
                   uint16_t prg_base, uint32_t prg_size);

/* Runs a subroutine to its RTS, the way a JSR from nowhere would, with A/X/Y
 * preloaded. Returns false if it faulted or ran past `max_steps` — a hang
 * guard, not a timing model. */
bool nes6502_call(Nes6502 *cpu, uint16_t addr, uint8_t a, uint32_t max_steps);

static inline bool nes6502_faulted(const Nes6502 *cpu) { return cpu->faulted; }

#endif /* NES6502_H */
