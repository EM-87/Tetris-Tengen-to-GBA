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
    mov     r0, #0x05000000
    mov     r1, #0x3E0
    strh    r1, [r0]                @ backdrop: green
1:  b       1b

    .balign 16
    .space  0x200                   @ over the BIOS's minimum of $100
.ifdef BIG
    .space  184000                  @ ...and, big, as large as the game
.endif
    .balign 16
