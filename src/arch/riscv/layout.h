/* layout — the virtual address map, and the sizes the linker script shares.
 *
 * Sv39 gives 39 bits of virtual address: 256 GiB, split in half. User space
 * is the bottom half and the kernel the top, which is the same shape the ARM
 * kernel used with TTBCR.N — but with one page table format instead of a
 * short-descriptor/long-descriptor choice, and with a sign-extension rule
 * (bits 63:39 must copy bit 38) that makes the kernel's half look like
 * 0xFFFF_FFC0_0000_0000 and up.
 *
 * These constants are also read by start.S, so they are plain #defines with
 * no C types in sight.
 */
#ifndef MINIKERN_ARCH_RISCV_LAYOUT_H
#define MINIKERN_ARCH_RISCV_LAYOUT_H

/* The JH7110 has one S7 monitor core and four U74s, so five hart ids exist
 * and the application cores are **not** 0..3. Stacks are allocated for all
 * of them rather than for the four that will run user code. */
#define MAX_HARTS 8
#define BOOT_STACK_SIZE 16384

#endif
