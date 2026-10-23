#include "core/pmm.h"

#include <stdbool.h>

#include "core/errno.h"

static bool page_used(const struct pmm *p, uint32_t i) {
    return (p->bitmap[i / 32u] >> (i % 32u)) & 1u;
}

static void set_page(struct pmm *p, uint32_t i, bool used) {
    uint32_t bit = 1u << (i % 32u);
    if (used && !page_used(p, i)) {
        p->bitmap[i / 32u] |= bit;
        p->free--;
    } else if (!used && page_used(p, i)) {
        p->bitmap[i / 32u] &= ~bit;
        p->free++;
    }
}

/* The index of a frame, or npages when it is not one of ours. */
static uint32_t frame_index(const struct pmm *p, phys_t phys) {
    uint64_t i;
    if (phys < p->base || (phys & (PMM_PAGE_SIZE - 1u)) != 0) {
        return p->npages;
    }
    i = ((uint64_t)phys - (uint64_t)p->base) >> PMM_PAGE_SHIFT;
    return i < p->npages ? (uint32_t)i : p->npages;
}

void pmm_set_refcounts(struct pmm *p, uint8_t *refs) {
    p->refs = refs;
    for (uint32_t i = 0; refs != 0 && i < p->npages; i++) {
        refs[i] = 0;
    }
}

int pmm_share(struct pmm *p, phys_t phys) {
    uint32_t i = frame_index(p, phys);
    if (i == p->npages || !page_used(p, i)) {
        return KERR_INVAL;
    }
    if (p->refs == 0) {
        return KERR_NOTSUP;
    }
    /* Saturating, and once there it is **sticky**: the frame is leaked
     * rather than ever freed again. A count that has stopped rising has
     * stopped being true, and being too low is the direction that hands a
     * live page back to the allocator. Leaking a page is much the better
     * failure. */
    if (p->refs[i] < 0xffu) {
        p->refs[i] = (uint8_t)(p->refs[i] + 1u);
    }
    return KERR_OK;
}

uint32_t pmm_shares(const struct pmm *p, phys_t phys) {
    uint32_t i = frame_index(p, phys);
    if (i == p->npages || !page_used(p, i)) {
        return 0;
    }
    return p->refs == 0 ? 1u : (uint32_t)p->refs[i] + 1u;
}

void pmm_init(struct pmm *p, phys_t base, uint32_t npages, uint32_t *bitmap) {
    *p = (struct pmm){.base = base, .npages = npages, .bitmap = bitmap};
    for (uint32_t w = 0; w < PMM_BITMAP_WORDS(npages); w++) {
        bitmap[w] = ~0u;
    }
}

/* Clips the page range [first, last) to the frames the pmm covers. */
static void mark_range(struct pmm *p, uint64_t first, uint64_t last, bool used) {
    uint64_t lo = (uint64_t)p->base >> PMM_PAGE_SHIFT;
    for (uint64_t pg = first < lo ? lo : first; pg < last && pg - lo < p->npages; pg++) {
        set_page(p, (uint32_t)(pg - lo), used);
    }
}

void pmm_release(struct pmm *p, phys_t phys, uint64_t len) {
    uint64_t start = ((uint64_t)phys + PMM_PAGE_SIZE - 1u) >> PMM_PAGE_SHIFT;
    uint64_t end = ((uint64_t)phys + len) >> PMM_PAGE_SHIFT;
    mark_range(p, start, end, false);
}

void pmm_reserve(struct pmm *p, phys_t phys, uint64_t len) {
    uint64_t start = (uint64_t)phys >> PMM_PAGE_SHIFT;
    uint64_t end = ((uint64_t)phys + len + PMM_PAGE_SIZE - 1u) >> PMM_PAGE_SHIFT;
    mark_range(p, start, end, true);
}

/* Length of the free run starting at frame i, capped at want. */
static uint32_t free_run(const struct pmm *p, uint32_t i, uint32_t want) {
    uint32_t n = 0;
    while (n < want && i + n < p->npages && !page_used(p, i + n)) {
        n++;
    }
    return n;
}

/* The single-page fast path: the first word with a clear bit, from hint. */
static int alloc_one(struct pmm *p, uint32_t *index) {
    uint32_t words = PMM_BITMAP_WORDS(p->npages);
    for (uint32_t k = 0; k < words; k++) {
        uint32_t w = (p->hint + k) % words;
        for (uint32_t b = 0; p->bitmap[w] != ~0u && b < 32u; b++) {
            if (w * 32u + b < p->npages && !page_used(p, w * 32u + b)) {
                p->hint = w;
                *index = w * 32u + b;
                return KERR_OK;
            }
        }
    }
    return KERR_NOMEM;
}

/* First aligned run of npages free frames. */
static int alloc_run(const struct pmm *p, uint32_t npages, uint32_t align, uint32_t *index) {
    for (uint32_t i = 0; i + npages <= p->npages; i += align) {
        if (free_run(p, i, npages) == npages) {
            *index = i;
            return KERR_OK;
        }
    }
    return KERR_NOMEM;
}

int pmm_alloc(struct pmm *p, uint32_t npages, uint32_t align_pages, phys_t *phys) {
    uint32_t align = align_pages == 0 ? 1u : align_pages;
    uint32_t index = 0;
    int rc;
    if (npages == 0 || npages > p->free) {
        return KERR_NOMEM;
    }
    rc = (npages == 1 && align == 1) ? alloc_one(p, &index) : alloc_run(p, npages, align, &index);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t k = 0; k < npages; k++) {
        /* **A frame handed out twice** is two owners of one page, which is
         * the shape of the corruption `ROADMAP.md` §0 is chasing: a fresh
         * allocation arrives already full of somebody else's data. The
         * bitmap is what prevents it, so this is the bitmap being asked to
         * confirm its own answer — cheap, and the alternative is to keep
         * believing it. */
        if (page_used(p, index + k)) {
            p->double_allocations++;
            return KERR_INVAL;
        }
        set_page(p, index + k, true);
    }
    *phys = p->base + (index << PMM_PAGE_SHIFT);
    return KERR_OK;
}

uint32_t pmm_double_allocations(const struct pmm *p) {
    return p->double_allocations;
}

int pmm_free(struct pmm *p, phys_t phys, uint32_t npages) {
    uint32_t index = (phys - p->base) >> PMM_PAGE_SHIFT;
    if (phys < p->base || (phys & (PMM_PAGE_SIZE - 1u)) != 0 || index >= p->npages ||
        npages > p->npages - index) {
        return KERR_INVAL;
    }
    for (uint32_t k = 0; k < npages; k++) {
        if (!page_used(p, index + k)) {
            return KERR_INVAL;
        }
    }
    for (uint32_t k = 0; k < npages; k++) {
        /* **A share is given up before a frame is freed.** A frame two
         * address spaces hold is freed by neither of them until the second
         * one lets go; freeing it on the first would hand a page that is
         * still mapped somewhere back to the allocator, and the next
         * allocation would be served live memory. */
        if (p->refs != 0 && p->refs[index + k] == 0xffu) {
            /* **Saturated, so it stays.** Once the count stops rising it has
             * also stopped being true, and a count that is too low is the
             * dangerous direction: decrementing from the ceiling would reach
             * zero while holders remain and hand a live frame back. This
             * leaks the frame instead, permanently and on purpose. */
            continue;
        }
        if (p->refs != 0 && p->refs[index + k] != 0) {
            p->refs[index + k] = (uint8_t)(p->refs[index + k] - 1u);
            continue;
        }
        set_page(p, index + k, false);
    }
    return KERR_OK;
}
