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

uint64_t pgtable_satp(const struct pgtable *pt, uint16_t asid) {
    return SATP_MODE_SV39 | ((uint64_t)asid << 44) | (pt->root_phys >> 12);
}
