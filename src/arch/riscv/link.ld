/* The kernel's image. 0x80200000 is where SBI firmware places a payload on
 * every RISC-V platform worth the name: OpenSBI's fw_jump default, and what
 * U-Boot's booti uses. The firmware itself lives below, at 0x80000000. */
ENTRY(_start)

SECTIONS {
    . = 0x80200000;
    __kernel_start = .;

    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    /* Where the executable part ends. The kernel maps itself read-execute
     * below this and read-write above it, so that its own code cannot be
     * written and its own data cannot be executed. */
    __text_end = .;

    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.sdata .sdata.*) }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
