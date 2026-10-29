#include "kernel/mem.h"

#include "arch/riscv/mmu.h"

#include <stddef.h>

#include "arch/riscv/pgtable.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/kheap.h"
#include "core/pmm.h"


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
/* Whether a frame is ever handed out a second time.
 *
 * **A diagnostic that splits the §0 hunt in half.** The corruption looks
 * like a page with two owners, and the two ways that can happen are a frame
 * mapped at two addresses — which an audit of the whole address space says
 * never happens — and a frame serving two *purposes*: a user page that is
 * also a thread's kernel stack, a page table, or a block of the kernel
 * heap. The second kind is invisible to that audit, because the kernel
 * reaches its own memory through the identity map and not through a user
 * entry.
 *
 * Both require **reuse**. With this on, nothing is ever reused: a free is a
 * leak. If the corruption survives that, no frame ever serves two purposes
 * and the whole aliasing family is wrong. 2 GiB is far more than this
 * reproducer allocates, so leaking everything costs nothing but honesty. */
static bool never_reuse;

void mem_never_reuse(bool on) {
    never_reuse = on;
}

/* Pages the kernel heap has taken from the frame allocator and given back.
 *
 * **The frames the heap gives back go to user pages**, which makes the heap
 * a second owner of the same pool: a page freed while something still holds
 * a pointer into it comes back as somebody's fresh anonymous memory, full
 * of whatever the kernel last wrote there. The user-side counter
 * (`uspace_freed_frames`) says "no frames freed" and means only "no
 * *mappings* went away", which is how this side stayed invisible. */
static uint64_t heap_pages_taken;
static uint64_t heap_pages_given_back;

void mem_heap_traffic(uint64_t *taken, uint64_t *given_back) {
    *taken = heap_pages_taken;
    *given_back = heap_pages_given_back;
}

static int heap_alloc_page(void *self, uint32_t npages, void **virt) {
    phys_t phys = 0;
    int rc;
    (void)self;
    rc = pmm_alloc(&frames, npages, 1, &phys);
    if (rc != KERR_OK) {
        return rc;
    }
    heap_pages_taken += npages;
    /* **The direct map, which is the addition that comment predicted.** It
     * was not quite "the only line": the drivers that hand memory to
     * hardware had the same assumption baked in, each in its own way, and
     * the compiler could not see any of them because an identity map makes
     * a physical address and a pointer the same number. */
    *virt = mmu_phys_to_virt(phys);
    return KERR_OK;
}

static bool heap_keeps_its_pages;

void mem_heap_keep_pages(bool on) {
    heap_keeps_its_pages = on;
}

static void heap_free_page(void *self, void *virt, uint32_t npages) {
    (void)self;
    heap_pages_given_back += npages;
    if (heap_keeps_its_pages || never_reuse) {
        /* Leaked on purpose: a page the heap never gives back cannot be
         * handed to a user mapping, so if the corruption stops when this is
         * on, what reaches user memory is a write through a pointer into
         * freed *kernel* memory. 1.5 MiB on a 2 GiB machine is a cheap
         * answer to that question. */
        return;
    }
    (void)pmm_free(&frames, mmu_virt_to_phys(virt), npages);
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
    /* **The physical bounds, asked for rather than taken from the linker
     * symbols.** Those read as the load address while the kernel executes
     * there and as its own high address afterwards — and this runs
     * afterwards. Reserving the high range left every physical page looking
     * taken: "0 of 2097152 pages free", on a machine with eight gigabytes.
     *
     * That the first symptom was total rather than partial was luck. A
     * reservation that had merely been the wrong *size* would have handed
     * the kernel's own pages to a process. */
    phys_t kernel_lo = 0;
    uint64_t kernel_bytes = 0;
    phys_t kernel_hi;
    mmu_kernel_phys_range(&kernel_lo, &kernel_bytes);
    kernel_hi = kernel_lo + kernel_bytes;
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

void mem_reserve_range(phys_t base, uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    pmm_reserve(&frames, base, bytes);
}

phys_t mem_base_address(void) {
    return frames.base;
}

const struct pmm *mem_frames(void) {
    return &frames;
}

/* **Is anybody else in here?**
 *
 * The frame allocator's bitmap is a read-modify-write on a shared word, and
 * what makes that safe is the kernel lock: one hart inside the kernel at a
 * time. Every theory about the four-hart race that ends "but the lock
 * prevents that" rests on it, and nothing had ever checked it — so this
 * does, at the one place where a lost update means a frame with two owners.
 *
 * A flag rather than a count, and set with an exchange rather than a store:
 * the question is only ever "was it already taken?". */
static uint32_t inside_the_allocator;
static uint32_t allocator_collisions;

uint32_t mem_allocator_collisions(void) {
    return allocator_collisions;
}

static bool enter_the_allocator(void) {
    if (__atomic_exchange_n(&inside_the_allocator, 1u, __ATOMIC_ACQ_REL) == 0u) {
        return true;
    }
    allocator_collisions++;
    return false;
}

static void leave_the_allocator(bool mine) {
    if (mine) {
        __atomic_store_n(&inside_the_allocator, 0u, __ATOMIC_RELEASE);
    }
}

int mem_alloc_pages(uint32_t npages, uint32_t align_pages, phys_t *phys) {
    bool mine = enter_the_allocator();
    int rc = pmm_alloc(&frames, npages, align_pages, phys);
    leave_the_allocator(mine);
    return rc;
}

int mem_free_pages(phys_t phys, uint32_t npages) {
    bool mine;
    int rc;
    if (never_reuse) {
        return KERR_OK;
    }
    mine = enter_the_allocator();
    rc = pmm_free(&frames, phys, npages);
    leave_the_allocator(mine);
    return rc;
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
