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

/* ── the kernel's half, by gigabyte-sized slots ───────────────────────────
 *
 * Sv39's level-2 index is bits 38:30, so each entry covers a gigabyte and
 * the kernel's half is entries 256..511. Laying the regions out on those
 * boundaries means each one is a whole number of top-level entries, which
 * is what lets a user page table share the kernel's half by copying a few
 * root entries rather than walking anything.
 *
 *   256  KERNEL_VIRT_BASE   the image, linked here, 1 GiB
 *   288  DIRECT_MAP_BASE    all of RAM, read-write, never executable
 *   320  DEVICE_MAP_BASE    MMIO, as drivers ask for it
 *
 * **Why a direct map at all.** The kernel has to be able to reach an
 * arbitrary physical page — a page table it is building, a DMA buffer, the
 * device tree the firmware left behind — and once the kernel is not
 * identity-mapped, "cast the physical address to a pointer" stops working.
 * A single linear window is the cheapest answer: the conversion is an
 * addition, and no allocation or mapping is needed to touch a page.
 *
 * **Why MMIO is separate from it.** Device registers must not be covered by
 * a mapping that is read-write-anything; they want their own pages, and on
 * this board they sit *below* RAM (`0x1000_0000`-ish against RAM's
 * `0x4000_0000`), so a window based at RAM's base could not reach them
 * with an unsigned offset at all. */
#define KERNEL_VIRT_BASE  0xFFFFFFC000000000ull
#define DIRECT_MAP_BASE   0xFFFFFFC800000000ull
#define DEVICE_MAP_BASE   0xFFFFFFD000000000ull

/* The image sits 2 MiB into its slot, matching the 2 MiB the boot protocol's
 * `text_offset` puts it at in physical memory — so the low 30 bits of a
 * kernel address are the same virtually and physically, which makes a fault
 * address recognisable at a glance. */
#define KERNEL_VIRT_OFFSET 0x200000ull

#endif
