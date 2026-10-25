/* Routing decisions, which need no device: a `netif` with an address and a
 * mask is enough, so the ranking rule can be tested here rather than on the
 * board. */
#include "core/net/net.h"

#include "core/errno.h"
#include "ktest.h"

static struct netif a_host_on_192_168_86(void) {
    struct netif nif = {0};
    nif.ip = IP4(192, 168, 86, 10);
    nif.mask = IP4(255, 255, 255, 0);
    nif.gateway = IP4(192, 168, 86, 1);
    return nif;
}

static void test_an_empty_table_keeps_the_old_two_rules(void) {
    struct netif nif = a_host_on_192_168_86();
    uint32_t len = 0;
    /* The property that matters most: with no routes, every caller behaves
     * exactly as it did before the table existed. */
    KTEST_CHECK(net_route_lookup(&nif, IP4(192, 168, 86, 20), NULL) == 0u);
    (void)net_route_lookup(&nif, IP4(8, 8, 8, 8), &len);
    KTEST_CHECK(len == NET_ROUTE_NONE);
}

static void test_a_specific_route_beats_a_default_one(void) {
    struct netif nif = a_host_on_192_168_86();
    uint32_t len = 0;
    /* **Added in the order that would break precedence-based ranking**: the
     * default first, so a table consulted in insertion order answers with
     * it. */
    KTEST_CHECK(net_route_add(&nif, IP4(0, 0, 0, 0), 0u, IP4(192, 168, 86, 254)) == KERR_OK);
    KTEST_CHECK(net_route_add(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0),
                              IP4(192, 168, 86, 9)) == KERR_OK);
    KTEST_CHECK(net_route_lookup(&nif, IP4(10, 1, 2, 3), &len) == IP4(192, 168, 86, 9));
    KTEST_CHECK(len == 8u);
    /* And something the /8 does not cover still takes the default. */
    KTEST_CHECK(net_route_lookup(&nif, IP4(8, 8, 8, 8), NULL) == IP4(192, 168, 86, 254));
}

static void test_a_default_route_does_not_beat_our_own_subnet(void) {
    struct netif nif = a_host_on_192_168_86();
    uint32_t len = NET_ROUTE_NONE;
    KTEST_CHECK(net_route_add(&nif, IP4(0, 0, 0, 0), 0u, IP4(192, 168, 86, 254)) == KERR_OK);
    /* The table matches — at length zero. A caller testing only *whether*
     * something matched would send a packet for its own subnet to a router,
     * which is the one answer that is always wrong. The length is what stops
     * it, and this is the test that would catch its absence. */
    KTEST_CHECK(net_route_lookup(&nif, IP4(192, 168, 86, 20), &len) == IP4(192, 168, 86, 254));
    KTEST_CHECK(len == 0u);
}

static void test_a_route_is_stored_masked(void) {
    struct netif nif = a_host_on_192_168_86();
    /* Given a host address with a short mask, which is what a careless
     * caller passes. It must match the prefix, not that one host. */
    KTEST_CHECK(net_route_add(&nif, IP4(10, 1, 2, 3), IP4(255, 0, 0, 0),
                              IP4(192, 168, 86, 9)) == KERR_OK);
    KTEST_CHECK(net_route_lookup(&nif, IP4(10, 9, 9, 9), NULL) == IP4(192, 168, 86, 9));
    KTEST_CHECK(net_route_at(&nif, 0) != NULL);
    KTEST_CHECK(net_route_at(&nif, 0)->dst == IP4(10, 0, 0, 0));
}

static void test_a_prefix_is_replaced_rather_than_duplicated(void) {
    struct netif nif = a_host_on_192_168_86();
    KTEST_CHECK(net_route_add(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0),
                              IP4(192, 168, 86, 9)) == KERR_OK);
    KTEST_CHECK(net_route_add(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0),
                              IP4(192, 168, 86, 8)) == KERR_OK);
    KTEST_CHECK(net_route_lookup(&nif, IP4(10, 1, 1, 1), NULL) == IP4(192, 168, 86, 8));
    /* One entry, so the answer cannot depend on which is scanned first. */
    KTEST_CHECK(net_route_at(&nif, 1) == NULL);
}

static void test_a_mask_with_a_hole_is_refused(void) {
    struct netif nif = a_host_on_192_168_86();
    /* 255.0.255.0 ranks by bit count the same as 255.255.0.0 and covers a
     * different set, so accepting it would make the table's ordering a lie.
     * Refused at the door, which is the only place it can be. */
    KTEST_CHECK(net_route_add(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 255, 0),
                              IP4(192, 168, 86, 9)) == KERR_INVAL);
    KTEST_CHECK(net_route_at(&nif, 0) == NULL);
}

static void test_the_table_fills_and_then_refuses(void) {
    struct netif nif = a_host_on_192_168_86();
    for (uint32_t i = 0; i < NET_ROUTES; i++) {
        KTEST_CHECK(net_route_add(&nif, IP4(10, (uint8_t)i, 0, 0), IP4(255, 255, 0, 0),
                                  IP4(192, 168, 86, 9)) == KERR_OK);
    }
    KTEST_CHECK(net_route_add(&nif, IP4(11, 0, 0, 0), IP4(255, 0, 0, 0),
                              IP4(192, 168, 86, 9)) == KERR_NOSPC);
}

static void test_a_route_can_be_removed(void) {
    struct netif nif = a_host_on_192_168_86();
    KTEST_CHECK(net_route_add(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0),
                              IP4(192, 168, 86, 9)) == KERR_OK);
    KTEST_CHECK(net_route_del(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0)) == KERR_OK);
    KTEST_CHECK(net_route_lookup(&nif, IP4(10, 1, 1, 1), NULL) == 0u);
    KTEST_CHECK(net_route_del(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0)) == KERR_NOENT);
}

static void test_an_on_link_route_sends_direct(void) {
    struct netif nif = a_host_on_192_168_86();
    uint32_t len = NET_ROUTE_NONE;
    /* A gateway of zero means the prefix needs no router. The lookup returns
     * zero for it, which is also what "nothing matched" returns — so the
     * length is what separates them, and a caller that confused the two
     * would send an on-link packet to the default gateway. */
    KTEST_CHECK(net_route_add(&nif, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0), 0u) == KERR_OK);
    KTEST_CHECK(net_route_lookup(&nif, IP4(10, 1, 1, 1), &len) == 0u);
    KTEST_CHECK(len == 8u);
}

static void test_a_null_interface_is_refused(void) {
    KTEST_CHECK(net_route_add(NULL, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0), 0u) == KERR_INVAL);
    KTEST_CHECK(net_route_del(NULL, IP4(10, 0, 0, 0), IP4(255, 0, 0, 0)) == KERR_INVAL);
    KTEST_CHECK(net_route_lookup(NULL, IP4(10, 1, 1, 1), NULL) == 0u);
    KTEST_CHECK(net_route_at(NULL, 0) == NULL);
}

int main(void) {
    KTEST_RUN(test_an_empty_table_keeps_the_old_two_rules);
    KTEST_RUN(test_a_specific_route_beats_a_default_one);
    KTEST_RUN(test_a_default_route_does_not_beat_our_own_subnet);
    KTEST_RUN(test_a_route_is_stored_masked);
    KTEST_RUN(test_a_prefix_is_replaced_rather_than_duplicated);
    KTEST_RUN(test_a_mask_with_a_hole_is_refused);
    KTEST_RUN(test_the_table_fills_and_then_refuses);
    KTEST_RUN(test_a_route_can_be_removed);
    KTEST_RUN(test_an_on_link_route_sends_direct);
    KTEST_RUN(test_a_null_interface_is_refused);
    return ktest_failures();
}
