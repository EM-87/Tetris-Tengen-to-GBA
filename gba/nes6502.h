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
    /* THE WHOLE 64KB ADDRESS SPACE, AS THE CODE SEES IT: the program bytes
     * at their own address and zero everywhere else. An instruction fetch is
     * then one load, `code[pc]`, with no range check in front of it — and a
     * fetch is the one thing the interpreter does more of than anything
     * else. Reads of anything that is not RAM or the APU come from here too,
     * which is the same answer the range checks used to give: the bytes
     * inside the slice, and zero outside it. Supplied by the caller, 64KB,
     * filled by nes6502_init. */
    const uint8_t *code;
    uint8_t apu[0x18];     /* $4000-$4017, last value written (also read back) */
} Nes6502Bus;

typedef struct {
    Nes6502Bus bus;
    uint8_t a, x, y, sp, p;
    uint16_t pc;
    bool faulted;          /* hit an undocumented opcode, a BRK, or ran away */
} Nes6502;

/* `code_view` is the caller's 64KB; `prg` is copied into it at `prg_base`
 * and the rest is zeroed. */
void nes6502_init(Nes6502 *cpu, uint8_t *ram, uint8_t *code_view,
                   const uint8_t *prg, uint16_t prg_base, uint32_t prg_size);

/* Runs a subroutine to its RTS, the way a JSR from nowhere would, with A/X/Y
 * preloaded. Returns false if it faulted or ran past `max_steps` — a hang
 * guard, not a timing model. */
bool nes6502_call(Nes6502 *cpu, uint16_t addr, uint8_t a, uint32_t max_steps);

static inline bool nes6502_faulted(const Nes6502 *cpu) { return cpu->faulted; }

#endif /* NES6502_H */
