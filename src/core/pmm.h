/* pmm — the physical page allocator: one bit per 4 KiB page frame.
 *
 * Every page starts out used. The boot code releases the RAM that is really
 * free (everything but the kernel image, the boot info and the initrd), so a
 * forgotten reservation fails safe. Allocation finds runs of free pages with
 * a given alignment, which is what translation tables (8 KiB for TTBR0) and
 * kernel stacks need. Pure: the bitmap's storage is the caller's. */
#ifndef MINIKERN_CORE_PMM_H
#define MINIKERN_CORE_PMM_H

#include <stdint.h>

#define PMM_PAGE_SIZE 4096u
#define PMM_PAGE_SHIFT 12u

struct pmm {
    uint32_t base;     /* physical address of frame 0, page aligned */
    uint32_t npages;
    uint32_t *bitmap;  /* npages bits, 1 = used */
    uint32_t free;     /* frames currently free */
    uint32_t hint;     /* word index where the next single-page search starts */
};

/* Words of bitmap storage npages need. */
#define PMM_BITMAP_WORDS(npages) (((npages) + 31u) / 32u)

/* Starts with all npages frames from base marked used. */
void pmm_init(struct pmm *p, uint32_t base, uint32_t npages, uint32_t *bitmap);

/* Marks the whole pages inside [phys, phys+len) free; the partial pages at
 * either end stay used. Pages outside the range the pmm covers are ignored. */
void pmm_release(struct pmm *p, uint32_t phys, uint32_t len);

/* Marks every page that [phys, phys+len) touches as used. */
void pmm_reserve(struct pmm *p, uint32_t phys, uint32_t len);

/* Allocates npages contiguous frames whose first frame's index (from base)
 * is a multiple of align_pages (0 or 1: any). Returns KERR_OK with *phys
 * set, or KERR_NOMEM. */
int pmm_alloc(struct pmm *p, uint32_t npages, uint32_t align_pages, uint32_t *phys);

/* Frees npages frames from phys. Returns KERR_OK, or KERR_INVAL when any of
 * them is outside the pmm, misaligned or already free (a double free). */
int pmm_free(struct pmm *p, uint32_t phys, uint32_t npages);

#endif
