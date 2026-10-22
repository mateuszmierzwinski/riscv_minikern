/* The kernel's image.
 *
 * The load address is **DRAM's base plus 2 MiB**, and DRAM's base is not the
 * same everywhere: QEMU's `virt` starts memory at `0x8000_0000`, and a real
 * **VisionFive 2 starts it at `0x4000_0000`** — measured on the board, whose
 * U-Boot reports `DRAM bank start = 0x40000000, size = 0x200000000` and
 * `kernel_addr_r=0x40200000`. The 2 MiB below the kernel is where the SBI
 * firmware lives.
 *
 * So the address is a parameter, with QEMU's as the default because that is
 * what the tests run on. `--defsym KERNEL_LOAD=0x40200000` builds the board's
 * image. Everything else about the kernel is already address-agnostic: it
 * reads where memory is from the device tree. */
ENTRY(_start)

SECTIONS {
    . = DEFINED(KERNEL_LOAD) ? KERNEL_LOAD : 0x80200000;
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

    /* How much of the image a loader must move, which the image header
     * reports. Everything from the first byte to the end of `.bss`, because
     * `.bss` is zeroed by the kernel itself and must therefore exist. */
    __image_size = __kernel_end - __kernel_start;

    /* The image header's jump is a hand-written encoding of "skip 64 bytes",
     * so the header must *be* 64 bytes. If it ever is not, the jump lands in
     * the middle of the header's data and the board executes a magic number
     * — which is the kind of failure that produces no output at all. */
    ASSERT(_start_after_header - __kernel_start == 64,
           "the RISC-V image header must be exactly 64 bytes")

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
