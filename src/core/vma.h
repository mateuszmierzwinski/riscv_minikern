/* vma — the regions of one user address space: what is mapped where, with
 * which rights. Pages themselves are faulted in lazily by the kernel; this
 * is the bookkeeping that decides whether a fault is legal, and where a new
 * mmap goes. Pure: a sorted fixed array, so it is host-tested.
 *
 * Ranges are half-open [start, end) and page aligned. Adjacent regions with
 * equal rights and flags are merged, which keeps Go's mmap/mprotect/munmap
 * churn from exhausting the array. */
#ifndef MINIKERN_CORE_VMA_H
#define MINIKERN_CORE_VMA_H

#include <stdbool.h>
#include <stdint.h>

#include "core/types.h"

#define VMA_MAX 128u
#define VMA_PAGE 4096u

/* prot is Linux PROT_* (1 read, 2 write, 4 exec; 0 = reserved, no access). */
#define VMA_GROWSDOWN 1u  /* the main stack: not merged with neighbours */
#define VMA_HUGE 2u       /* asked for in 2 MiB pages, and backed in them */

struct vma {
    virt_t start;
    virt_t end;
    uint32_t prot;
    uint32_t flags;
};

struct vma_set {
    struct vma v[VMA_MAX];
    uint32_t n;
};

/* The region holding `addr`, or NULL.
 *
 * What demand paging is built on: a fault is only worth backing if the
 * address is **inside a region the process was given**, and the region is
 * what says with which rights. Without this the fault handler would have to
 * ask the page tables, which cannot answer — an unbacked page and an
 * address nobody ever asked for look identical there, and backing the
 * second would turn a wild pointer into a silently working one.
 *
 * Half-open, like every range in this file: `end` is the first address
 * *not* in the region, so an address exactly at `end` belongs to whatever
 * comes next and not to this one.
 *
 * The pointer is into the set and is invalidated by any insert or remove,
 * which is why it is const and why callers read what they need out of it
 * and do not keep it. */
const struct vma *vma_find(const struct vma_set *s, virt_t addr);

/* Adds a region. KERR_INVAL when misaligned, empty or overlapping an
 * existing one; KERR_NOMEM when the set is full. */
int vma_insert(struct vma_set *s, virt_t start, virt_t end, uint32_t prot, uint32_t flags);

/* Removes [start, end) from every region it touches, splitting partial
 * ones. Calls gone(ctx, lo, hi) for each sub-range actually removed, so the
 * caller can drop the pages. KERR_NOMEM if a split needs a slot it lacks
 * (nothing is changed then); unmapped holes are not an error. */
int vma_remove(struct vma_set *s, virt_t start, virt_t end,
               void (*gone)(void *ctx, virt_t lo, virt_t hi), void *ctx);

/* Sets prot on [start, end). KERR_NOMEM (Linux's answer) when part of the
 * range is unmapped or a split needs a slot it lacks; nothing changes then. */
int vma_protect(struct vma_set *s, virt_t start, virt_t end, uint32_t prot);

/* Whether every byte of [start, end) is outside every region.
 *
 * The question `MAP_FIXED_NOREPLACE` asks, and the reason it exists: before
 * it, the only way for a program to find out whether an address was free
 * was to take it, and a wrong guess cost whatever was there. One region can
 * answer, because the set is sorted and non-overlapping — the first region
 * ending after `start` is the only one that could reach into the range. */
bool vma_range_free(const struct vma_set *s, virt_t start, virt_t end);

/* A free gap of len bytes inside [lo, hi): the highest one below hi, or
 * exactly hint when [hint, hint+len) is free and inside the window.
 * KERR_OK with *addr, or KERR_NOMEM.
 *
 * `align` is the alignment the result must have, in bytes, and 0 or 1 means
 * any. It exists for huge pages: a 2 MiB page can only be mapped at a 2 MiB
 * boundary, so a mapping that wants one has to be *placed* at one — and
 * aligning afterwards is not the same thing, because the aligned address
 * may have walked out of the gap that was found. */
int vma_find_free(const struct vma_set *s, virt_t len, virt_t hint, virt_t lo, virt_t hi,
                  virt_t align, virt_t *addr);

#endif
