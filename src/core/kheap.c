#include "core/kheap.h"

#include "core/errno.h"
#include "core/pmm.h"

#define KHEAP_MIN_SHIFT   4u           /* 16-byte smallest chunk */
#define KHEAP_MAGIC_LIVE  0x6b4d4c56u  /* "kMLV" */
#define KHEAP_MAGIC_FREE  0x6b4d4652u  /* "kMFR" */
#define KHEAP_PAGES_FLAG  0x80000000u  /* header size field: a page-run block */

/* Precedes every block. size is the class index, or FLAG | page count. */
struct kheap_header {
    uint32_t size;
    uint32_t magic;
};

/* A free chunk: its header, then the list link where the object was. */
struct kheap_chunk {
    struct kheap_header hdr;
    struct kheap_chunk *next;
};

static uint32_t class_bytes(uint32_t c) {
    return 1u << (c + KHEAP_MIN_SHIFT);
}

/* The smallest class whose chunk holds n bytes plus the header. */
static uint32_t class_for(size_t n) {
    uint32_t c = 0;
    while (class_bytes(c) < n + sizeof(struct kheap_header)) {
        c++;
    }
    return c;
}

/* Carves one fresh page into chunks of class c onto its free list. */
static int refill(struct kheap *h, uint32_t c) {
    void *page;
    uint32_t size = class_bytes(c);
    if (h->pages.alloc(h->pages.self, 1, &page) != KERR_OK) {
        return KERR_NOMEM;
    }
    for (uint32_t off = 0; off + size <= PMM_PAGE_SIZE; off += size) {
        struct kheap_chunk *k = (struct kheap_chunk *)((uint8_t *)page + off);
        k->hdr = (struct kheap_header){.size = c, .magic = KHEAP_MAGIC_FREE};
        k->next = h->free_list[c];
        h->free_list[c] = k;
    }
    return KERR_OK;
}

static void *alloc_small(struct kheap *h, size_t n) {
    uint32_t c = class_for(n);
    struct kheap_chunk *k;
    if (h->free_list[c] == NULL && refill(h, c) != KERR_OK) {
        return NULL;
    }
    k = h->free_list[c];
    h->free_list[c] = k->next;
    k->hdr.magic = KHEAP_MAGIC_LIVE;
    h->live_bytes += class_bytes(c);
    return (uint8_t *)k + sizeof(struct kheap_header);
}

static void *alloc_pages(struct kheap *h, size_t n) {
    uint32_t npages = (uint32_t)((n + sizeof(struct kheap_header) + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE);
    struct kheap_header *hdr;
    void *mem;
    if (h->pages.alloc(h->pages.self, npages, &mem) != KERR_OK) {
        return NULL;
    }
    hdr = mem;
    *hdr = (struct kheap_header){.size = KHEAP_PAGES_FLAG | npages, .magic = KHEAP_MAGIC_LIVE};
    h->live_bytes += npages * PMM_PAGE_SIZE;
    return hdr + 1;
}

void kheap_init(struct kheap *h, const struct page_port *pages) {
    *h = (struct kheap){.pages = *pages};
}

void *kheap_alloc(struct kheap *h, size_t n) {
    if (n == 0 || n > 0x10000000u) {
        return NULL;
    }
    return n <= KHEAP_MAX_SMALL ? alloc_small(h, n) : alloc_pages(h, n);
}

void *kheap_zalloc(struct kheap *h, size_t n) {
    uint8_t *p = kheap_alloc(h, n);
    for (size_t i = 0; p != NULL && i < n; i++) {
        p[i] = 0;
    }
    return p;
}

int kheap_free(struct kheap *h, void *p) {
    struct kheap_header *hdr = (struct kheap_header *)p - 1;
    if (p == NULL) {
        return KERR_OK;
    }
    if (((uintptr_t)p & 7u) != 0 || hdr->magic != KHEAP_MAGIC_LIVE) {
        return KERR_INVAL;
    }
    if (hdr->size & KHEAP_PAGES_FLAG) {
        uint32_t npages = hdr->size & ~KHEAP_PAGES_FLAG;
        hdr->magic = KHEAP_MAGIC_FREE;
        h->live_bytes -= npages * PMM_PAGE_SIZE;
        h->pages.free(h->pages.self, hdr, npages);
        return KERR_OK;
    }
    if (hdr->size >= KHEAP_CLASSES) {
        return KERR_INVAL;
    }
    hdr->magic = KHEAP_MAGIC_FREE;
    ((struct kheap_chunk *)hdr)->next = h->free_list[hdr->size];
    h->free_list[hdr->size] = (struct kheap_chunk *)hdr;
    h->live_bytes -= class_bytes(hdr->size);
    return KERR_OK;
}
