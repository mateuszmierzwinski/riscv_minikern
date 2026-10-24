#include "arch/riscv/pgtable.h"

#include <stddef.h>

#include "core/errno.h"

#define SATP_MODE_SV39 (8ull << 60)
#define PTE_PPN_SHIFT 10u
#define PPN_BITS 44u

/* The index into a level's table. Level 2 is the root. */
static uint32_t index_at(virt_t va, uint32_t level) {
    return (uint32_t)((va >> (12u + 9u * level)) & (PT_ENTRIES - 1u));
}

/* How many bytes one entry at this level covers. */
static uint64_t span_at(uint32_t level) {
    return PAGE_SIZE << (9u * level);
}

static uint64_t pte_make(phys_t pa, uint64_t flags) {
    return ((pa >> 12) & ((1ull << PPN_BITS) - 1u)) << PTE_PPN_SHIFT | flags;
}

static phys_t pte_phys(uint64_t pte) {
    return ((pte >> PTE_PPN_SHIFT) & ((1ull << PPN_BITS) - 1u)) << 12;
}

/* A leaf has at least one permission bit; a valid entry without any is a
 * pointer to the next level. That one rule replaces three descriptor
 * formats on the architecture this came from. */
static bool pte_is_leaf(uint64_t pte) {
    return (pte & PTE_V) != 0 && (pte & (PTE_R | PTE_W | PTE_X)) != 0;
}

static bool pte_is_table(uint64_t pte) {
    return (pte & PTE_V) != 0 && (pte & (PTE_R | PTE_W | PTE_X)) == 0;
}

bool pgtable_va_is_canonical(virt_t va) {
    /* Bits 63:39 must all equal bit 38, so shifting the address left by 25
     * and arithmetic-shifting it back must reproduce it. */
    return (virt_t)(((int64_t)(va << 25)) >> 25) == va;
}

/* The level a mapping of this size lives at, or PT_LEVELS when the size is
 * not one a leaf can cover. */
static uint32_t level_for_size(uint64_t size) {
    switch (size) {
        case PAGE_SIZE: return 0;
        case MEGAPAGE_SIZE: return 1;
        case GIGAPAGE_SIZE: return 2;
        default: return PT_LEVELS;
    }
}

int pgtable_create(struct pgtable *pt, const struct pt_mem *mem) {
    void *root = NULL;
    phys_t phys = 0;
    int rc;
    pt->mem = *mem;
    rc = pt->mem.alloc(pt->mem.self, &root, &phys);
    if (rc != KERR_OK) {
        return rc;
    }
    pt->root = root;
    pt->root_phys = phys;
    return KERR_OK;
}

/* Frees the tables below one entry, recursively. Depth is bounded by
 * PT_LEVELS, so the recursion is three deep and cannot run away. */
static void free_table(struct pgtable *pt, uint64_t *table, uint32_t level) {
    if (level > 0) {
        for (uint32_t i = 0; i < PT_ENTRIES; i++) {
            if (pte_is_table(table[i])) {
                free_table(pt, pt->mem.to_virt(pt->mem.self, pte_phys(table[i])), level - 1u);
            }
        }
    }
    pt->mem.free(pt->mem.self, table);
}

void pgtable_destroy(struct pgtable *pt) {
    if (pt->root != NULL) {
        free_table(pt, pt->root, PT_LEVELS - 1u);
        pt->root = NULL;
        pt->root_phys = 0;
    }
}

/* One entry, tested against a frame: reports its virtual address when the
 * entry is a leaf that covers the frame. Returns 1 when it reported. */
static uint32_t report_if_covers(uint64_t pte, virt_t va, uint64_t span, phys_t phys, virt_t *out,
                                 uint32_t max, uint32_t found) {
    if (!pte_is_leaf(pte) || pte_phys(pte) > phys || pte_phys(pte) + span <= phys) {
        return 0;
    }
    if (found < max) {
        out[found] = va;
    }
    return 1;
}

/* Makes every store this hart has already made visible to the others before
 * the next one.
 *
 * It is `__atomic_thread_fence` and not inline assembly because this file is
 * host-tested: the arithmetic of a page table is pure, and it is tested on
 * the machine that compiles it. The builtin is the same `fence` instruction
 * on RISC-V and the right thing on the host. */
static void publish_barrier(void) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

/* Walks to the table that holds `va`'s entry at `level`, creating the
 * tables above it when `create` is set. NULL when a table is missing and
 * none may be made, or when a large mapping already covers the address —
 * which is a refusal, not a failure to find. */
static uint64_t *walk_to(struct pgtable *pt, virt_t va, uint32_t level, bool create) {
    uint64_t *table = pt->root;
    for (uint32_t l = PT_LEVELS - 1u; l > level; l--) {
        uint64_t *entry = &table[index_at(va, l)];
        void *next = NULL;
        phys_t phys = 0;
        if (pte_is_leaf(*entry)) {
            return NULL; /* a larger mapping is already here */
        }
        if (!pte_is_table(*entry)) {
            if (!create || pt->mem.alloc(pt->mem.self, &next, &phys) != KERR_OK) {
                return NULL;
            }
            /* **The table's contents before the pointer to it.**
             *
             * Four harts walk these tables at once, in hardware, while one
             * of them edits under the kernel lock — and the lock means
             * nothing to an MMU. The allocator has just zeroed this page,
             * and on a weakly ordered machine another hart may observe the
             * store that publishes it *before* those zeroes: its walk then
             * descends into a table full of whatever the page held last
             * time, and a page-table walk that reads stale entries does not
             * fault. It translates to the wrong page. */
            publish_barrier();
            *entry = pte_make(phys, PTE_V);
        } else {
            next = pt->mem.to_virt(pt->mem.self, pte_phys(*entry));
        }
        table = next;
    }
    return table;
}

int pgtable_map(struct pgtable *pt, virt_t va, phys_t pa, uint64_t size, uint64_t prot) {
    uint32_t level = level_for_size(size);
    uint64_t *table;
    if (level >= PT_LEVELS || !pgtable_va_is_canonical(va)) {
        return KERR_INVAL;
    }
    /* A large mapping must be aligned to what it covers, on both sides. */
    if ((va & (size - 1u)) != 0 || (pa & (size - 1u)) != 0) {
        return KERR_INVAL;
    }
    table = walk_to(pt, va, level, true);
    if (table == NULL) {
        return KERR_NOMEM;
    }
    if ((table[index_at(va, level)] & PTE_V) != 0) {
        return KERR_EXIST; /* replacing a mapping silently is how aliases happen */
    }
    /* A and D are set here rather than left to hardware: this kernel does
     * not use them for paging decisions, and an implementation that faults
     * to set them would fault on every first access. */
    /* **The page's contents before the entry that reaches it**, for the
     * same reason as the table above: the caller has just zeroed this frame
     * or filled it from a file, and another hart's MMU may see the entry
     * first and read what the frame held when it belonged to something
     * else. */
    publish_barrier();
    table[index_at(va, level)] = pte_make(pa, prot | PTE_V | PTE_A | PTE_D);
    return KERR_OK;
}

/* The largest mapping that fits at this address and this much remaining. */
static uint64_t best_size(virt_t va, phys_t pa, uint64_t left) {
    const uint64_t sizes[] = {GIGAPAGE_SIZE, MEGAPAGE_SIZE, PAGE_SIZE};
    for (uint32_t i = 0; i < 3u; i++) {
        if (left >= sizes[i] && (va & (sizes[i] - 1u)) == 0 && (pa & (sizes[i] - 1u)) == 0) {
            return sizes[i];
        }
    }
    return PAGE_SIZE;
}

int pgtable_map_range(struct pgtable *pt, virt_t va, phys_t pa, uint64_t bytes, uint64_t prot) {
    uint64_t done = 0;
    if ((va % PAGE_SIZE) != 0 || (pa % PAGE_SIZE) != 0 || (bytes % PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }
    while (done < bytes) {
        uint64_t size = best_size(va + done, pa + done, bytes - done);
        int rc = pgtable_map(pt, va + done, pa + done, size, prot);
        if (rc != KERR_OK) {
            return rc;
        }
        done += size;
    }
    return KERR_OK;
}

bool pgtable_lookup(const struct pgtable *pt, virt_t va, phys_t *pa, uint64_t *prot, uint64_t *size) {
    const uint64_t *table = pt->root;
    if (!pgtable_va_is_canonical(va)) {
        return false;
    }
    for (uint32_t l = PT_LEVELS; l-- > 0;) {
        uint64_t pte = table[index_at(va, l)];
        if (pte_is_leaf(pte)) {
            uint64_t span = span_at(l);
            *pa = pte_phys(pte) + (va & (span - 1u));
            /* The software's own bits come back too. A caller that asks
             * what a page is needs to know whether it is shared
             * copy-on-write as much as whether it is writable — and a fault
             * handler that could not see `PTE_COW` could not tell a copy it
             * must make from a program writing where it should not. */
            *prot = pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_G | PTE_RSW0 | PTE_RSW1);
            *size = span;
            return true;
        }
        if (!pte_is_table(pte)) {
            return false;
        }
        table = pt->mem.to_virt(pt->mem.self, pte_phys(pte));
    }
    return false;
}

uint64_t pgtable_unmap(struct pgtable *pt, virt_t va) {
    uint64_t *table = pt->root;
    if (!pgtable_va_is_canonical(va)) {
        return 0;
    }
    for (uint32_t l = PT_LEVELS; l-- > 0;) {
        uint64_t *entry = &table[index_at(va, l)];
        if (pte_is_leaf(*entry)) {
            *entry = 0;
            return span_at(l);
        }
        if (!pte_is_table(*entry)) {
            return 0;
        }
        table = pt->mem.to_virt(pt->mem.self, pte_phys(*entry));
    }
    return 0;
}

/* Every virtual address in this table that maps `phys`.
 *
 * The question a corrupted "fresh" page asks: a frame the allocator has
 * just handed out, already holding somebody's data, is reachable from
 * somewhere — and the table is the only place that can say where. One
 * mapping is the expected answer. **Two is the bug**, and knowing the
 * second address says what it is: a heap page, a stack, a page table.
 *
 * Walks all three levels and reports leaves at any of them, so a 2 MiB or
 * 1 GiB mapping that covers the frame counts too. Linear in the tables that
 * exist, which is why nothing calls it on a hot path. */
uint32_t pgtable_find_mappings(const struct pgtable *pt, phys_t phys, virt_t *out, uint32_t max) {
    uint32_t found = 0;
    for (uint32_t i2 = 0; i2 < PT_ENTRIES; i2++) {
        uint64_t e2 = pt->root[i2];
        const uint64_t *t1;
        if (pte_is_leaf(e2)) {
            found += report_if_covers(e2, (virt_t)i2 << 30, GIGAPAGE_SIZE, phys, out, max, found);
            continue;
        }
        if (!pte_is_table(e2)) {
            continue;
        }
        t1 = pt->mem.to_virt(pt->mem.self, pte_phys(e2));
        for (uint32_t i1 = 0; i1 < PT_ENTRIES; i1++) {
            uint64_t e1 = t1[i1];
            const uint64_t *t0;
            virt_t base1 = ((virt_t)i2 << 30) | ((virt_t)i1 << 21);
            if (pte_is_leaf(e1)) {
                found += report_if_covers(e1, base1, MEGAPAGE_SIZE, phys, out, max, found);
                continue;
            }
            if (!pte_is_table(e1)) {
                continue;
            }
            t0 = pt->mem.to_virt(pt->mem.self, pte_phys(e1));
            for (uint32_t i0 = 0; i0 < PT_ENTRIES; i0++) {
                found += report_if_covers(t0[i0], base1 | ((virt_t)i0 << 12), PAGE_SIZE, phys, out,
                                          max, found);
            }
        }
    }
    return found;
}

/* The entry for `va` at the leaf level, or NULL when no table reaches it.
 *
 * Unlike `walk_to`, it does not create tables: a page that is being swapped
 * out is mapped, so its tables exist, and a page whose tables do not exist
 * was never swapped. */
static uint64_t *leaf_entry(const struct pgtable *pt, virt_t va) {
    const uint64_t *table = pt->root;
    if (!pgtable_va_is_canonical(va)) {
        return NULL;
    }
    for (uint32_t l = PT_LEVELS; l-- > 1u;) {
        uint64_t pte = table[index_at(va, l)];
        if (!pte_is_table(pte)) {
            return NULL;
        }
        table = pt->mem.to_virt(pt->mem.self, pte_phys(pte));
    }
    return (uint64_t *)&table[index_at(va, 0)];
}

bool pgtable_accessed(const struct pgtable *pt, virt_t va) {
    const uint64_t *entry = leaf_entry((struct pgtable *)pt, va);
    return entry != NULL && pte_is_leaf(*entry) && (*entry & PTE_A) != 0;
}

bool pgtable_set_accessed(struct pgtable *pt, virt_t va, bool dirty) {
    uint64_t *entry = leaf_entry(pt, va);
    if (entry == NULL || !pte_is_leaf(*entry)) {
        return false;
    }
    *entry |= PTE_A | (dirty ? PTE_D : 0u);
    return true;
}

bool pgtable_clear_accessed(struct pgtable *pt, virt_t va) {
    uint64_t *entry = leaf_entry(pt, va);
    if (entry == NULL || !pte_is_leaf(*entry) || (*entry & PTE_A) == 0) {
        return false;
    }
    *entry &= ~(uint64_t)PTE_A;
    return true;
}

int pgtable_set_swapped(struct pgtable *pt, virt_t va, uint32_t slot) {
    uint64_t *entry = leaf_entry(pt, va);
    if (entry == NULL || !pte_is_leaf(*entry)) {
        return KERR_NOENT;
    }
    *entry = ((uint64_t)slot << PTE_SWAP_SHIFT) | PTE_SWAPPED;
    return KERR_OK;
}

bool pgtable_swapped_slot(const struct pgtable *pt, virt_t va, uint32_t *slot) {
    const uint64_t *entry = leaf_entry(pt, va);
    if (entry == NULL || (*entry & PTE_V) != 0 || (*entry & PTE_SWAPPED) == 0) {
        return false;
    }
    *slot = (uint32_t)(*entry >> PTE_SWAP_SHIFT);
    return true;
}

void pgtable_each_leaf(const struct pgtable *pt,
                       void (*each)(void *ctx, virt_t va, phys_t pa, uint64_t span, uint64_t flags),
                       void *ctx) {
    for (uint32_t i2 = 0; i2 < PT_ENTRIES; i2++) {
        uint64_t e2 = pt->root[i2];
        const uint64_t *t1;
        if (pte_is_leaf(e2)) {
            each(ctx, (virt_t)i2 << 30, pte_phys(e2), GIGAPAGE_SIZE, e2);
            continue;
        }
        if (!pte_is_table(e2)) {
            continue;
        }
        t1 = pt->mem.to_virt(pt->mem.self, pte_phys(e2));
        for (uint32_t i1 = 0; i1 < PT_ENTRIES; i1++) {
            uint64_t e1 = t1[i1];
            const uint64_t *t0;
            virt_t base1 = ((virt_t)i2 << 30) | ((virt_t)i1 << 21);
            if (pte_is_leaf(e1)) {
                each(ctx, base1, pte_phys(e1), MEGAPAGE_SIZE, e1);
                continue;
            }
            if (!pte_is_table(e1)) {
                continue;
            }
            t0 = pt->mem.to_virt(pt->mem.self, pte_phys(e1));
            for (uint32_t i0 = 0; i0 < PT_ENTRIES; i0++) {
                if (pte_is_leaf(t0[i0])) {
                    each(ctx, base1 | ((virt_t)i0 << 12), pte_phys(t0[i0]), PAGE_SIZE, t0[i0]);
                }
            }
        }
    }
}

uint64_t pgtable_satp(const struct pgtable *pt, uint16_t asid) {
    return SATP_MODE_SV39 | ((uint64_t)asid << 44) | (pt->root_phys >> 12);
}
