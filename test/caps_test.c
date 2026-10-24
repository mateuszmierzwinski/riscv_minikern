/* The capability rules, which are all about what may *not* change.
 *
 * Every test here is a refusal, because that is where a capability model
 * earns its keep: granting is easy and a model that granted correctly while
 * letting a process raise its own permitted set would be worse than no
 * model at all — it would read as privilege separation while providing
 * none.
 */
#include "core/caps.h"
#include "ktest.h"

static struct cap_sets root_sets(void) {
    struct cap_sets s;
    cap_sets_init(&s, 0u);
    return s;
}

static struct cap_sets user_sets(void) {
    struct cap_sets s;
    cap_sets_init(&s, 1000u);
    return s;
}

static void test_root_starts_with_everything_and_a_user_with_nothing(void) {
    struct cap_sets r = root_sets(), u = user_sets();
    KTEST_CHECK(r.effective == cap_all());
    KTEST_CHECK(r.permitted == cap_all());
    KTEST_CHECK(cap_in(r.effective, CAP_SYS_ADMIN));
    KTEST_CHECK(u.effective == 0u);
    KTEST_CHECK(!cap_in(u.effective, CAP_SYS_ADMIN));
    /* **The bounding set is full for both.** It is a ceiling, not a grant:
     * a user whose bounding set started empty could never be given a
     * capability by anybody, including root. */
    KTEST_CHECK(u.bounding == cap_all());
}

static void test_the_top_capability_is_in_the_set(void) {
    /* `cap_all` is built by two shifts rather than one, because shifting by
     * 64 is undefined — and the value that would be wrong is exactly the
     * highest capability, the one a naive expression gets wrong silently. */
    KTEST_CHECK(cap_in(cap_all(), CAP_LAST_CAP));
    KTEST_CHECK(cap_in(cap_all(), 0u));
    KTEST_CHECK(!cap_valid(CAP_LAST_CAP + 1u));
    KTEST_CHECK(cap_bit(CAP_LAST_CAP + 1u) == 0u);
    /* An undefined number is in no set, however the set was built: a
     * `cap_in` that answered yes would be reporting privilege that no check
     * could ever consult. */
    KTEST_CHECK(!cap_in(cap_all(), 63u));
}

static void test_permitted_may_only_shrink(void) {
    struct cap_sets now = user_sets(), want = user_sets();
    now.permitted = cap_bit(CAP_NET_RAW) | cap_bit(CAP_KILL);
    /* Dropping one is allowed. */
    want = now;
    want.permitted = cap_bit(CAP_KILL);
    want.effective = cap_bit(CAP_KILL);
    KTEST_CHECK(cap_change_allowed(&now, &want, false));
    /* Adding one is not — this is the whole guarantee. */
    want = now;
    want.permitted = now.permitted | cap_bit(CAP_SYS_ADMIN);
    KTEST_CHECK(!cap_change_allowed(&now, &want, false));
}

static void test_effective_cannot_exceed_permitted(void) {
    struct cap_sets now = user_sets(), want;
    now.permitted = cap_bit(CAP_KILL);
    want = now;
    want.effective = cap_bit(CAP_KILL) | cap_bit(CAP_NET_RAW);
    KTEST_CHECK(!cap_change_allowed(&now, &want, false));
    want.effective = cap_bit(CAP_KILL);
    KTEST_CHECK(cap_change_allowed(&now, &want, false));
}

static void test_the_bounding_set_needs_setpcap_and_only_shrinks(void) {
    struct cap_sets now = root_sets(), want;
    want = now;
    want.bounding = now.bounding & ~cap_bit(CAP_SYS_MODULE);
    /* Shrinking it is the irreversible step, so it is gated. */
    KTEST_CHECK(!cap_change_allowed(&now, &want, false));
    KTEST_CHECK(cap_change_allowed(&now, &want, true));
    /* Growing it is refused even with the capability: "irreversible" has
     * to mean irreversible, or dropping privilege is only a suggestion. */
    now.bounding = cap_bit(CAP_KILL);
    want = now;
    want.bounding = cap_bit(CAP_KILL) | cap_bit(CAP_SYS_ADMIN);
    KTEST_CHECK(!cap_change_allowed(&now, &want, true));
}

static void test_inheritable_needs_the_right_to_it(void) {
    struct cap_sets now = user_sets(), want;
    now.permitted = cap_bit(CAP_NET_RAW);
    want = now;
    /* A process may make inheritable what it holds. */
    want.inheritable = cap_bit(CAP_NET_RAW);
    KTEST_CHECK(cap_change_allowed(&now, &want, false));
    /* But not what it does not: otherwise it could hand a child a
     * capability it never had. */
    want.inheritable = cap_bit(CAP_SYS_ADMIN);
    KTEST_CHECK(!cap_change_allowed(&now, &want, false));
}

static void test_ambient_needs_both_permitted_and_inheritable(void) {
    struct cap_sets now = user_sets(), want;
    now.permitted = cap_bit(CAP_NET_RAW);
    now.inheritable = cap_bit(CAP_NET_RAW);
    want = now;
    want.ambient = cap_bit(CAP_NET_RAW);
    KTEST_CHECK(cap_change_allowed(&now, &want, false));
    /* Permitted but not inheritable is not enough, because ambient is what
     * survives an exec and inheritable is the statement that it should. */
    want.inheritable = 0u;
    KTEST_CHECK(!cap_change_allowed(&now, &want, false));
}

static void test_exec_recomputes_rather_than_carries(void) {
    struct cap_sets s = user_sets();
    s.permitted = cap_bit(CAP_NET_RAW);
    s.effective = cap_bit(CAP_NET_RAW);
    s.inheritable = cap_bit(CAP_NET_RAW);
    /* **Without an ambient bit, nothing survives.** That is the correct
     * answer and not an oversight: inheritable alone only takes effect for
     * an executable carrying capability bits, and there are none here. */
    cap_after_exec(&s, 1000u, false);
    KTEST_CHECK(s.permitted == 0u);
    KTEST_CHECK(s.effective == 0u);

    /* With one, it does. */
    s = user_sets();
    s.permitted = cap_bit(CAP_NET_RAW);
    s.inheritable = cap_bit(CAP_NET_RAW);
    s.ambient = cap_bit(CAP_NET_RAW);
    cap_after_exec(&s, 1000u, false);
    KTEST_CHECK(cap_in(s.effective, CAP_NET_RAW));

    /* Becoming root across the exec grants the bounding set — which is
     * what makes a setuid-root program work at all. */
    s = user_sets();
    cap_after_exec(&s, 0u, false);
    KTEST_CHECK(s.effective == cap_all());

    /* **`no_new_privs` beats all of it**, including the root case: a
     * process that asked for an unprivileged exec must not gain privilege
     * from the program it runs, which is the one guarantee that makes an
     * unprivileged `seccomp` filter safe to allow. */
    s = user_sets();
    s.ambient = cap_bit(CAP_NET_RAW);
    s.permitted = cap_bit(CAP_NET_RAW);
    s.inheritable = cap_bit(CAP_NET_RAW);
    cap_after_exec(&s, 0u, true);
    KTEST_CHECK(s.ambient == 0u);
    KTEST_CHECK(!cap_in(s.effective, CAP_SYS_ADMIN));
}

static void test_the_bounding_set_bounds_an_exec(void) {
    struct cap_sets s = user_sets();
    s.bounding = cap_bit(CAP_KILL); /* everything else dropped for good */
    cap_after_exec(&s, 0u, false);
    /* Root, and still only one capability: a dropped bounding bit cannot
     * come back through an exec, which is what "no descendant can regain
     * it" has to mean to be worth anything. */
    KTEST_CHECK(s.effective == cap_bit(CAP_KILL));
    KTEST_CHECK(!cap_in(s.effective, CAP_SYS_ADMIN));
}

static void test_an_undefined_bit_is_refused(void) {
    struct cap_sets now = root_sets(), want = root_sets();
    want.permitted = cap_all() | ((uint64_t)1 << 50);
    /* Not masked away silently: a process that believed it held capability
     * 50 would be wrong in a way nothing here could tell it. */
    KTEST_CHECK(!cap_change_allowed(&now, &want, true));
}

int main(void) {
    KTEST_RUN(test_root_starts_with_everything_and_a_user_with_nothing);
    KTEST_RUN(test_the_top_capability_is_in_the_set);
    KTEST_RUN(test_permitted_may_only_shrink);
    KTEST_RUN(test_effective_cannot_exceed_permitted);
    KTEST_RUN(test_the_bounding_set_needs_setpcap_and_only_shrinks);
    KTEST_RUN(test_inheritable_needs_the_right_to_it);
    KTEST_RUN(test_ambient_needs_both_permitted_and_inheritable);
    KTEST_RUN(test_exec_recomputes_rather_than_carries);
    KTEST_RUN(test_the_bounding_set_bounds_an_exec);
    KTEST_RUN(test_an_undefined_bit_is_refused);
    return ktest_failures();
}
