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

    /* The two arguments the boot protocol gave us, kept across the switch
     * below: everything until `kmain` is assembly, so saved registers are
     * free to use. */
    mv      s0, a0
    mv      s1, a1

    /* Zero .bss before anything is written to it. `la` is PC-relative, and
     * translation is off, so these are physical addresses — the same
     * bytes the linked (high) addresses will name once it is on. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b

    /* Where the image physically is. After the jump below `__kernel_start`
     * reads as the linked address, so this is the only moment the load
     * address can be read rather than computed. */
2:  la      t0, _start
    la      t1, kernel_load_phys
    sd      t0, 0(t1)

    /* ── the boot table: translation on before any C runs ─────────────────
     *
     * **The kernel is linked in its own half and loaded low**, so it cannot
     * run C at the load address: an absolute address the linker wrote — a
     * function-pointer table, a switch's jump table, a static port struct —
     * names the high alias, while a PC-relative one names wherever the code
     * is executing. Linked low and executed high (the previous design), the
     * two disagreed, and every object had two addresses: the board showed a
     * child's exit waking `0x403371c8` while its parent slept on
     * `0xffffffc0003371c8`. So nothing in C ever runs low.
     *
     * One root table, gigapages only: slots 0..255 identity (RWX, so the
     * early boot reaches the device tree, the firmware's console and the
     * table arena exactly as it did with translation off), and slot 256 —
     * `KERNEL_VIRT_BASE` — the gigabyte the image was loaded into. The image
     * sits `KERNEL_VIRT_OFFSET` into that gigabyte on every board, because
     * the loader puts it at DRAM's base plus 2 MiB. `mmu_init` replaces this
     * table with the real one; nothing frees it, and it is 4 KiB. */
    la      t2, boot_root_table
    li      t3, 0
    li      t4, 256
3:  slli    t5, t3, 28          /* PPN of gigabyte i, at bit 10: (i << 30) >> 12 << 10 */
    ori     t5, t5, 0xcf        /* V R W X A D */
    slli    t6, t3, 3
    add     t6, t6, t2
    sd      t5, 0(t6)
    addi    t3, t3, 1
    bltu    t3, t4, 3b
    srli    t5, t0, 30          /* the gigabyte the image is in */
    slli    t5, t5, 28
    ori     t5, t5, 0xcf
    addi    t6, t2, 2047        /* slot 256 is byte 2048, one past the immediate's reach */
    sd      t5, 1(t6)
    srli    t5, t2, 12
    li      t6, 8               /* Sv39 */
    slli    t6, t6, 60
    or      t5, t5, t6
    csrw    satp, t5
    sfence.vma zero, zero

    /* Still executing at the load address, which the identity slots map.
     * The literal holds the linked address, so this is the jump. */
    ld      t0, .Lstart_high
    jr      t0

    .balign 8
.Lstart_high:
    .dword  _start_high

_start_high:
    /* This hart's stack, now at its linked address. One per hart, indexed
     * by the hart id, so a secondary that arrives later does not land on
     * the boot hart's stack. */
    la      t0, boot_stacks
    li      t1, BOOT_STACK_SIZE
    addi    t2, s0, 1           /* stacks grow down: hart n uses slot n+1 */
    mul     t1, t1, t2
    add     sp, t0, t1

    mv      a0, s0
    mv      a1, s1
    call    kmain               /* a0 = hart id, a1 = device tree, as given */

    /* kmain does not return. If it ever does, stop this hart rather than
     * falling into whatever follows in memory. */
4:  wfi
    j       4b

    .section .bss
    .align 12
    .globl boot_stacks
boot_stacks:
    .space BOOT_STACK_SIZE * MAX_HARTS

    .align 12
boot_root_table:
    .space 4096

    .align 3
    .globl kernel_load_phys
kernel_load_phys:
    .space 8

/* _secondary_start — where a hart started by the firmware arrives.
 *
 * `sbi_hart_start` enters it **in S-mode with translation off**, at a
 * physical address, with the hart id in `a0` and whatever was passed as
 * `opaque` in `a1`. So this is `_start` without the parts that are the boot
 * hart's alone: no `.bss` to zero (it is already zeroed, and zeroing it
 * again would wipe what the boot hart has been doing), and no device tree to
 * read.
 *
 * The firmware needs the *physical* address of this entry
 * (`mmu_virt_to_phys` of the symbol), and the code here runs there only
 * until the kernel's table is on and the jump has been taken.
 */
    .section .text
    .globl _secondary_start
_secondary_start:
    csrw    sie, zero
    csrci   sstatus, 2

    /* The kernel's own table, which `a1` carries. It maps the image at its
     * load address as well as its linked one, so the next instruction is
     * still fetchable; the literal then takes this hart to the linked
     * address, before any C — the same rule, and the same reason, as the
     * boot hart's. */
    csrw    satp, a1
    sfence.vma zero, zero
    ld      t0, .Lsecondary_high
    jr      t0

    .balign 8
.Lsecondary_high:
    .dword  _secondary_high

_secondary_high:
    la      t0, boot_stacks
    li      t1, BOOT_STACK_SIZE
    addi    t2, a0, 1
    mul     t1, t1, t2
    add     sp, t0, t1

    call    smp_secondary_main  /* a0 = hart id */

1:  wfi
    j       1b

