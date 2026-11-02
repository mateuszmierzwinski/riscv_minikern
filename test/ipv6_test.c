/* IPv6 on one link: the link-local address, Neighbor Discovery and echo,
 * from frames built here with a checksum computed here — not by the code
 * under test — and including the hostile shapes a parser of untrusted
 * input has to survive: a zero-length option, a forged hop limit, a lying
 * payload length, a bad checksum, echo to a group. */
#include "core/net/net.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"
#include "net_frames.h"

static const uint8_t our_mac[6] = {0x02, 0x4d, 0x4b, 0x00, 0x00, 0x01};
static const uint8_t peer_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const uint8_t peer_ip[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
static const uint8_t all_nodes[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static struct capture cap;
static struct netif nif;

static void fresh(void) {
    struct netdev_port dev = {.send = capture_send, .self = &cap};
    struct net_events ev = {0};
    memset(&cap, 0, sizeof cap);
    net_init(&nif, our_mac, &dev, &ev);
}

/* RFC 1071 over the pseudo-header and the message, independently of the stack. */
static uint16_t icmp6_sum(const uint8_t *src, const uint8_t *dst, const uint8_t *m, uint32_t len) {
    uint32_t sum = 0;
    uint8_t tail[8] = {0, 0, (uint8_t)(len >> 8), (uint8_t)len, 0, 0, 0, 58};
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

int main(void) {
    KTEST_RUN(test_the_link_local_address_comes_from_the_mac);
    KTEST_RUN(test_addresses_are_written_the_rfc_5952_way);
    KTEST_RUN(test_a_solicitation_is_answered);
    KTEST_RUN(test_echo_is_answered_once_the_neighbour_is_known);
    KTEST_RUN(test_an_unknown_asker_is_solicited_first);
    KTEST_RUN(test_forged_and_malformed_packets_are_refused);
    KTEST_RUN(test_no_amplifier_and_no_unasked_neighbours);
    return ktest_failures();
}
