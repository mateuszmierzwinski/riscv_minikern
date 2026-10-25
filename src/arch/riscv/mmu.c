#include "arch/riscv/mmu.h"

#include "arch/riscv/layout.h"

#include <stddef.h>

#include "arch/riscv/sbi.h"
#include "core/console.h"
#include "core/errno.h"

/* Provided by the linker script (arch/riscv/link.ld). */
extern char __kernel_start[];
extern char __text_end[];
extern char __kernel_end[];

/* A bump allocator over the memory above the kernel image. It never frees,
 * which is correct for the kernel's own map: that map lives as long as the
 * machine does. A process's tables will come from the page allocator
 * instead, once there is one. */
static struct {
    phys_t base;
    uint64_t bytes;
    uint64_t at;
    uint32_t used;
} arena;

static struct pgtable kernel_table;
static bool enabled;
/* Where RAM begins, so the direct map is a fixed offset from it. Recorded
 * at `mmu_init` because the board's base is read from the device tree and
 * is not the same as QEMU's. */
static phys_t ram_origin;
static uint64_t ram_span;
/* Where the next device mapping goes in the MMIO window. Bump-allocated:
 * device registers are mapped once at probe and never unmapped, so there is
 * nothing to reclaim and a bump pointer is the whole allocator. */
static uint64_t device_next;

void mmu_set_table_arena(phys_t base, uint64_t bytes) {
    arena.base = base;
    arena.bytes = bytes;
    arena.at = 0;
    arena.used = 0;
}

/* Table memory. Identity-mapped while translation is off, and still
 * identity-mapped afterwards, so one pointer is valid throughout — which is
 * what makes the switch survivable. */
static int arena_alloc(void *self, void **virt, phys_t *phys) {
    (void)self;
    if (arena.at + PT_BYTES > arena.bytes) {
        return KERR_NOMEM;
    }
    *phys = arena.base + arena.at;
    *virt = (void *)(uintptr_t)*phys;
    arena.at += PT_BYTES;
    arena.used++;
    for (uint32_t i = 0; i < PT_BYTES; i++) {
        ((volatile uint8_t *)*virt)[i] = 0;
    }
    return KERR_OK;
}

static void arena_free(void *self, void *virt) {
    (void)self;
    (void)virt; /* the kernel's own map is never torn down */
}

static void *arena_to_virt(void *self, phys_t phys) {
    (void)self;
    return (void *)(uintptr_t)phys;
}

/* Rounds a physical address down and a length up to whole pages, because a
 * mapping that covers part of a page covers all of it whether or not the
 * caller meant that. */
static uint64_t page_down(uint64_t v) {
    return v & ~(PAGE_SIZE - 1u);
}

static uint64_t page_up(uint64_t v) {
    return (v + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
}

/* Where the kernel's bytes physically are.
 *
 * **`__kernel_start` is a high symbol read through a PC-relative
 * instruction**, so while the kernel is still executing at its load address
 * this yields that load address — which is exactly what is wanted here and
 * is *not* what it yields after the jump. Everything in this file that
 * takes a linker symbol runs before the jump, and that is why it can. */
static uint64_t kernel_phys_base(void) {
    return page_down((uint64_t)(uintptr_t)__kernel_start);
}

/* The image's size, as a difference of two linker symbols — which is a
 * link-time constant and so means the same thing in either phase. */
static uint64_t kernel_text_bytes(void) {
    return page_up((uint64_t)(uintptr_t)__text_end) - kernel_phys_base();
}

static uint64_t kernel_total_bytes(void) {
    return page_up((uint64_t)(uintptr_t)__kernel_end) - kernel_phys_base();
}

/* The kernel's own image: code read-execute, the rest read-write.
 *
 * **Mapped twice, on purpose.** Once at the address it is linked for, in
 * the kernel's own half, which is where it will run; and once identity, so
 * that the instruction after `satp` is written is still fetchable. The
 * identity half is what the jump leaves behind, and it is dropped
 * separately once nothing is holding a low address. */
static int map_kernel_image(struct pgtable *pt) {
    uint64_t phys = kernel_phys_base();
    uint64_t text = kernel_text_bytes();
    uint64_t all = kernel_total_bytes();
    uint64_t high = KERNEL_VIRT_BASE + KERNEL_VIRT_OFFSET;
    int rc = pgtable_map_range(pt, high, phys, text, PT_PROT_RX);
    rc = rc != KERR_OK ? rc
                       : pgtable_map_range(pt, high + text, phys + text, all - text, PT_PROT_RW);
    rc = rc != KERR_OK ? rc : pgtable_map_range(pt, phys, phys, text, PT_PROT_RX);
    return rc != KERR_OK
               ? rc
               : pgtable_map_range(pt, phys + text, phys + text, all - text, PT_PROT_RW);
}

/* How far a low kernel address has to move to become its high self.
 *
 * Valid only before the jump, for the same reason `kernel_phys_base` is. */
uint64_t mmu_high_offset(void) {
    return (KERNEL_VIRT_BASE + KERNEL_VIRT_OFFSET) - kernel_phys_base();
}

/* Where the kernel's image physically is, **recorded before the jump**.
 *
 * `__kernel_start` is read through a PC-relative instruction, so it means
 * the load address while the kernel is executing there and its own high
 * address afterwards. Anything that needs the *physical* bounds after the
 * jump — the page allocator, which must not hand out the kernel's own
 * pages — has to ask for them rather than take the symbol.
 *
 * The first thing this broke was loud, which was luck: the allocator
 * reserved a high range that does not intersect RAM, decided every page was
 * taken, and reported "0 of 2097152 pages free". A reservation that had
 * merely been the wrong size would have handed out the kernel's own memory
 * and corrupted something days later. */
static phys_t image_base;
static uint64_t image_bytes;

void mmu_kernel_phys_range(phys_t *base, uint64_t *bytes) {
    *base = image_base;
    *bytes = image_bytes;
}

/* Everything else in RAM, read-write and not executable: the device tree the
 * firmware left there, the table arena, and whatever the page allocator will
 * hand out later. Split around the kernel image, which is already mapped. */
static int map_remaining_ram(struct pgtable *pt, phys_t base, uint64_t bytes) {
    uint64_t kernel_lo = page_down((uint64_t)(uintptr_t)__kernel_start);
    uint64_t kernel_hi = page_up((uint64_t)(uintptr_t)__kernel_end);
    int rc = KERR_OK;
    if (base < kernel_lo) {
        rc = pgtable_map_range(pt, base, base, kernel_lo - base, PT_PROT_RW);
    }
    if (rc == KERR_OK && kernel_hi < base + bytes) {
        rc = pgtable_map_range(pt, kernel_hi, kernel_hi, base + bytes - kernel_hi, PT_PROT_RW);
    }
    return rc;
}

/* The switch itself.
 *
 * `sfence.vma` after the write, because the implementation is allowed to
 * have cached translations from before — and on a machine where it has
 * cached none, the fence costs nothing. The write and the fence are one
 * statement so that nothing can be scheduled between them. */
static void enable_translation(uint64_t satp) {
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero" : : "r"(satp) : "memory");
}

/* All of RAM, once more, in the direct-map window.
 *
 * Read-write and **never executable**: the kernel's code is reachable
 * through its own mapping, and a second executable alias of it would be a
 * second place from which to run it — which is the kind of thing a
 * write-what-where bug turns into an exploit. */
static int map_direct_window(struct pgtable *pt, phys_t base, uint64_t bytes) {
    return pgtable_map_range(pt, DIRECT_MAP_BASE, base, bytes, PT_PROT_RW);
}

int mmu_init(phys_t ram_base, uint64_t ram_bytes) {
    struct pt_mem mem = {.alloc = arena_alloc, .free = arena_free, .to_virt = arena_to_virt};
    int rc = pgtable_create(&kernel_table, &mem);
    if (rc != KERR_OK) {
        return rc;
    }
    ram_origin = ram_base;
    ram_span = ram_bytes;
    /* Before any mapping, and so before the jump: this is the only moment
     * the linker symbols still mean the load address. */
    image_base = (phys_t)kernel_phys_base();
    image_bytes = kernel_total_bytes();
    rc = map_kernel_image(&kernel_table);
    rc = rc != KERR_OK ? rc : map_remaining_ram(&kernel_table, ram_base, ram_bytes);
    rc = rc != KERR_OK ? rc : map_direct_window(&kernel_table, ram_base, ram_bytes);
    if (rc != KERR_OK) {
        /* Translation is still off and the machine is as it was. */
        return rc;
    }
    enable_translation(pgtable_satp(&kernel_table, 0));
    enabled = true;
    return KERR_OK;
}

void *mmu_phys_to_virt(phys_t phys) {
    /* **Before translation is on, a physical address is already usable.**
     * The early boot path reaches the device tree and the table arena this
     * way, and returning a direct-map address then would be a pointer into
     * a mapping that does not exist yet. */
    if (!enabled) {
        return (void *)(uintptr_t)phys;
    }
    return (void *)(uintptr_t)(DIRECT_MAP_BASE + (phys - ram_origin));
}

phys_t mmu_virt_to_phys(const void *virt) {
    uint64_t v = (uint64_t)(uintptr_t)virt;
    if (v >= DIRECT_MAP_BASE && v < DIRECT_MAP_BASE + ram_span) {
        return (phys_t)(ram_origin + (v - DIRECT_MAP_BASE));
    }
    /* **A kernel-image address, which is also RAM.** A DMA buffer that is a
     * static array lives in `.bss`, so this is not a corner case: it is how
     * every driver's descriptor ring is addressed. */
    if (v >= KERNEL_VIRT_BASE + KERNEL_VIRT_OFFSET && v < DIRECT_MAP_BASE) {
        return (phys_t)(ram_origin + KERNEL_VIRT_OFFSET + (v - KERNEL_VIRT_BASE -
                                                           KERNEL_VIRT_OFFSET));
    }
    /* Identity, which is what every address is until the kernel moves. */
    return (phys_t)v;
}

void *mmu_map_device(phys_t base, uint64_t bytes) {
    uint64_t at = page_down(base);
    uint64_t end = page_up(base + bytes);
    uint64_t span = end - at;
    uint64_t window;
    if (!enabled) {
        /* With translation off the registers are already where they are. */
        return (void *)(uintptr_t)base;
    }
    /* **Mapped into the MMIO window, not identity.** An identity mapping of
     * a device puts it in the user half of the address space, where it is
     * both a collision risk for a process's own mappings and an address a
     * kernel pointer check cannot tell from a user one. */
    window = DEVICE_MAP_BASE + device_next;
    for (uint64_t off = 0; off < span; off += PAGE_SIZE) {
        phys_t already = 0;
        uint64_t prot = 0, size = 0;
        if (pgtable_lookup(&kernel_table, window + off, &already, &prot, &size)) {
            continue; /* an earlier call mapped it */
        }
        if (pgtable_map(&kernel_table, window + off, at + off, PAGE_SIZE, PT_PROT_RW) !=
            KERR_OK) {
            return NULL;
        }
    }
    device_next += span;
    /* A mapping the implementation has not seen before needs no fence, but
     * one that replaces a cached non-mapping does, and there is no way to
     * tell which this was. */
    __asm__ volatile("sfence.vma zero, zero" : : : "memory");
    /* The offset within the first page is carried over, so a caller that
     * asked for a register block starting part way into a page gets a
     * pointer to its registers rather than to the page. */
    return (void *)(uintptr_t)(window + (base - at));
}

struct pgtable *mmu_kernel_table(void) {
    return &kernel_table;
}

bool mmu_enabled(void) {
    return enabled;
}

uint32_t mmu_table_pages_used(void) {
    return arena.used;
}

void mmu_flush_tlb_local(void) {
    __asm__ volatile("sfence.vma zero, zero" : : : "memory");
}

void mmu_flush_tlb_everywhere(void) {
    /* This hart, then the rest. The local fence is not redundant: the
     * firmware call covers the other harts, and an implementation may cache
     * the absence of a translation, so even a newly *added* mapping needs
     * one here. */
    __asm__ volatile("sfence.vma zero, zero" : : : "memory");
    if (sbi_has_rfence()) {
        /* `hart_mask_base` of -1 means every hart; a size of -1 means the
         * whole address space. A subset would be wrong: a thread of this
         * process can be running on any hart. */
        (void)sbi_remote_sfence_vma(0, (uint64_t)-1, 0, (uint64_t)-1);
    }
}

bool mmu_can_flush_remotely(void) {
    return sbi_has_rfence();
}
