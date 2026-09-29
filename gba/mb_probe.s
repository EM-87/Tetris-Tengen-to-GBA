@ mb_probe.s — the smallest Single-Pak image there can be: a header, the
@ multiboot entries, and three stores that turn the screen GREEN, then a
@ loop. It exists to tell, on a real console, whether a failing Single-Pak
@ send is the sending (nothing green) or the game being sent (green here,
@ not there). Built twice: as it is, and with BIG defined, padded to the
@ size of the game, which separates "too big" from "wrong". The cartridge
@ sends it on L+SELECT (small) and R+SELECT (big).

    .section .text
    .arm
    .global _start
_start:
    b       probe_start             @ 0x000 (ignored by a multiboot slave)
    .space  156                     @ 0x004 Nintendo logo (gbafix)
    .byte   'M','B','P','R','O','B','E',0,0,0,0,0   @ 0x0A0 title
    .byte   'C','T','T','E'         @ 0x0AC game code
    .byte   '0','0'                 @ 0x0B0 maker
    .byte   0x96                    @ 0x0B2 fixed
    .byte   0x00                    @ 0x0B3 unit
    .byte   0x00                    @ 0x0B4 device
    .space  7                       @ 0x0B5 reserved
    .byte   0x00                    @ 0x0BC version
    .byte   0x00                    @ 0x0BD complement (gbafix)
    .space  2                       @ 0x0BE reserved
    b       probe_start             @ 0x0C0 RAM entry point
    .byte   0x00                    @ 0x0C4 boot mode
    .byte   0x00                    @ 0x0C5 slave number
    .space  26                      @ 0x0C6
    b       probe_start             @ 0x0E0 JOY BUS entry point
    .word   0                       @ keeps the 0x0C0 branch off 28 bytes
probe_start:
    mov     r0, #0x04000000
    mov     r1, #0
    strh    r1, [r0]                @ DISPCNT: mode 0, nothing on
    add     r2, r0, #0x200
    strh    r1, [r2, #8]            @ IME off
    mov     r3, #0x05000000
    mov     r1, #0x3E0
    strh    r1, [r3]                @ backdrop: green
.ifdef BIG
    @ THE BIG PROBE BLINKS WITHOUT INTERRUPTS: the vertical blank's flag in
    @ IF is watched by hand, as the Single-Pak slave now does (IME stays
    @ off: on two SPs no image taken over the cable could take an interrupt,
    @ this probe included when it tried). Green and blue, every 32 frames.
    @ A slave that blinks can do what the game's slave needs.
    mov     r1, #8
    strh    r1, [r0, #4]            @ DISPSTAT: raise IF's vertical blank
    ldr     r1, =0xFFFF
    strh    r1, [r2, #2]            @ IF: nothing pending
    mov     r4, #0                  @ the frame count
2:  ldrh    r1, [r2, #2]
    tst     r1, #1
    beq     2b                      @ wait for the vertical blank's flag
    mov     r1, #1
    strh    r1, [r2, #2]            @ ...acknowledge it
    add     r4, r4, #1
    tst     r4, #32
    moveq   r1, #0x3E0              @ green
    movne   r1, #0x7C00             @ blue
    strh    r1, [r3]
    b       2b
    .pool
.endif
1:  b       1b

    .balign 16
    .space  0x200                   @ over the BIOS's minimum of $100
.ifdef BIG
    .space  184000                  @ ...and, big, as large as the game
.endif
    .balign 16
