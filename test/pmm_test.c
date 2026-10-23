#include "core/pmm.h"

#include "core/types.h"

#include "core/errno.h"
#include "ktest.h"

#define BASE 0x40000000u
#define NPAGES 100u

static uint32_t bits[PMM_BITMAP_WORDS(NPAGES)];
static struct pmm p;

static void fresh(void) {
    pmm_init(&p, BASE, NPAGES, bits);
    pmm_release(&p, BASE, NPAGES * PMM_PAGE_SIZE);
}

static void test_starts_all_used(void) {
    phys_t phys;
    pmm_init(&p, BASE, NPAGES, bits);
    KTEST_CHECK(p.free == 0);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &phys) == KERR_NOMEM);
}

static void test_release_rounds_inward(void) {
    pmm_init(&p, BASE, NPAGES, bits);
    pmm_release(&p, BASE + 100, 3 * PMM_PAGE_SIZE);  /* touches 4 pages, 2 whole */
    KTEST_CHECK(p.free == 2);
    pmm_release(&p, BASE - PMM_PAGE_SIZE, 2 * PMM_PAGE_SIZE);  /* half outside */
    KTEST_CHECK(p.free == 3);
    pmm_release(&p, 0xfffff000u, 0x2000u);  /* wraps past 4 GiB: ignored, no overflow */
    KTEST_CHECK(p.free == 3);
}

static void test_reserve_rounds_outward(void) {
    fresh();
    pmm_reserve(&p, BASE + 100, 10);
    KTEST_CHECK(p.free == NPAGES - 1);
    pmm_reserve(&p, BASE + PMM_PAGE_SIZE - 1, 2);  /* straddles pages 0 and 1 */
    KTEST_CHECK(p.free == NPAGES - 2);
}

static void test_alloc_free_roundtrip(void) {
    phys_t a, b;
    fresh();
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &a) == KERR_OK);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &b) == KERR_OK);
    KTEST_CHECK(a != b && a >= BASE && b >= BASE);
    KTEST_CHECK(p.free == NPAGES - 2);
    KTEST_CHECK(pmm_free(&p, a, 1) == KERR_OK);
    KTEST_CHECK(pmm_free(&p, a, 1) == KERR_INVAL);  /* double free */
    KTEST_CHECK(p.free == NPAGES - 1);
}

static void test_aligned_runs(void) {
    phys_t a, b;
    fresh();
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &a) == KERR_OK);  /* frame 0 */
    KTEST_CHECK(pmm_alloc(&p, 2, 2, &b) == KERR_OK);
    KTEST_CHECK(((b - BASE) >> PMM_PAGE_SHIFT) % 2 == 0 && b != a);
    KTEST_CHECK(pmm_alloc(&p, 4, 4, &b) == KERR_OK);
    KTEST_CHECK(((b - BASE) >> PMM_PAGE_SHIFT) % 4 == 0);
}

static void test_exhaustion(void) {
    phys_t a;
    uint32_t n = 0;
    fresh();
    while (pmm_alloc(&p, 1, 0, &a) == KERR_OK) {
        n++;
    }
    KTEST_CHECK(n == NPAGES);
    KTEST_CHECK(pmm_free(&p, BASE + 50 * PMM_PAGE_SIZE, 1) == KERR_OK);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &a) == KERR_OK && a == BASE + 50 * PMM_PAGE_SIZE);
    KTEST_CHECK(pmm_alloc(&p, 101, 0, &a) == KERR_NOMEM);
    KTEST_CHECK(pmm_alloc(&p, 0, 0, &a) == KERR_NOMEM);
}

static void test_bad_frees(void) {
    fresh();
    KTEST_CHECK(pmm_free(&p, BASE - PMM_PAGE_SIZE, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE + 1, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE + NPAGES * PMM_PAGE_SIZE, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE, NPAGES + 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, BASE, 1) == KERR_INVAL);  /* free already */
}

/* Frames above 4 GiB, which is the reason this allocator's addresses widened.
 * A VisionFive 2 with 8 GiB has its top of memory past 0x2_0000_0000, so a
 * 32-bit physical address would wrap in the middle of the board's own RAM —
 * and the symptom would be an allocator handing out an address that already
 * belongs to something else, which is as bad as a bug gets. */
static void test_frames_above_four_gibibytes(void) {
    static uint32_t high_bits[PMM_BITMAP_WORDS(64)];
    struct pmm p;
    phys_t got = 0;
    const phys_t base = 0x240000000ull; /* 9 GiB: past where 32 bits reach */
    pmm_init(&p, base, 64, high_bits);
    pmm_release(&p, base, 64ull * PMM_PAGE_SIZE);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &got) == KERR_OK);
    KTEST_CHECK(got == base);
    KTEST_CHECK(got >> 32 == 2u); /* the high word survived */
    KTEST_CHECK(pmm_alloc(&p, 2, 2, &got) == KERR_OK);
    KTEST_CHECK(got == base + 2ull * PMM_PAGE_SIZE);
    KTEST_CHECK(pmm_free(&p, got, 2) == KERR_OK);
    /* And an address below the base is still rejected rather than wrapping
     * into a valid-looking index. */
    KTEST_CHECK(pmm_free(&p, 0x1000, 1) == KERR_INVAL);
    KTEST_CHECK(pmm_free(&p, base - PMM_PAGE_SIZE, 1) == KERR_INVAL);
}

/* ── sharing ───────────────────────────────────────────────────────────── */

static uint8_t refs[NPAGES];

/* A frame two address spaces hold is freed by neither until the second lets
 * go. Freeing it on the first would hand a page that is still mapped
 * somewhere back to the allocator, and the next allocation would be served
 * live memory — which is the failure this whole mechanism exists to stop. */
static void test_a_shared_frame_survives_one_free(void) {
    phys_t phys = 0;
    fresh();
    pmm_set_refcounts(&p, refs);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &phys) == KERR_OK);
    KTEST_CHECK(pmm_shares(&p, phys) == 1u);
    KTEST_CHECK(pmm_share(&p, phys) == KERR_OK);
    KTEST_CHECK(pmm_shares(&p, phys) == 2u);

    KTEST_CHECK(pmm_free(&p, phys, 1) == KERR_OK);
    KTEST_CHECK(pmm_shares(&p, phys) == 1u); /* still held, still allocated */
    {
        phys_t other = 0;
        KTEST_CHECK(pmm_alloc(&p, 1, 0, &other) == KERR_OK);
        KTEST_CHECK(other != phys); /* and not handed out again */
        KTEST_CHECK(pmm_free(&p, other, 1) == KERR_OK);
    }
    KTEST_CHECK(pmm_free(&p, phys, 1) == KERR_OK);
    KTEST_CHECK(pmm_shares(&p, phys) == 0u); /* the last one freed it */
}

static void test_sharing_needs_an_allocated_frame(void) {
    phys_t phys = 0;
    fresh();
    pmm_set_refcounts(&p, refs);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &phys) == KERR_OK);
    KTEST_CHECK(pmm_free(&p, phys, 1) == KERR_OK);
    KTEST_CHECK(pmm_share(&p, phys) == KERR_INVAL);      /* free */
    KTEST_CHECK(pmm_share(&p, BASE - PMM_PAGE_SIZE) == KERR_INVAL); /* not ours */
    KTEST_CHECK(pmm_share(&p, BASE + 1u) == KERR_INVAL);  /* not a frame */
}

/* Without somewhere to count, the allocator behaves exactly as it did
 * before: every allocated frame is held once and every free is a free. */
static void test_without_storage_nothing_changes(void) {
    phys_t phys = 0;
    fresh();
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &phys) == KERR_OK);
    KTEST_CHECK(pmm_shares(&p, phys) == 1u);
    KTEST_CHECK(pmm_share(&p, phys) == KERR_NOTSUP);
    KTEST_CHECK(pmm_free(&p, phys, 1) == KERR_OK);
    KTEST_CHECK(pmm_shares(&p, phys) == 0u);
}

/* A count that saturates leaks its frame rather than freeing it early. */
static void test_the_count_saturates_rather_than_wrapping(void) {
    phys_t phys = 0;
    fresh();
    pmm_set_refcounts(&p, refs);
    KTEST_CHECK(pmm_alloc(&p, 1, 0, &phys) == KERR_OK);
    for (uint32_t i = 0; i < 600u; i++) {
        KTEST_CHECK(pmm_share(&p, phys) == KERR_OK);
    }
    KTEST_CHECK(pmm_shares(&p, phys) == 256u); /* 255 + the allocation */
    for (uint32_t i = 0; i < 600u; i++) {
        (void)pmm_free(&p, phys, 1);
    }
    /* It is still allocated: leaked, not handed back while someone may
     * still hold it. */
    KTEST_CHECK(pmm_shares(&p, phys) != 0u || p.free < NPAGES);
}

int main(void) {
    KTEST_RUN(test_starts_all_used);
    KTEST_RUN(test_release_rounds_inward);
    KTEST_RUN(test_reserve_rounds_outward);
    KTEST_RUN(test_alloc_free_roundtrip);
    KTEST_RUN(test_aligned_runs);
    KTEST_RUN(test_exhaustion);
    KTEST_RUN(test_bad_frees);
    KTEST_RUN(test_frames_above_four_gibibytes);
    KTEST_RUN(test_a_shared_frame_survives_one_free);
    KTEST_RUN(test_sharing_needs_an_allocated_frame);
    KTEST_RUN(test_without_storage_nothing_changes);
    KTEST_RUN(test_the_count_saturates_rather_than_wrapping);
    return ktest_failures();
}
