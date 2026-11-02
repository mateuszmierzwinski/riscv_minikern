/* net_ipv6 — IPv6 on one link (RFC 8200): a link-local address, Neighbor
 * Discovery for it (RFC 4861) and ICMPv6 echo (RFC 4443).
 *
 * What is here is what one link needs and can be judged on the board's LAN,
 * which has no router: an address of our own from the MAC, the two
 * multicast groups that address must hear (all-nodes and its solicited-node
 * group), answering a neighbour that asks for us and remembering it, and
 * echo, UDP, and whatever registers for IPv6 (TCP). No extension headers,
 * no fragments, no SLAAC — a packet with any other next header is not ours
 * and is counted, not guessed at. */
#include "core/errno.h"
#include "core/net/net_internal.h"

#define IP6_HLEN 40u
#define IPPROTO_ICMPV6 58u
#define ICMP6_ECHO_REQUEST 128u
#define ICMP6_ECHO_REPLY 129u
#define ND_NEIGHBOR_SOLICIT 135u
#define ND_NEIGHBOR_ADVERT 136u
#define ND_OPT_SOURCE_LL 1u
#define ND_OPT_TARGET_LL 2u
#define ND_HLEN 24u          /* type, code, sum, flags, target */
#define ND_HOP_LIMIT 255u    /* RFC 4861 §7.1.1: only from the link itself */
#define NA_SOLICITED 0x40u
#define NA_OVERRIDE 0x20u
#define ECHO_HOP_LIMIT 64u
#define UDP_HOP_LIMIT 64u
#define IPPROTO_TCP6 6u        /* TCP's number; tcp.h is above this layer */
#define TX6_L4(nif) ((nif)->tx + ETH_HLEN + IP6_HLEN)

static const uint8_t all_nodes[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t unspecified[16] = {0};

static bool ip6_equal(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 16u; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* ff02::1:ffXX:XXXX — the group a neighbour asks us on (RFC 4291 §2.7.1). */
static void solicited_node(const uint8_t *ip, uint8_t out[16]) {
    static const uint8_t prefix[13] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
    net_copy(out, prefix, 13u);
    net_copy(out + 13, ip + 13, 3u);
}

/* 33:33 and the group's low 32 bits (RFC 2464 §7). */
static void multicast_mac(const uint8_t *group, uint8_t out[ETH_ALEN]) {
    out[0] = 0x33;
    out[1] = 0x33;
    net_copy(out + 2, group + 12, 4u);
}

void ipv6_init(struct netif *nif) {
    uint8_t *a = nif->ip6_link_local;
    a[0] = 0xfe;
    a[1] = 0x80;
    a[8] = nif->mac[0] ^ 0x02u; /* the universal/local bit, inverted */
    a[9] = nif->mac[1];
    a[10] = nif->mac[2];
    a[11] = 0xff;
    a[12] = 0xfe;
    a[13] = nif->mac[3];
    a[14] = nif->mac[4];
    a[15] = nif->mac[5];
}

/* A destination this interface answers: its address, or a group it is in. */
static bool ours(const struct netif *nif, const uint8_t *dst) {
    uint8_t group[16];
    solicited_node(nif->ip6_link_local, group);
    return ip6_equal(dst, nif->ip6_link_local) || ip6_equal(dst, all_nodes) ||
           ip6_equal(dst, group);
}

bool ipv6_wants_frame(const struct netif *nif, const uint8_t *dst_mac) {
    uint8_t group[16], mac[ETH_ALEN];
    solicited_node(nif->ip6_link_local, group);
    multicast_mac(group, mac);
    if (mac_equal(dst_mac, mac)) {
        return true;
    }
    multicast_mac(all_nodes, mac);
    return mac_equal(dst_mac, mac);
}

/* The upper-layer checksum's pseudo-header: both addresses, the length and
 * the next header (RFC 8200 §8.1), the same for ICMPv6, UDP and TCP. */
uint32_t net_ip6_pseudo_sum(const uint8_t *src, const uint8_t *dst, uint32_t len, uint8_t next) {
    uint8_t tail[8] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8),
                       (uint8_t)len, 0, 0, 0, next};
    return net_sum(tail, 8u, net_sum(dst, 16u, net_sum(src, 16u, 0)));
}

/* Where the checksum sits in each upper header this sends. */
static uint32_t checksum_offset(uint8_t next) {
    return next == IPPROTO_UDP ? 6u : (next == IPPROTO_TCP6 ? 16u : 2u);
}

uint8_t *net_l4_buffer6(struct netif *nif) {
    return TX6_L4(nif);
}

bool net_ip6_is_local(const struct netif *nif, const uint8_t ip[16]) {
    return ip6_equal(ip, nif->ip6_link_local);
}

bool net_ip6_on_link(const uint8_t ip[16]) {
    return (ip[0] == 0xfe && (ip[1] & 0xc0u) == 0x80u) || (ip[0] == 0xff && (ip[1] & 0x0fu) == 0x02u);
}

/* ── neighbours ────────────────────────────────────────────────────────── */

static struct nd_entry *neighbour(struct netif *nif, const uint8_t *ip) {
    for (uint32_t i = 0; i < NET_ND_ENTRIES; i++) {
        if (nif->nd[i].used && ip6_equal(nif->nd[i].ip, ip)) {
            return &nif->nd[i];
        }
    }
    return NULL;
}

/* A slot for an address, reused round-robin when the table is full. */
static struct nd_entry *remember_asked(struct netif *nif, const uint8_t *ip) {
    static uint32_t next;
    struct nd_entry *e = &nif->nd[next++ % NET_ND_ENTRIES];
    net_copy(e->ip, ip, 16u);
    e->used = true;
    e->mac_known = false;
    return e;
}

static void release_held(struct netif *nif, const uint8_t *ip, const uint8_t *mac);

/* Remembers where an address answers, and sends what was waiting for it. */
static void learn(struct netif *nif, const uint8_t *ip, const uint8_t *mac) {
    struct nd_entry *e = neighbour(nif, ip);
    if (e == NULL) {
        e = remember_asked(nif, ip);
    }
    net_copy(e->mac, mac, ETH_ALEN);
    e->mac_known = true;
    release_held(nif, ip, mac);
}

/* ── out ───────────────────────────────────────────────────────────────── */

/* Puts the IPv6 header in front of what is at TX6_L4 and fills the upper
 * header's checksum, where both addresses are known. Returns the packet's
 * length. A UDP sum of zero goes out as all ones: zero means "none", which
 * IPv6 does not allow (RFC 8200 §8.1). */
static uint32_t ipv6_build(struct netif *nif, const uint8_t *dst, uint8_t next, uint8_t hop_limit,
                           uint32_t len) {
    uint8_t *h = nif->tx + ETH_HLEN;
    uint8_t *sum_at = TX6_L4(nif) + checksum_offset(next);
    uint16_t sum;
    net_zero(h, IP6_HLEN);
    h[0] = 0x60;
    put16(h + 4, (uint16_t)len);
    h[6] = next;
    h[7] = hop_limit;
    net_copy(h + 8, nif->ip6_link_local, 16u);
    net_copy(h + 24, dst, 16u);
    put16(sum_at, 0);
    sum = net_checksum(TX6_L4(nif), len, net_ip6_pseudo_sum(h + 8, dst, len, next));
    put16(sum_at, sum == 0 && next == IPPROTO_UDP ? 0xffffu : sum);
    return IP6_HLEN + len;
}

static int ipv6_send_to(struct netif *nif, const uint8_t *mac, const uint8_t *dst, uint8_t next,
                        uint8_t hop_limit, uint32_t len) {
    return eth_send(nif, mac, ETHERTYPE_IPV6, ipv6_build(nif, dst, next, hop_limit, len));
}

/* Sends the held packet once its neighbour has answered. */
static void release_held(struct netif *nif, const uint8_t *ip, const uint8_t *mac) {
    if (nif->held6_len == 0 || !ip6_equal(nif->held6_dst, ip)) {
        return;
    }
    net_copy(nif->tx + ETH_HLEN, nif->held6, nif->held6_len);
    (void)eth_send(nif, mac, ETHERTYPE_IPV6, nif->held6_len);
    nif->held6_len = 0;
}

/* Asks who has `target`, from our address to its solicited-node group,
 * with our link-layer address so the answer can come straight back. */
static void solicit(struct netif *nif, const uint8_t *target) {
    uint8_t *m = TX6_L4(nif), group[16], mac[ETH_ALEN];
    net_zero(m, ND_HLEN + 8u);
    m[0] = ND_NEIGHBOR_SOLICIT;
    net_copy(m + 8, target, 16u);
    m[ND_HLEN] = ND_OPT_SOURCE_LL;
    m[ND_HLEN + 1u] = 1u;
    net_copy(m + ND_HLEN + 2u, nif->mac, ETH_ALEN);
    solicited_node(target, group);
    multicast_mac(group, mac);
    (void)ipv6_send_to(nif, mac, group, IPPROTO_ICMPV6, ND_HOP_LIMIT, ND_HLEN + 8u);
}

/* Unicast to a neighbour: its remembered MAC; or, not yet known, the packet
 * held (one, as IPv4 holds one for ARP) and a solicitation sent — the first
 * reply to a host that still had our MAC cached was lost without it. */
static int ipv6_send(struct netif *nif, const uint8_t *dst, uint8_t next, uint8_t hop_limit, uint32_t len) {
    struct nd_entry *e = neighbour(nif, dst);
    if (e != NULL && e->mac_known) {
        return ipv6_send_to(nif, e->mac, dst, next, hop_limit, len);
    }
    if (e == NULL) {
        (void)remember_asked(nif, dst);
    }
    nif->held6_len = ipv6_build(nif, dst, next, hop_limit, len);
    net_copy(nif->held6, nif->tx + ETH_HLEN, nif->held6_len);
    net_copy(nif->held6_dst, dst, 16u);
    solicit(nif, dst);
    return KERR_AGAIN;
}

/* ── in ────────────────────────────────────────────────────────────────── */

/* Walks ND options for the link-layer address of `want`; false when an
 * option is malformed — a length of zero would otherwise loop for ever,
 * which is the class of bug this parser is written against. */
static bool nd_option(const uint8_t *opt, uint32_t len, uint8_t want, const uint8_t **mac) {
    *mac = NULL;
    while (len >= 2u) {
        uint32_t bytes = (uint32_t)opt[1] * 8u;
        if (bytes == 0u || bytes > len) {
            return false;
        }
        if (opt[0] == want && bytes >= 8u) {
            *mac = opt + 2;
        }
        opt += bytes;
        len -= bytes;
    }
    return len == 0u;
}

/* Answers a solicitation for our address: to the asker, or to all nodes
 * when it asked from :: (duplicate-address detection). */
static void advertise(struct netif *nif, const uint8_t *to, bool solicited) {
    uint8_t *m = TX6_L4(nif), mac[ETH_ALEN];
    net_zero(m, ND_HLEN + 8u);
    m[0] = ND_NEIGHBOR_ADVERT;
    m[4] = (uint8_t)(NA_OVERRIDE | (solicited ? NA_SOLICITED : 0u));
    net_copy(m + 8, nif->ip6_link_local, 16u);
    m[ND_HLEN] = ND_OPT_TARGET_LL;
    m[ND_HLEN + 1u] = 1u;
    net_copy(m + ND_HLEN + 2u, nif->mac, ETH_ALEN);
    if (solicited) {
        (void)ipv6_send(nif, to, IPPROTO_ICMPV6, ND_HOP_LIMIT, ND_HLEN + 8u);
        return;
    }
    multicast_mac(all_nodes, mac);
    (void)ipv6_send_to(nif, mac, all_nodes, IPPROTO_ICMPV6, ND_HOP_LIMIT, ND_HLEN + 8u);
}

static void solicitation_in(struct netif *nif, const uint8_t *src, const uint8_t *m,
                            uint32_t len) {
    const uint8_t *mac = NULL;
    if (!ip6_equal(m + 8, nif->ip6_link_local)) {
        return; /* someone else's address */
    }
    if (!nd_option(m + ND_HLEN, len - ND_HLEN, ND_OPT_SOURCE_LL, &mac)) {
        nif->stats.drop_ip6_nd++;
        return;
    }
    if (ip6_equal(src, unspecified)) {
        advertise(nif, all_nodes, false);
        return;
    }
    if (mac != NULL) {
        learn(nif, src, mac);
    }
    advertise(nif, src, true);
}

/* An advertisement completes a solicitation we sent; one we did not ask
 * for teaches nothing (RFC 4861 §7.2.5: no entry, no new one). */
static void advertisement_in(struct netif *nif, const uint8_t *m, uint32_t len) {
    const uint8_t *mac = NULL;
    if (!nd_option(m + ND_HLEN, len - ND_HLEN, ND_OPT_TARGET_LL, &mac)) {
        nif->stats.drop_ip6_nd++;
        return;
    }
    if (mac != NULL && neighbour(nif, m + 8) != NULL) {
        learn(nif, m + 8, mac);
    }
}

static void echo_in(struct netif *nif, const uint8_t *src, const uint8_t *dst, const uint8_t *m,
                    uint32_t len) {
    if (!ip6_equal(dst, nif->ip6_link_local)) {
        return; /* no echo to a group: no amplifier */
    }
    if (nif->icmp_tokens == 0) {
        nif->stats.drop_rate_limited++;
        return;
    }
    nif->icmp_tokens--;
    net_copy(TX6_L4(nif), m, len);
    TX6_L4(nif)[0] = ICMP6_ECHO_REPLY;
    (void)ipv6_send(nif, src, IPPROTO_ICMPV6, ECHO_HOP_LIMIT, len);
}

static void icmp6_in(struct netif *nif, const uint8_t *h, const uint8_t *m, uint32_t len) {
    bool nd = m[0] == ND_NEIGHBOR_SOLICIT || m[0] == ND_NEIGHBOR_ADVERT;
    if (net_checksum(m, len, net_ip6_pseudo_sum(h + 8, h + 24, len, IPPROTO_ICMPV6)) != 0) {
        nif->stats.drop_checksum++;
        return;
    }
    if (nd && (h[7] != ND_HOP_LIMIT || m[1] != 0 || len < ND_HLEN)) {
        nif->stats.drop_ip6_nd++;
        return;
    }
    if (m[0] == ND_NEIGHBOR_SOLICIT) {
        solicitation_in(nif, h + 8, m, len);
    } else if (m[0] == ND_NEIGHBOR_ADVERT) {
        advertisement_in(nif, m, len);
    } else if (m[0] == ICMP6_ECHO_REQUEST && len >= 8u) {
        echo_in(nif, h + 8, h + 24, m, len);
    }
}

/* UDP over IPv6 (RFC 8200 §8.1): the checksum is **mandatory**, so a zero
 * one is refused rather than taken as "not computed" as IPv4 allows; and a
 * multicast source is nobody to answer. What passes goes to the same port
 * table IPv4 feeds. */
static void udp6_in(struct netif *nif, const uint8_t *h, const uint8_t *m, uint32_t len) {
    struct udp_datagram d;
    uint32_t ulen;
    if (len < UDP_HLEN || (ulen = get16(m + 4)) < UDP_HLEN || ulen > len || h[8] == 0xff) {
        nif->stats.drop_malformed++;
        return;
    }
    if (get16(m + 6) == 0 || net_checksum(m, ulen, net_ip6_pseudo_sum(h + 8, h + 24, ulen, IPPROTO_UDP)) != 0) {
        nif->stats.drop_checksum++;
        return;
    }
    d = (struct udp_datagram){.sport = get16(m), .dport = get16(m + 2), .data = m + UDP_HLEN,
                              .len = ulen - UDP_HLEN, .src6 = h + 8, .dst6 = h + 24};
    udp_dispatch(nif, &d);
}

int net_ip6_send(struct netif *nif, const uint8_t dst[16], uint8_t proto, uint32_t len) {
    if (!net_ip6_on_link(dst) || dst[0] == 0xff) {
        return KERR_NETUNREACH; /* and no TCP to a group */
    }
    return ipv6_send(nif, dst, proto, UDP_HOP_LIMIT, len);
}

/* Hands a packet to the protocol that registered for it over IPv6. */
static void deliver_upper6(struct netif *nif, const uint8_t *h, const uint8_t *p, uint32_t len) {
    for (uint32_t i = 0; i < NET_IP_PROTOS; i++) {
        if (nif->ip_handler[i].input6 != NULL && nif->ip_proto[i] == h[6]) {
            nif->stats.rx_ip6++;
            nif->ip_handler[i].input6(nif->ip_handler[i].self, nif, h + 8, h + 24, p, len);
            return;
        }
    }
    nif->stats.drop_not_ours++;
}

int net_udp6_send(struct netif *nif, const uint8_t dst[16], uint16_t sport, uint16_t dport, const void *data,
                  uint32_t len) {
    uint8_t *u = TX6_L4(nif), mac[ETH_ALEN];
    if (!net_ip6_on_link(dst)) {
        return KERR_NETUNREACH;
    }
    if (len > NET_UDP6_MAX) {
        return KERR_INVAL;
    }
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint16_t)(UDP_HLEN + len));
    net_copy(u + UDP_HLEN, data, len);
    if (dst[0] == 0xff) {
        multicast_mac(dst, mac);
        return ipv6_send_to(nif, mac, dst, IPPROTO_UDP, UDP_HOP_LIMIT, UDP_HLEN + len);
    }
    return ipv6_send(nif, dst, IPPROTO_UDP, UDP_HOP_LIMIT, UDP_HLEN + len);
}

void ipv6_input(struct netif *nif, const uint8_t *p, uint32_t len) {
    uint32_t payload;
    if (len < IP6_HLEN || (p[0] >> 4) != 6u) {
        nif->stats.drop_malformed++;
        return;
    }
    payload = get16(p + 4);
    if (payload > len - IP6_HLEN || payload > ETH_MTU - IP6_HLEN || payload < 4u) {
        nif->stats.drop_malformed++;
        return;
    }
    if (!ours(nif, p + 24)) {
        nif->stats.drop_not_ours++;
        return;
    }
    if (p[6] != IPPROTO_ICMPV6 && p[6] != IPPROTO_UDP) {
        deliver_upper6(nif, p, p + IP6_HLEN, payload);
        return;
    }
    nif->stats.rx_ip6++;
    if (p[6] == IPPROTO_UDP) {
        udp6_in(nif, p, p + IP6_HLEN, payload);
        return;
    }
    icmp6_in(nif, p, p + IP6_HLEN, payload);
}

/* ── text ──────────────────────────────────────────────────────────────── */

/* The longest run of two or more zero groups, by its first group; 8 none. */
static uint32_t longest_zero_run(const uint8_t *a, uint32_t *run_len) {
    uint32_t best = 8u, best_len = 0, at = 0;
    while (at < 8u) {
        uint32_t n = 0;
        while (at + n < 8u && a[2u * (at + n)] == 0 && a[2u * (at + n) + 1u] == 0) {
            n++;
        }
        if (n > best_len && n >= 2u) {
            best = at;
            best_len = n;
        }
        at += n == 0u ? 1u : n;
    }
    *run_len = best_len;
    return best;
}

static uint32_t put_group(char *out, uint32_t group) {
    static const char hex[] = "0123456789abcdef";
    uint32_t n = 0;
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        uint32_t d = (group >> (uint32_t)shift) & 0xfu;
        if (d != 0u || started || shift == 0) {
            out[n++] = hex[d];
            started = true;
        }
    }
    return n;
}

uint32_t ip6_format(const uint8_t a[16], char *out) {
    uint32_t run_len = 0, n = 0;
    uint32_t run = longest_zero_run(a, &run_len);
    for (uint32_t g = 0; g < 8u;) {
        if (g == run) {
            out[n++] = ':';
            out[n++] = ':';
            g += run_len;
            continue;
        }
        if (g != 0u && g != run + run_len) {
            out[n++] = ':';
        }
        n += put_group(out + n, ((uint32_t)a[2u * g] << 8) | a[2u * g + 1u]);
        g++;
    }
    out[n] = '\0';
    return n;
}
