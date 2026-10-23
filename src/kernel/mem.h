/* mem — the kernel's live memory: physical frames, and objects.
 *
 * `core/pmm.h` and `core/kheap.h` are pure and host-tested; this is the one
 * instance of each, built over the memory the device tree described, with
 * everything that must not be handed out reserved first.
 *
 * **Every page starts out used**, and the boot code releases what is really
 * free. A forgotten reservation therefore loses memory rather than handing
 * out memory something else is already using, which is the only direction
 * this kind of mistake may fail in.
 */
#ifndef MINIKERN_KERNEL_MEM_H
#define MINIKERN_KERNEL_MEM_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bootinfo.h"
#include "core/pmm.h"
#include "core/ports.h"
#include "core/types.h"

/* Builds the frame allocator and the heap over the RAM the boot info
 * describes, reserving the kernel image, the device tree and the page-table
 * arena. Returns KERR_OK, or KERR_NOMEM when the bitmap itself does not fit
 * in the space reserved for it. */
int mem_init(const struct bootinfo *info, phys_t arena_base, uint64_t arena_bytes);

/* Frames. `align_pages` of 0 or 1 means any alignment. */
int mem_alloc_pages(uint32_t npages, uint32_t align_pages, phys_t *phys);

/* The frame allocator itself, for the counters it keeps about its own
 * answers — see `pmm_double_allocations`. */
const struct pmm *mem_frames(void);

/* Times two harts were inside the frame allocator at once. The kernel lock
 * is what makes that impossible; any value but zero means it does not. */
uint32_t mem_allocator_collisions(void);

/* Pages the kernel heap has taken from the frame allocator and given back.
 * The heap and user mappings draw on one pool, so a page the heap gives
 * back while something still points into it returns as somebody's fresh
 * anonymous memory. */
void mem_heap_traffic(uint64_t *taken, uint64_t *given_back);

/* Stops the kernel heap returning pages to the frame allocator, so that a
 * page it has finished with can never become a user page. It leaks, and it
 * is a diagnostic: see `mem.c`. */
void mem_heap_keep_pages(bool on);
int mem_free_pages(phys_t phys, uint32_t npages);

/* Says that one more address space is using this frame, so freeing it once
 * does not take it away from the other. See `pmm_share`. */
int mem_share_page(phys_t phys);

/* How many address spaces hold a frame. 1 for an ordinary allocation. */
uint32_t mem_page_shares(phys_t phys);

/* Kernel objects, from the heap over those frames. */
void *kmalloc(uint32_t bytes);
void *kzalloc(uint32_t bytes);
void kfree(void *p);

/* What is left, in whole pages, and what there was to begin with. For the
 * boot log and for a test that wants to prove nothing leaked. */
uint32_t mem_free_pages_count(void);
uint32_t mem_total_pages(void);

/* The page port the heap draws from, for anything else that needs frames
 * through an interface rather than directly. */
struct page_port mem_page_port(void);

#endif
