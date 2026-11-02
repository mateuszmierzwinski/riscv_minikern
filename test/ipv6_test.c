/* IPv6 on one link: the link-local address, Neighbor Discovery and echo,
 * from frames built here with a checksum computed here — not by the code
 * under test — and including the hostile shapes a parser of untrusted
 * input has to survive: a zero-length option, a forged hop limit, a lying
 * payload length, a bad checksum, echo to a group. */
#include "core/net/net.h"
#include "core/net/tcp.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"
#include "net_frames.h"

static const uint8_t our_mac[6] = {0x02, 0x4d, 0x4b, 0x00, 0x00, 0x01};
static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const uint8_t peer_ip[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
static const uint8_t all_nodes[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static struct capture cap;
static const uint8_t f_big[NET_UDP6_MAX + 1];
static struct netif nif;

static void fresh(void) {
    struct netdev_port dev = {.send = capture_send, .self = &cap};
    struct net_events ev = {0};
    memset(&cap, 0, sizeof cap);
    net_init(&nif, our_mac, &dev, &ev);
}

/* RFC 1071 over the pseudo-header and the message, independently of the stack. */
static uint16_t upper_sum(const uint8_t *src, const uint8_t *dst, const uint8_t *m, uint32_t len, uint8_t next) {
    uint32_t sum = 0;
    uint8_t tail[8] = {0, 0, (uint8_t)(len >> 8), (uint8_t)len, 0, 0, 0, next};
    const uint8_t *parts[4] = {src, dst, tail, m};
    uint32_t lens[4] = {16, 16, 8, len};
    for (int p = 0; p < 4; p++) {
        for (uint32_t i = 0; i < lens[p]; i += 2) {
            sum += (uint32_t)(parts[p][i] << 8) | (i + 1 < lens[p] ? parts[p][i + 1] : 0);
        }
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t icmp6_sum(const uint8_t *src, const uint8_t *dst, const uint8_t *m, uint32_t len) {
    return upper_sum(src, dst, m, len, 58);
}

/* An Ethernet + IPv6 + ICMPv6 frame; `m` already has type and body, its
 * checksum is filled. Returns the frame length. */
static uint32_t frame6(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src, const uint8_t *dst,
                       uint8_t hop, uint8_t *m, uint32_t len) {
    uint16_t sum;
    m[2] = m[3] = 0;
    sum = icmp6_sum(src, dst, m, len);
    m[2] = (uint8_t)(sum >> 8);
    m[3] = (uint8_t)sum;
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, peer_mac, 6);
    f[12] = 0x86;
    f[13] = 0xdd;
    memset(f + 14, 0, 40);
    f[14] = 0x60;
    f[18] = (uint8_t)(len >> 8);
    f[19] = (uint8_t)len;
    f[20] = 58;
    f[21] = hop;
    memcpy(f + 22, src, 16);
    memcpy(f + 38, dst, 16);
    memcpy(f + 54, m, len);
    return 54 + len;
}

/* A solicitation for our address from the peer, with its link-layer option. */
static uint32_t solicitation(uint8_t *f, uint8_t hop, uint8_t opt_len) {
    uint8_t m[32] = {135};
    uint8_t group[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff};
    uint8_t mac[6] = {0x33, 0x33, 0xff};
    memcpy(m + 8, nif.ip6_link_local, 16);
    m[24] = 1;
    m[25] = opt_len;
    memcpy(m + 26, peer_mac, 6);
    memcpy(group + 13, nif.ip6_link_local + 13, 3);
    memcpy(mac + 3, nif.ip6_link_local + 13, 3);
    return frame6(f, mac, peer_ip, group, hop, m, 32);
}

static uint32_t echo(uint8_t *f, const uint8_t *dst, uint16_t seq) {
    uint8_t m[16] = {128, 0, 0, 0, 0x12, 0x34, (uint8_t)(seq >> 8), (uint8_t)seq, 'm', 'i', 'n', 'i'};
    return frame6(f, our_mac, peer_ip, dst, 64, m, 16);
}

/* An Ethernet + IPv6 + UDP frame from the peer to `dst`, port 5000 to
 * 7777, carrying "hello"; `sum` 0 asks for the right checksum, anything
 * else is written as it is (a forged one, or the forbidden zero via 1). */
static uint32_t udp6(uint8_t *f, const uint8_t *src, const uint8_t *dst, uint16_t sum) {
    uint8_t u[13] = {0x13, 0x88, 0x1e, 0x61, 0, 13, 0, 0, 'h', 'e', 'l', 'l', 'o'};
    uint16_t good = upper_sum(src, dst, u, 13, 17);
    uint16_t put = sum == 0 ? good : (sum == 1 ? 0 : sum);
    memcpy(f, our_mac, 6);
    memcpy(f + 6, peer_mac, 6);
    f[12] = 0x86;
    f[13] = 0xdd;
    memset(f + 14, 0, 40);
    f[14] = 0x60;
    f[19] = 13;
    f[20] = 17;
    f[21] = 64;
    memcpy(f + 22, src, 16);
    memcpy(f + 38, dst, 16);
    u[6] = (uint8_t)(put >> 8);
    u[7] = (uint8_t)put;
    memcpy(f + 54, u, 13);
    return 54 + 13;
}

/* What a bound port was handed. */
static struct {
    uint32_t n;
    struct udp_datagram d;
    uint8_t src6[16];
    uint8_t data[16];
} got;

static void take(void *self, struct netif *n, const struct udp_datagram *d) {
    (void)self;
    (void)n;
    got.n++;
    got.d = *d;
    memcpy(got.src6, d->src6, 16);
    memcpy(got.data, d->data, d->len < 16 ? d->len : 16);
}

static void bind_7777(void) {
    struct udp_handler h = {.receive = take};
    memset(&got, 0, sizeof got);
    KTEST_CHECK(net_udp_bind(&nif, 7777, &h) == KERR_OK);
}

static void test_the_link_local_address_comes_from_the_mac(void) {
    char text[40];
    fresh();
    ip6_format(nif.ip6_link_local, text);
    KTEST_CHECK(strcmp(text, "fe80::4d:4bff:fe00:1") == 0);
}

static void test_addresses_are_written_the_rfc_5952_way(void) {
    static const uint8_t zero[16] = {0}, loop[16] = {[15] = 1};
    static const uint8_t two_runs[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    static const uint8_t no_run[16] = {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8};
    static const uint8_t trailing[16] = {0xfe, 0x80};
    char text[40];
    ip6_format(zero, text);
    KTEST_CHECK(strcmp(text, "::") == 0);
    ip6_format(loop, text);
    KTEST_CHECK(strcmp(text, "::1") == 0);
    ip6_format(two_runs, text);
    KTEST_CHECK(strcmp(text, "2001:db8::1:0:0:1") == 0); /* the first of two equal runs */
    ip6_format(no_run, text);
    KTEST_CHECK(strcmp(text, "1:2:3:4:5:6:7:8") == 0);
    ip6_format(trailing, text);
    KTEST_CHECK(strcmp(text, "fe80::") == 0);
}

/* A solicitation is answered: to the asker's MAC, solicited, our address and
 * MAC in it, hop limit 255, and a checksum that verifies. */
static void test_a_solicitation_is_answered(void) {
    uint8_t f[128];
    const uint8_t *r;
    fresh();
    net_input(&nif, f, solicitation(f, 255, 1));
    KTEST_CHECK(cap.n == 1);
    r = cap.frame[0];
    KTEST_CHECK(memcmp(r, peer_mac, 6) == 0 && r[12] == 0x86 && r[13] == 0xdd);
    KTEST_CHECK(r[21] == 255 && memcmp(r + 38, peer_ip, 16) == 0);
    KTEST_CHECK(r[54] == 136 && (r[58] & 0x40) != 0 && memcmp(r + 62, nif.ip6_link_local, 16) == 0);
    KTEST_CHECK(r[78] == 2 && memcmp(r + 80, our_mac, 6) == 0);
    KTEST_CHECK(icmp6_sum(r + 22, r + 38, r + 54, cap.len[0] - 54) == 0);
}

/* Echo, once the asker is known; the reply is the request turned round. */
static void test_echo_is_answered_once_the_neighbour_is_known(void) {
    uint8_t f[128];
    fresh();
    net_input(&nif, f, solicitation(f, 255, 1));
    net_input(&nif, f, echo(f, nif.ip6_link_local, 7));
    KTEST_CHECK(cap.n == 2 && cap.frame[1][54] == 129);
    KTEST_CHECK(memcmp(cap.frame[1] + 58, f + 58, 12) == 0); /* id, seq, data */
    KTEST_CHECK(icmp6_sum(cap.frame[1] + 22, cap.frame[1] + 38, cap.frame[1] + 54, 16) == 0);
}

/* Echo from someone unknown: we ask who they are, holding the reply, and
 * send it when their advertisement arrives; the next one goes straight. */
static void test_an_unknown_asker_is_solicited_first(void) {
    uint8_t f[128], m[32] = {136, 0, 0, 0, 0x60};
    fresh();
    net_input(&nif, f, echo(f, nif.ip6_link_local, 1));
    KTEST_CHECK(cap.n == 1 && cap.frame[0][54] == 135 && memcmp(cap.frame[0] + 62, peer_ip, 16) == 0);
    memcpy(m + 8, peer_ip, 16);
    m[24] = 2;
    m[25] = 1;
    memcpy(m + 26, peer_mac, 6);
    net_input(&nif, f, frame6(f, our_mac, peer_ip, nif.ip6_link_local, 255, m, 32));
    KTEST_CHECK(cap.n == 2 && cap.frame[1][54] == 129 && memcmp(cap.frame[1], peer_mac, 6) == 0);
    KTEST_CHECK(cap.frame[1][61] == 1); /* the held reply: sequence 1 */
    KTEST_CHECK(icmp6_sum(cap.frame[1] + 22, cap.frame[1] + 38, cap.frame[1] + 54, 16) == 0);
    net_input(&nif, f, echo(f, nif.ip6_link_local, 2));
    KTEST_CHECK(cap.n == 3 && cap.frame[2][54] == 129 && cap.frame[2][61] == 2);
}

/* The hostile shapes: none is answered, none hangs, each is counted. */
static void test_forged_and_malformed_packets_are_refused(void) {
    uint8_t f[128];
    uint32_t len;
    fresh();
    net_input(&nif, f, solicitation(f, 64, 1)); /* routed from elsewhere */
    net_input(&nif, f, solicitation(f, 255, 0)); /* option length 0: the endless loop */
    net_input(&nif, f, solicitation(f, 255, 9)); /* option longer than the packet */
    len = solicitation(f, 255, 1);
    f[60] ^= 0xffu; /* the checksum */
    net_input(&nif, f, len);
    len = solicitation(f, 255, 1);
    f[19] = 200; /* a payload length past the frame */
    net_input(&nif, f, len);
    KTEST_CHECK(cap.n == 0);
    KTEST_CHECK(nif.stats.drop_ip6_nd == 3 && nif.stats.drop_checksum == 1);
    KTEST_CHECK(nif.stats.drop_malformed == 1);
}

/* No echo to a group, and an advertisement nobody asked for teaches nothing. */
static void test_no_amplifier_and_no_unasked_neighbours(void) {
    uint8_t f[128], m[32] = {136, 0, 0, 0, 0x20};
    fresh();
    net_input(&nif, f, solicitation(f, 255, 1));
    cap.n = 0;
    net_input(&nif, f, echo(f, all_nodes, 3));
    KTEST_CHECK(cap.n == 0);
    fresh();
    memcpy(m + 8, peer_ip, 16);
    m[24] = 2;
    m[25] = 1;
    memcpy(m + 26, peer_mac, 6);
    net_input(&nif, f, frame6(f, our_mac, peer_ip, nif.ip6_link_local, 255, m, 32));
    for (uint32_t i = 0; i < NET_ND_ENTRIES; i++) {
        KTEST_CHECK(!nif.nd[i].used);
    }
}

/* UDP over IPv6 reaches the same port table as IPv4, with both addresses,
 * and a reply goes back with a checksum over IPv6's pseudo-header. */
static void test_udp_arrives_and_a_reply_goes_back(void) {
    uint8_t f[128];
    const uint8_t *r;
    fresh();
    bind_7777();
    net_input(&nif, f, solicitation(f, 255, 1)); /* the peer is known */
    net_input(&nif, f, udp6(f, peer_ip, nif.ip6_link_local, 0));
    KTEST_CHECK(got.n == 1 && got.d.sport == 5000 && got.d.dport == 7777 && got.d.len == 5);
    KTEST_CHECK(got.d.src == 0 && memcmp(got.src6, peer_ip, 16) == 0 && memcmp(got.data, "hello", 5) == 0);
    KTEST_CHECK(net_udp6_send(&nif, peer_ip, 7777, 5000, "HELLO", 5) == KERR_OK);
    r = cap.frame[cap.n - 1];
    KTEST_CHECK(memcmp(r, peer_mac, 6) == 0 && r[20] == 17 && memcmp(r + 22, nif.ip6_link_local, 16) == 0);
    KTEST_CHECK(fg16(r + 54) == 7777 && fg16(r + 56) == 5000 && fg16(r + 58) == 13);
    KTEST_CHECK(upper_sum(r + 22, r + 38, r + 54, 13, 17) == 0 && memcmp(r + 62, "HELLO", 5) == 0);
}

/* IPv6 makes the UDP checksum mandatory: zero is refused, as is a wrong one
 * and a multicast source; none reaches the port. */
static void test_udp_without_a_true_checksum_is_refused(void) {
    static const uint8_t group_src[16] = {0xff, 0x02, [15] = 1};
    uint8_t f[128];
    fresh();
    bind_7777();
    net_input(&nif, f, udp6(f, peer_ip, nif.ip6_link_local, 1));
    net_input(&nif, f, udp6(f, peer_ip, nif.ip6_link_local, 0xbeef));
    net_input(&nif, f, udp6(f, group_src, nif.ip6_link_local, 0));
    KTEST_CHECK(got.n == 0 && nif.stats.drop_checksum == 2 && nif.stats.drop_malformed == 1);
}

/* Off the link is unreachable without a router; on it, an unknown
 * neighbour is asked first and the datagram held for the answer. */
static void test_udp_out_reaches_the_link_only(void) {
    static const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, [15] = 1};
    fresh();
    KTEST_CHECK(net_udp6_send(&nif, global, 1, 2, "x", 1) == KERR_NETUNREACH && cap.n == 0);
    KTEST_CHECK(net_udp6_send(&nif, peer_ip, 1, 2, "x", 1) == KERR_AGAIN);
    KTEST_CHECK(cap.n == 1 && cap.frame[0][54] == 135 && nif.held6_len == 40 + 9);
    KTEST_CHECK(net_udp6_send(&nif, all_nodes, 1, 2, "x", 1) == KERR_OK);
    KTEST_CHECK(cap.n == 2 && cap.frame[1][0] == 0x33 && cap.frame[1][5] == 1);
    KTEST_CHECK(net_udp6_send(&nif, peer_ip, 1, 2, f_big, NET_UDP6_MAX + 1) == KERR_INVAL);
}

/* ── TCP ───────────────────────────────────────────────────────────────── */

static struct tcp_stack tcp;

/* A 20-byte TCP segment from the peer's port 5000 to our 7778, over IPv6. */
static uint32_t tcp6(uint8_t *f, uint8_t flags, uint32_t seq, uint32_t ack) {
    uint8_t t[20] = {0x13, 0x88, 0x1e, 0x62, 0, 0, 0, 0, 0, 0, 0, 0, 0x50, 0, 0xff, 0xff};
    uint16_t sum;
    fp32(t + 4, seq);
    fp32(t + 8, ack);
    t[13] = flags;
    sum = upper_sum(peer_ip, nif.ip6_link_local, t, 20, 6);
    fp16(t + 16, sum);
    memcpy(f, our_mac, 6);
    memcpy(f + 6, peer_mac, 6);
    f[12] = 0x86;
    f[13] = 0xdd;
    memset(f + 14, 0, 40);
    f[14] = 0x60;
    f[19] = 20;
    f[20] = 6;
    f[21] = 64;
    memcpy(f + 22, peer_ip, 16);
    memcpy(f + 38, nif.ip6_link_local, 16);
    memcpy(f + 54, t, 20);
    return 54 + 20;
}

/* A listener on 7778 hearing `families`, with the peer already known. */
static struct tcp_conn *listener(uint8_t families) {
    uint8_t f[128];
    struct tcp_conn *l;
    fresh();
    KTEST_CHECK(tcp_init(&tcp, &nif, 1000) == KERR_OK);
    net_input(&nif, f, solicitation(f, 255, 1));
    l = tcp_alloc(&tcp);
    KTEST_CHECK(l != NULL && tcp_bind(l, 0, 7778) == KERR_OK);
    l->families = families;
    KTEST_CHECK(tcp_listen(l, 4) == KERR_OK);
    cap.n = 0;
    return l;
}

/* A dual-stack listener completes a handshake over IPv6: the SYN-ACK goes
 * to the peer's MAC as IPv6 with a checksum over IPv6's pseudo-header and an
 * MSS 20 bytes smaller than IPv4's, and the accepted connection is IPv6. */
static void test_tcp_handshake_over_ipv6(void) {
    uint8_t f[128];
    const uint8_t *r;
    struct tcp_conn *c, *l = listener(TCP_FAMILY_V4 | TCP_FAMILY_V6);
    net_input(&nif, f, tcp6(f, 0x02, 500, 0));
    KTEST_CHECK(cap.n == 1);
    r = cap.frame[0];
    KTEST_CHECK(memcmp(r, peer_mac, 6) == 0 && r[20] == 6 && (r[54 + 13] & 0x12) == 0x12);
    KTEST_CHECK(fg32(r + 54 + 8) == 501 && fg16(r + 54 + 22) == 1440);
    KTEST_CHECK(upper_sum(r + 22, r + 38, r + 54, fg16(r + 18), 6) == 0);
    net_input(&nif, f, tcp6(f, 0x10, 501, fg32(r + 54 + 4) + 1));
    c = tcp_accept(l);
    KTEST_CHECK(c != NULL && c->v6 && c->state == TCP_ESTABLISHED && c->remote_port == 5000);
    KTEST_CHECK(memcmp(c->remote_ip6, peer_ip, 16) == 0 && c->local_ip == 0 && c->remote_ip == 0);
}

/* A listener that hears IPv4 alone answers an IPv6 SYN with a reset —
 * over IPv6, to the sender. */
static void test_an_ipv4_listener_refuses_ipv6(void) {
    uint8_t f[128];
    struct tcp_conn *l = listener(TCP_FAMILY_V4);
    net_input(&nif, f, tcp6(f, 0x02, 500, 0));
    KTEST_CHECK(tcp_accept(l) == NULL && cap.n == 1 && cap.frame[0][20] == 6);
    KTEST_CHECK((cap.frame[0][54 + 13] & 0x04) != 0 && fg32(cap.frame[0] + 54 + 8) == 501);
    KTEST_CHECK(upper_sum(cap.frame[0] + 22, cap.frame[0] + 38, cap.frame[0] + 54, 20, 6) == 0);
}

/* An active open: off the link is unreachable, on it the SYN goes out. */
static void test_tcp_connects_on_the_link_only(void) {
    static const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, [15] = 1};
    struct tcp_conn *c;
    (void)listener(TCP_FAMILY_V4);
    c = tcp_alloc(&tcp);
    KTEST_CHECK(tcp_connect6(c, global, 80) == KERR_NETUNREACH && cap.n == 0);
    KTEST_CHECK(tcp_connect6(c, all_nodes, 80) == KERR_NETUNREACH && cap.n == 0);
    KTEST_CHECK(tcp_connect6(c, peer_ip, 80) == KERR_OK && c->state == TCP_SYN_SENT);
    KTEST_CHECK(cap.n == 1 && cap.frame[0][20] == 6 && cap.frame[0][54 + 13] == 0x02);
    KTEST_CHECK(memcmp(cap.frame[0] + 22, nif.ip6_link_local, 16) == 0);
}

int main(void) {
    KTEST_RUN(test_the_link_local_address_comes_from_the_mac);
    KTEST_RUN(test_addresses_are_written_the_rfc_5952_way);
    KTEST_RUN(test_a_solicitation_is_answered);
    KTEST_RUN(test_echo_is_answered_once_the_neighbour_is_known);
    KTEST_RUN(test_an_unknown_asker_is_solicited_first);
    KTEST_RUN(test_forged_and_malformed_packets_are_refused);
    KTEST_RUN(test_no_amplifier_and_no_unasked_neighbours);
    KTEST_RUN(test_udp_arrives_and_a_reply_goes_back);
    KTEST_RUN(test_udp_without_a_true_checksum_is_refused);
    KTEST_RUN(test_udp_out_reaches_the_link_only);
    KTEST_RUN(test_tcp_handshake_over_ipv6);
    KTEST_RUN(test_an_ipv4_listener_refuses_ipv6);
    KTEST_RUN(test_tcp_connects_on_the_link_only);
    return ktest_failures();
}
