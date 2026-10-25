#include "core/net/net.h"

#include "core/errno.h"

/* **Why a table at all, when there is one interface and one gateway.**
 *
 * `next_hop` used to answer with two rules: a destination on our own subnet
 * is reached directly, everything else goes to the gateway DHCP gave us.
 * That is correct for a machine on one flat network and silently wrong for
 * every other shape — a second subnet behind a router on this one is
 * unreachable not because the packet is refused but because it is sent to
 * the default gateway, which is a different router. There is nowhere to say
 * otherwise.
 *
 * The table says otherwise. It is **longest prefix match**, which is the
 * only rule that makes a specific route beat a general one without the
 * order of insertion mattering — and the order is exactly what a table
 * built by hand, or later by netlink, cannot be trusted to get right. */

/* How specific a mask is. Popcount rather than counting leading ones,
 * because a non-contiguous mask is not this function's to reject: it is
 * accepted at insertion or not at all, and a comparison that silently
 * reorders such a mask is worse than one that ranks it by bit count. */
static uint32_t prefix_len(ip4_t mask) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        n += (mask >> i) & 1u;
    }
    return n;
}

/* A mask with no holes. Required at insertion, so that `prefix_len` is only
 * ever asked about masks where bit count and specificity are the same
 * thing. */
static bool mask_is_contiguous(ip4_t mask) {
    return (mask & (~mask >> 1)) == 0u;
}

int net_route_add(struct netif *nif, ip4_t dst, ip4_t mask, ip4_t gateway) {
    if (nif == NULL || !mask_is_contiguous(mask)) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < NET_ROUTES; i++) {
        struct route *r = &nif->route[i];
        /* **Replaced, not added twice.** A prefix with two gateways is a
         * table whose answer depends on which entry is scanned first, and
         * nothing above would report the ambiguity. */
        if (r->used && r->dst == (dst & mask) && r->mask == mask) {
            r->gateway = gateway;
            return KERR_OK;
        }
    }
    for (uint32_t i = 0; i < NET_ROUTES; i++) {
        struct route *r = &nif->route[i];
        if (!r->used) {
            /* Stored masked, so that a route given a host address and a
             * short mask matches what it was meant to match. */
            *r = (struct route){.dst = dst & mask, .mask = mask, .gateway = gateway, .used = true};
            return KERR_OK;
        }
    }
    return KERR_NOSPC;
}

int net_route_del(struct netif *nif, ip4_t dst, ip4_t mask) {
    if (nif == NULL) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < NET_ROUTES; i++) {
        struct route *r = &nif->route[i];
        if (r->used && r->dst == (dst & mask) && r->mask == mask) {
            r->used = false;
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

const struct route *net_route_at(const struct netif *nif, uint32_t index) {
    if (nif == NULL || index >= NET_ROUTES || !nif->route[index].used) {
        return NULL;
    }
    return &nif->route[index];
}

/* The gateway for dst, and how specific the route that chose it was.
 *
 * `*found_len` is what lets the caller compare this against the routes it
 * knows about itself — the interface's own subnet above all, which is a
 * route (`nif->mask`, direct) that is not in this table. Returning only a
 * gateway would make an explicit default route beat the on-link rule, which
 * is the one answer that is always wrong. */
ip4_t net_route_lookup(const struct netif *nif, ip4_t dst, uint32_t *found_len) {
    uint32_t best = 0;
    ip4_t via = 0;
    bool any = false;
    if (nif == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < NET_ROUTES; i++) {
        const struct route *r = &nif->route[i];
        uint32_t len;
        if (!r->used || (dst & r->mask) != r->dst) {
            continue;
        }
        len = prefix_len(r->mask);
        if (!any || len > best) {
            best = len;
            via = r->gateway;
            any = true;
        }
    }
    if (found_len != NULL) {
        /* **Not "no match" when a /0 route matched.** A default route's
         * length is zero, so a caller testing the length alone would read a
         * matched default route as nothing matching — and fall back to the
         * same thing by luck, until the two differed. */
        *found_len = any ? best : NET_ROUTE_NONE;
    }
    return via;
}
