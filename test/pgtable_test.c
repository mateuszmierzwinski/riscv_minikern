/* Sv39 arithmetic, on the host, with ordinary memory standing in for tables.
 *
 * This is the test the ARM project's equivalent earned its place by: it
 * caught a descriptor-type check that missed every non-executable page,
 * which on hardware would have been an unexplained fault storm. The point is
 * to be wrong here, where a failure prints a line, rather than on a board,
 * where it prints nothing.
 */
#include "arch/riscv/pgtable.h"

#include <stdint.h>
#include <string.h>

#include "core/errno.h"
#include "ktest.h"

/* A page source over a static arena. The physical address is the offset
 * within the arena plus a base that is deliberately **not** zero, so that
 * any code confusing a physical address with a pointer fails visibly. */
#define ARENA_PAGES 64u
#define FAKE_PHYS_BASE 0x40000000ull

static uint8_t arena[ARENA_PAGES][PT_BYTES] __attribute__((aligned(PT_BYTES)));
static bool arena_used[ARENA_PAGES];
static uint32_t arena_allocs;

static int arena_alloc(void *self, void **virt, phys_t *phys) {
    (void)self;
    for (uint32_t i = 0; i < ARENA_PAGES; i++) {
        if (!arena_used[i]) {
            arena_used[i] = true;
            arena_allocs++;
            memset(arena[i], 0, PT_BYTES);
            *virt = arena[i];
            *phys = FAKE_PHYS_BASE + (uint64_t)i * PT_BYTES;
            return KERR_OK;
        }
    }
    return KERR_NOMEM;
}

static void arena_free(void *self, void *virt) {
    (void)self;
    for (uint32_t i = 0; i < ARENA_PAGES; i++) {
        if (arena[i] == (uint8_t *)virt) {
            arena_used[i] = false;
        }
    }
}

static void *arena_to_virt(void *self, phys_t phys) {
    (void)self;
    return arena[(phys - FAKE_PHYS_BASE) / PT_BYTES];
}

static const struct pt_mem arena_mem = {
    .alloc = arena_alloc,
    .free = arena_free,
    .to_virt = arena_to_virt,
};

static struct pgtable fresh(void) {
    struct pgtable pt;
    memset(arena_used, 0, sizeof arena_used);
    arena_allocs = 0;
    KTEST_CHECK(pgtable_create(&pt, &arena_mem) == KERR_OK);
    return pt;
}

/* An address is canonical only when bits 63:39 copy bit 38. The hole
 * between the halves is not an address at all, and a kernel that maps into
 * it has built something that faults on first use. */
static void test_canonical_addresses(void) {
    KTEST_CHECK(pgtable_va_is_canonical(0));
    KTEST_CHECK(pgtable_va_is_canonical(0x0000003fffffffffull));   /* top of the bottom half */
    KTEST_CHECK(pgtable_va_is_canonical(0xffffffc000000000ull));   /* base of the kernel half */
    KTEST_CHECK(pgtable_va_is_canonical(0xffffffffffffffffull));
    KTEST_CHECK(!pgtable_va_is_canonical(0x0000004000000000ull));  /* in the hole */
    KTEST_CHECK(!pgtable_va_is_canonical(0x8000000000000000ull));
    KTEST_CHECK(!pgtable_va_is_canonical(0xffffffbfffffffffull));
}

static void test_maps_and_reads_back_a_page(void) {
    struct pgtable pt = fresh();
    phys_t pa = 0;
    uint64_t prot = 0, size = 0;
    KTEST_CHECK(pgtable_map(&pt, 0x1000, 0x80002000ull, PAGE_SIZE, PT_PROT_RX) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0x1000, &pa, &prot, &size));
    KTEST_CHECK(pa == 0x80002000ull && size == PAGE_SIZE);
    KTEST_CHECK(prot == PT_PROT_RX);
    /* An offset within the page comes back in the physical address. */
    KTEST_CHECK(pgtable_lookup(&pt, 0x1abc, &pa, &prot, &size) && pa == 0x80002abcull);
    /* The page next door is not mapped by it. */
    KTEST_CHECK(!pgtable_lookup(&pt, 0x2000, &pa, &prot, &size));
    pgtable_destroy(&pt);
}

/* A leaf at level 1 or 2 *is* a large mapping — there is no separate
 * descriptor format for one, which is the simplification over ARM's
 * sections. The size must divide both addresses. */
static void test_large_pages(void) {
    struct pgtable pt = fresh();
    phys_t pa = 0;
    uint64_t prot = 0, size = 0;
    KTEST_CHECK(pgtable_map(&pt, 0x40000000ull, 0x40000000ull, MEGAPAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0x40000000ull + 0x1234, &pa, &prot, &size));
    KTEST_CHECK(pa == 0x40000000ull + 0x1234 && size == MEGAPAGE_SIZE);
    KTEST_CHECK(pgtable_map(&pt, 0xffffffc000000000ull, 0ull, GIGAPAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0xffffffc000000000ull + 0x2000, &pa, &prot, &size));
    KTEST_CHECK(pa == 0x2000 && size == GIGAPAGE_SIZE);
    pgtable_destroy(&pt);
}

/* Misalignment is refused rather than silently rounded: a megapage mapped
 * at a 4 KiB-aligned address appears to work and covers the wrong two
 * megabytes. */
static void test_misaligned_large_mappings_are_refused(void) {
    struct pgtable pt = fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x1000, 0x40000000ull, MEGAPAGE_SIZE, PT_PROT_RW) == KERR_INVAL);
    KTEST_CHECK(pgtable_map(&pt, 0x40000000ull, 0x1000, MEGAPAGE_SIZE, PT_PROT_RW) == KERR_INVAL);
    KTEST_CHECK(pgtable_map(&pt, 0x1000, 0x1000, 8192, PT_PROT_RW) == KERR_INVAL);
    KTEST_CHECK(pgtable_map(&pt, 0x0000004000000000ull, 0, PAGE_SIZE, PT_PROT_RW) == KERR_INVAL);
    pgtable_destroy(&pt);
}

/* Replacing a live mapping is refused. Two virtual addresses pointing at one
 * page is sometimes wanted; one virtual address silently repointed is a bug
 * that shows up as data appearing in the wrong place. */
static void test_a_live_mapping_is_not_replaced(void) {
    struct pgtable pt = fresh();
    KTEST_CHECK(pgtable_map(&pt, 0x1000, 0x80000000ull, PAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_map(&pt, 0x1000, 0x90000000ull, PAGE_SIZE, PT_PROT_RW) == KERR_EXIST);
    /* And a page inside an existing megapage cannot be carved out of it. */
    KTEST_CHECK(pgtable_map(&pt, 0x40000000ull, 0x40000000ull, MEGAPAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_map(&pt, 0x40001000ull, 0x50000000ull, PAGE_SIZE, PT_PROT_RW) == KERR_NOMEM);
    pgtable_destroy(&pt);
}

static void test_unmap_reports_what_it_removed(void) {
    struct pgtable pt = fresh();
    phys_t pa = 0;
    uint64_t prot = 0, size = 0;
    KTEST_CHECK(pgtable_map(&pt, 0x1000, 0x80000000ull, PAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_map(&pt, 0x40000000ull, 0x40000000ull, MEGAPAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_unmap(&pt, 0x1000) == PAGE_SIZE);
    KTEST_CHECK(!pgtable_lookup(&pt, 0x1000, &pa, &prot, &size));
    /* Anywhere inside a megapage removes the whole megapage. */
    KTEST_CHECK(pgtable_unmap(&pt, 0x40000000ull + 0x5000) == MEGAPAGE_SIZE);
    KTEST_CHECK(!pgtable_lookup(&pt, 0x40000000ull, &pa, &prot, &size));
    KTEST_CHECK(pgtable_unmap(&pt, 0x1000) == 0);
    pgtable_destroy(&pt);
}

/* A range uses the largest pages that fit, which is what makes the kernel's
 * linear map affordable: a GiB as one entry instead of 262144. */
static void test_a_range_uses_the_largest_pages(void) {
    struct pgtable pt = fresh();
    phys_t pa = 0;
    uint64_t prot = 0, size = 0;
    uint32_t before = arena_allocs;
    KTEST_CHECK(pgtable_map_range(&pt, 0xffffffc000000000ull, 0x40000000ull,
                                  2ull * GIGAPAGE_SIZE, PT_PROT_RW) == KERR_OK);
    /* Two gigapages means the root table and nothing below it. */
    KTEST_CHECK(arena_allocs == before);
    KTEST_CHECK(pgtable_lookup(&pt, 0xffffffc000000000ull, &pa, &prot, &size));
    KTEST_CHECK(size == GIGAPAGE_SIZE && pa == 0x40000000ull);
    KTEST_CHECK(pgtable_lookup(&pt, 0xffffffc040000000ull, &pa, &prot, &size));
    KTEST_CHECK(size == GIGAPAGE_SIZE && pa == 0x80000000ull);
    pgtable_destroy(&pt);
}

/* An unaligned range still maps, with smaller pages at its edges. */
static void test_a_range_fills_its_edges(void) {
    struct pgtable pt = fresh();
    phys_t pa = 0;
    uint64_t prot = 0, size = 0;
    KTEST_CHECK(pgtable_map_range(&pt, 0x1000, 0x80001000ull, MEGAPAGE_SIZE + 2 * PAGE_SIZE,
                                  PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(pgtable_lookup(&pt, 0x1000, &pa, &prot, &size) && size == PAGE_SIZE);
    KTEST_CHECK(pgtable_lookup(&pt, 0x1000 + MEGAPAGE_SIZE, &pa, &prot, &size));
    KTEST_CHECK(pa == 0x80001000ull + MEGAPAGE_SIZE);
    pgtable_destroy(&pt);
}

/* The value satp wants: Sv39 selected, the address-space id, and the root's
 * physical page number — not its address. */
static void test_satp_value(void) {
    struct pgtable pt = fresh();
    uint64_t satp = pgtable_satp(&pt, 0x123);
    KTEST_CHECK((satp >> 60) == 8u);
    KTEST_CHECK(((satp >> 44) & 0xffffu) == 0x123u);
    KTEST_CHECK((satp & 0xfffffffffffull) == (pt.root_phys >> 12));
    pgtable_destroy(&pt);
}

/* Running out of table memory is reported, not ignored. */
static void test_exhaustion_is_reported(void) {
    struct pgtable pt = fresh();
    int rc = KERR_OK;
    for (uint64_t va = 0; rc == KERR_OK && va < 0x40000000ull; va += GIGAPAGE_SIZE / 512) {
        rc = pgtable_map(&pt, va, 0x80000000ull, PAGE_SIZE, PT_PROT_RW);
    }
    KTEST_CHECK(rc == KERR_NOMEM);
    pgtable_destroy(&pt);
}

/* A page that has gone out to swap: the entry remembers where, and every
 * other reader has to see "not mapped" — which is the whole reason the
 * valid bit is clear. The case that matters is the third one: an address
 * that was never mapped must not look swapped, or a fault on a wild
 * pointer would try to read slot 0 out of the swap file. */
static void test_a_swapped_page_is_remembered_but_not_mapped(void) {
    struct pgtable pt;
    phys_t pa = 0;
    uint64_t flags = 0, size = 0;
    uint32_t slot = 0;
    KTEST_CHECK(pgtable_create(&pt, &arena_mem) == KERR_OK);
    KTEST_CHECK(pgtable_map(&pt, 0x4000u, 0x90000u, PAGE_SIZE, PT_PROT_RW | PT_PROT_USER) ==
                KERR_OK);
    KTEST_CHECK(pgtable_set_swapped(&pt, 0x4000u, 1234u) == KERR_OK);
    /* Not mapped, to everybody who asks the ordinary way. */
    KTEST_CHECK(!pgtable_lookup(&pt, 0x4000u, &pa, &flags, &size));
    /* And remembered, to whoever asks the one question that matters. */
    KTEST_CHECK(pgtable_swapped_slot(&pt, 0x4000u, &slot) && slot == 1234u);
    /* An address that was never mapped is absent, not swapped. */
    KTEST_CHECK(!pgtable_swapped_slot(&pt, 0x8000u, &slot));
    /* Nor is a page that is mapped. */
    KTEST_CHECK(pgtable_map(&pt, 0x5000u, 0x91000u, PAGE_SIZE, PT_PROT_RW | PT_PROT_USER) ==
                KERR_OK);
    KTEST_CHECK(!pgtable_swapped_slot(&pt, 0x5000u, &slot));
    /* Swapping out a page that is not there is refused rather than
     * inventing an entry for it. */
    KTEST_CHECK(pgtable_set_swapped(&pt, 0x9000u, 7u) == KERR_NOENT);
    pgtable_destroy(&pt);
}

/* A megapage's window is free only while nothing at all is in it: a 4 KiB
 * page, the level-0 table it left behind after an unmap, or a megapage
 * already there each make it taken — the question THP asks before it
 * allocates 2 MiB. */
static void test_a_megapage_window_is_free_only_when_empty(void) {
    struct pgtable pt = fresh();
    const virt_t w = 0x40000000ull;
    KTEST_CHECK(pgtable_megapage_free(&pt, w));
    KTEST_CHECK(pgtable_megapage_free(&pt, w + 0x5000));
    KTEST_CHECK(pgtable_map(&pt, w + 0x3000, 0x80000000ull, PAGE_SIZE, PT_PROT_RW) == KERR_OK);
    KTEST_CHECK(!pgtable_megapage_free(&pt, w));
    KTEST_CHECK(pgtable_unmap(&pt, w + 0x3000) == PAGE_SIZE);
    KTEST_CHECK(!pgtable_megapage_free(&pt, w)); /* the table stays */
    KTEST_CHECK(pgtable_megapage_free(&pt, w + MEGAPAGE_SIZE));
    KTEST_CHECK(pgtable_map(&pt, w + MEGAPAGE_SIZE, 0x40000000ull, MEGAPAGE_SIZE, PT_PROT_RW) ==
                KERR_OK);
    KTEST_CHECK(!pgtable_megapage_free(&pt, w + MEGAPAGE_SIZE + 0x1000));
    KTEST_CHECK(!pgtable_megapage_free(&pt, 0x0000800000000000ull)); /* not canonical */
    pgtable_destroy(&pt);
}

int main(void) {
    KTEST_RUN(test_canonical_addresses);
    KTEST_RUN(test_a_swapped_page_is_remembered_but_not_mapped);
    KTEST_RUN(test_maps_and_reads_back_a_page);
    KTEST_RUN(test_large_pages);
    KTEST_RUN(test_misaligned_large_mappings_are_refused);
    KTEST_RUN(test_a_live_mapping_is_not_replaced);
    KTEST_RUN(test_unmap_reports_what_it_removed);
    KTEST_RUN(test_a_range_uses_the_largest_pages);
    KTEST_RUN(test_a_range_fills_its_edges);
    KTEST_RUN(test_satp_value);
    KTEST_RUN(test_exhaustion_is_reported);
    KTEST_RUN(test_a_megapage_window_is_free_only_when_empty);
    return ktest_failures();
}
