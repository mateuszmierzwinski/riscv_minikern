#include "arch/riscv/mmu.h"

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

/* The kernel's own image: code read-execute, the rest read-write. */
static int map_kernel_image(struct pgtable *pt) {
    uint64_t text = page_down((uint64_t)(uintptr_t)__kernel_start);
    uint64_t text_end = page_up((uint64_t)(uintptr_t)__text_end);
    uint64_t data_end = page_up((uint64_t)(uintptr_t)__kernel_end);
    int rc = pgtable_map_range(pt, text, text, text_end - text, PT_PROT_RX);
    return rc != KERR_OK ? rc
                         : pgtable_map_range(pt, text_end, text_end, data_end - text_end, PT_PROT_RW);
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

int mmu_init(phys_t ram_base, uint64_t ram_bytes) {
    struct pt_mem mem = {.alloc = arena_alloc, .free = arena_free, .to_virt = arena_to_virt};
    int rc = pgtable_create(&kernel_table, &mem);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = map_kernel_image(&kernel_table);
    rc = rc != KERR_OK ? rc : map_remaining_ram(&kernel_table, ram_base, ram_bytes);
    if (rc != KERR_OK) {
        /* Translation is still off and the machine is as it was. */
        return rc;
    }
    enable_translation(pgtable_satp(&kernel_table, 0));
    enabled = true;
    return KERR_OK;
}

int mmu_map_device(phys_t base, uint64_t bytes) {
    uint64_t at = page_down(base);
    uint64_t end = page_up(base + bytes);
    if (!enabled) {
        return KERR_OK; /* with translation off, the registers are already there */
    }
    for (; at < end; at += PAGE_SIZE) {
        phys_t already = 0;
        uint64_t prot = 0, size = 0;
        int rc;
        if (pgtable_lookup(&kernel_table, at, &already, &prot, &size)) {
            continue; /* mapped already, by an earlier call or by the RAM map */
        }
        rc = pgtable_map(&kernel_table, at, at, PAGE_SIZE, PT_PROT_RW);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    /* A mapping the implementation has not seen before needs no fence, but
     * one that replaces a cached non-mapping does, and there is no way to
     * tell which this was. */
    __asm__ volatile("sfence.vma zero, zero" : : : "memory");
    return KERR_OK;
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
