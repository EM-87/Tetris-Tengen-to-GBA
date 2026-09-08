/* gba.ld — cartridge-boot layout for a GBA ROM.
 *
 * ROM is mapped at 0x08000000 and execution starts there, so .init (the
 * header + crt0) must come first. Code and read-only data stay in ROM;
 * writable data is copied to, and .bss lives in, the 32KB of fast internal
 * WRAM at 0x03000000. The 256KB of external WRAM at 0x02000000 is left for
 * anything large enough to be worth the slower bus.
 *
 * .iwram is the exception: code marked with IWRAM_CODE is copied into that
 * internal WRAM at startup and runs from there. Only one thing needs it —
 * the 6502 interpreter that runs the cartridge's own sound engine — but that
 * thing runs a few thousand instructions every frame, and internal WRAM is
 * 32 bits wide with no wait states while the cartridge bus is 16 bits with
 * several. */

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

    .iwram : {
        __iwram_start = .;
        *(.iwram .iwram.*)
        . = ALIGN(4);
        __iwram_end = .;
    } > iwram AT> rom
    __iwram_lma = LOADADDR(.iwram);

    .data : {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > iwram AT> rom
    __data_lma = LOADADDR(.data);

    /* Uninitialised buffers too big for internal WRAM. Nothing is copied
     * here at startup; whatever lands in it is filled in by code. */
    .ewram (NOLOAD) : {
        *(.ewram .ewram.*)
        . = ALIGN(4);
    } > ewram

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > iwram

    /* Stacks grow down from the top of IWRAM. The IRQ stack is 256 bytes,
     * which is all the serial handler in gba/link.c needs on top of the six
     * registers the BIOS pushes before calling it; the rest is the system
     * stack. It stops at $03007F00 deliberately: the BIOS keeps its own
     * variables in the 256 bytes above that, including the interrupt vector
     * the handler is installed through. */
    __sp_irq = ORIGIN(iwram) + LENGTH(iwram) - 0x100;
    __sp_usr = __sp_irq - 0x100;

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
