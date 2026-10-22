/* start.S — where the firmware hands the machine over.
 *
 * OpenSBI (or U-Boot's SBI implementation) enters a payload in **S-mode** at
 * the image's link address, with:
 *
 *     a0 = the hart id this is running on
 *     a1 = the physical address of a flattened device tree
 *
 * Both are passed straight through to the C entry point, because both are
 * facts a kernel cannot work out for itself. **a0 is not necessarily 0**:
 * measured here, QEMU's `virt` picks a boot hart by lottery, and
 * `sifive_u` — which models a monitor core alongside its application cores,
 * exactly as the JH7110 does with its S7 beside four U74s — boots on hart
 * **1**. A kernel that assumes it is hart 0 is wrong on the target board.
 *
 * Compare what the ARM equivalent had to do first: drop from HYP to SVC
 * (and if any core skipped it, its MMU writes landed on the hypervisor's
 * registers and it died on its next instruction fetch), hand the counter
 * and the coprocessors down to PL1, and set up a bring-up page table
 * because the identity map was about to disappear. None of that exists
 * here: the firmware leaves this in the one mode the kernel wants, with
 * translation off and nothing to negotiate.
 */
/* Shares its sizes with the C side, which is why layout.h holds nothing but
 * #defines — a .S file goes through the preprocessor, not a C parser. */
#include "arch/riscv/layout.h"

    .section .text.entry, "ax"
    .globl _start
_start:
    /* ── the RISC-V Linux image header ─────────────────────────────────────
     *
     * Sixty-four bytes that make this image bootable by **the standard
     * loader path** — U-Boot's `booti`, which is how a kernel is started on
     * a real board and from an SD card. Without it the loader refuses the
     * image with "Bad Linux RISC-V Image magic!", and the alternatives all
     * involve entering the kernel without the two arguments the boot
     * protocol passes: the hart id and the device tree.
     *
     * The layout is the one Linux documents (`boot-image-header.rst`), and
     * the fields that matter are:
     *
     *   - `code0`/`code1`: the first two instructions, which must jump over
     *     the header. The entry point *is* the header's address, so the
     *     header has to begin with working code.
     *   - `text_offset`: where the image wants to sit relative to the base
     *     of memory. A loader relocates the image to `ram_base +
     *     text_offset`, so 2 MiB here is what puts the kernel at
     *     `0x4020_0000` on a board whose memory starts at `0x4000_0000` and
     *     at `0x8020_0000` under QEMU — one number that is right on both.
     *   - `image_size`: how much to move, which the link script computes.
     */
    /* `.option norvc` because **`j` assembles to two bytes, not four**, when
     * the compressed extension is on — and it is, this being rv64gc. The
     * header's first field is four bytes wide, so a compressed jump shifts
     * every field after it by two: the first attempt produced a text_offset
     * of 0xf000000000000020 and a magic nowhere near where a loader looks.
     * The same trap caught this project once before, in the code that steps
     * over a faulting instruction. */
    /* The jump is written as its **encoding**, not as a mnemonic.
     *
     * `j` assembles to two bytes on this ISA — the compressed extension is
     * on, this being rv64gc — and the header's first field is four bytes
     * wide, so a compressed jump shifts every field after it by two: the
     * first attempt produced a text offset of 0xf000000000000020 and put the
     * magic two bytes from where a loader looks for it. `.option norvc`
     * around it did not help, so the encoding is spelled out.
     *
     * 0x0400006f is `jal x0, +64`: opcode 0x6f, rd 0, and an immediate of 64
     * in the J-type's scattered bit order (bits 10:1 of the offset sit at
     * 30:21, so 64 >> 1 = 32 lands as 32 << 21). 64 is the header's own
     * size, and the link script asserts that it still is. */
    .word   0x0400006f              /* code0: jal x0, +64 — over the header */
    .word   0                       /* code1 */
    .quad   0x200000                /* text_offset: 2 MiB above DRAM's base */
    .quad   __image_size            /* image_size, from the link script */
    .quad   0                       /* flags: little-endian, no constraints */
    .word   0x00000002              /* version 0.2 of this header */
    .word   0                       /* res1 */
    .quad   0                       /* res2 */
    .ascii  "RISCV\0\0\0"           /* magic (deprecated but still checked) */
    .ascii  "RSC\x05"                /* magic2 */
    .word   0                       /* res3: a PE/COFF offset, unused */

    /* Global so the link script can assert the header's size against it. A
     * local label is invisible to the linker, and the assertion that keeps
     * the hand-written jump honest is worth a symbol. */
    .globl _start_after_header
_start_after_header:
    /* No translation yet and no interrupts: make that explicit rather than
     * inherited, so a firmware that left either on cannot surprise us. */
    csrw    satp, zero
    csrw    sie, zero
    csrci   sstatus, 2          /* SIE = 0 */

    /* This hart's stack. One per hart, indexed by a0, so a secondary that
     * arrives later does not land on the boot hart's stack. */
    la      t0, boot_stacks
    li      t1, BOOT_STACK_SIZE
    addi    t2, a0, 1           /* stacks grow down: hart n uses slot n+1 */
    mul     t1, t1, t2
    add     sp, t0, t1

    /* Zero .bss before any C runs, because C assumes it. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b

2:  call    kmain               /* a0 = hart id, a1 = device tree, as given */

    /* kmain does not return. If it ever does, stop this hart rather than
     * falling into whatever follows in memory. */
3:  wfi
    j       3b

    .section .bss
    .align 12
    .globl boot_stacks
boot_stacks:
    .space BOOT_STACK_SIZE * MAX_HARTS

/* _secondary_start — where a hart started by the firmware arrives.
 *
 * `sbi_hart_start` enters it **in S-mode with translation off**, at a
 * physical address, with the hart id in `a0` and whatever was passed as
 * `opaque` in `a1`. So this is `_start` without the parts that are the boot
 * hart's alone: no `.bss` to zero (it is already zeroed, and zeroing it
 * again would wipe what the boot hart has been doing), and no device tree to
 * read.
 *
 * That it is a physical address is only convenient because the kernel is
 * still identity-mapped. When the kernel moves to the top half (P0.4), this
 * entry has to stay reachable physically — which is what a trampoline page
 * is for, and it is the one piece of that move that cannot be deferred.
 */
    .section .text
    .globl _secondary_start
_secondary_start:
    csrw    sie, zero
    csrci   sstatus, 2

    la      t0, boot_stacks
    li      t1, BOOT_STACK_SIZE
    addi    t2, a0, 1
    mul     t1, t1, t2
    add     sp, t0, t1

    call    smp_secondary_main  /* a0 = hart id, a1 = the satp to adopt */

1:  wfi
    j       1b
