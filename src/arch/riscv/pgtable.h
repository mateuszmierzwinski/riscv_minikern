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
};

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
int pgtable_map(struct pgtable *pt, virt_t va, phys_t pa, uint64_t size, uint64_t prot);

/* Maps a range, using the largest pages that fit and align. This is how the
 * kernel's own linear map is built: a gigapage per GiB where it can, which
 * is one entry instead of 262144. */
int pgtable_map_range(struct pgtable *pt, virt_t va, phys_t pa, uint64_t bytes, uint64_t prot);

/* What `va` translates to, and with what permissions. False when unmapped.
 * `size` reports whether it was found as a page, a megapage or a gigapage. */
bool pgtable_lookup(const struct pgtable *pt, virt_t va, phys_t *pa, uint64_t *prot, uint64_t *size);

/* Removes the mapping `va` falls in, and says how many bytes it covered.
 * Zero when nothing was mapped. */
uint64_t pgtable_unmap(struct pgtable *pt, virt_t va);

/* The value `satp` wants for this address space, with Sv39 selected and an
 * address-space id. */
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
