/* The regions of a user address space.
 *
 * This file decides where every anonymous mapping goes, so a mistake in it
 * is a mapping placed on top of something that is already there — which does
 * not fault and does not fail. It shows up as a program finding zeroes where
 * it left data.
 *
 * The cases below are mostly about **addresses that do not fit in 32 bits**.
 * This code came from a 32-bit ARM kernel, and that is precisely why it sat
 * in the tree unused: user space here is 256 GiB and the mmap window starts
 * at 128 GiB, so every address in anger is above 4 GiB. Widening it, the
 * same truncation was reintroduced twice — once in the gap search and once
 * in a range check — and each time the symptom was every mmap failing.
 */
#include "core/vma.h"

#include "core/errno.h"
#include "ktest.h"

/* The real window this kernel places into, which is the point: all of it is
 * above 4 GiB. */
#define WINDOW_LO 0x2000000000ull
#define WINDOW_HI 0x3FFFFFF000ull
#define PAGE 4096ull

static struct vma_set set;

static void fresh(void) {
    set.n = 0;
}

static void test_insert_and_find(void) {
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO, WINDOW_LO + 4u * PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_find(&set, WINDOW_LO) != NULL);
    KTEST_CHECK(vma_find(&set, WINDOW_LO + 3u * PAGE) != NULL);
    KTEST_CHECK(vma_find(&set, WINDOW_LO + 4u * PAGE) == NULL);
    KTEST_CHECK(vma_find(&set, WINDOW_LO - PAGE) == NULL);
}

static void test_overlap_is_refused(void) {
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO, WINDOW_LO + 4u * PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 2u * PAGE, WINDOW_LO + 6u * PAGE, 3u, 0) ==
                KERR_INVAL);
    KTEST_CHECK(vma_insert(&set, WINDOW_LO - PAGE, WINDOW_LO + PAGE, 3u, 0) == KERR_INVAL);
    /* Exactly abutting is not overlapping. */
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 4u * PAGE, WINDOW_LO + 6u * PAGE, 3u, 0) == KERR_OK);
}

/* **The regression this file exists for.** A gap search that held an
 * address in 32 bits looks in the bottom 4 GiB of a window that begins at
 * 128 GiB, finds nothing, and every mmap fails. */
static void test_the_search_works_above_four_gibibytes(void) {
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_find_free(&set, 16u * PAGE, 0, WINDOW_LO, WINDOW_HI, &at) == KERR_OK);
    KTEST_CHECK(at >= WINDOW_LO);
    KTEST_CHECK(at + 16u * PAGE <= WINDOW_HI);
    KTEST_CHECK(at > 0xFFFFFFFFull);
}

static void test_the_highest_gap_is_taken(void) {
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_find_free(&set, PAGE, 0, WINDOW_LO, WINDOW_HI, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_HI - PAGE);
    KTEST_CHECK(vma_insert(&set, at, WINDOW_HI, 3u, 0) == KERR_OK);
    /* The next one goes below it, not on top of it. */
    KTEST_CHECK(vma_find_free(&set, PAGE, 0, WINDOW_LO, WINDOW_HI, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_HI - 2u * PAGE);
}

static void test_a_free_hint_is_honoured(void) {
    virt_t want = WINDOW_LO + 0x100000ull;
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_find_free(&set, PAGE, want, WINDOW_LO, WINDOW_HI, &at) == KERR_OK);
    KTEST_CHECK(at == want);
}

/* A hint that lands on something taken must be ignored rather than obeyed:
 * obeying it is how a mapping is placed over live memory. */
static void test_an_occupied_hint_is_ignored(void) {
    virt_t want = WINDOW_LO + 0x100000ull;
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_insert(&set, want, want + PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_find_free(&set, PAGE, want, WINDOW_LO, WINDOW_HI, &at) == KERR_OK);
    KTEST_CHECK(at != want);
}

/* **A reservation is as occupied as anything else.** It has no pages, which
 * is why the page tables could not answer this question and why the set
 * exists: a hint inside one must not be honoured. */
static void test_a_reservation_occupies_its_range(void) {
    virt_t want = WINDOW_LO + 0x200000ull;
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_insert(&set, want, want + 0x100000ull, 0u, 0) == KERR_OK); /* PROT_NONE */
    KTEST_CHECK(vma_find_free(&set, PAGE, want + PAGE, WINDOW_LO, WINDOW_HI, &at) == KERR_OK);
    KTEST_CHECK(at < want || at >= want + 0x100000ull);
}

static void test_remove_splits_a_region(void) {
    virt_t at = WINDOW_LO + 0x400000ull;
    fresh();
    KTEST_CHECK(vma_insert(&set, at, at + 8u * PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_remove(&set, at + 2u * PAGE, at + 4u * PAGE, 0, 0) == KERR_OK);
    KTEST_CHECK(vma_find(&set, at + PAGE) != NULL);
    KTEST_CHECK(vma_find(&set, at + 2u * PAGE) == NULL);
    KTEST_CHECK(vma_find(&set, at + 3u * PAGE) == NULL);
    KTEST_CHECK(vma_find(&set, at + 4u * PAGE) != NULL);
}

/* Removing a range nothing occupies is not an error: a runtime unmaps
 * speculatively, and refusing would turn that into a failure. */
static void test_removing_a_hole_is_not_an_error(void) {
    fresh();
    KTEST_CHECK(vma_remove(&set, WINDOW_LO, WINDOW_LO + PAGE, 0, 0) == KERR_OK);
}

static void test_a_full_window_has_no_room(void) {
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO, WINDOW_HI, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_find_free(&set, PAGE, 0, WINDOW_LO, WINDOW_HI, &at) == KERR_NOMEM);
}

int main(void) {
    KTEST_RUN(test_insert_and_find);
    KTEST_RUN(test_overlap_is_refused);
    KTEST_RUN(test_the_search_works_above_four_gibibytes);
    KTEST_RUN(test_the_highest_gap_is_taken);
    KTEST_RUN(test_a_free_hint_is_honoured);
    KTEST_RUN(test_an_occupied_hint_is_ignored);
    KTEST_RUN(test_a_reservation_occupies_its_range);
    KTEST_RUN(test_remove_splits_a_region);
    KTEST_RUN(test_removing_a_hole_is_not_an_error);
    KTEST_RUN(test_a_full_window_has_no_room);
    return ktest_failures();
}
