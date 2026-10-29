#include "core/vma.h"

#include <stddef.h>

#include <stdbool.h>

#include "core/errno.h"

static bool aligned_range(virt_t start, virt_t end) {
    return start < end && (start % VMA_PAGE) == 0 && (end % VMA_PAGE) == 0;
}

/* Index of the first region ending after addr (n when none). */
static uint32_t first_after(const struct vma_set *s, virt_t addr) {
    uint32_t i = 0;
    while (i < s->n && s->v[i].end <= addr) {
        i++;
    }
    return i;
}

const struct vma *vma_find(const struct vma_set *s, virt_t addr) {
    uint32_t i;
    if (s == NULL) {
        return NULL;
    }
    i = first_after(s, addr);
    return (i < s->n && s->v[i].start <= addr) ? &s->v[i] : NULL;
}

static void open_slot(struct vma_set *s, uint32_t at) {
    for (uint32_t k = s->n; k > at; k--) {
        s->v[k] = s->v[k - 1];
    }
    s->n++;
}

static void close_slot(struct vma_set *s, uint32_t at) {
    for (uint32_t k = at; k + 1 < s->n; k++) {
        s->v[k] = s->v[k + 1];
    }
    s->n--;
}

/* Two regions that can be one. **A file-backed region is never merged**:
 * its neighbour reads from a different place in a different file, and a
 * merged region could only name one of them. */
static bool mergeable(const struct vma *a, const struct vma *b) {
    return a->end == b->start && a->prot == b->prot && a->flags == b->flags && a->flags == 0 &&
           a->file == NULL && b->file == NULL;
}

/* Joins each region with an equal neighbour that touches it. */
static void merge_all(struct vma_set *s) {
    for (uint32_t i = 0; i + 1 < s->n;) {
        if (mergeable(&s->v[i], &s->v[i + 1])) {
            s->v[i].end = s->v[i + 1].end;
            close_slot(s, i + 1);
        } else {
            i++;
        }
    }
}

int vma_insert_backed(struct vma_set *s, virt_t start, virt_t end, uint32_t prot, uint32_t flags,
                      void *file, uint64_t offset) {
    uint32_t i = first_after(s, start);
    if (!aligned_range(start, end) || (i < s->n && s->v[i].start < end)) {
        return KERR_INVAL;
    }
    if (s->n == VMA_MAX) {
        return KERR_NOMEM;
    }
    open_slot(s, i);
    s->v[i] = (struct vma){
        .start = start, .end = end, .prot = prot, .flags = flags, .file = file, .offset = offset};
    merge_all(s);
    return KERR_OK;
}

int vma_insert(struct vma_set *s, virt_t start, virt_t end, uint32_t prot, uint32_t flags) {
    return vma_insert_backed(s, start, end, prot, flags, NULL, 0);
}

/* Region boundaries cut() must create: one per range end that falls
 * strictly inside a region. That many free slots are needed, at least
 * transiently, before anything is changed. */
static uint32_t splits_needed(const struct vma_set *s, virt_t start, virt_t end) {
    const struct vma *a = vma_find(s, start);
    const struct vma *b = vma_find(s, end);
    return (a != 0 && a->start < start ? 1u : 0u) + (b != 0 && b->start < end ? 1u : 0u);
}

/* Called with the new half of every file-backed region that is cut. */
static void (*split_hook)(const struct vma *half);

void vma_set_split_hook(void (*hook)(const struct vma *half)) {
    split_hook = hook;
}

/* Splits the region at index i at addr (strictly inside it).
 *
 * The upper half's `offset` moves with its start, so a file-backed region
 * cut in two still reads from the right place in the file. **And it holds
 * its own reference to the file**, through the hook: each region gives one
 * back when it goes, so two halves sharing one reference gave back two —
 * which a partial `mprotect` of a file mapping did, freeing the file under
 * the regions still using it. */
static void split_at(struct vma_set *s, uint32_t i, virt_t addr) {
    open_slot(s, i + 1);
    s->v[i + 1] = s->v[i];
    s->v[i].end = addr;
    s->v[i + 1].start = addr;
    s->v[i + 1].offset += addr - s->v[i].start;
    if (s->v[i + 1].file != 0 && split_hook != 0) {
        split_hook(&s->v[i + 1]);
    }
}

/* Splits regions so that start and end fall on region boundaries. */
static void cut(struct vma_set *s, virt_t start, virt_t end) {
    uint32_t i = first_after(s, start);
    if (i < s->n && s->v[i].start < start) {
        split_at(s, i, start);
    }
    i = first_after(s, end);
    if (i < s->n && s->v[i].start < end) {
        split_at(s, i, end);
    }
}

int vma_remove(struct vma_set *s, virt_t start, virt_t end,
               void (*gone)(void *ctx, const struct vma *v), void *ctx) {
    if (!aligned_range(start, end)) {
        return KERR_INVAL;
    }
    if (s->n + splits_needed(s, start, end) > VMA_MAX) {
        return KERR_NOMEM;
    }
    cut(s, start, end);
    for (uint32_t i = first_after(s, start); i < s->n && s->v[i].start < end;) {
        if (gone != 0) {
            gone(ctx, &s->v[i]);
        }
        close_slot(s, i);
    }
    return KERR_OK;
}

/* True when every byte of [start, end) is inside some region. */
static bool fully_mapped(const struct vma_set *s, virt_t start, virt_t end) {
    virt_t at = start;
    for (uint32_t i = first_after(s, start); i < s->n && at < end; i++) {
        if (s->v[i].start > at) {
            return false;
        }
        at = s->v[i].end;
    }
    return at >= end;
}

int vma_protect(struct vma_set *s, virt_t start, virt_t end, uint32_t prot) {
    if (!aligned_range(start, end)) {
        return KERR_INVAL;
    }
    if (!fully_mapped(s, start, end) || s->n + splits_needed(s, start, end) > VMA_MAX) {
        return KERR_NOMEM;
    }
    cut(s, start, end);
    for (uint32_t i = first_after(s, start); i < s->n && s->v[i].start < end; i++) {
        s->v[i].prot = prot;
    }
    merge_all(s);
    return KERR_OK;
}

bool vma_range_free(const struct vma_set *s, virt_t start, virt_t end) {
    uint32_t i;
    if (s == NULL) {
        return false;
    }
    i = first_after(s, start);
    return i == s->n || s->v[i].start >= end;
}

/* The highest address a len-byte gap inside [lo, hi) can start at,
 * scanning regions from the top, with the result aligned down to `align`.
 *
 * The alignment is applied **inside** the gap and then checked against the
 * gap's own lower edge, because aligning down can walk out of the bottom of
 * a gap that was only just big enough — which would hand out an address
 * overlapping the region below it. */
static int highest_gap(const struct vma_set *s, virt_t len, virt_t lo, virt_t hi, virt_t align,
                       virt_t *addr) {
    /* 64 bits, like everything else here. This held `hi` in a `uint32_t`,
     * which on a 39-bit address space silently searched the bottom 4 GiB of
     * a window that starts at 128 GiB — so every search failed and every
     * `mmap` with it. The widening is the whole reason this file was never
     * built on RISC-V, and a missed declaration reproduces the original
     * defect exactly. */
    virt_t top = hi;
    virt_t floor = lo;
    for (uint32_t i = s->n; i-- > 0;) {
        const struct vma *v = &s->v[i];
        if (v->start >= top) {
            continue;
        }
        if (v->end <= top && top - (v->end > lo ? v->end : lo) >= len) {
            floor = v->end > lo ? v->end : lo;
            break;
        }
        top = v->start;
        if (top < lo || top - lo < len) {
            return KERR_NOMEM;
        }
    }
    if (top < lo || top - lo < len) {
        return KERR_NOMEM;
    }
    *addr = top - len;
    if (align > 1u) {
        *addr &= ~(align - 1u);
        if (*addr < floor || *addr < lo) {
            return KERR_NOMEM;
        }
    }
    return KERR_OK;
}

int vma_find_free(const struct vma_set *s, virt_t len, virt_t hint, virt_t lo, virt_t hi,
                  virt_t align, virt_t *addr) {
    virt_t want = align > 1u ? align : VMA_PAGE;
    if (len == 0 || len % VMA_PAGE != 0 || hi <= lo || len > hi - lo) {
        return KERR_NOMEM;
    }
    if (hint % want == 0 && hint >= lo && hint <= hi - len && vma_range_free(s, hint, hint + len)) {
        *addr = hint;
        return KERR_OK;
    }
    return highest_gap(s, len, lo, hi, align, addr);
}
