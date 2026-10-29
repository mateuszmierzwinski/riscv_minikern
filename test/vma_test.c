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
    KTEST_CHECK(vma_find_free(&set, 16u * PAGE, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at >= WINDOW_LO);
    KTEST_CHECK(at + 16u * PAGE <= WINDOW_HI);
    KTEST_CHECK(at > 0xFFFFFFFFull);
}

static void test_the_highest_gap_is_taken(void) {
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_find_free(&set, PAGE, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_HI - PAGE);
    KTEST_CHECK(vma_insert(&set, at, WINDOW_HI, 3u, 0) == KERR_OK);
    /* The next one goes below it, not on top of it. */
    KTEST_CHECK(vma_find_free(&set, PAGE, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_HI - 2u * PAGE);
}

static void test_a_free_hint_is_honoured(void) {
    virt_t want = WINDOW_LO + 0x100000ull;
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_find_free(&set, PAGE, want, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at == want);
}

/* A hint that lands on something taken must be ignored rather than obeyed:
 * obeying it is how a mapping is placed over live memory. */
static void test_an_occupied_hint_is_ignored(void) {
    virt_t want = WINDOW_LO + 0x100000ull;
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_insert(&set, want, want + PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_find_free(&set, PAGE, want, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
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
    KTEST_CHECK(vma_find_free(&set, PAGE, want + PAGE, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
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
    KTEST_CHECK(vma_find_free(&set, PAGE, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_NOMEM);
}

/* Looking a region up by address, which demand paging needs: a fault is
 * only worth backing if it lands in a region the process was given. */
static void test_find_finds_the_region_an_address_is_in(void) {
    struct vma_set s;
    const struct vma *v;
    memset(&s, 0, sizeof s);
    KTEST_CHECK(vma_insert(&s, 0x10000u, 0x20000u, 1u, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&s, 0x80000u, 0x90000u, 3u, 0) == KERR_OK);
    v = vma_find(&s, 0x10000u);
    KTEST_CHECK(v != NULL && v->start == 0x10000u);
    /* Inside, not just at the start. */
    v = vma_find(&s, 0x1ffffu);
    KTEST_CHECK(v != NULL && v->start == 0x10000u);
    /* The rights come back with it: that is what the fault handler needs. */
    v = vma_find(&s, 0x85000u);
    KTEST_CHECK(v != NULL && v->prot == 3u);
    /* **Half-open**: the end address belongs to whatever is next, not to
     * this region. Getting that wrong backs a page one past every mapping. */
    KTEST_CHECK(vma_find(&s, 0x20000u) == NULL);
    /* Outside everything, which is a wild pointer and must stay one. */
    KTEST_CHECK(vma_find(&s, 0x5000u) == NULL);
    KTEST_CHECK(vma_find(&s, 0x100000u) == NULL);
    KTEST_CHECK(vma_find(NULL, 0x10000u) == NULL);
}

/* How `mremap` grows a mapping where it stands: it records the pages past
 * the end, and the set merges the tail into the region it extends. One
 * region afterwards, not two, which is what keeps a runtime that grows a
 * mapping repeatedly from spending a slot per growth. */
static void test_growing_in_place_is_an_insert_that_merges(void) {
    const struct vma *v;
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO, WINDOW_LO + 4u * PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 4u * PAGE, WINDOW_LO + 8u * PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(set.n == 1u);
    v = vma_find(&set, WINDOW_LO + 7u * PAGE);
    KTEST_CHECK(v != NULL && v->start == WINDOW_LO && v->end == WINDOW_LO + 8u * PAGE);
}

/* And how it finds out that it cannot: the insert *is* the test, because a
 * range overlapping a region is refused. A growth that asked a separate
 * "is it free?" question and then inserted would have two answers to keep
 * in step. */
static void test_an_occupied_tail_refuses_the_growth(void) {
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO, WINDOW_LO + 4u * PAGE, 3u, 0) == KERR_OK);
    /* The blocker, with different rights so it cannot merge away. */
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 4u * PAGE, WINDOW_LO + 8u * PAGE, 1u, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 4u * PAGE, WINDOW_LO + 6u * PAGE, 3u, 0) ==
                KERR_INVAL);
    KTEST_CHECK(set.n == 2u);
}

/* What `munmap` has to do and did not: a range that is removed from the
 * record is free again, so the gap search offers it. While nothing removed
 * it, the addresses were spoken for for ever — and once pages are backed on
 * demand, a region that still claims a range answers a touch of it with a
 * fresh page instead of a fault. */
static void test_a_removed_range_is_offered_again(void) {
    virt_t at = 0;
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_HI - 4u * PAGE, WINDOW_HI, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_find_free(&set, 4u * PAGE, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_HI - 8u * PAGE);
    KTEST_CHECK(vma_remove(&set, WINDOW_HI - 4u * PAGE, WINDOW_HI, NULL, NULL) == KERR_OK);
    KTEST_CHECK(vma_find(&set, WINDOW_HI - PAGE) == NULL);
    KTEST_CHECK(vma_find_free(&set, 4u * PAGE, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_HI - 4u * PAGE);
}

/* `MAP_FIXED_NOREPLACE`'s question, which nothing could ask before: is this
 * exact range free? One region answers it, because the set is sorted — and
 * the case that matters is a range that *starts* free and runs into
 * something, which a check of the first page alone would have called free.
 * That mistake has been made in this file before, in the hinted placement
 * path, and it cost live memory. */
static void test_a_range_is_free_only_if_all_of_it_is(void) {
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 4u * PAGE, WINDOW_LO + 8u * PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_range_free(&set, WINDOW_LO, WINDOW_LO + 4u * PAGE));
    KTEST_CHECK(!vma_range_free(&set, WINDOW_LO, WINDOW_LO + 5u * PAGE));
    KTEST_CHECK(!vma_range_free(&set, WINDOW_LO + 7u * PAGE, WINDOW_LO + 12u * PAGE));
    KTEST_CHECK(!vma_range_free(&set, WINDOW_LO + 5u * PAGE, WINDOW_LO + 6u * PAGE));
    /* Half-open at both ends: a range ending exactly where a region begins,
     * and one beginning exactly where it ends, are both free. */
    KTEST_CHECK(vma_range_free(&set, WINDOW_LO + 8u * PAGE, WINDOW_LO + 12u * PAGE));
}

/* Placement for a huge page: the result has to be aligned to the page size
 * being asked for, and aligning *after* a gap is found is not the same
 * thing — the aligned address can fall out of the bottom of a gap that was
 * only just big enough, which would put the mapping on top of the region
 * below it. */
static void test_an_aligned_gap_is_found_or_refused(void) {
    const virt_t mega = 2u * 1024u * 1024u;
    virt_t at = 0;
    fresh();
    /* An empty window: the highest 2 MiB-aligned address that fits. */
    KTEST_CHECK(vma_find_free(&set, mega, 0, WINDOW_LO, WINDOW_HI, mega, &at) == KERR_OK);
    KTEST_CHECK(at % mega == 0 && at + mega <= WINDOW_HI);
    /* A gap of exactly the length asked for, at an address that is not
     * aligned: there is nowhere to put it, and the answer is no rather than
     * an address inside the region below. */
    fresh();
    KTEST_CHECK(vma_insert(&set, WINDOW_LO, WINDOW_LO + mega + PAGE, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_insert(&set, WINDOW_LO + 2u * mega + PAGE, WINDOW_HI, 3u, 0) == KERR_OK);
    KTEST_CHECK(vma_find_free(&set, mega, 0, WINDOW_LO, WINDOW_HI, mega, &at) == KERR_NOMEM);
    /* The same gap without the alignment demand is perfectly usable. */
    KTEST_CHECK(vma_find_free(&set, mega, 0, WINDOW_LO, WINDOW_HI, 0, &at) == KERR_OK);
    KTEST_CHECK(at == WINDOW_LO + mega + PAGE);
}

/* References a file-backed region holds, as the kernel's hook and `gone`
 * would count them: one per region, taken at a cut, given back on removal. */
static int refs;

static void take_ref(const struct vma *half) {
    (void)half;
    refs++;
}

static void give_ref(void *ctx, const struct vma *v) {
    (void)ctx;
    refs -= v->file != NULL ? 1 : 0;
}

/* Cutting a file-backed region gives the new half its own reference, so a
 * partial protect then a removal of every piece ends at zero — never
 * below, which is a file freed while a region still reads it. */
static void test_each_half_of_a_cut_file_region_holds_a_reference(void) {
    static int file;
    fresh();
    refs = 1;
    vma_set_split_hook(take_ref);
    KTEST_CHECK(vma_insert_backed(&set, WINDOW_LO, WINDOW_LO + 8u * PAGE, 3u, 0, &file, 0) ==
                KERR_OK);
    KTEST_CHECK(vma_protect(&set, WINDOW_LO + 2u * PAGE, WINDOW_LO + 3u * PAGE, 1u) == KERR_OK);
    KTEST_CHECK(set.n == 3u && refs == 3);
    KTEST_CHECK(vma_remove(&set, WINDOW_LO + 5u * PAGE, WINDOW_LO + 6u * PAGE, give_ref, NULL) ==
                KERR_OK);
    KTEST_CHECK(set.n == 4u && refs == 4);
    KTEST_CHECK(vma_remove(&set, WINDOW_LO, WINDOW_LO + 8u * PAGE, give_ref, NULL) == KERR_OK);
    KTEST_CHECK(set.n == 0 && refs == 0);
    vma_set_split_hook(NULL);
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
    KTEST_RUN(test_find_finds_the_region_an_address_is_in);
    KTEST_RUN(test_growing_in_place_is_an_insert_that_merges);
    KTEST_RUN(test_an_occupied_tail_refuses_the_growth);
    KTEST_RUN(test_a_removed_range_is_offered_again);
    KTEST_RUN(test_a_range_is_free_only_if_all_of_it_is);
    KTEST_RUN(test_an_aligned_gap_is_found_or_refused);
    KTEST_RUN(test_each_half_of_a_cut_file_region_holds_a_reference);
    return ktest_failures();
}
