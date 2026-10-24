#include "kernel/swap.h"

#include "kernel/mem.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "kernel/disk.h"
#include "kernel/xfsmount.h"

#define SWAP_PAGE_BYTES 4096u
#define SWAP_BITMAP_WORDS ((SWAP_MAX_SLOTS + 31u) / 32u)

static struct {
    /* Where each slot lives on the device, as a byte offset. Resolved once
     * at enable time, because the file's blocks do not move: swapping only
     * overwrites them. */
    uint64_t at[SWAP_MAX_SLOTS];
    uint32_t taken[SWAP_BITMAP_WORDS];
    uint32_t slots;
    uint32_t used;
    uint32_t hint; /* where the next search starts, so it is not always 0 */
    uint32_t swappiness;
    uint64_t written;
    uint64_t read_back;
    bool on;
} swap;

static bool slot_taken(uint32_t i) {
    return (swap.taken[i / 32u] >> (i % 32u)) & 1u;
}

static void take_slot(uint32_t i) {
    swap.taken[i / 32u] |= 1u << (i % 32u);
    swap.used++;
}

static void give_back_slot(uint32_t i) {
    if (!slot_taken(i)) {
        return; /* freeing a slot twice would make `used` a lie */
    }
    swap.taken[i / 32u] &= ~(1u << (i % 32u));
    swap.used--;
}

/* The first free slot from the hint onwards, or `SWAP_NO_SLOT`. */
static uint32_t find_slot(void) {
    for (uint32_t k = 0; k < swap.slots; k++) {
        uint32_t i = (swap.hint + k) % swap.slots;
        if (!slot_taken(i)) {
            swap.hint = i;
            return i;
        }
    }
    return SWAP_NO_SLOT;
}

int swap_enable(const char *path, uint32_t swappiness) {
    uint32_t pages = 0;
    int rc;
    if (swap.on) {
        return KERR_EXIST;
    }
    if (path == NULL || path[0] != '/') {
        return KERR_INVAL;
    }
    rc = xfsmount_map_file(path, swap.at, SWAP_MAX_SLOTS, &pages);
    if (rc != KERR_OK) {
        return rc;
    }
    if (pages == 0) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < SWAP_BITMAP_WORDS; i++) {
        swap.taken[i] = 0;
    }
    swap.slots = pages;
    swap.used = 0;
    swap.hint = 0;
    swap.written = 0;
    swap.read_back = 0;
    swap.swappiness = swappiness > 100u ? 100u : swappiness;
    swap.on = true;
    kprintf("swap: %s, %u pages (%u MiB), swappiness %u\n", path, pages,
            (uint32_t)((uint64_t)pages * SWAP_PAGE_BYTES / (1024u * 1024u)), swap.swappiness);
    return KERR_OK;
}

int swap_disable(void) {
    if (!swap.on) {
        return KERR_NODEV;
    }
    if (swap.used != 0) {
        /* **Refused rather than guessed at.** Turning swap off properly
         * means reading every page that is out back into the address space
         * that owns it, and this layer does not know those address spaces.
         * Forgetting them instead would leave processes with mappings that
         * point into a file nobody is reading any more. */
        return KERR_BUSY;
    }
    swap.on = false;
    swap.slots = 0;
    return KERR_OK;
}

bool swap_enabled(void) {
    return swap.on;
}

uint32_t swap_swappiness(void) {
    return swap.swappiness;
}

/* An absolute watermark, for a test. Zero means "derive it".
 *
 * A machine with 8 GiB cannot be put under real memory pressure by any
 * program this kernel can run, so the pressure has to be moved to the
 * program rather than the program to the pressure. This is the same kind of
 * instrument as `keepadvice` and the replay refusal: a knob that exists so
 * that a path which only happens on a full machine can be reached on an
 * empty one. */
static uint32_t watermark_override;

void swap_set_watermark(uint32_t pages) {
    watermark_override = pages;
}

uint32_t swap_watermark(void) {
    uint32_t total = mem_total_pages();
    if (watermark_override != 0) {
        return watermark_override;
    }
    /* **Scaled by swappiness, and zero at zero.** Swappiness is the
     * operator saying how willing the machine should be to page out, and
     * the one thing it must mean exactly is that *nothing* is paged out
     * unasked at zero — otherwise the knob does not include "off", which is
     * the setting people actually check for.
     *
     * The fraction is deliberately small: a watermark is the point at which
     * reclaim *starts*, not a target, so it wants to be far enough above
     * empty that a burst of allocation does not run the machine out while
     * pages are being written, and no further. A sixteenth of memory at the
     * highest setting is a few hundred megabytes on this board. */
    return (uint32_t)((uint64_t)total * swap_swappiness() / (16u * 127u));
}

void swap_set_swappiness(uint32_t swappiness) {
    swap.swappiness = swappiness > 100u ? 100u : swappiness;
}

int swap_out(phys_t page, uint32_t *slot) {
    struct bcache *cache = disk_cache();
    uint32_t chosen;
    int rc;
    if (!swap.on || cache == NULL) {
        return KERR_NODEV;
    }
    chosen = find_slot();
    if (chosen == SWAP_NO_SLOT) {
        return KERR_NOSPC;
    }
    /* Through the block cache, which is also what reads it back — so a page
     * still sitting dirty in the cache is read back correctly. Swap needs
     * the page to survive until it is wanted again, not to survive a crash:
     * there is nothing to recover a swapped page *into* after a reboot. */
    rc = bcache_pwrite(cache, swap.at[chosen], (const void *)(uintptr_t)page, SWAP_PAGE_BYTES);
    if (rc != KERR_OK) {
        return rc;
    }
    take_slot(chosen);
    swap.written++;
    *slot = chosen;
    return KERR_OK;
}

int swap_in(uint32_t slot, phys_t page) {
    struct bcache *cache = disk_cache();
    int rc;
    if (!swap.on || cache == NULL) {
        return KERR_NODEV;
    }
    if (slot >= swap.slots || !slot_taken(slot)) {
        /* A slot nobody wrote. The caller found this number in a page table
         * entry, so being asked for one that was never taken means the
         * entry was not a swap entry — which is worth refusing loudly
         * rather than serving whatever is in the file at that offset. */
        return KERR_INVAL;
    }
    rc = bcache_pread(cache, swap.at[slot], (void *)(uintptr_t)page, SWAP_PAGE_BYTES);
    if (rc != KERR_OK) {
        return rc;
    }
    give_back_slot(slot);
    swap.read_back++;
    return KERR_OK;
}

void swap_release(uint32_t slot) {
    if (swap.on && slot < swap.slots) {
        give_back_slot(slot);
    }
}

void swap_report(uint32_t *slots, uint32_t *used, uint64_t *written, uint64_t *read_back) {
    *slots = swap.slots;
    *used = swap.used;
    *written = swap.written;
    *read_back = swap.read_back;
}
