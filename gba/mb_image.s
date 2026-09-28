@ mb_image.s — the Single-Pak slave's image, carried inside the cartridge's
@ ROM so that the cartridge can send it (link_multiboot_send). It is the
@ same program built by `make` for external WRAM (gba/mb.ld,
@ -DTENGEN_MULTIBOOT), padded to a multiple of 16 bytes as the BIOS wants.

    .section .rodata.slave_image, "a"
    .balign 4
    .global kSlaveImage
    .global kSlaveImageEnd
kSlaveImage:
    .incbin "build/tengen_mb.mb"
    .balign 16
kSlaveImageEnd:
