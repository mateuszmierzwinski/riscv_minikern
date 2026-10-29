/* pgtable — Sv39 translation tables, as pure arithmetic.
 *
 * **Pure on purpose, and host-tested before anything installs one.** The
 * equivalent in the ARM kernel this migrated from is what caught a
 * descriptor-type bug that would otherwise have been an unexplained fault
 * storm on hardware — a test asked "is this a small page?" with `== 0b10`
 * and so missed every non-executable page, because the type field's low bit
 * is the execute-never flag. Nothing here touches a CSR; installing a table
 * is `mmu.h`'s job.
 *
 * Sv39: a 39-bit virtual address, three levels of 512 entries, 4 KiB pages.
 *
 *     38      30 29      21 20      12 11        0
 *     | VPN[2]  |  VPN[1]  |  VPN[0]  |  offset  |
 *
 * Bits 63:39 of a virtual address must all equal bit 38 — the address is
 * sign-extended — so the bottom half (user space) looks like
 * `0x0000_0000_0000_0000`… and the top half (the kernel) like
 * `0xFFFF_FFC0_0000_0000`…, with a 255 TiB hole between them that is not an
 * address at all. An address in that hole faults on use, and this module
 * refuses it at map time instead.
 *
 * A **leaf** entry has at least one of R, W, X set; an entry with V set and
 * none of them is a **pointer** to the next level. That single distinction
 * replaces ARM's separate section/page/table descriptor formats, and it is
 * why a 2 MiB "megapage" and a 1 GiB "gigapage" need no special descriptor:
 * a leaf at level 1 or level 2 *is* the large mapping.
 */
#ifndef MINIKERN_ARCH_RISCV_PGTABLE_H
#define MINIKERN_ARCH_RISCV_PGTABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/types.h"

/* **A page that is out in swap**, recorded in the entry it used to live in.
 *
 * `V` is clear, so the hardware ignores the entry and a walk reports "not
 * mapped" — which is what a fault handler needs to hear, and what every
 * existing caller already copes with. Both `pte_is_leaf` and `pte_is_table`
 * require `V`, so a swapped entry is invisible to them by construction
 * rather than by everyone remembering.
 *
 * The marker is what distinguishes it from an entry that is simply **absent**
 * (all zero). Without one, a fault on a swapped page would be backed with a
 * fresh zero page and the page's contents would be gone — silently, which
 * is the worst way to lose memory. The slot sits above the flag bits, where
 * a physical page number would be; nothing reads it as one, because nothing
 * reads an invalid entry at all. */
#define PTE_SWAPPED (1ull << 1) /* `R`, which means nothing while `V` is clear */
#define PTE_SWAP_SHIFT 10u

#define PT_LEVELS 3u
#define PT_ENTRIES 512u            /* per level: 4096 bytes / 8 */
#define PT_BYTES 4096u
#define PAGE_SIZE 4096ull
#define MEGAPAGE_SIZE (2ull * 1024 * 1024)
#define GIGAPAGE_SIZE (1024ull * 1024 * 1024)

/* Permission and attribute bits, in their architectural positions. */
#define PTE_V (1ull << 0)   /* valid */
#define PTE_R (1ull << 1)
#define PTE_W (1ull << 2)
#define PTE_X (1ull << 3)
#define PTE_U (1ull << 4)   /* reachable from user mode */
#define PTE_G (1ull << 5)   /* global: not flushed per address space */
#define PTE_A (1ull << 6)   /* accessed */
#define PTE_D (1ull << 7)   /* dirty */

/* **Bits 8 and 9 belong to the software.** Sv39 reserves them for whatever
 * the operating system wants to record about a page, and the hardware
 * ignores them — so a fact the kernel needs on every fault can live in the
 * entry itself rather than in a table beside it.
 *
 * `PTE_COW` marks a page that two address spaces share and that must be
 * copied before either writes to it. The entry is left read-only, so a
 * store faults; the handler sees this bit and knows the fault is a copy to
 * perform rather than a program's mistake. Without it the two are
 * indistinguishable, and a kernel that guessed would either kill a
 * well-behaved program or silently let one process write into another's
 * memory. */
#define PTE_RSW0 (1ull << 8)
#define PTE_RSW1 (1ull << 9)
#define PTE_COW PTE_RSW0

/* What the hardware actually looks at, so a `prot` value can be compared
 * without the software's own bits getting in the way. */
#define PTE_HARDWARE_BITS (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_G | PTE_A | PTE_D)

/* What a caller asks for, which is the permission bits and nothing else:
 * V, A and D are the table's business. */
#define PT_PROT_R (PTE_R)
#define PT_PROT_RW (PTE_R | PTE_W)
#define PT_PROT_RX (PTE_R | PTE_X)
#define PT_PROT_RWX (PTE_R | PTE_W | PTE_X)
#define PT_PROT_USER (PTE_U)
#define PT_PROT_GLOBAL (PTE_G)

/* Where tables come from. `alloc` returns zeroed, 4 KiB-aligned table memory
 * as both a pointer and a physical address; `to_virt` turns a physical
 * address found in an entry back into a pointer. A host test supplies all
 * three from ordinary memory, which is what makes this testable at all. */
struct pt_mem {
    int (*alloc)(void *self, void **virt, phys_t *phys);
    void (*free)(void *self, void *virt);
    void *(*to_virt)(void *self, phys_t phys);
    void *self;
};

struct pgtable {
    uint64_t *root;
    phys_t root_phys;
    struct pt_mem mem;
    /* Root slots whose level-1 table belongs to somebody else.
     *
     * **A page table cannot otherwise tell.** `share_kernel_mappings` hands
     * a process the kernel's own level-1 table for a gigapage the kernel
     * has mappings in, and `walk_to` then returns that table like any
     * other — so a mapping made afterwards is written into the kernel's
     * address space, where the kernel and every other process can see it.
     * One page's worth of entries displaced that way cost a session: the
     * thermal sampler faulted on the housekeeping beat, reaching a device
     * window whose entry a process's text had taken.
     *
     * Marked rather than refused, for now. Refusing changes behaviour that
     * the board is currently green with, and an attempt to avoid the
     * sharing altogether took it from green to silence. Counting makes the
     * event visible on every boot, which is the difference between a defect
     * and a mystery. */
    uint64_t borrowed[PT_ENTRIES / 64u];
};

/* Says that this slot's level-1 table is the kernel's, not this table's. */
void pgtable_mark_borrowed(struct pgtable *pt, uint32_t slot);

/* How many mappings have been written into a table the process borrowed —
 * which is to say, into the kernel's own address space. Zero is the
 * invariant; anything else is the bug above, happening now. */
uint32_t pgtable_writes_into_borrowed(void);

/* The address of the first mapping that went into a borrowed table, which is
 * what identifies the region responsible. Meaningless when the count is 0. */
virt_t pgtable_first_borrowed_va(void);

/* Called for each leaf mapping a walk finds. Returning anything but KERR_OK
 * stops the walk, and that value becomes the walk's result. */
typedef int (*pgtable_leaf_fn)(void *ctx, virt_t va, phys_t pa, uint64_t prot, uint64_t size);

/* Visits every leaf mapping below `below`.
 *
 * **The page tables are the authority on what a process has**, where the
 * region record says what it is entitled to — and the two are different
 * lists, because a program's segments are mapped by the loader and are not
 * recorded. Anything that must act on what is really there, `fork` above
 * all, walks this rather than the regions. */
int pgtable_for_each_leaf(const struct pgtable *pt, virt_t below, pgtable_leaf_fn fn, void *ctx);

/* An empty address space. KERR_OK or KERR_NOMEM. */
int pgtable_create(struct pgtable *pt, const struct pt_mem *mem);

/* Frees every table the address space owns, but none of the memory it
 * mapped: a page table does not own the pages it points at. */
void pgtable_destroy(struct pgtable *pt);

/* Maps `size` bytes at `va` to `pa` with `prot` (PT_PROT_*).
 *
 * `size` must be PAGE_SIZE, MEGAPAGE_SIZE or GIGAPAGE_SIZE, and both
 * addresses must be aligned to it — a misaligned large mapping is the kind
 * of mistake that appears to work until the address it actually covers
 * matters. Returns KERR_INVAL for a bad size, a misaligned address, a
 * virtual address that is not sign-extended, or an attempt to replace an
 * existing mapping; KERR_NOMEM when a table cannot be allocated. */
/* A table entry pointing at `table`, with the publish barrier already done.
 * For a caller that has to install a level-1 table of its own — giving a
 * process its own table for a gigapage instead of sharing the kernel's. */
uint64_t pgtable_table_entry(phys_t table);

int pgtable_map(struct pgtable *pt, virt_t va, phys_t pa, uint64_t size, uint64_t prot);

/* Maps a range, using the largest pages that fit and align. This is how the
 * kernel's own linear map is built: a gigapage per GiB where it can, which
 * is one entry instead of 262144. */
int pgtable_map_range(struct pgtable *pt, virt_t va, phys_t pa, uint64_t bytes, uint64_t prot);

/* Whether a 2 MiB leaf could be put at `va`'s window: its level-1 entry is
 * empty — no megapage, and no level-0 table (even an emptied one, which
 * unmapping leaves behind). Asked **before** allocating 2 MiB for one: the
 * refusal after allocating cost a zeroed megapage per 4 KiB fault. */
bool pgtable_megapage_free(const struct pgtable *pt, virt_t va);

/* What `va` translates to, and with what permissions. False when unmapped.
 * `size` reports whether it was found as a page, a megapage or a gigapage. */
bool pgtable_lookup(const struct pgtable *pt, virt_t va, phys_t *pa, uint64_t *prot, uint64_t *size);

/* Removes the mapping `va` falls in, and says how many bytes it covered.
 * Zero when nothing was mapped. */
uint64_t pgtable_unmap(struct pgtable *pt, virt_t va);

/* The value `satp` wants for this address space, with Sv39 selected and an
 * address-space id. */
/* Every virtual address in this table that maps `phys`, up to `max` of
 * them, with the count returned (which may exceed `max`). One is normal;
 * **two is a frame with two owners**, and the second address says what it
 * is. Walks the tables, so it is not for a hot path. */
uint32_t pgtable_find_mappings(const struct pgtable *pt, phys_t phys, virt_t *out, uint32_t max);

/* Every leaf in this table, reported to `each`.
 *
 * The walk itself, handed over rather than repeated: an audit that asks
 * "which frame does each address use?" needs the whole set, and the
 * alternative is a copy of this loop wherever the question is asked. `span`
 * is the mapping's size, so a 2 MiB leaf is one call rather than 512. */
void pgtable_each_leaf(const struct pgtable *pt,
                       void (*each)(void *ctx, virt_t va, phys_t pa, uint64_t span, uint64_t flags),
                       void *ctx);

/* Records that `va`'s page has gone out to `slot`, replacing its mapping
 * with an invalid entry that remembers where. The page must be mapped now;
 * KERR_NOENT when it is not. */
/* The **accessed** bit of a leaf entry, read and cleared.
 *
 * `pgtable_map` sets `PTE_A` on every mapping it makes, deliberately — an
 * implementation that *faults* to set it would fault on every first access,
 * and this kernel would rather not depend on which kind of core it is on.
 * The consequence for a reclaim sweep is that a page's `A` means "mapped or
 * touched" the first time it is seen and "touched since I last looked"
 * every time after, which is exactly how a clock algorithm expects to be
 * told: the first pass clears, later passes decide.
 *
 * **Clearing it needs the translation flushed**, which the caller does.
 * A hart whose TLB holds the entry with `A` already set has no reason to
 * write the table again, so without a flush the bit would stay clear
 * however much the page is used — and a clock would evict the busiest page
 * on the machine. */
bool pgtable_accessed(const struct pgtable *pt, virt_t va);
bool pgtable_clear_accessed(struct pgtable *pt, virt_t va);

/* Sets `A` (and `D` for a store) on a leaf entry, which **this core
 * requires software to do**.
 *
 * RISC-V allows either behaviour: a core may update those bits itself, or
 * raise a page fault so that software can. This kernel assumed the first —
 * `CLAUDE.md` said so — on evidence that could not tell the cases apart,
 * because `pgtable_map` pre-sets both bits on every mapping and so no page
 * was ever reached with `A` clear. The first thing that cleared one (the
 * reclaim clock) hung the machine instantly: the fault recurred for ever,
 * since the tables and the hardware did not disagree about anything — `A`
 * was simply clear and the core wanted software to set it.
 *
 * So the JH7110's U74 is the **second** kind, and this is the path that
 * makes it work. */
bool pgtable_set_accessed(struct pgtable *pt, virt_t va, bool dirty);

int pgtable_set_swapped(struct pgtable *pt, virt_t va, uint32_t slot);

/* Whether `va`'s entry says its page is out in swap, and which slot. False
 * for a mapped page and for one that was never there — the three cases a
 * fault has to tell apart. */
bool pgtable_swapped_slot(const struct pgtable *pt, virt_t va, uint32_t *slot);

uint64_t pgtable_satp(const struct pgtable *pt, uint16_t asid);

/* The physical address an entry points at — the next table, or the page.
 * Exported because sharing the kernel's mappings into a process's table has
 * to follow one (kernel/uspace.c). */
static inline phys_t pte_phys_of(uint64_t pte) {
    return ((pte >> 10) & ((1ull << 44) - 1u)) << 12;
}

/* True when a virtual address is canonical: bits 63:39 all copy bit 38.
 * Exported because callers that build addresses want to assert it. */
bool pgtable_va_is_canonical(virt_t va);

#endif
