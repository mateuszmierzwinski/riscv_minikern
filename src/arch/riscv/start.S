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
