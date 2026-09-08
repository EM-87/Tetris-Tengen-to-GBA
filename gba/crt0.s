@ crt0.s — minimal GBA startup: ROM header, stacks, .data copy, .bss clear.
@
@ Written by hand rather than taken from devkitARM so the ROM builds with a
@ stock arm-none-eabi toolchain (see gba_hw.h for the same reasoning).
@
@ NOTE ON THE HEADER: bytes 4..159 are the Nintendo logo, which the real BIOS
@ compares against its own copy before booting. It is left zeroed here — this
@ ROM therefore runs in emulators (mGBA, VBA-M, no$gba) but will NOT boot on
@ real hardware or a BIOS-strict emulator. Filling it in, and fixing up the
@ header checksum at byte 189, is what devkitPro's `gbafix` does; see
@ ../README.md. That step needs Nintendo's logo data, which is not
@ reproduced here.

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

rom_header_end:
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
