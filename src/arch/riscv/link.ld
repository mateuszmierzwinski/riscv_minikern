/* The kernel's image. 0x80200000 is where SBI firmware places a payload on
 * every RISC-V platform worth the name: OpenSBI's fw_jump default, and what
 * U-Boot's booti uses. The firmware itself lives below, at 0x80000000. */
ENTRY(_start)

SECTIONS {
    . = 0x80200000;

    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    . = ALIGN(4096);
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
