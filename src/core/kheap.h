/* kheap — the kernel's small-object allocator.
 *
 * Objects up to KHEAP_MAX_SMALL bytes come from power-of-two size classes:
 * a page is carved into equal chunks and each class keeps a free list.
 * Anything larger takes whole pages. Every block carries an 8-byte header
 * (its class or page count, and a magic word), so kheap_free needs only the
 * pointer, and a pointer it did not hand out, or a double free, is caught
 * instead of corrupting a list. Objects are 8-byte aligned. Single core, IRQs
 * masked in the kernel: no lock (CLAUDE.md §2, concurrency). */
#ifndef MINIKERN_CORE_KHEAP_H
#define MINIKERN_CORE_KHEAP_H

#include <stddef.h>
#include <stdint.h>

#include "core/ports.h"

#define KHEAP_CLASSES   8u     /* 16, 32, ..., 2048-byte chunks */
#define KHEAP_MAX_SMALL 2040u  /* the largest chunk less its header */

struct kheap_chunk;

struct kheap {
    struct page_port pages;
    struct kheap_chunk *free_list[KHEAP_CLASSES];
    uint32_t live_bytes;  /* chunk/page bytes handed out, headers included */
};

void kheap_init(struct kheap *h, const struct page_port *pages);

/* n bytes, uninitialised, or NULL when out of memory or n is 0. */
void *kheap_alloc(struct kheap *h, size_t n);

/* n bytes, zeroed. */
void *kheap_zalloc(struct kheap *h, size_t n);

/* Returns KERR_OK, or KERR_INVAL for NULL-free-safe misuse: a pointer kheap
 * did not return, or one already freed. kheap_free(h, NULL) is KERR_OK. */
int kheap_free(struct kheap *h, void *p);

#endif
