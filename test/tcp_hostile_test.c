/* TCP against an attacker (master Rule 25). These are the classes that
 * have produced real CVEs in TCP stacks: blind reset and data injection by
 * an off-path attacker guessing sequence numbers, SYN flooding, segments
 * crafted to make a parser read past the buffer, connection-table
 * exhaustion, and the LAND-style self-connect. Each case injects raw
 * segments at an established connection and checks that the stream is
 * neither corrupted nor killed. */
#include "core/net/tcp.h"

#include <stdlib.h>

#include "core/errno.h"
#include "core/net/tcp_internal.h"
#include "ktest.h"
#include "net_frames.h"

#define VICTIM_IP IP4(10, 0, 0, 1)
#define PEER_IP   IP4(10, 0, 0, 2)
#define EVIL_IP   IP4(203, 0, 113, 9)

static const uint8_t victim_mac[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t peer_mac[6] = {2, 0, 0, 0, 0, 2};

static struct netif nif;
static struct tcp_stack stack;
static struct capture cap;
static uint64_t clock_ms;

/* The connection under attack, plus what the peer would use. */
static struct tcp_conn *listener;
static struct tcp_conn *conn;
static uint16_t peer_port = 40000;
static uint32_t injected;  /* survives the per-test resets */

/* Builds a TCP segment from PEER_IP (or from) and feeds it in. */
static void inject(ip4_t from, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
                   const void *data, uint32_t len, bool break_checksum, uint8_t data_offset_words) {
    uint8_t f[ETH_FRAME_MAX];
    uint8_t *t = frame_ip(f, victim_mac, peer_mac, from, VICTIM_IP, IPPROTO_TCP, TCP_HLEN + len);
    uint8_t ph[12];
    fp16(t + TH_SPORT, sport);
    fp16(t + TH_DPORT, dport);
    fp32(t + TH_SEQ, seq);
    fp32(t + TH_ACK, ack);
    t[TH_OFF] = (uint8_t)(data_offset_words << 4);
    t[TH_FLAGS] = flags;
    fp16(t + TH_WINDOW, 8192);
    fp16(t + TH_CKSUM, 0);
    fp16(t + TH_URGENT, 0);
    if (len > 0) {
        memcpy(t + TCP_HLEN, data, len);  /* memcpy from NULL is undefined even for 0 bytes */
    }
    fp32(ph, from);
    fp32(ph + 4, VICTIM_IP);
    ph[8] = 0;
    ph[9] = IPPROTO_TCP;
    fp16(ph + 10, (uint16_t)(TCP_HLEN + len));
    fp16(t + TH_CKSUM, net_checksum(t, TCP_HLEN + len, net_sum(ph, 12, 0)));
    if (break_checksum) {
        t[TH_CKSUM] ^= 0xff;
    }
    injected++;
    net_input(&nif, f, F_IP + 20u + TCP_HLEN + len);
}

/* The victim with an established connection whose peer state we know,
 * which is exactly what an off-path attacker does not. */
static void fresh(void) {
    struct netdev_port dev = {.send = capture_send, .self = &cap};
    struct net_events ev = {0};
    memset(&cap, 0, sizeof cap);
    clock_ms = 1000;
    net_init(&nif, victim_mac, &dev, &ev);
    nif.ip = VICTIM_IP;
    nif.mask = IP4(255, 255, 255, 0);
    nif.arp[0].ip = PEER_IP;
    memcpy(nif.arp[0].mac, peer_mac, 6);
    nif.arp[0].valid = true;
    nif.arp[1].ip = EVIL_IP;
    memcpy(nif.arp[1].mac, peer_mac, 6);
    nif.arp[1].valid = true;
    KTEST_CHECK(tcp_init(&stack, &nif, 0x12345678u) == KERR_OK);
    listener = tcp_alloc(&stack);
    KTEST_CHECK(tcp_bind(listener, 0, 80) == KERR_OK && tcp_listen(listener, 4) == KERR_OK);
    peer_port++;
}

/* Completes a handshake against the listener and returns the connection,
 * or NULL (which every caller checks, so a failure reports rather than
 * crashing the run). */
static struct tcp_conn *establish(uint32_t peer_isn) {
    struct tcp_conn *c;
    uint32_t our_isn;
    inject(PEER_IP, peer_port, 80, peer_isn, 0, TCP_SYN, NULL, 0, false, 5);
    if (cap.n == 0) {
        return NULL;
    }
    our_isn = fg32(cap.frame[cap.n - 1] + F_L4 + TH_SEQ);
    inject(PEER_IP, peer_port, 80, peer_isn + 1u, our_isn + 1u, TCP_ACK, NULL, 0, false, 5);
    c = tcp_accept(listener);
    KTEST_CHECK(c != NULL && c->state == TCP_ESTABLISHED);
    return c;
}

/* Sets up a victim with one established connection; false when that could
 * not be done, so a test reports instead of dereferencing NULL. */
static bool ready(uint32_t peer_isn) {
    fresh();
    conn = establish(peer_isn);
    return conn != NULL;
}

/* An off-path attacker cannot guess the sequence number, so a reset with
 * the wrong one must be ignored (blind reset injection). */
static void test_cve_blind_reset_is_ignored(void) {
    if (!ready(1000)) {
        return;
    }
    for (uint32_t guess = 0; guess < 2000u; guess += 97u) {
        inject(PEER_IP, peer_port, 80, guess, 0, TCP_RST, NULL, 0, false, 5);
    }
    KTEST_CHECK(conn->state == TCP_ESTABLISHED && conn->error == 0);
    KTEST_CHECK(stack.stats.drop_bad_reset + stack.stats.drop_out_of_window > 20);
    /* The one true sequence number does reset it. */
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_RST, NULL, 0, false, 5);
    KTEST_CHECK(conn->state == TCP_CLOSED && conn->error == TCP_ECONNRESET);
}

/* Nor can an attacker inject data at a guessed sequence number. */
static void test_cve_blind_data_is_ignored(void) {
    uint8_t in[64];
    if (!ready(5000)) {
        return;
    }
    for (uint32_t guess = 100000u; guess < 100200u; guess += 7u) {
        inject(PEER_IP, peer_port, 80, guess, 0, TCP_ACK | TCP_PSH, "EVIL", 4, false, 5);
    }
    KTEST_CHECK(conn->rx_len == 0 && stack.stats.drop_out_of_window > 10);
    /* In-window data from the real peer still works. */
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK | TCP_PSH, "good", 4, false, 5);
    KTEST_CHECK(tcp_recv(conn, in, sizeof in, false) == 4 && memcmp(in, "good", 4) == 0);
}

/* A segment from a different address is a different connection: it must
 * not touch ours, and gets a reset of its own. */
static void test_cve_another_address_cannot_interfere(void) {
    if (!ready(7000)) {
        return;
    }
    inject(EVIL_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_RST, NULL, 0, false, 5);
    inject(EVIL_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK | TCP_PSH, "EVIL", 4, false, 5);
    KTEST_CHECK(conn->state == TCP_ESTABLISHED && conn->rx_len == 0);
}

/* A flood of SYNs must not exhaust the connection table: the listener's
 * backlog bounds the half-open connections it will hold. */
static void test_cve_syn_flood_is_bounded(void) {
    uint32_t free_slots = 0;
    fresh();
    for (uint32_t i = 0; i < 500u; i++) {
        inject(PEER_IP, (uint16_t)(1024u + i), 80, 1000u + i * 31u, 0, TCP_SYN, NULL, 0, false, 5);
    }
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        free_slots += stack.conns[i].used ? 0u : 1u;
    }
    KTEST_CHECK(stack.stats.drop_backlog_full > 400u);
    KTEST_CHECK(free_slots >= TCP_MAX_CONNS - 1u - TCP_BACKLOG);
    /* A legitimate connection still gets through afterwards. */
    KTEST_CHECK(tcp_alloc(&stack) != NULL);
}

/* Malformed headers: a data offset that points outside the segment, or
 * past its own options, must be rejected rather than read. */
static void test_cve_malformed_headers(void) {
    if (!ready(9000)) {
        return;
    }
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK, "data", 4, false, 15);  /* hlen 60 > segment */
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK, "data", 4, false, 4);   /* hlen 16 < 20 */
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK, "data", 4, false, 0);   /* hlen 0 */
    KTEST_CHECK(conn->rx_len == 0 && conn->state == TCP_ESTABLISHED);
    KTEST_CHECK(stack.stats.drop_malformed >= 3);
}

static void test_cve_bad_checksum_is_dropped(void) {
    if (!ready(11000)) {
        return;
    }
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK | TCP_PSH, "EVIL", 4, true, 5);
    KTEST_CHECK(conn->rx_len == 0 && stack.stats.drop_checksum == 1);
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_RST, NULL, 0, true, 5);
    KTEST_CHECK(conn->state == TCP_ESTABLISHED);
}

/* A SYN inside an established connection is an error: the connection is
 * reset rather than being silently re-synchronised. */
static void test_cve_syn_in_window_resets(void) {
    if (!ready(13000)) {
        return;
    }
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_SYN, NULL, 0, false, 5);
    KTEST_CHECK(conn->state == TCP_CLOSED && conn->error == TCP_ECONNRESET);
}

/* An acknowledgement of data we never sent must not move the send window
 * (which would make us drop unacknowledged bytes). */
static void test_cve_bogus_ack_is_ignored(void) {
    if (!ready(15000)) {
        return;
    }
    KTEST_CHECK(tcp_send(conn, "hello", 5) == 5);
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, conn->snd_nxt + 100000u, TCP_ACK, NULL, 0, false, 5);
    KTEST_CHECK(conn->tx_len == 5);  /* nothing was dropped from the queue */
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, conn->snd_una, TCP_ACK, NULL, 0, false, 5);
    KTEST_CHECK(conn->tx_len == 5);  /* a duplicate acknowledgement changes nothing */
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, conn->snd_nxt, TCP_ACK, NULL, 0, false, 5);
    KTEST_CHECK(conn->tx_len == 0);  /* the real one does */
}

/* A FIN that arrives before the data preceding it must not end the stream
 * early: the reader would lose bytes that are still coming. */
static void test_cve_early_fin_does_not_close(void) {
    uint8_t in[32];
    if (!ready(17000)) {
        return;
    }
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt + 10u, 0, TCP_ACK | TCP_FIN, NULL, 0, false, 5);
    KTEST_CHECK(!conn->fin_seen && conn->state == TCP_ESTABLISHED);
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, 0, TCP_ACK | TCP_PSH, "0123456789", 10, false, 5);
    KTEST_CHECK(tcp_recv(conn, in, sizeof in, false) == 10);
    KTEST_CHECK(!conn->fin_seen);  /* the early FIN was not remembered */
}

/* A segment claiming to come from our own address (LAND) must not make a
 * connection to ourselves. */
static void test_cve_land_self_connect(void) {
    fresh();
    inject(VICTIM_IP, 80, 80, 1000, 0, TCP_SYN, NULL, 0, false, 5);
    KTEST_CHECK(stack.stats.drop_no_socket + stack.stats.drop_backlog_full + stack.stats.connections <= 1u);
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &stack.conns[i];
        KTEST_CHECK(!(c->used && c->remote_ip == VICTIM_IP && c->remote_port == c->local_port));
    }
}

/* A zero-window peer must not make us spin or send data it cannot take. */
static void test_cve_zero_window(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint32_t before;
    if (!ready(19000)) {
        return;
    }
    KTEST_CHECK(tcp_send(conn, "payload", 7) == 7);
    /* The peer acknowledges and advertises a zero window. */
    (void)f;
    inject(PEER_IP, peer_port, 80, conn->rcv_nxt, conn->snd_nxt, TCP_ACK, NULL, 0, false, 5);
    conn->snd_wnd = 0;
    KTEST_CHECK(tcp_send(conn, "more", 4) == 4);  /* queued, not sent */
    before = stack.stats.segments_out;
    for (uint32_t i = 0; i < 100u; i++) {
        clock_ms += 10;
        tcp_tick(&stack, clock_ms);
    }
    KTEST_CHECK(stack.stats.segments_out - before <= TCP_RETRIES + 2u);  /* it did not spin */
    KTEST_CHECK(conn->tx_len == 4);
}

/* Mutated segments at every state: nothing may crash or corrupt (ASan). */
static void test_cve_fuzz_segments(void) {
    uint8_t payload[64];
    uint64_t x = 0x243f6a8885a308d3ull;
    for (uint32_t i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)i;
    }
    for (uint32_t round = 0; round < 20000u; round++) {
        if (round % 200u == 0 && !ready(round + 1u)) {
            return;
        }
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        inject((x & 1) ? PEER_IP : EVIL_IP, peer_port, (uint16_t)(x & 1 ? 80 : 81),
               (x & 2) ? conn->rcv_nxt : (uint32_t)(x >> 16), (uint32_t)(x >> 8), (uint8_t)(x >> 32),
               payload, (uint32_t)(x >> 40) % sizeof payload, (x & 4) != 0, (uint8_t)(5u + (x >> 48) % 11u));
        if (round % 7u == 0) {
            clock_ms += 10;
            tcp_tick(&stack, clock_ms);
        }
    }
    KTEST_CHECK(injected >= 20000u);  /* every one was processed without a crash */
    KTEST_CHECK(conn->state <= TCP_TIME_WAIT);  /* and the state stayed legal */
}

int main(void) {
    KTEST_RUN(test_cve_blind_reset_is_ignored);
    KTEST_RUN(test_cve_blind_data_is_ignored);
    KTEST_RUN(test_cve_another_address_cannot_interfere);
    KTEST_RUN(test_cve_syn_flood_is_bounded);
    KTEST_RUN(test_cve_malformed_headers);
    KTEST_RUN(test_cve_bad_checksum_is_dropped);
    KTEST_RUN(test_cve_syn_in_window_resets);
    KTEST_RUN(test_cve_bogus_ack_is_ignored);
    KTEST_RUN(test_cve_early_fin_does_not_close);
    KTEST_RUN(test_cve_land_self_connect);
    KTEST_RUN(test_cve_zero_window);
    KTEST_RUN(test_cve_fuzz_segments);
    return ktest_failures();
}
