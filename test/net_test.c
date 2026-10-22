/* The IPv4 stack, end to end, against crafted and hostile frames. A fake
 * netdev captures what the stack sends; frames "from the network" are built
 * here with their own checksums. */
#include "core/net/net.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/errno.h"
#include "ktest.h"
#include "net_frames.h"

static const uint8_t our_mac[6] = {0x02, 0x4d, 0x4b, 0x00, 0x00, 0x01};
static const uint8_t gw_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const uint32_t xid = 0x1234abcdu;

static struct capture cap;
static struct netif nif;
static int bound_calls;
static int echo_replies;
static uint16_t last_echo_seq;

static void on_bound(void *self, const struct netif *n) {
    (void)self;
    (void)n;
    bound_calls++;
}

static void on_echo(void *self, ip4_t from, uint16_t seq) {
    (void)self;
    (void)from;
    echo_replies++;
    last_echo_seq = seq;
}

static void fresh(void) {
    struct netdev_port dev = {.send = capture_send, .self = &cap};
    struct net_events ev = {.dhcp_bound = on_bound, .echo_reply = on_echo};
    memset(&cap, 0, sizeof cap);
    bound_calls = echo_replies = 0;
    net_init(&nif, our_mac, &dev, &ev);
}

/* Configured as slirp would leave us. */
static void configured(void) {
    fresh();
    nif.ip = IP4(10, 0, 2, 15);
    nif.mask = IP4(255, 255, 255, 0);
    nif.gateway = IP4(10, 0, 2, 2);
}

/* The frames this test builds all come from the gateway's MAC. */
static uint8_t *ip_frame(uint8_t *f, const uint8_t *dst_mac, ip4_t src, ip4_t dst, uint8_t proto, uint32_t plen) {
    return frame_ip(f, dst_mac, gw_mac, src, dst, proto, plen);
}

static uint32_t udp_frame(uint8_t *f, const uint8_t *dst_mac, ip4_t src, ip4_t dst, uint16_t sport, uint16_t dport,
                          const void *data, uint32_t len) {
    return frame_udp(f, dst_mac, gw_mac, src, dst, sport, dport, data, len);
}

/* A DHCP reply of type msg offering yiaddr, with the usual options. */
static uint32_t dhcp_reply_leased(uint8_t *f, uint8_t msg, ip4_t yiaddr, uint32_t with_xid,
                                  const uint8_t *chaddr, uint32_t lease);

static uint32_t dhcp_reply(uint8_t *f, uint8_t msg, ip4_t yiaddr, uint32_t with_xid, const uint8_t *chaddr) {
    return dhcp_reply_leased(f, msg, yiaddr, with_xid, chaddr, 86400);
}

static uint32_t dhcp_reply_leased(uint8_t *f, uint8_t msg, ip4_t yiaddr, uint32_t with_xid,
                                  const uint8_t *chaddr, uint32_t lease) {
    uint8_t m[300] = {0};
    uint32_t at = 240;
    m[0] = 2;
    m[1] = 1;
    m[2] = 6;
    fp32(m + 4, with_xid);
    fp32(m + 16, yiaddr);
    memcpy(m + 28, chaddr, 6);
    fp32(m + 236, 0x63825363u);
    m[at++] = 53, m[at++] = 1, m[at++] = msg;
    m[at++] = 54, m[at++] = 4, fp32(m + at, IP4(10, 0, 2, 2)), at += 4;
    m[at++] = 1, m[at++] = 4, fp32(m + at, IP4(255, 255, 255, 0)), at += 4;
    m[at++] = 3, m[at++] = 4, fp32(m + at, IP4(10, 0, 2, 2)), at += 4;
    m[at++] = 6, m[at++] = 4, fp32(m + at, IP4(10, 0, 2, 3)), at += 4;
    m[at++] = 51, m[at++] = 4, fp32(m + at, lease), at += 4;
    m[at] = 255;
    return udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4_BROADCAST, 67, 68, m, sizeof m);
}

/* The DHCP message type option of a frame we sent. */
static int sent_dhcp_type(uint32_t i) {
    const uint8_t *m = cap.frame[i] + 14 + 20 + 8;
    for (uint32_t at = 240; at + 2 < 300 && m[at] != 255; at += 2u + m[at + 1]) {
        if (m[at] == 53) {
            return m[at + 2];
        }
    }
    return -1;
}

static void test_ip4_parse(void) {
    ip4_t ip = 0;
    KTEST_CHECK(ip4_parse("10.0.2.2", 8, &ip) == KERR_OK && ip == IP4(10, 0, 2, 2));
    KTEST_CHECK(ip4_parse("255.255.255.255", 15, &ip) == KERR_OK && ip == IP4_BROADCAST);
    KTEST_CHECK(ip4_parse("0.0.0.0", 7, &ip) == KERR_OK && ip == 0);
    KTEST_CHECK(ip4_parse("10.0.2.256", 10, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("10.0.2", 6, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("10.0.2.2.2", 10, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("10.0.2.x", 8, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("10.0.2.2 ", 9, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("1.2.3.0004", 10, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("", 0, &ip) == KERR_INVAL);
    KTEST_CHECK(ip4_parse("10.0.2.2", 5, &ip) == KERR_INVAL);  /* bounded by len */
}

static void test_checksum_rfc1071(void) {
    static const uint8_t v[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};  /* RFC 1071 §3 */
    KTEST_CHECK(net_checksum(v, sizeof v, 0) == (uint16_t)~0xddf2u);
    KTEST_CHECK(net_checksum(v, 3, 0) == (uint16_t)~(0x0001u + 0xf200u));  /* odd length */
}

static void test_dhcp_conversation(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint32_t len;
    fresh();
    dhcp_start(&nif, xid);
    KTEST_CHECK(cap.n == 1 && sent_dhcp_type(0) == 1);  /* DISCOVER */
    KTEST_CHECK(memcmp(cap.frame[0], "\xff\xff\xff\xff\xff\xff", 6) == 0);
    KTEST_CHECK(fg32(cap.frame[0] + 14 + 16) == IP4_BROADCAST && fg16(cap.frame[0] + 34) == 68);
    KTEST_CHECK(fg32(cap.frame[0] + 42 + 4) == xid && fg32(cap.frame[0] + 42 + 236) == 0x63825363u);
    len = dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, our_mac);
    net_input(&nif, f, len);
    KTEST_CHECK(cap.n == 2 && sent_dhcp_type(1) == 3);  /* REQUEST */
    len = dhcp_reply(f, 5, IP4(10, 0, 2, 15), xid, our_mac);
    net_input(&nif, f, len);
    KTEST_CHECK(bound_calls == 1 && nif.ip == IP4(10, 0, 2, 15));
    KTEST_CHECK(nif.gateway == IP4(10, 0, 2, 2) && nif.mask == IP4(255, 255, 255, 0) && nif.dns == IP4(10, 0, 2, 3));
    KTEST_CHECK(nif.lease_s == 86400);
}

/* A lease is a loan with a term. These four cases are its clock: ask the
 * granting server at half, ask anyone at seven eighths, and stop using the
 * address when it runs out — an address past its lease is someone else's to
 * hand out, and keeping it is a conflict rather than a convenience. */
static void bind_with_lease(uint8_t *f, uint32_t lease) {
    fresh();
    dhcp_start(&nif, xid);
    net_input(&nif, f, dhcp_reply_leased(f, 2, IP4(10, 0, 2, 15), xid, our_mac, lease));
    net_input(&nif, f, dhcp_reply_leased(f, 5, IP4(10, 0, 2, 15), xid, our_mac, lease));
    KTEST_CHECK(nif.ip == IP4(10, 0, 2, 15) && nif.dhcp.state == DHCP_BOUND);
}

/* Advances the clock and ticks, as the kernel's timer hook would, and
 * answers any ARP request that goes out on the way.
 *
 * A renewal is sent **to the server**, so it needs that server's hardware
 * address first — and this stack believes only ARP replies it asked for, so
 * the request has to be answered for the renewal to leave at all. Which is
 * the real behaviour, not an artefact of the test. */
static void run_until(uint64_t ms) {
    uint8_t f[ETH_FRAME_MAX];
    for (uint64_t t = nif.now_ms; t <= ms; t += 100u) {
        net_tick(&nif, t);
        for (uint32_t i = 0; i < cap.n; i++) {
            if (frame_is_arp_request(cap.frame[i], cap.len[i], IP4(10, 0, 2, 2))) {
                uint32_t len = frame_arp_reply(f, cap.frame[i], IP4(10, 0, 2, 2), gw_mac);
                cap.frame[i][12] = 0; /* answered: do not answer it again */
                net_input(&nif, f, len);
            }
        }
    }
}

/* The last DHCP message we sent, or -1. The frames in between are the ARP
 * exchange a unicast renewal needs first. */
static int last_dhcp_frame(void) {
    for (uint32_t i = cap.n; i-- > 0;) {
        if (cap.len[i] >= 42u + 240u && fg16(cap.frame[i] + 12) == 0x0800u &&
            fg16(cap.frame[i] + 36) == 67u) {
            return (int)i;
        }
    }
    return -1;
}

static void test_dhcp_renews_at_half_the_lease(void) {
    uint8_t f[ETH_FRAME_MAX];
    int at;
    bind_with_lease(f, 100);           /* T1 at 50 s, T2 at 87.5 s */
    cap.n = 0;
    run_until(40000);
    KTEST_CHECK(cap.n == 0);           /* nothing yet: the lease is young */
    run_until(56000);                  /* past T1, and past the first retry */
    KTEST_CHECK(nif.dhcp.state == DHCP_RENEWING);
    at = last_dhcp_frame();
    KTEST_CHECK(at >= 0);
    if (at < 0) {
        return;
    }
    KTEST_CHECK(sent_dhcp_type((uint32_t)at) == 3);  /* REQUEST */
    /* A renewal goes **to the server**, from our own address, and carries
     * the address in ciaddr rather than as a requested-address option. */
    KTEST_CHECK(fg32(cap.frame[at] + 14 + 12) == IP4(10, 0, 2, 15));
    KTEST_CHECK(fg32(cap.frame[at] + 14 + 16) == IP4(10, 0, 2, 2));
    KTEST_CHECK(fg32(cap.frame[at] + 42 + 12) == IP4(10, 0, 2, 15));
}

static void test_dhcp_rebinds_at_seven_eighths(void) {
    uint8_t f[ETH_FRAME_MAX];
    bind_with_lease(f, 100);
    cap.n = 0;
    run_until(90000);                  /* past T2, with no server answering */
    KTEST_CHECK(nif.dhcp.state == DHCP_REBINDING);
    /* Rebinding has stopped expecting that server: it asks the subnet. */
    int at = last_dhcp_frame();
    KTEST_CHECK(at >= 0 && fg32(cap.frame[at] + 14 + 16) == IP4_BROADCAST);
    KTEST_CHECK(nif.ip == IP4(10, 0, 2, 15)); /* still ours until it expires */
}

static void test_dhcp_gives_the_address_up_when_the_lease_expires(void) {
    uint8_t f[ETH_FRAME_MAX];
    bind_with_lease(f, 100);
    run_until(101000);
    KTEST_CHECK(nif.ip == 0);          /* not ours: it must not be used */
    KTEST_CHECK(nif.dhcp.state == DHCP_SELECTING);
}

/* An answered renewal puts the clock back and keeps the address, without
 * the bound callback firing again — nothing above has changed. */
static void test_an_answered_renewal_extends_the_lease(void) {
    uint8_t f[ETH_FRAME_MAX];
    bind_with_lease(f, 100);
    run_until(51000);
    KTEST_CHECK(nif.dhcp.state == DHCP_RENEWING);
    net_input(&nif, f, dhcp_reply_leased(f, 5, IP4(10, 0, 2, 15), xid, our_mac, 100));
    KTEST_CHECK(nif.dhcp.state == DHCP_BOUND);
    KTEST_CHECK(nif.ip == IP4(10, 0, 2, 15));
    cap.n = 0;
    run_until(90000);
    KTEST_CHECK(cap.n == 0);           /* the new T1 is 50 s away again */
}

/* A server that says no takes the address away: it must not be used while
 * another is being looked for. */
static void test_a_naked_renewal_drops_the_address(void) {
    uint8_t f[ETH_FRAME_MAX];
    bind_with_lease(f, 100);
    run_until(51000);
    net_input(&nif, f, dhcp_reply_leased(f, 6, IP4(10, 0, 2, 15), xid, our_mac, 100));
    KTEST_CHECK(nif.ip == 0 && nif.dhcp.state == DHCP_SELECTING);
}

static void test_dhcp_retransmits_with_backoff(void) {
    fresh();
    dhcp_start(&nif, xid);
    net_tick(&nif, 500);
    KTEST_CHECK(cap.n == 1);
    net_tick(&nif, 1000);
    KTEST_CHECK(cap.n == 2);
    net_tick(&nif, 2500);
    KTEST_CHECK(cap.n == 2);  /* the second wait is 2 s */
    net_tick(&nif, 3000);
    KTEST_CHECK(cap.n == 3);
}

static void test_dhcp_rejects_foreign_replies(void) {
    static const uint8_t other_mac[6] = {2, 0, 0, 0, 0, 9};
    uint8_t f[ETH_FRAME_MAX];
    fresh();
    dhcp_start(&nif, xid);
    net_input(&nif, f, dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid + 1, our_mac));  /* wrong xid */
    net_input(&nif, f, dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, other_mac));  /* not our MAC */
    net_input(&nif, f, dhcp_reply(f, 2, IP4(127, 0, 0, 1), xid, our_mac));    /* loopback */
    net_input(&nif, f, dhcp_reply(f, 2, IP4_BROADCAST, xid, our_mac));
    net_input(&nif, f, dhcp_reply(f, 2, IP4(224, 0, 0, 1), xid, our_mac));    /* multicast */
    KTEST_CHECK(cap.n == 1 && nif.dhcp.state == DHCP_SELECTING);
    net_input(&nif, f, dhcp_reply(f, 5, IP4(10, 0, 2, 15), xid, our_mac));    /* ACK unasked */
    KTEST_CHECK(nif.ip == 0 && bound_calls == 0);
}

static void test_dhcp_hostile_options(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint32_t len;
    uint8_t *opts;
    fresh();
    dhcp_start(&nif, xid);
    len = dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, our_mac);
    opts = f + 42 + 240;
    opts[4] = 200;  /* the server-id option claims 200 bytes */
    fp16(f + 40, 0);  /* checksum off, so the length lie reaches the parser */
    net_input(&nif, f, len);
    KTEST_CHECK(cap.n == 1 && nif.stats.drop_malformed >= 1);
    len = dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, our_mac);
    memset(f + 42 + 240, 0, 60);  /* all PAD, no END */
    fp16(f + 40, 0);
    net_input(&nif, f, len);
    KTEST_CHECK(cap.n == 1);
    len = dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, our_mac);
    net_input(&nif, f, 14 + 20 + 8 + 100);  /* truncated frame: UDP length lies */
    KTEST_CHECK(cap.n == 1);
}

static void test_dhcp_nak_restarts(void) {
    uint8_t f[ETH_FRAME_MAX];
    fresh();
    dhcp_start(&nif, xid);
    net_input(&nif, f, dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, our_mac));
    KTEST_CHECK(nif.dhcp.state == DHCP_REQUESTING);
    net_input(&nif, f, dhcp_reply(f, 6, 0, xid, our_mac));
    KTEST_CHECK(nif.dhcp.state == DHCP_SELECTING);
    net_tick(&nif, 1);
    KTEST_CHECK(sent_dhcp_type(cap.n - 1) == 1);
}

/* An ARP packet from the gateway. */
static uint32_t arp_frame(uint8_t *f, uint16_t op, ip4_t spa, ip4_t tpa) {
    memcpy(f, op == 1 ? (const uint8_t *)"\xff\xff\xff\xff\xff\xff" : our_mac, 6);
    memcpy(f + 6, gw_mac, 6);
    fp16(f + 12, 0x0806);
    fp16(f + 14, 1);
    fp16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    fp16(f + 20, op);
    memcpy(f + 22, gw_mac, 6);
    fp32(f + 28, spa);
    memcpy(f + 32, our_mac, 6);
    fp32(f + 38, tpa);
    return 42;
}

static void test_arp_answers_and_resists_poisoning(void) {
    uint8_t f[64];
    configured();
    net_input(&nif, f, arp_frame(f, 1, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15)));
    KTEST_CHECK(cap.n == 1 && fg16(cap.frame[0] + 20) == 2);  /* reply */
    KTEST_CHECK(memcmp(cap.frame[0] + 22, our_mac, 6) == 0 && fg32(cap.frame[0] + 38) == IP4(10, 0, 2, 2));
    net_input(&nif, f, arp_frame(f, 2, IP4(10, 0, 2, 77), IP4(10, 0, 2, 15)));  /* unsolicited */
    KTEST_CHECK(nif.stats.drop_arp_unsolicited == 1);
    net_input(&nif, f, arp_frame(f, 1, IP4(10, 0, 2, 2), IP4(10, 0, 2, 99)));  /* not for us */
    KTEST_CHECK(cap.n == 1);
    net_input(&nif, f, 30);  /* short */
    KTEST_CHECK(nif.stats.drop_malformed == 1);
}

static void test_udp_waits_for_arp_then_sends(void) {
    uint8_t f[64];
    configured();
    KTEST_CHECK(net_udp_send(&nif, IP4(10, 0, 2, 2), 1000, 2000, "hi", 2) == KERR_AGAIN);
    KTEST_CHECK(cap.n == 1 && fg16(cap.frame[0] + 12) == 0x0806);  /* ARP request first */
    net_input(&nif, f, arp_frame(f, 2, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15)));
    KTEST_CHECK(cap.n == 2 && fg16(cap.frame[1] + 12) == 0x0800);
    KTEST_CHECK(memcmp(cap.frame[1], gw_mac, 6) == 0 && fg16(cap.frame[1] + 36) == 2000);
    KTEST_CHECK(net_udp_send(&nif, IP4(8, 8, 8, 8), 1000, 53, "q", 1) == KERR_OK);  /* via the gateway */
    KTEST_CHECK(memcmp(cap.frame[2], gw_mac, 6) == 0 && fg32(cap.frame[2] + 30) == IP4(8, 8, 8, 8));
}

/* An ICMP echo request; corrupt breaks its checksum. */
static uint32_t ping_frame(uint8_t *f, uint16_t seq, bool corrupt) {
    uint8_t *ic = ip_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 1, 16);
    memset(ic, 0, 16);
    ic[0] = 8;
    fp16(ic + 4, 0x77);
    fp16(ic + 6, seq);
    memcpy(ic + 8, "payload!", 8);
    fp16(ic + 2, net_checksum(ic, 16, 0));
    ic[9] ^= corrupt ? 1 : 0;
    return 14 + 20 + 16;
}

static void learn_gateway(void) {
    uint8_t f[64];
    (void)net_ping(&nif, IP4(10, 0, 2, 2), 1, 1);
    net_input(&nif, f, arp_frame(f, 2, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15)));
    cap.n = 0;
}

static void test_icmp_echo(void) {
    uint8_t f[ETH_FRAME_MAX];
    configured();
    learn_gateway();
    net_input(&nif, f, ping_frame(f, 5, false));
    KTEST_CHECK(cap.n == 1 && cap.frame[0][34] == 0 && fg16(cap.frame[0] + 40) == 5);
    KTEST_CHECK(memcmp(cap.frame[0] + 42, "payload!", 8) == 0);
    KTEST_CHECK(net_checksum(cap.frame[0] + 34, 16, 0) == 0 && net_checksum(cap.frame[0] + 14, 20, 0) == 0);
    net_input(&nif, f, ping_frame(f, 6, true));
    KTEST_CHECK(cap.n == 1 && nif.stats.drop_checksum == 1);
}

static void test_icmp_is_rate_limited(void) {
    uint8_t f[ETH_FRAME_MAX];
    configured();
    learn_gateway();
    for (int i = 0; i < 100; i++) {
        net_input(&nif, f, ping_frame(f, (uint16_t)i, false));
    }
    KTEST_CHECK(cap.n == NET_ICMP_BURST && nif.stats.drop_rate_limited == 100 - NET_ICMP_BURST);
    net_tick(&nif, 1000);
    net_input(&nif, f, ping_frame(f, 1, false));
    KTEST_CHECK(cap.n == NET_ICMP_BURST + 1);
}

static void test_echo_reply_event(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint8_t *ic;
    configured();
    ic = ip_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 1, 8);
    memset(ic, 0, 8);
    fp16(ic + 6, 42);
    fp16(ic + 2, net_checksum(ic, 8, 0));
    net_input(&nif, f, 14 + 20 + 8);
    KTEST_CHECK(echo_replies == 1 && last_echo_seq == 42);
}

static void test_ip_hostile_headers(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint32_t len;
    configured();
    len = ping_frame(f, 1, false);
    fp16(f + 14 + 2, 1400);  /* total length beyond the frame */
    fp16(f + 14 + 10, 0), fp16(f + 14 + 10, net_checksum(f + 14, 20, 0));
    net_input(&nif, f, len);
    KTEST_CHECK(nif.stats.drop_malformed == 1);
    len = ping_frame(f, 1, false);
    f[14] = 0x4f;  /* IHL 60 bytes, longer than the packet */
    net_input(&nif, f, len);
    KTEST_CHECK(nif.stats.drop_malformed == 2);
    len = ping_frame(f, 1, false);
    fp16(f + 14 + 6, 0x2000);  /* more fragments */
    fp16(f + 14 + 10, 0), fp16(f + 14 + 10, net_checksum(f + 14, 20, 0));
    net_input(&nif, f, len);
    KTEST_CHECK(nif.stats.drop_fragment == 1);
    len = ping_frame(f, 1, false);
    f[14] = 0x65;  /* IPv6 version nibble */
    net_input(&nif, f, len);
    KTEST_CHECK(nif.stats.drop_malformed == 3 && cap.n == 0);
}

/* UDP handler that records what arrived. */
static struct udp_datagram got;
static int got_n;

static void record(void *self, struct netif *n, const struct udp_datagram *d) {
    (void)self;
    (void)n;
    got = *d;
    got_n++;
}

static void test_udp_demux(void) {
    uint8_t f[ETH_FRAME_MAX];
    struct udp_handler h = {.receive = record};
    uint32_t len;
    configured();
    got_n = 0;
    KTEST_CHECK(net_udp_bind(&nif, 7, &h) == KERR_OK);
    KTEST_CHECK(net_udp_bind(&nif, 7, &h) == KERR_EXIST);
    net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 5555, 7, "echo", 4));
    KTEST_CHECK(got_n == 1 && got.len == 4 && memcmp(got.data, "echo", 4) == 0 && got.sport == 5555);
    net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 5555, 9, "x", 1));
    KTEST_CHECK(nif.stats.drop_no_port == 1);
    len = udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 5555, 7, "echo", 4);
    f[len - 1] ^= 0xff;
    net_input(&nif, f, len);
    KTEST_CHECK(got_n == 1 && nif.stats.drop_checksum == 1);
    len = udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 5555, 7, "echo", 4);
    fp16(f + 38, 200);  /* UDP length beyond the IP payload */
    net_input(&nif, f, len);
    KTEST_CHECK(got_n == 1 && nif.stats.drop_malformed == 1);
    net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 99), 5555, 7, "echo", 4));
    KTEST_CHECK(got_n == 1 && nif.stats.drop_not_ours == 1);  /* another host's address */
}

static void test_martian_sources_dropped(void) {
    uint8_t f[ETH_FRAME_MAX];
    struct udp_handler h = {.receive = record};
    static const ip4_t martians[] = {IP4(10, 0, 2, 15), IP4(10, 0, 2, 255), IP4_BROADCAST, IP4(224, 0, 0, 5),
                                     IP4(240, 1, 2, 3), IP4(127, 0, 0, 1)};
    configured();
    got_n = 0;
    (void)net_udp_bind(&nif, 9, &h);
    for (uint32_t i = 0; i < sizeof martians / sizeof martians[0]; i++) {
        net_input(&nif, f, udp_frame(f, our_mac, martians[i], IP4(10, 0, 2, 15), 5555, 9, "x", 1));
    }
    KTEST_CHECK(got_n == 0 && nif.stats.drop_not_ours == 6);
}

static void test_no_smurf_amplifier(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint8_t *ic;
    configured();
    learn_gateway();
    ic = ip_frame(f, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", IP4(10, 0, 2, 2), IP4(10, 0, 2, 255), 1, 8);
    memset(ic, 0, 8);
    ic[0] = 8;
    fp16(ic + 2, net_checksum(ic, 8, 0));
    net_input(&nif, f, 14 + 20 + 8);
    KTEST_CHECK(cap.n == 0);
}

static void test_echo_service_resists_reflection(void) {
    uint8_t f[ETH_FRAME_MAX];
    struct udp_handler echo = net_echo_service();
    configured();
    learn_gateway();
    (void)net_udp_bind(&nif, 7, &echo);
    net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 5555, 7, "hello", 5));
    KTEST_CHECK(cap.n == 1 && fg16(cap.frame[0] + 36) == 5555 && memcmp(cap.frame[0] + 42, "hello", 5) == 0);
    net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 19, 7, "loop", 4));  /* chargen */
    net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 7, 7, "loop", 4));   /* echo */
    net_input(&nif, f, udp_frame(f, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", IP4(10, 0, 2, 2),
                                 IP4(10, 0, 2, 255), 5555, 7, "fraggle", 7));
    KTEST_CHECK(cap.n == 1);
    for (int i = 0; i < 100; i++) {
        net_input(&nif, f, udp_frame(f, our_mac, IP4(10, 0, 2, 2), IP4(10, 0, 2, 15), 5555, 7, "flood", 5));
    }
    KTEST_CHECK(cap.n == NET_ICMP_BURST);
}

static void test_udp_unbind_and_ephemeral(void) {
    struct udp_handler h = {.receive = record};
    uint16_t a, b;
    configured();
    a = net_udp_ephemeral(&nif);
    KTEST_CHECK(a >= NET_EPHEMERAL_LO && net_udp_bind(&nif, a, &h) == KERR_OK);
    b = net_udp_ephemeral(&nif);
    KTEST_CHECK(b >= NET_EPHEMERAL_LO && b != a);
    net_udp_unbind(&nif, a);
    KTEST_CHECK(net_udp_bind(&nif, a, &h) == KERR_OK);
    for (uint16_t p = 1; p < 64; p++) {
        (void)net_udp_bind(&nif, p, &h);
    }
    KTEST_CHECK(net_udp_bind(&nif, 999, &h) == KERR_NOMEM);
}

/* A handler that answers every datagram from its own port: exercises a
 * send from inside local delivery. */
static void reply_back(void *self, struct netif *n, const struct udp_datagram *d) {
    (void)self;
    got_n++;
    if (d->sport != 4000) {
        (void)net_udp_send(n, d->src, d->dport, 4000, "pong", 4);
    }
}

static void test_loopback_delivery(void) {
    struct udp_handler rec = {.receive = record}, rep = {.receive = reply_back};
    configured();
    got_n = 0;
    (void)net_udp_bind(&nif, 4000, &rec);
    (void)net_udp_bind(&nif, 5000, &rep);
    KTEST_CHECK(net_udp_send(&nif, IP4(127, 0, 0, 1), 4001, 4000, "hi", 2) == KERR_OK);
    /* A loopback destination gets a loopback source, so the checksum the
     * sender computed is the one the receiver verifies. */
    KTEST_CHECK(got_n == 1 && got.len == 2 && got.src == IP4(127, 0, 0, 1) && got.dst == IP4(127, 0, 0, 1));
    KTEST_CHECK(cap.n == 0);  /* nothing went to the wire */
    /* Our own interface address loops back too, with that address as the
     * source, so a reply can find its way home. */
    KTEST_CHECK(net_udp_send(&nif, IP4(10, 0, 2, 15), 4002, 5000, "ping", 4) == KERR_OK);
    KTEST_CHECK(got_n == 3 && memcmp(got.data, "pong", 4) == 0 && cap.n == 0);  /* reply delivered too */
    KTEST_CHECK(got.src == IP4(10, 0, 2, 15));
    for (int i = 0; i < 20; i++) {
        nif.input_depth = 1;  /* inside input: packets wait in the queue */
        (void)net_udp_send(&nif, IP4(127, 0, 0, 1), 4001, 4000, "x", 1);
    }
    nif.input_depth = 0;
    KTEST_CHECK(nif.stats.drop_loop_full == 20 - NET_LOOP_QUEUE);
}

/* Random bytes, random lengths, and mutations of valid frames: nothing may
 * crash or read past the frame (run under ASan/UBSan). */
static void test_fuzz_does_not_crash(void) {
    uint8_t f[ETH_FRAME_MAX + 16];
    uint64_t x = 0x9e3779b97f4a7c15ull;
    configured();
    for (int i = 0; i < 200000; i++) {
        uint32_t len;
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        len = (i % 3 == 0) ? dhcp_reply(f, 2, IP4(10, 0, 2, 15), xid, our_mac) : ping_frame(f, 1, false);
        f[(x >> 8) % len] ^= (uint8_t)(x >> 32);
        f[(x >> 16) % len] = (uint8_t)(x >> 40);
        if (i % 5 == 0) {
            nif.dhcp.state = DHCP_SELECTING;
            nif.dhcp.xid = xid;
        }
        net_input(&nif, f, (uint32_t)((x >> 24) % (len + 8)));
        cap.n = 0;
    }
    KTEST_CHECK(nif.stats.rx_frames == 200000);
}

int main(void) {
    KTEST_RUN(test_ip4_parse);
    KTEST_RUN(test_checksum_rfc1071);
    KTEST_RUN(test_dhcp_conversation);
    KTEST_RUN(test_dhcp_renews_at_half_the_lease);
    KTEST_RUN(test_dhcp_rebinds_at_seven_eighths);
    KTEST_RUN(test_dhcp_gives_the_address_up_when_the_lease_expires);
    KTEST_RUN(test_an_answered_renewal_extends_the_lease);
    KTEST_RUN(test_a_naked_renewal_drops_the_address);
    KTEST_RUN(test_dhcp_retransmits_with_backoff);
    KTEST_RUN(test_dhcp_rejects_foreign_replies);
    KTEST_RUN(test_dhcp_hostile_options);
    KTEST_RUN(test_dhcp_nak_restarts);
    KTEST_RUN(test_arp_answers_and_resists_poisoning);
    KTEST_RUN(test_udp_waits_for_arp_then_sends);
    KTEST_RUN(test_icmp_echo);
    KTEST_RUN(test_icmp_is_rate_limited);
    KTEST_RUN(test_echo_reply_event);
    KTEST_RUN(test_ip_hostile_headers);
    KTEST_RUN(test_udp_demux);
    KTEST_RUN(test_martian_sources_dropped);
    KTEST_RUN(test_no_smurf_amplifier);
    KTEST_RUN(test_echo_service_resists_reflection);
    KTEST_RUN(test_udp_unbind_and_ephemeral);
    KTEST_RUN(test_loopback_delivery);
    KTEST_RUN(test_fuzz_does_not_crash);
    return ktest_failures();
}
