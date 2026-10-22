/* A user program's image. 0x10000 is where this kernel's processes begin —
 * low, well inside the bottom half of an Sv39 address space, and nowhere
 * near where the kernel lives. */
ENTRY(_start)

SECTIONS {
    /* `SIZEOF_HEADERS` matters more than it looks. Without it the first
     * loadable segment starts after the ELF header and the program headers,
     * so the header table is in the *file* but in no segment — and a kernel
     * then has nothing to put in AT_PHDR, because there is no address where
     * those headers exist in memory. A Go runtime reads AT_PHDR to find its
     * own segments, so "0" there is not a cosmetic gap.
     *
     * Leaving room for them at the start of the segment puts them in memory
     * at a known address, which is what every real linker does. */
    . = 0x10000 + SIZEOF_HEADERS;

    .text : { KEEP(*(.text.start)) *(.text .text.*) }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.sdata .sdata.*) }

    .bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
