/* mb.ld — the Single-Pak slave's layout: the whole program in the 256KB of
 * external WRAM at 0x02000000, where the BIOS of a console with no cartridge
 * puts what the cable sends it (GBATEK, "BIOS Multi Boot"), and runs it
 * from.
 *
 * The same program as gba.ld's in every other way: .iwram and .data are
 * copied to internal WRAM at startup from where they were loaded, .bss and
 * the stacks live there, and .ewram — the 6502's 64KB address space — is
 * laid after the image, so image and buffers together have to fit in the
 * 256KB. The link says so if they do not, and so does the image's own
 * limit: the BIOS takes at most $3FF40 bytes. */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
}

SECTIONS
{
    .init : {
        KEEP(*(.init))
    } > ewram

    .text : {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } > ewram

    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4);
    } > ewram

    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT> ewram
    __iwram_lma = LOADADDR(.iwram);

    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > iwram AT> ewram
    __data_lma = LOADADDR(.data);
    /* Where the image the BIOS receives ends: everything above is loaded,
     * everything below is laid out at run time. */
    __image_end = __data_lma + SIZEOF(.data);

    .ewram ALIGN(__image_end, 16) (NOLOAD) : {
        *(.ewram .ewram.*)
        . = ALIGN(4);
        __ewram_end = .;
    } > ewram

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > iwram

    __sp_irq = ORIGIN(iwram) + LENGTH(iwram) - 0x100;
    __sp_usr = __sp_irq - 0x100;
    ASSERT(__sp_usr - __bss_end >= 0x1000,
           "IWRAM: less than 4KB left for the stack above .bss")
    ASSERT(__image_end - ORIGIN(ewram) <= 0x3FF40,
           "Single-Pak image larger than the BIOS will receive")
    ASSERT(__ewram_end <= ORIGIN(ewram) + LENGTH(ewram),
           "Single-Pak: image and EWRAM buffers do not fit in 256KB")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
