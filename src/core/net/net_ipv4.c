/* net_ipv4 — IPv4 (RFC 791) in and out, and ICMP echo (RFC 792). */
#include "core/errno.h"
#include "core/net/net_internal.h"

#define IP_VERSION_IHL5  0x45u
#define IP_FLAG_MF       0x2000u
#define IP_FRAG_OFFSET   0x1fffu
#define IP_DEFAULT_TTL   64u
#define ICMP_ECHO_REPLY  0u
#define ICMP_ECHO_REQUEST 8u
#define ICMP_HLEN        8u
#define PING_PAYLOAD     32u

static uint16_t ip_id;

/* Broadcast for us: all-ones, or our subnet's broadcast. */
static bool is_broadcast(const struct netif *nif, ip4_t dst) {
    return dst == IP4_BROADCAST || (nif->mask != 0 && dst == (nif->ip | ~nif->mask));
}

static void deliver_upper(struct netif *nif, uint8_t proto, ip4_t src, ip4_t dst, const uint8_t *p,
                          uint32_t len);

/* A source no real peer has: ours (a reflection or LAND), broadcast,
 * multicast or class E, or loopback. 0.0.0.0 is allowed: a DHCP server
 * may answer before addresses exist. */
static bool martian_src(const struct netif *nif, ip4_t src) {
    return (nif->ip != 0 && src == nif->ip) || is_broadcast(nif, src) || (src >> 28) >= 0xeu || (src >> 24) == 127u;
}

/* Accept unicast to our address; while unconfigured, only broadcast (DHCP). */
static bool accept_dst(const struct netif *nif, ip4_t dst) {
    return (nif->ip != 0 && dst == nif->ip) || is_broadcast(nif, dst);
}

/* Header checks: version, length fields within the packet, checksum. */
static int validate(const struct netif *nif, const uint8_t *p, uint32_t len, uint32_t *hlen, uint32_t *total) {
    (void)nif;
    if (len < IP_HLEN || (p[0] >> 4) != 4u) {
        return KERR_INVAL;
    }
    *hlen = (uint32_t)(p[0] & 0xfu) * 4u;
    *total = get16(p + 2);
    if (*hlen < IP_HLEN || *hlen > *total || *total > len) {
        return KERR_INVAL;
    }
    return net_checksum(p, *hlen, 0) == 0 ? KERR_OK : KERR_FAULT;
}

bool net_is_local(const struct netif *nif, ip4_t ip) {
    return (ip >> 24) == 127u || (nif->ip != 0 && ip == nif->ip);
}

/* Queues a locally addressed packet; it is delivered after the current
 * input (or right away from a send outside input), never re-entrantly. */
static int loop_queue(struct netif *nif, uint32_t ip_len) {
    uint32_t slot;
    if (nif->loop_count == NET_LOOP_QUEUE) {
        nif->stats.drop_loop_full++;
        return KERR_AGAIN;
    }
    slot = (nif->loop_head + nif->loop_count++) % NET_LOOP_QUEUE;
    net_copy(nif->loop[slot], TX_IP(nif), ip_len);
    nif->loop_len[slot] = ip_len;
    if (nif->input_depth == 0) {
        ipv4_drain_loop(nif);
    }
    return KERR_OK;
}

static void deliver(struct netif *nif, const uint8_t *p, uint32_t len, bool local);

void ipv4_drain_loop(struct netif *nif) {
    nif->input_depth++;
    while (nif->loop_count > 0) {
        uint32_t slot = nif->loop_head;
        nif->loop_head = (nif->loop_head + 1u) % NET_LOOP_QUEUE;
        nif->loop_count--;
        deliver(nif, nif->loop[slot], nif->loop_len[slot], true);
    }
    nif->input_depth--;
}

void ipv4_input(struct netif *nif, const uint8_t *p, uint32_t len) {
    deliver(nif, p, len, false);
}

/* One IPv4 packet in; local ones skip the martian-source check (their
 * source is ours by design). */
static void deliver(struct netif *nif, const uint8_t *p, uint32_t len, bool local) {
    uint32_t hlen, total;
    ip4_t src, dst;
    int rc = validate(nif, p, len, &hlen, &total);
    if (rc != KERR_OK) {
        rc == KERR_FAULT ? nif->stats.drop_checksum++ : nif->stats.drop_malformed++;
        return;
    }
    if (get16(p + 6) & (IP_FLAG_MF | IP_FRAG_OFFSET)) {
        nif->stats.drop_fragment++;
        return;
    }
    src = get32(p + 12);
    dst = get32(p + 16);
    if (local ? !net_is_local(nif, dst) : (!accept_dst(nif, dst) || martian_src(nif, src))) {
        nif->stats.drop_not_ours++;
        return;
    }
    if (p[9] == IPPROTO_ICMP) {
        icmp_input(nif, src, dst, p + hlen, total - hlen);
    } else if (p[9] == IPPROTO_UDP) {
        udp_input(nif, src, dst, p + hlen, total - hlen);
    } else {
        deliver_upper(nif, p[9], src, dst, p + hlen, total - hlen);
    }
}

/* Hands a packet to whatever registered for its protocol number. */
static void deliver_upper(struct netif *nif, uint8_t proto, ip4_t src, ip4_t dst, const uint8_t *p,
                          uint32_t len) {
    for (uint32_t i = 0; i < NET_IP_PROTOS; i++) {
        if (nif->ip_handler[i].input != NULL && nif->ip_proto[i] == proto) {
            nif->ip_handler[i].input(nif->ip_handler[i].self, nif, src, dst, p, len);
            return;
        }
    }
    nif->stats.drop_not_ours++;
}

int net_ip_register(struct netif *nif, uint8_t proto, const struct ip_handler *h) {
    for (uint32_t i = 0; i < NET_IP_PROTOS; i++) {
        if (nif->ip_handler[i].input != NULL && nif->ip_proto[i] == proto) {
            return KERR_EXIST;
        }
    }
    for (uint32_t i = 0; i < NET_IP_PROTOS; i++) {
        if (nif->ip_handler[i].input == NULL) {
            nif->ip_proto[i] = proto;
            nif->ip_handler[i] = *h;
            return KERR_OK;
        }
    }
    return KERR_NOMEM;
}

int net_ip_send(struct netif *nif, ip4_t dst, uint8_t proto, uint32_t payload_len) {
    return ipv4_send(nif, dst, proto, payload_len);
}

int net_ip_send_from(struct netif *nif, ip4_t src, ip4_t dst, uint8_t proto, uint32_t payload_len) {
    return ipv4_send_from(nif, src, dst, proto, payload_len);
}

ip4_t net_source_for(const struct netif *nif, ip4_t dst) {
    return ipv4_source_for(nif, dst);
}

uint8_t *net_l4_buffer(struct netif *nif) {
    return TX_L4(nif);
}

/* How specific the interface's own subnet is, so that an explicit route can
 * be compared against it rather than assumed to beat it or lose to it. */
static uint32_t on_link_len(const struct netif *nif) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        n += (nif->mask >> i) & 1u;
    }
    return n;
}

/* The next hop for dst: the most specific route that covers it.
 *
 * Three sources, ranked by prefix length rather than by precedence, which is
 * what keeps a specific route from being beaten by a general one that
 * happens to be consulted first:
 *
 *   - the **routing table**, for anything the operator or a protocol added;
 *   - the interface's **own subnet**, which is a route (direct, `nif->mask`)
 *     that is not in the table;
 *   - the **default gateway**, which is a route of length zero.
 *
 * With an empty table this is exactly the two rules it replaced, which is
 * the property that matters: every existing caller is unchanged. */
static ip4_t next_hop(const struct netif *nif, ip4_t dst) {
    uint32_t from_table = NET_ROUTE_NONE;
    ip4_t via = net_route_lookup(nif, dst, &from_table);
    bool on_link = nif->mask != 0 && (dst & nif->mask) == (nif->ip & nif->mask);
    if (is_broadcast(nif, dst)) {
        return dst;
    }
    if (from_table != NET_ROUTE_NONE && !(on_link && on_link_len(nif) > from_table)) {
        /* A gateway of zero means the table called this prefix on-link. */
        return via != 0 ? via : dst;
    }
    if (on_link) {
        return dst;
    }
    return nif->gateway != 0 ? nif->gateway : dst;
}

ip4_t ipv4_source_for(const struct netif *nif, ip4_t dst) {
    return (dst >> 24) == 127u ? IP4(127, 0, 0, 1) : nif->ip;
}

static void write_header(struct netif *nif, ip4_t src, ip4_t dst, uint8_t proto, uint32_t payload_len) {
    uint8_t *h = TX_IP(nif);
    h[0] = IP_VERSION_IHL5;
    h[1] = 0;
    put16(h + 2, (uint16_t)(IP_HLEN + payload_len));
    put16(h + 4, ip_id++);
    put16(h + 6, 0);
    h[8] = IP_DEFAULT_TTL;
    h[9] = proto;
    put16(h + 10, 0);
    put32(h + 12, src);
    put32(h + 16, dst);
    put16(h + 10, net_checksum(h, IP_HLEN, 0));
}

/* Keeps the packet in nif->tx until hop's MAC is known (one at a time). */
static int hold(struct netif *nif, ip4_t hop, uint32_t ip_len) {
    net_copy(nif->held, TX_IP(nif), ip_len);
    nif->held_len = ip_len;
    nif->held_hop = hop;
    (void)arp_request(nif, hop);
    return KERR_AGAIN;
}

/* Sends what is in nif->tx. unresolved: go to the broadcast MAC rather
 * than resolving the next hop (see net_udp_send_unresolved). */
static int ipv4_emit(struct netif *nif, ip4_t src, ip4_t dst, uint8_t proto, uint32_t payload_len,
                     bool unresolved) {
    ip4_t hop = next_hop(nif, dst);
    const struct arp_entry *e;
    if (payload_len > ETH_MTU - IP_HLEN) {
        return KERR_INVAL;
    }
    write_header(nif, src != 0 ? src : ipv4_source_for(nif, dst), dst, proto, payload_len);
    if (net_is_local(nif, dst)) {
        return loop_queue(nif, IP_HLEN + payload_len);
    }
    if (unresolved || is_broadcast(nif, dst)) {
        return eth_send(nif, mac_broadcast, ETHERTYPE_IPV4, IP_HLEN + payload_len);
    }
    e = arp_lookup(nif, hop);
    if (e == NULL) {
        return hold(nif, hop, IP_HLEN + payload_len);
    }
    return eth_send(nif, e->mac, ETHERTYPE_IPV4, IP_HLEN + payload_len);
}

int ipv4_send(struct netif *nif, ip4_t dst, uint8_t proto, uint32_t payload_len) {
    return ipv4_emit(nif, 0, dst, proto, payload_len, false);
}

int ipv4_send_from(struct netif *nif, ip4_t src, ip4_t dst, uint8_t proto, uint32_t payload_len) {
    return ipv4_emit(nif, src, dst, proto, payload_len, false);
}

int ipv4_send_unresolved(struct netif *nif, ip4_t dst, uint8_t proto, uint32_t payload_len) {
    return ipv4_emit(nif, 0, dst, proto, payload_len, true);
}

void ipv4_release_held(struct netif *nif, ip4_t hop) {
    const struct arp_entry *e = arp_lookup(nif, hop);
    if (nif->held_len == 0 || nif->held_hop != hop || e == NULL) {
        return;
    }
    net_copy(TX_IP(nif), nif->held, nif->held_len);
    (void)eth_send(nif, e->mac, ETHERTYPE_IPV4, nif->held_len);
    nif->held_len = 0;
}

/* Echo request: the same identifier, sequence and data come back. */
static void echo_reply(struct netif *nif, ip4_t src, const uint8_t *p, uint32_t len) {
    uint8_t *r = TX_L4(nif);
    if (nif->icmp_tokens == 0) {
        nif->stats.drop_rate_limited++;
        return;
    }
    nif->icmp_tokens--;
    net_copy(r, p, len);
    r[0] = ICMP_ECHO_REPLY;
    put16(r + 2, 0);
    put16(r + 2, net_checksum(r, len, 0));
    (void)ipv4_send(nif, src, IPPROTO_ICMP, len);
}

/* Echo requests to a broadcast address are ignored (no smurf amplifier). */
void icmp_input(struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len) {
    if (len < ICMP_HLEN || len > ETH_MTU - IP_HLEN) {
        nif->stats.drop_malformed++;
        return;
    }
    if (net_checksum(p, len, 0) != 0) {
        nif->stats.drop_checksum++;
        return;
    }
    if (p[0] == ICMP_ECHO_REQUEST && src != 0 && !is_broadcast(nif, dst)) {
        echo_reply(nif, src, p, len);
    } else if (p[0] == ICMP_ECHO_REPLY && nif->events.echo_reply != NULL) {
        nif->events.echo_reply(nif->events.self, src, get16(p + 6));
    }
}

int net_ping(struct netif *nif, ip4_t dst, uint16_t id, uint16_t seq) {
    uint8_t *r = TX_L4(nif);
    r[0] = ICMP_ECHO_REQUEST;
    r[1] = 0;
    put16(r + 2, 0);
    put16(r + 4, id);
    put16(r + 6, seq);
    for (uint32_t i = 0; i < PING_PAYLOAD; i++) {
        r[ICMP_HLEN + i] = (uint8_t)('a' + i % 26u);
    }
    put16(r + 2, net_checksum(r, ICMP_HLEN + PING_PAYLOAD, 0));
    return ipv4_send(nif, dst, IPPROTO_ICMP, ICMP_HLEN + PING_PAYLOAD);
}
