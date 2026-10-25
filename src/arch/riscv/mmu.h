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

/* Maps a device's registers into the kernel's address space, identically,
 * read-write and **not** executable.
 *
 * In 4 KiB pages even when a larger one would fit: device registers sit next
 * to other devices' registers, and a 2 MiB mapping of a 4 KiB UART would
 * quietly make a megabyte of other peripherals reachable — and on some parts,
 * reachable means a stray write away from reprogrammed. Safe to call twice
 * for the same device; the second call is a no-op rather than an error. */
/* Maps a device's registers and **returns where to reach them**.
 *
 * It used to map them identity and return only a status, so every driver
 * cast the physical address to a pointer. That works exactly as long as the
 * kernel is identity-mapped, and silently stops the moment it is not — the
 * driver would then read whatever is at that address in the kernel's own
 * half. Returning the address makes the dependency explicit and the
 * conversion impossible to forget: there is nothing else to use.
 *
 * `NULL` on failure, so a caller that ignores the error dereferences
 * nothing rather than the wrong thing. */
void *mmu_map_device(phys_t base, uint64_t bytes);

/* **A physical address the kernel can read, and the reverse.**
 *
 * `mmu_phys_to_virt` answers through the direct map, which covers all of
 * RAM; it is how the kernel reaches a page table it is building or a page
 * it is about to hand to a device.
 *
 * `mmu_virt_to_phys` is the one that matters for correctness, because
 * **hardware does not use the kernel's addresses**. Every descriptor and
 * buffer address written into a device register has to go through it, and
 * before the kernel moved out of the identity map the two were the same
 * number — so a missing conversion was invisible. */
/* How far a kernel address moves when the kernel goes to its own half: the
 * difference between where it is linked and where it was loaded. Only
 * meaningful before the jump — afterwards a linker symbol already reads as
 * its high self. */
uint64_t mmu_high_offset(void);

/* The kernel image's **physical** bounds, recorded while the linker symbols
 * still meant them. Anything that reserves the kernel's own memory has to
 * use these: after the jump the symbols read as high addresses, and a
 * reservation made from them covers memory that does not exist. */
void mmu_kernel_phys_range(phys_t *base, uint64_t *bytes);

/* Enters `fn` at the kernel's own address, on `sp_high`, with `a0` and `a1`
 * as its arguments. **Does not return** — the stack it leaves behind is full
 * of addresses that are about to stop meaning anything (`arch/riscv/start.S`).
 */
void mmu_enter_high(void *fn, uint64_t a0, uint64_t a1, uint64_t sp_high);

void *mmu_phys_to_virt(phys_t phys);
phys_t mmu_virt_to_phys(const void *virt);

/* The kernel's table, for anything that needs to add to it. */
struct pgtable *mmu_kernel_table(void);

/* True once translation is on. */
bool mmu_enabled(void);

/* How many 4 KiB pages the table allocator has handed out, which is the
 * honest measure of what the kernel's own map costs. */
uint32_t mmu_table_pages_used(void);

/* Discards cached translations on **every** hart, after a page table has
 * been changed.
 *
 * Local first with `sfence.vma`, because this hart's own TLB is not covered
 * by the firmware call, and then the others through RFENCE. Both halves are
 * needed and neither is optional: adding a mapping may need a fence even
 * locally, since an implementation is permitted to cache the fact that a
 * translation was absent.
 *
 * It is a whole-address-space flush rather than a single page. A page-at-a-
 * time flush is cheaper and would be worth having once a profile asks for
 * it; what it is not worth is being clever about *which* harts, because a
 * thread of this process can be on any of them.
 */
void mmu_flush_tlb_everywhere(void);

/* This hart only, which is enough when a mapping has been **added**.
 *
 * The asymmetry is the whole performance story. Removing or narrowing a
 * mapping must reach every hart at once, because a stale entry grants access
 * that is supposed to be gone. *Adding* one does not: the worst a stale
 * entry can do is say "absent" for something that is now present, and a hart
 * that faults on it fences locally and retries. So a broadcast belongs on
 * unmap and not on map — and that matters, because a garbage collector maps
 * constantly and unmaps rarely. Measured here: flushing every hart on every
 * `mmap` turned a 1.6x parallel speedup into 0.6x, which is slower than one
 * hart. */
void mmu_flush_tlb_local(void);

/* Whether the cross-hart part is actually available. A machine whose
 * firmware has no RFENCE cannot be made safe for more than one hart by this
 * kernel. */
bool mmu_can_flush_remotely(void);

#endif
