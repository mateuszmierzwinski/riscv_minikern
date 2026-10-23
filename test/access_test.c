/* The permission rule, on the host.
 *
 * The cases that matter are the ones nobody produces by hand: an owner
 * denied by their own bits, a group whose bits are narrower than other's,
 * root against a mode that denies everybody.
 */
#include "core/access.h"

#include <stdint.h>

#include "ktest.h"

#define OWNER 1000u
#define GROUP 1000u
#define OTHER 1001u

static void test_the_ordinary_modes(void) {
    /* 0644: owner reads and writes, everybody else reads. */
    KTEST_CHECK(access_permitted(OWNER, GROUP, OWNER, GROUP, 0644u, ACCESS_READ));
    KTEST_CHECK(access_permitted(OWNER, GROUP, OWNER, GROUP, 0644u, ACCESS_WRITE));
    KTEST_CHECK(access_permitted(OTHER, 7u, OWNER, GROUP, 0644u, ACCESS_READ));
    KTEST_CHECK(!access_permitted(OTHER, 7u, OWNER, GROUP, 0644u, ACCESS_WRITE));
    /* 0600: nobody but the owner. */
    KTEST_CHECK(!access_permitted(OTHER, 7u, OWNER, GROUP, 0600u, ACCESS_READ));
}

static void test_the_class_is_exclusive(void) {
    /* **0077: everybody except me.** The owner must be denied, which only
     * happens if the owner's class is the whole answer. A check that fell
     * through to the group bits would grant the owner everything and make
     * this mode mean its opposite. */
    KTEST_CHECK(!access_permitted(OWNER, GROUP, OWNER, GROUP, 0077u, ACCESS_READ));
    /* The group and other still get what the mode says. */
    KTEST_CHECK(access_permitted(OTHER, GROUP, OWNER, GROUP, 0077u, ACCESS_READ));
    KTEST_CHECK(access_permitted(OTHER, 7u, OWNER, GROUP, 0077u, ACCESS_WRITE));
    /* And the same shape one class along: a group member denied where
     * other is allowed. */
    KTEST_CHECK(!access_permitted(OTHER, GROUP, OWNER, GROUP, 0607u, ACCESS_READ));
    KTEST_CHECK(access_permitted(OTHER, 7u, OWNER, GROUP, 0607u, ACCESS_READ));
}

static void test_every_requested_bit_is_required(void) {
    uint32_t rw = ACCESS_READ | ACCESS_WRITE;
    KTEST_CHECK(access_permitted(OWNER, GROUP, OWNER, GROUP, 0600u, rw));
    /* Read-only: a request for both is denied rather than half-granted. */
    KTEST_CHECK(!access_permitted(OWNER, GROUP, OWNER, GROUP, 0400u, rw));
    KTEST_CHECK(access_permitted(OWNER, GROUP, OWNER, GROUP, 0400u, ACCESS_READ));
}

static void test_root_passes_everything(void) {
    /* Including a mode that denies every class, which is the case that
     * proves it is a definition and not a shortcut — root has to be able to
     * fix a file nobody can open. */
    KTEST_CHECK(access_permitted(ACCESS_ROOT, 0u, OWNER, GROUP, 0000u, ACCESS_READ));
    KTEST_CHECK(access_permitted(ACCESS_ROOT, 0u, OWNER, GROUP, 0000u,
                                 ACCESS_READ | ACCESS_WRITE | ACCESS_EXEC));
}

static void test_asking_nothing_is_permitted(void) {
    /* How a caller probes for existence without wanting access to it. */
    KTEST_CHECK(access_permitted(OTHER, 7u, OWNER, GROUP, 0000u, 0u));
}

static void test_the_execute_bit_is_its_own(void) {
    /* A readable file is not a runnable one, which is the distinction that
     * stops a data file being executed. */
    KTEST_CHECK(!access_permitted(OWNER, GROUP, OWNER, GROUP, 0644u, ACCESS_EXEC));
    KTEST_CHECK(access_permitted(OWNER, GROUP, OWNER, GROUP, 0744u, ACCESS_EXEC));
}

int main(void) {
    KTEST_RUN(test_the_ordinary_modes);
    KTEST_RUN(test_the_class_is_exclusive);
    KTEST_RUN(test_every_requested_bit_is_required);
    KTEST_RUN(test_root_passes_everything);
    KTEST_RUN(test_asking_nothing_is_permitted);
    KTEST_RUN(test_the_execute_bit_is_its_own);
    return ktest_failures();
}
