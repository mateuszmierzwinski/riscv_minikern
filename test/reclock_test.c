/* POSIX record locks, on the host.
 *
 * The sequences are SQLite's own: a shared lock on one byte range, an
 * upgrade to a reserved and then an exclusive lock on another, and the
 * unlock of a range from the middle of a lock that must leave both ends.
 */
#include "core/reclock.h"

#include "core/errno.h"
#include "ktest.h"

#define FILE_A 11u
#define FILE_B 12u
#define ME 100u
#define OTHER 200u

static void test_readers_share_and_writers_do_not(void) {
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_READ, 0, 10) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, OTHER, RECLOCK_READ, 5, 15) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, OTHER, RECLOCK_WRITE, 5, 15) == KERR_AGAIN);
    /* Beside my reader, not over it (the end is exclusive): no conflict,
     * and over OTHER's own reader it is an upgrade. */
    KTEST_CHECK(reclock_set(FILE_A, OTHER, RECLOCK_WRITE, 10, 20) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_READ, 12, 13) == KERR_AGAIN);
    reclock_release(FILE_A, OTHER);
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_READ, 12, 13) == KERR_OK);
}

static void test_an_owner_never_conflicts_with_itself(void) {
    /* An upgrade from read to write over the owner's own lock is how every
     * SQLite write begins; refusing it would make no write possible. */
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_READ, 0, 100) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, 0, 100) == KERR_OK);
    KTEST_CHECK(reclock_count() == 1u);
}

static void test_files_are_separate(void) {
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, 0, RECLOCK_EOF) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_B, OTHER, RECLOCK_WRITE, 0, RECLOCK_EOF) == KERR_OK);
}

static void test_unlocking_the_middle_keeps_both_ends(void) {
    struct reclock_range seen;
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, 0, 100) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_UNLOCK, 40, 60) == KERR_OK);
    KTEST_CHECK(reclock_count() == 2u);
    KTEST_CHECK(!reclock_conflict(FILE_A, OTHER, RECLOCK_WRITE, 40, 60, NULL));
    KTEST_CHECK(reclock_conflict(FILE_A, OTHER, RECLOCK_READ, 30, 41, &seen));
    KTEST_CHECK(seen.start == 0u && seen.end == 40u && seen.owner == ME);
    KTEST_CHECK(reclock_conflict(FILE_A, OTHER, RECLOCK_READ, 59, 61, &seen));
    KTEST_CHECK(seen.start == 60u && seen.end == 100u);
}

static void test_a_downgrade_lets_readers_in(void) {
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, 0, 10) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_READ, 0, 10) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, OTHER, RECLOCK_READ, 0, 10) == KERR_OK);
}

static void test_to_the_end_of_the_file(void) {
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, 1000, RECLOCK_EOF) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_A, OTHER, RECLOCK_READ, 1u << 30, (1u << 30) + 1u) == KERR_AGAIN);
    KTEST_CHECK(reclock_set(FILE_A, OTHER, RECLOCK_READ, 0, 1000) == KERR_OK);
}

static void test_bad_requests_change_nothing(void) {
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_READ, 10, 10) == KERR_INVAL);
    KTEST_CHECK(reclock_set(FILE_A, ME, 7u, 0, 10) == KERR_INVAL);
    KTEST_CHECK(reclock_count() == 0u);
}

static void test_a_full_table_refuses_whole(void) {
    /* Every slot taken, then a request that would split one: refused,
     * and the lock it would have split is still all there. */
    reclock_reset();
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, i * 10u, i * 10u + 5u) == KERR_OK);
    }
    KTEST_CHECK(reclock_set(FILE_B, ME, RECLOCK_READ, 0, 1) == KERR_NOLCK);
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_UNLOCK, 1, 2) == KERR_NOLCK);
    KTEST_CHECK(reclock_conflict(FILE_A, OTHER, RECLOCK_READ, 1, 2, NULL));
    /* An unlock that removes a whole lock needs no slot. */
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_UNLOCK, 0, 5) == KERR_OK);
}

static void test_release_by_owner(void) {
    reclock_reset();
    KTEST_CHECK(reclock_set(FILE_A, ME, RECLOCK_WRITE, 0, 1) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_B, ME, RECLOCK_WRITE, 0, 1) == KERR_OK);
    KTEST_CHECK(reclock_set(FILE_B, OTHER, RECLOCK_WRITE, 5, 6) == KERR_OK);
    reclock_release_owner(ME);
    KTEST_CHECK(reclock_count() == 1u);
}

static void test_identity_is_stable_and_never_zero(void) {
    KTEST_CHECK(reclock_identity("/var/lib/blog/blog.db") ==
                reclock_identity("/var/lib/blog/blog.db"));
    KTEST_CHECK(reclock_identity("/var/lib/blog/blog.db") !=
                reclock_identity("/var/lib/blog/blog.db-journal"));
    KTEST_CHECK(reclock_identity("") != 0u);
}

int main(void) {
    KTEST_RUN(test_readers_share_and_writers_do_not);
    KTEST_RUN(test_an_owner_never_conflicts_with_itself);
    KTEST_RUN(test_files_are_separate);
    KTEST_RUN(test_unlocking_the_middle_keeps_both_ends);
    KTEST_RUN(test_a_downgrade_lets_readers_in);
    KTEST_RUN(test_to_the_end_of_the_file);
    KTEST_RUN(test_bad_requests_change_nothing);
    KTEST_RUN(test_a_full_table_refuses_whole);
    KTEST_RUN(test_release_by_owner);
    KTEST_RUN(test_identity_is_stable_and_never_zero);
    return ktest_failures();
}
