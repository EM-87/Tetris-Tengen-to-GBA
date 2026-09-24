/*
 * gba_hw.h — the handful of GBA hardware definitions this port actually uses.
 *
 * Deliberately NOT libgba/tonc: those live in devkitPro, and depending on
 * them would mean the ROM can only be built on a machine with devkitARM
 * installed. Everything here is public hardware documentation and fits in
 * one screen, so the port builds with a plain arm-none-eabi-gcc toolchain.
 * If devkitARM is available it still works — this just doesn't require it.
 */
#ifndef GBA_HW_H
#define GBA_HW_H

#include <stdint.h>

typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

#define REG_DISPCNT   (*(vu16 *)0x04000000)
#define REG_DISPSTAT  (*(vu16 *)0x04000004)
#define REG_VCOUNT    (*(vu16 *)0x04000006)
#define REG_BG0CNT    (*(vu16 *)0x04000008)
#define REG_BG1CNT    (*(vu16 *)0x0400000A)
#define REG_BG2CNT    (*(vu16 *)0x0400000C)
#define REG_BG3CNT    (*(vu16 *)0x0400000E)
#define REG_BG1HOFS   (*(vu16 *)0x04000014)
#define REG_BG1VOFS   (*(vu16 *)0x04000016)
#define REG_BG2HOFS   (*(vu16 *)0x04000018)
#define REG_BG2VOFS   (*(vu16 *)0x0400001A)
#define REG_BG3HOFS   (*(vu16 *)0x0400001C)
#define REG_BG3VOFS   (*(vu16 *)0x0400001E)
#define REG_KEYINPUT  (*(vu16 *)0x04000130)

/* INTERRUPTS. Two sources and one handler (irq_handler in video.c): the
 * vertical blank, which is what vsync() sleeps on, and the serial port,
 * which is what keeps a linked match in lockstep (see link.h). */
#define REG_IE        (*(vu16 *)0x04000200)
#define REG_IF        (*(vu16 *)0x04000202)
#define REG_IME       (*(vu16 *)0x04000208)
#define IRQ_VBLANK    0x0001
#define IRQ_SERIAL    0x0080
#define DSTAT_VBL_IRQ 0x0008   /* DISPSTAT: raise IRQ_VBLANK at line 160 */

/* The BIOS jumps through this pointer on every interrupt, and its IntrWait
 * family sleeps until the halfword below it has the flags it wants — which
 * the handler has to OR in itself. Both addresses are the BIOS's, not this
 * program's; they sit just above the IRQ stack the linker script sets up,
 * which is why that stack stops at $03007F00. */
#define BIOS_IRQ_VECTOR (*(void (**)(void))0x03007FFC)
#define BIOS_IF_MIRROR  (*(vu16 *)0x03007FF8)

/* ARM, and in internal WRAM: code that runs under an interrupt, or often
 * enough that fetching it over the 16-bit cartridge bus would matter. */
#define IWRAM_CODE __attribute__((section(".iwram"), long_call, target("arm")))

/* DISPCNT */
#define DCNT_MODE0    0x0000
#define DCNT_BG0      0x0100
#define DCNT_BG1      0x0200
#define DCNT_BG2      0x0400
#define DCNT_BG3      0x0800
#define DCNT_OBJ      0x1000
#define DCNT_OBJ_1D   0x0040  /* sprite tiles laid out linearly, not in a grid */
#define DCNT_WIN0     0x2000

/* Window 0: a rectangle with its own list of layers, and everything outside
 * it (and outside window 1) gets WINOUT's. Bits 0-3 the backgrounds, 4 the
 * sprites, 5 the colour effects. */
#define REG_WIN0H     (*(vu16 *)0x04000040)   /* left << 8 | right (exclusive) */
#define REG_WIN0V     (*(vu16 *)0x04000044)   /* top << 8 | bottom (exclusive) */
#define REG_WININ     (*(vu16 *)0x04000048)
#define REG_WINOUT    (*(vu16 *)0x0400004A)

/* BGxCNT */
#define BG_4BPP       0x0000
#define BG_SIZE_32x32 0x0000
/* 0 is nearest the viewer; on a tie the lower-numbered background wins, which
 * is why anything meant to sit ON another layer needs this set explicitly. */
#define BG_PRIORITY(n)    ((uint16_t)(n))
#define BG_CHARBLOCK(n)   ((uint16_t)((n) << 2))
#define BG_SCREENBLOCK(n) ((uint16_t)((n) << 8))

#define MEM_PALETTE   ((vu16 *)0x05000000)
#define MEM_PALETTE_OBJ ((vu16 *)0x05000200)
#define MEM_OAM       ((vu16 *)0x07000000)
/* Sprite tiles live in their own VRAM window in tile modes. */
#define MEM_OBJ_TILES ((vu16 *)0x06010000)
#define MEM_VRAM      ((vu16 *)0x06000000)
/* Charblocks are 16KB, screenblocks 2KB, both inside the same VRAM window —
 * which is why a screenblock index has to be chosen high enough not to
 * collide with the tile data below it. */
#define MEM_CHARBLOCK(n)   ((vu16 *)(0x06000000 + (n) * 0x4000))
#define MEM_SCREENBLOCK(n) ((vu16 *)(0x06000000 + (n) * 0x0800))

/* KEYINPUT is ACTIVE LOW: a bit reads 0 while its button is held. */
#define KEY_A      0x0001
#define KEY_B      0x0002
#define KEY_SELECT 0x0004
#define KEY_START  0x0008
#define KEY_RIGHT  0x0010
#define KEY_LEFT   0x0020
#define KEY_UP     0x0040
#define KEY_DOWN   0x0080
#define KEY_R      0x0100
#define KEY_L      0x0200
#define KEY_MASK   0x03FF

/* One OAM entry: three attribute words plus a padding word the affine
 * transform data lives in. attr0 carries Y and shape, attr1 X, flips and
 * size, attr2 the tile index and palette bank. */
#define OBJ_ATTR0_Y(y)      ((y) & 0xFF)
#define OBJ_ATTR0_HIDDEN    0x0200
#define OBJ_ATTR1_X(x)      ((x) & 0x1FF)
#define OBJ_ATTR1_HFLIP     0x1000
#define OBJ_ATTR2_PAL(n)    ((uint16_t)((n) << 12))
/* 0 is nearest the viewer. A sprite is drawn behind every background whose
 * own priority is LOWER than its number, and only where that background has
 * an opaque pixel — the GBA's version of the NES's "behind background". */
#define OBJ_ATTR2_PRIO(n)   ((uint16_t)((n) << 10))

/* BGR555. Each component is 0-31; note the byte order is the reverse of the
 * RGB most tooling hands you. */
static inline uint16_t rgb15(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)((b << 10) | (g << 5) | r);
}

/* ----------------------------------------------------------------------- *
 * The save memory
 *
 * A GBA cartridge can carry battery-backed SRAM at $0E000000, and this one
 * does: it is how the port keeps the high-score table the NES cartridge could
 * only hold until the power went off (see leader_load in gba/main.c).
 *
 * TWO RULES, and both of them bite silently. The bus to it is EIGHT BITS
 * WIDE, so every access has to be a byte access — a halfword or word read
 * comes back with the same byte repeated and a write of one corrupts its
 * neighbours — and the code doing the accessing has to be compiled for it,
 * which is why the accessors below take and return `unsigned char` through a
 * volatile pointer rather than being memcpy'd. And an emulator or flash cart
 * decides a game HAS save memory by finding one of a handful of magic strings
 * in the ROM image, which is what kSaveSignature is for; without it the reads
 * come back as open bus and the table quietly never survives.
 * ----------------------------------------------------------------------- */
#define MEM_SRAM ((volatile unsigned char *)0x0E000000)
#define SRAM_SIZE 0x8000

static inline unsigned char sram_read(unsigned offset) {
    return MEM_SRAM[offset];
}
static inline void sram_write(unsigned offset, unsigned char value) {
    MEM_SRAM[offset] = value;
}

#endif /* GBA_HW_H */
