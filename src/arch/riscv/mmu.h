/* mmu — installing a translation table, and the memory to build one from.
 *
 * Separate from `pgtable.h` on purpose: that module is arithmetic and is
 * host-tested, while this one writes `satp` and issues fences, which only a
 * machine can do. The division is what let the arithmetic be proved before
 * this existed.
 *
 * Enabling translation is the one operation in a kernel that cannot be
 * partially correct. The instruction after `csrw satp` is fetched through
 * the table that was just installed, so if the kernel's own code is not
 * mapped at the address it is executing from, there is no fault handler to
 * report it — the machine simply stops fetching. That is why the first table
 * this builds maps the kernel **identically**: same addresses, before and
 * after.
 */
#ifndef MINIKERN_ARCH_RISCV_MMU_H
#define MINIKERN_ARCH_RISCV_MMU_H

#include <stdbool.h>

#include "arch/riscv/pgtable.h"
#include "core/types.h"

/* Gives the table allocator the memory it may carve pages from: the region
 * immediately above the kernel image. Called once, before mmu_init. */
void mmu_set_table_arena(phys_t base, uint64_t bytes);

/* Builds the kernel's address space over `ram_base`..`ram_base + ram_bytes`
 * and switches translation on.
 *
 * The kernel's own text is mapped read-execute and everything else
 * read-write, so that its code cannot be written and its data cannot be
 * executed — the cheapest version of a protection that costs nothing to set
 * up correctly and is awkward to retrofit.
 *
 * Returns KERR_OK with translation enabled, or an error with it still off:
 * a failure here leaves the machine exactly as it was rather than halfway. */
int mmu_init(phys_t ram_base, uint64_t ram_bytes);

/* The kernel's table, for anything that needs to add to it. */
struct pgtable *mmu_kernel_table(void);

/* True once translation is on. */
bool mmu_enabled(void);

/* How many 4 KiB pages the table allocator has handed out, which is the
 * honest measure of what the kernel's own map costs. */
uint32_t mmu_table_pages_used(void);

#endif
