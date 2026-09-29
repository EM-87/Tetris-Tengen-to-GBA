@ crt0.s — minimal GBA startup: ROM header, stacks, .data copy, .bss clear.
@
@ Written by hand rather than taken from devkitARM so the ROM builds with a
@ stock arm-none-eabi toolchain (see gba_hw.h for the same reasoning).
@
@ NOTE ON THE HEADER: bytes 4..159 are the Nintendo logo, which the real BIOS
@ compares against its own copy before booting. It is left zeroed HERE, in
@ the source, and filled in by the build: `make gba` runs devkitPro's
@ `gbafix` (vendored in tools/gbafix/) over the linked image, which writes the
@ logo and the header checksum at byte 189, and tools/check_header.py
@ verifies both. The .gba it produces boots on real hardware; the .elf does
@ not, and nothing should be flashed from it.

    .section .init, "ax"
    .global _start
    .arm

_start:
    b       rom_header_end

    .space  156         @ 0x004 Nintendo logo (see note above)
    .byte   'T','E','N','G','E','N','T','E','T','R','I','S'  @ 0x0A0 title (12)
    .byte   'C','T','T','E'                                  @ 0x0AC game code
    .byte   '0','0'                                          @ 0x0B0 maker code
    .byte   0x96        @ 0x0B2 fixed value, the BIOS checks this one
    .byte   0x00        @ 0x0B3 main unit code
    .byte   0x00        @ 0x0B4 device type
    .space  7           @ 0x0B5 reserved
    .byte   0x00        @ 0x0BC software version
    .byte   0x00        @ 0x0BD complement check (gbafix computes this)
    .space  2           @ 0x0BE reserved

.ifdef MULTIBOOT
    @ THE SINGLE-PAK IMAGE'S OWN ENTRIES (GBATEK, "Multiboot Header"): the
    @ BIOS of the console that received it over the cable jumps to 0x0C0,
    @ and writes the boot mode and this console's slave number just after.
    @ 0x0E0 is the JOY BUS entry, which nothing here uses but which points
    @ at the same start so that nothing lands in the middle of the header.
    b       rom_header_end  @ 0x0C0 RAM entry point
    .byte   0x00            @ 0x0C4 boot mode (the BIOS writes it)
    .byte   0x00            @ 0x0C5 slave number (the BIOS writes it)
    .space  26              @ 0x0C6 unused
    b       rom_header_end  @ 0x0E0 JOY BUS entry point
    @ One word of nothing before the code, so the branch at 0x0C0 is not 28
    @ bytes long: mGBA takes an image whose 0x0C0 branch is exactly that for
    @ one of an old toolchain's that only looks like multiboot, and would
    @ run this one as a cartridge. The BIOS does not mind either way.
    .word   0
.endif

rom_header_end:
    @ INTERRUPTS OFF BEFORE ANYTHING ELSE. From a cartridge they are off
    @ already; from the cable they may not be: the BIOS that received the
    @ Single-Pak image did it on the serial interrupt, and the cartridge
    @ goes back to its lobby and starts talking the moment the image is
    @ across. An interrupt taken while the lines below are still laying out
    @ internal WRAM goes through a vector that is not this program's. IME
    @ off, IE emptied and IF acknowledged; irq_init switches on only what
    @ this program uses.
    mov     r0, #0x04000000
    add     r0, r0, #0x200          @ IE
    mov     r1, #0
    strh    r1, [r0, #8]            @ IME = 0
    strh    r1, [r0]                @ IE = 0
    ldr     r1, =0xFFFF
    strh    r1, [r0, #2]            @ IF: acknowledge whatever is pending

    @ ...AND EVERY DMA CHANNEL AND TIMER STOPPED. From a cartridge they are
    @ stopped already; after a Single-Pak transfer the BIOS that animated
    @ its logo may have left one running, and a DMA that fires on each
    @ vertical blank into internal WRAM would rewrite the code about to be
    @ copied there — the interrupt handler first. On two SPs the slave died
    @ on its first interrupt (INFERRED to be this).
    mov     r0, #0x04000000
    mov     r1, #0
    add     r2, r0, #0xB8           @ DMA0CNT
    str     r1, [r2]
    str     r1, [r2, #12]           @ DMA1CNT
    str     r1, [r2, #24]           @ DMA2CNT
    str     r1, [r2, #36]           @ DMA3CNT
    add     r2, r0, #0x100          @ TM0CNT
    str     r1, [r2]
    str     r1, [r2, #4]            @ TM1CNT
    str     r1, [r2, #8]            @ TM2CNT
    str     r1, [r2, #12]           @ TM3CNT

.ifdef MULTIBOOT
    @ ...and a sign that this program has started, for whoever is watching
    @ a real console: the BIOS's logo goes and the screen shows nothing but
    @ the backdrop, RED, which main then turns to other colours as it gets
    @ through its start (MB_STAGE, main.c) until it draws the lobby. The
    @ colour a slave stops on is the step it stopped in.
    mov     r0, #0x04000000
    mov     r1, #0
    strh    r1, [r0]                @ DISPCNT: mode 0, nothing on
    mov     r0, #0x05000000
    @ RED if the BIOS started this in system mode, as GBATEK says it does;
    @ BLUE if in any other: then the stacks below could not be set.
    mrs     r4, cpsr
    and     r4, r4, #0x1F
    cmp     r4, #0x1F
    moveq   r1, #0x1F               @ backdrop: red
    movne   r1, #0x7C00             @ ...or blue
    strh    r1, [r0]
    @ ...held for two seconds (120 frames counted on VCOUNT), so it can be
    @ seen before the next step replaces it or goes wrong.
    ldr     r2, =0x04000006
    mov     r3, #120
8:  ldrh    r1, [r2]
    cmp     r1, #160
    bcs     8b                      @ out of any blank under way
9:  ldrh    r1, [r2]
    cmp     r1, #160
    bcc     9b                      @ ...to the start of the next
    subs    r3, r3, #1
    bne     8b
.endif

    @ THE SUPERVISOR'S STACK, which every BIOS call (SWI) runs on. A
    @ cartridge boot leaves it at $03007FE0, where the BIOS keeps it; after
    @ a Single-Pak transfer nothing says it is still there, and a BIOS call
    @ pushing into this program's own internal WRAM would be the first
    @ vsync() overwriting code. So it is put where the BIOS puts it, as
    @ devkitARM's startup does.
    mov     r0, #0x13               @ supervisor mode
    msr     cpsr_c, r0
    ldr     sp, =0x03007FE0

    @ IRQ stack. Nothing here enables interrupts, but leaving the IRQ stack
    @ pointer unset is the kind of thing that only bites once something does.
    mov     r0, #0x12               @ IRQ mode, IRQ+FIQ disabled
    msr     cpsr_c, r0
    ldr     sp, =__sp_irq

    @ System mode is where main() runs.
    mov     r0, #0x1f
    msr     cpsr_c, r0
    ldr     sp, =__sp_usr

    @ Cartridge wait states: 3/1 for the ROM bus plus the prefetch buffer.
    @ The default (4/2, no prefetch) is the conservative power-on setting;
    @ every commercial cartridge sets this, and code fetched from ROM runs
    @ noticeably faster for it.
    ldr     r0, =0x04000204
    ldr     r1, =0x4317
    strh    r1, [r0]

    @ Copy .iwram (code that must run from internal WRAM) into place.
    ldr     r0, =__iwram_lma
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
6:  cmp     r1, r2
    bcs     7f
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       6b
7:

    @ Copy .data from its ROM load address into RAM.
    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
1:  cmp     r1, r2
    bcs     2f
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       1b
2:

    @ Zero .bss.
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
3:  cmp     r0, r1
    bcs     4f
    str     r2, [r0], #4
    b       3b
4:

    ldr     r0, =main
    bl      call_via_r0
    @ main() is not expected to return; if it does, stop here rather than
    @ running off into whatever follows in ROM.
5:  b       5b

call_via_r0:
    bx      r0

    .pool
