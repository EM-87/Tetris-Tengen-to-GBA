/* gba.ld — cartridge-boot layout for a GBA ROM.
 *
 * ROM is mapped at 0x08000000 and execution starts there, so .init (the
 * header + crt0) must come first. Code and read-only data stay in ROM;
 * writable data is copied to, and .bss lives in, the 32KB of fast internal
 * WRAM at 0x03000000. The 256KB of external WRAM at 0x02000000 is left for
 * anything large enough to be worth the slower bus. */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
}

SECTIONS
{
    .init : {
        KEEP(*(.init))
    } > rom

    .text : {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } > rom

    .rodata : {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4);
    } > rom

    __data_lma = .;

    .data : AT (__data_lma) {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > iwram

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > iwram

    /* Stacks grow down from the top of IWRAM. The IRQ stack is small because
     * nothing here takes interrupts; the rest is the system stack. */
    __sp_irq = ORIGIN(iwram) + LENGTH(iwram) - 0x100;
    __sp_usr = __sp_irq - 0x100;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
