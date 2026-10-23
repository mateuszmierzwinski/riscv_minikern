#include "kernel/mem.h"

#include <stddef.h>

#include "arch/riscv/pgtable.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/kheap.h"
#include "core/pmm.h"

extern char __kernel_start[];
extern char __kernel_end[];

/* The bitmap lives in the kernel's .bss rather than in the memory it
 * describes, which keeps the bootstrap honest: there is no moment where the
 * allocator is allocating the storage it needs to exist.
 *
 * 8 GiB of 4 KiB frames is 2 Mi frames, which is 256 KiB of bitmap. That is
 * sized for the largest VisionFive 2 so the same image runs on every one of
 * them; a board with less simply uses less of it. */
#define MAX_RAM_BYTES (8ull * 1024 * 1024 * 1024)
#define MAX_FRAMES (MAX_RAM_BYTES / PMM_PAGE_SIZE)

/* One share count per frame; see `pmm_set_refcounts`. At a byte each this
 * is 2 MiB for the 8 GiB a board can carry, which is the price of being able
 * to share a page at all. */
static uint8_t frame_refs[MAX_FRAMES];

static uint32_t frame_bits[PMM_BITMAP_WORDS(MAX_FRAMES)];
static struct pmm frames;
static struct kheap heap;
static uint32_t total_pages;

/* The heap asks for pages through a port, so it has no idea what is under
 * it — which is what let it be host-tested before any of this existed. */
static int heap_alloc_page(void *self, uint32_t npages, void **virt) {
    phys_t phys = 0;
    int rc;
    (void)self;
    rc = pmm_alloc(&frames, npages, 1, &phys);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The kernel's map is an identity map, so a frame's physical address is
     * also its address. When the kernel moves to the top half this becomes
     * an addition, and it is the only line that has to change. */
    *virt = (void *)(uintptr_t)phys;
    return KERR_OK;
}

static void heap_free_page(void *self, void *virt, uint32_t npages) {
    (void)self;
    (void)pmm_free(&frames, (phys_t)(uintptr_t)virt, npages);
}

struct page_port mem_page_port(void) {
    struct page_port p = {.alloc = heap_alloc_page, .free = heap_free_page, .self = NULL};
    return p;
}

/* What must never be handed out.
 *
 * **Everything below the kernel's load address belongs to the firmware.**
 * OpenSBI loads itself at the base of RAM and the kernel 2 MiB above it, so
 * that gap is the firmware's text, data and stacks — and it is still running:
 * every SBI call this kernel makes, including the one that prints, executes
 * there. Handing those frames out meant the virtio driver's queue landed on
 * top of the firmware, and the store was refused by the physical memory
 * protection OpenSBI sets up to defend itself. That refusal is luck: on a
 * machine with no such protection the write would have landed, and the
 * machine would have failed later, somewhere else, for no visible reason.
 *
 * The rule is therefore positional and needs no agreement with the
 * firmware: the kernel knows where it was loaded, and anything in RAM below
 * that was put there by whoever loaded it.
 *
 * Also reserved: the kernel image itself, the page-table arena, and the
 * device tree the firmware left in RAM — the command line still points into
 * it. */
static void reserve_what_is_in_use(const struct bootinfo *info, phys_t arena_base,
                                   uint64_t arena_bytes) {
    phys_t kernel_lo = (phys_t)(uintptr_t)__kernel_start;
    phys_t kernel_hi = (phys_t)(uintptr_t)__kernel_end;
    if (info->mem_base < kernel_lo) {
        pmm_reserve(&frames, info->mem_base, kernel_lo - info->mem_base);
    }
    pmm_reserve(&frames, kernel_lo, kernel_hi - kernel_lo);
    pmm_reserve(&frames, arena_base, arena_bytes);
    if (info->block_size != 0) {
        /* The blob's own address is not in the boot info — what is kept is
         * its size — so the arena's neighbour above it is reserved
         * generously rather than precisely. A frame lost is better than a
         * frame handed out twice. */
        pmm_reserve(&frames, arena_base + arena_bytes, info->block_size);
    }
}

int mem_init(const struct bootinfo *info, phys_t arena_base, uint64_t arena_bytes) {
    uint64_t pages = info->mem_size / PMM_PAGE_SIZE;
    if (pages == 0) {
        return KERR_INVAL;
    }
    if (pages > MAX_FRAMES) {
        kprintf("mem: %llu MiB is more than this build tracks; using %llu MiB\n",
                (unsigned long long)(info->mem_size / (1024u * 1024u)),
                (unsigned long long)(MAX_RAM_BYTES / (1024u * 1024u)));
        pages = MAX_FRAMES;
    }
    total_pages = (uint32_t)pages;
    pmm_init(&frames, info->mem_base, total_pages, frame_bits);
    /* **Somewhere to count shares**, before any frame is handed out.
     *
     * A frame that two address spaces hold — because `fork` gave them the
     * same page to copy on write, or because they asked for shared memory —
     * must not be freed when the first lets go. One byte per frame buys
     * that, and it has to exist before the first allocation or the counts
     * start out wrong. */
    pmm_set_refcounts(&frames, frame_refs);
    pmm_release(&frames, info->mem_base, (uint64_t)total_pages * PMM_PAGE_SIZE);
    reserve_what_is_in_use(info, arena_base, arena_bytes);
    kheap_init(&heap, &(struct page_port){.alloc = heap_alloc_page, .free = heap_free_page});
    return KERR_OK;
}

int mem_alloc_pages(uint32_t npages, uint32_t align_pages, phys_t *phys) {
    return pmm_alloc(&frames, npages, align_pages, phys);
}

int mem_free_pages(phys_t phys, uint32_t npages) {
    return pmm_free(&frames, phys, npages);
}

int mem_share_page(phys_t phys) {
    return pmm_share(&frames, phys);
}

uint32_t mem_page_shares(phys_t phys) {
    return pmm_shares(&frames, phys);
}

void *kmalloc(uint32_t bytes) {
    return kheap_alloc(&heap, bytes);
}

void *kzalloc(uint32_t bytes) {
    return kheap_zalloc(&heap, bytes);
}

void kfree(void *p) {
    (void)kheap_free(&heap, p);
}

uint32_t mem_free_pages_count(void) {
    return frames.free;
}

uint32_t mem_total_pages(void) {
    return total_pages;
}
