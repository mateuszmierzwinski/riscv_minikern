/* TCP against a peer and against hostile segments. Two stacks are wired to
 * each other through a programmable wire (which can lose, duplicate and
 * reorder), so handshakes, data transfer, retransmission and close run for
 * real rather than against a script. */
#include "core/net/tcp.h"

#include <stdio.h>
#include <stdlib.h>

#include "core/errno.h"
#include "core/net/tcp_internal.h"
#include "ktest.h"
#include "net_frames.h"

#define A_IP IP4(10, 0, 0, 1)
#define B_IP IP4(10, 0, 0, 2)
#define WIRE_MAX 512

static const uint8_t a_mac[6] = {2, 0, 0, 0, 0, 0xa};
static const uint8_t b_mac[6] = {2, 0, 0, 0, 0, 0xb};

/* One side of the wire. */
struct host {
    struct netif nif;
    struct tcp_stack tcp;
    const uint8_t *mac;
};

/* A frame in flight between the two hosts. */
struct inflight {
    uint8_t frame[ETH_FRAME_MAX];
    uint32_t len;
    struct host *to;
};

/* The wire: it carries frames and can be told to misbehave. */
static struct {
    struct inflight q[WIRE_MAX];
    uint32_t n;
    uint32_t drop_every;   /* lose every n-th frame */
    /* Loses exactly one frame, the n-th data-carrying one. A single early
     * loss is what leaves a receiver holding a run above a gap, which is the
     * only state SACK has anything to say about — `drop_every` keeps losing
     * and the hole never stays still long enough to be reported. */
    uint32_t drop_once_at;
    uint32_t dup_every;    /* deliver every n-th frame twice */
    bool reorder;          /* deliver each pair back to front */
    uint32_t carried;
    uint32_t dropped;
} wire;

static struct host host_a, host_b;
static uint64_t clock_ms;

/* Queues a frame for the other host. */
static int wire_send(void *self, const uint8_t *frame, uint32_t len) {
    struct host *from = self;
    struct host *to = from == &host_a ? &host_b : &host_a;
    wire.carried++;
    if (wire.drop_every != 0 && wire.carried % wire.drop_every == 0) {
        wire.dropped++;
        return KERR_OK;
    }
    if (wire.drop_once_at != 0 && len > 60u) {
        /* Longer than a bare header, so the one lost frame carries data
         * rather than being the handshake's own acknowledgement. */
        wire.drop_once_at--;
        if (wire.drop_once_at == 0) {
            wire.dropped++;
            return KERR_OK;
        }
    }
    if (wire.n >= WIRE_MAX) {
        return KERR_AGAIN;
    }
    memcpy(wire.q[wire.n].frame, frame, len);
    wire.q[wire.n].len = len;
    wire.q[wire.n].to = to;
    wire.n++;
    if (wire.dup_every != 0 && wire.carried % wire.dup_every == 0 && wire.n < WIRE_MAX) {
        wire.q[wire.n] = wire.q[wire.n - 1];
        wire.n++;
    }
    return KERR_OK;
}

/* Delivers everything queued, in the wire's chosen order. Static, because
 * a queue of full frames is far too large for the stack. */
static struct inflight batch[WIRE_MAX];

static void wire_pump(void) {
    for (uint32_t round = 0; round < 64 && wire.n > 0; round++) {
        uint32_t n = wire.n;
        memcpy(batch, wire.q, n * sizeof batch[0]);
        wire.n = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = wire.reorder ? (i ^ 1u) : i;
            k = k < n ? k : i;
            net_input(&batch[k].to->nif, batch[k].frame, batch[k].len);
        }
    }
}

/* Advances time on both stacks and moves the wire along. */
static void run_ms(uint64_t ms) {
    for (uint64_t t = 0; t < ms; t += 10) {
        clock_ms += 10;
        wire_pump();
        tcp_tick(&host_a.tcp, clock_ms);
        tcp_tick(&host_b.tcp, clock_ms);
        wire_pump();
    }
}

static void host_init(struct host *h, ip4_t ip, const uint8_t *mac, const uint8_t *peer_mac, ip4_t peer_ip) {
    struct netdev_port dev = {.send = wire_send, .self = h};
    struct net_events ev = {0};
    net_init(&h->nif, mac, &dev, &ev);
    h->nif.ip = ip;
    h->nif.mask = IP4(255, 255, 255, 0);
    h->mac = mac;
    /* Prime the ARP cache, so the test exercises TCP and not ARP. */
    h->nif.arp[0].ip = peer_ip;
    memcpy(h->nif.arp[0].mac, peer_mac, 6);
    h->nif.arp[0].valid = true;
    KTEST_CHECK(tcp_init(&h->tcp, &h->nif, ip * 7919u) == KERR_OK);
}

static void fresh(void) {
    memset(&wire, 0, sizeof wire);
    memset(&host_a, 0, sizeof host_a);
    memset(&host_b, 0, sizeof host_b);
    clock_ms = 1000;
    host_init(&host_a, A_IP, a_mac, b_mac, B_IP);
    host_init(&host_b, B_IP, b_mac, a_mac, A_IP);
}

/* A listener on B, and a connection from A to it. */
static void connect_pair(struct tcp_conn **client, struct tcp_conn **server, struct tcp_conn **listener) {
    struct tcp_conn *l = tcp_alloc(&host_b.tcp);
    struct tcp_conn *c = tcp_alloc(&host_a.tcp);
    KTEST_CHECK(l != NULL && c != NULL);
    KTEST_CHECK(tcp_bind(l, 0, 80) == KERR_OK);
    KTEST_CHECK(tcp_listen(l, 4) == KERR_OK);
    KTEST_CHECK(tcp_connect(c, B_IP, 80) == KERR_OK);
    run_ms(200);
    *server = tcp_accept(l);
    *client = c;
    *listener = l;
}

static void test_handshake(void) {
    struct tcp_conn *c, *s, *l;
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(c->state == TCP_ESTABLISHED);
    KTEST_CHECK(s != NULL && s->state == TCP_ESTABLISHED);
    KTEST_CHECK(s->remote_ip == A_IP && s->remote_port == c->local_port);
    KTEST_CHECK(c->remote_port == 80 && c->local_port >= NET_EPHEMERAL_LO);
    KTEST_CHECK(tcp_connect_done(c));
}

static void test_refused_when_nobody_listens(void) {
    struct tcp_conn *c;
    fresh();
    c = tcp_alloc(&host_a.tcp);
    KTEST_CHECK(tcp_connect(c, B_IP, 9999) == KERR_OK);
    run_ms(200);
    KTEST_CHECK(c->state == TCP_CLOSED && c->error == TCP_ECONNREFUSED);
    KTEST_CHECK(host_b.tcp.stats.resets_sent >= 1);
}

static void test_connect_times_out_when_silent(void) {
    struct tcp_conn *c;
    fresh();
    wire.drop_every = 1;  /* nothing gets through */
    c = tcp_alloc(&host_a.tcp);
    KTEST_CHECK(tcp_connect(c, B_IP, 80) == KERR_OK);
    run_ms(60000);
    KTEST_CHECK(c->state == TCP_CLOSED && c->error == TCP_ETIMEDOUT);
    KTEST_CHECK(host_a.tcp.stats.retransmits >= TCP_RETRIES);
}

/* Data both ways, and the bytes arrive in order and whole. */
static void test_data_both_ways(void) {
    struct tcp_conn *c, *s, *l;
    uint8_t out[3000], in[3000];
    int32_t got;
    fresh();
    connect_pair(&c, &s, &l);
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i * 131u + 7u);
    }
    KTEST_CHECK(tcp_send(c, out, sizeof out) == (int32_t)sizeof out);
    run_ms(500);
    got = tcp_recv(s, in, sizeof in, false);
    KTEST_CHECK(got == (int32_t)sizeof out && memcmp(in, out, sizeof out) == 0);
    KTEST_CHECK(tcp_send(s, "pong", 4) == 4);
    run_ms(200);
    KTEST_CHECK(tcp_recv(c, in, sizeof in, false) == 4 && memcmp(in, "pong", 4) == 0);
}

/* A transfer larger than the buffers, fed in as the queue drains. */
static void test_stream_larger_than_the_buffer(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[40000], in[40000];
    uint32_t sent = 0, received = 0;
    fresh();
    connect_pair(&c, &s, &l);
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i * 29u + 3u);
    }
    for (uint32_t round = 0; round < 400 && received < sizeof out; round++) {
        if (sent < sizeof out) {
            int32_t n = tcp_send(c, out + sent, sizeof out - sent);
            sent += n > 0 ? (uint32_t)n : 0u;
        }
        run_ms(50);
        for (int32_t n = tcp_recv(s, in + received, sizeof in - received, false); n > 0;
             n = tcp_recv(s, in + received, sizeof in - received, false)) {
            received += (uint32_t)n;
        }
    }
    KTEST_CHECK(received == sizeof out && memcmp(in, out, sizeof out) == 0);
}

/* The same, over a wire that loses every seventh frame. */
static void test_stream_survives_loss(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[20000], in[20000];
    uint32_t sent = 0, received = 0;
    fresh();
    connect_pair(&c, &s, &l);
    wire.drop_every = 7;
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i ^ 0x5au);
    }
    for (uint32_t round = 0; round < 2000 && received < sizeof out; round++) {
        if (sent < sizeof out) {
            int32_t n = tcp_send(c, out + sent, sizeof out - sent);
            sent += n > 0 ? (uint32_t)n : 0u;
        }
        run_ms(50);
        for (int32_t n = tcp_recv(s, in + received, sizeof in - received, false); n > 0;
             n = tcp_recv(s, in + received, sizeof in - received, false)) {
            received += (uint32_t)n;
        }
    }
    KTEST_CHECK(received == sizeof out && memcmp(in, out, sizeof out) == 0);
    KTEST_CHECK(host_a.tcp.stats.retransmits > 0 && wire.dropped > 0);
}

/* ── selective acknowledgement ─────────────────────────────────────────── */

/* Both ends must come out of the handshake agreeing that SACK is available.
 * This is the gate on everything else: blocks are only ever sent to a peer
 * that offered the option, so if this is false the rest is silently inert
 * and every other SACK test would pass by doing nothing. */
static void test_sack_is_negotiated_both_ways(void) {
    struct tcp_conn *c, *s, *l;
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(c->sack_ok);
    KTEST_CHECK(s != NULL && s->sack_ok);
}

/* A receiver holding data above a gap must report it as blocks.
 *
 * Driven by dropping one segment and then sending more: the receiver holds
 * what follows the hole, and its acknowledgements must carry blocks
 * describing it. The assertion is on the sender's recorded blocks rather
 * than on bytes on the wire, because that is the thing the retransmission
 * path actually consults — a stack that emitted perfect options and never
 * parsed them back would pass a wire-level check and change nothing. */
static void test_a_gap_is_reported_as_blocks(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[6000], in[6000];
    uint32_t sent = 0, received = 0;
    fresh();
    connect_pair(&c, &s, &l);
    /* One drop, early, so that everything after it is held out of order. */
    wire.drop_once_at = 1;
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)i;
    }
    for (uint32_t round = 0; round < 400 && received < sizeof out; round++) {
        if (sent < sizeof out) {
            int32_t n = tcp_send(c, out + sent, sizeof out - sent);
            sent += n > 0 ? (uint32_t)n : 0u;
        }
        run_ms(10);
        for (int32_t n = tcp_recv(s, in + received, sizeof in - received, false); n > 0;
             n = tcp_recv(s, in + received, sizeof in - received, false)) {
            received += (uint32_t)n;
        }
    }
    /* **The counters, not the live field.** `c->sacked_blocks` holds what
     * the *last* acknowledgement reported, and a SACK option is a complete
     * statement — so the moment the hole is filled it is correctly empty
     * again. Sampling it between rounds therefore catches it only by luck,
     * which is this project's "a detector read at the wrong moment" lesson
     * for the fourth time. The counters are cumulative and say the same
     * thing without the race.
     *
     * Both directions, named separately, because a stack can emit perfect
     * options and silently never parse them back — and that failure looks
     * right on the wire. */
    KTEST_CHECK(host_b.tcp.stats.sack_blocks_sent > 0);
    KTEST_CHECK(host_a.tcp.stats.sack_blocks_received > 0);
    /* And the stream is still correct, which is the point: SACK is an
     * improvement on recovery, never a change to what arrives. */
    KTEST_CHECK(received == sizeof out && memcmp(in, out, sizeof out) == 0);
}

/* A block the sender never sent must be discarded.
 *
 * A peer can claim anything, and a block above `snd_nxt` accepted literally
 * would shorten a retransmission — potentially to nothing, which is a stall
 * a remote host could cause on purpose. Checked by hand rather than over the
 * wire, because no cooperating peer produces it. */
static void test_a_block_beyond_what_was_sent_is_ignored(void) {
    struct tcp_conn *c, *s, *l;
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(tcp_send(c, (const uint8_t *)"hello", 5) == 5);
    run_ms(100);
    {
        /* Three blocks: one entirely in the future, one inverted, and one
         * already covered by the cumulative acknowledgement. None may
         * survive. */
        struct tcp_seg seg = {.flags = TCP_ACK, .ack = c->snd_una, .wnd = 4096u, .sack_blocks = 3u};
        seg.sack_left[0] = c->snd_nxt + 1000u;
        seg.sack_right[0] = c->snd_nxt + 2000u;
        seg.sack_left[1] = c->snd_nxt;
        seg.sack_right[1] = c->snd_una; /* right at or below left: inverted */
        seg.sack_left[2] = c->snd_una - 100u;
        seg.sack_right[2] = c->snd_una;
        tcp_take_sack_blocks(c, &seg);
        KTEST_CHECK(c->sacked_blocks == 0);
    }
}

/* Duplicates and reordering must not corrupt the stream. */
static void test_duplicates_and_reordering(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[8000], in[8000];
    uint32_t sent = 0, received = 0;
    fresh();
    connect_pair(&c, &s, &l);
    wire.dup_every = 3;
    wire.reorder = true;
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i * 7u);
    }
    for (uint32_t round = 0; round < 1000 && received < sizeof out; round++) {
        if (sent < sizeof out) {
            int32_t n = tcp_send(c, out + sent, sizeof out - sent);
            sent += n > 0 ? (uint32_t)n : 0u;
        }
        run_ms(50);
        for (int32_t n = tcp_recv(s, in + received, sizeof in - received, false); n > 0;
             n = tcp_recv(s, in + received, sizeof in - received, false)) {
            received += (uint32_t)n;
        }
    }
    KTEST_CHECK(received == sizeof out && memcmp(in, out, sizeof out) == 0);
}

/* A segment that arrives before its turn is kept, not thrown away.
 *
 * The stack's own counters are the evidence: with reordering on, segments
 * must be **held** rather than dropped, and the stream must still arrive
 * whole. Before the reassembly queue existed every one of these was a drop
 * and a resend, which is correct and costs a round trip per loss. */
static void test_early_segments_are_held(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[8000], in[8000];
    uint32_t sent = 0, received = 0;
    fresh();
    connect_pair(&c, &s, &l);
    wire.reorder = true;
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)(i * 11u + 3u);
    }
    for (uint32_t round = 0; round < 1000 && received < sizeof out; round++) {
        if (sent < sizeof out) {
            int32_t n = tcp_send(c, out + sent, sizeof out - sent);
            sent += n > 0 ? (uint32_t)n : 0u;
        }
        run_ms(50);
        for (int32_t n = tcp_recv(s, in + received, sizeof in - received, false); n > 0;
             n = tcp_recv(s, in + received, sizeof in - received, false)) {
            received += (uint32_t)n;
        }
    }
    KTEST_CHECK(received == sizeof out && memcmp(in, out, sizeof out) == 0);
    KTEST_CHECK(host_b.tcp.stats.held_out_of_order > 0);
}

/* Congestion control: the window starts at ten segments, grows while
 * acknowledgements come back, and is **halved** by a loss rather than
 * carrying on regardless. Sending what the peer's window allows and
 * nothing else — which is what this replaces — is right on a cable with
 * nothing on it and wrong on anything shared. */
static void test_the_window_grows_and_a_loss_halves_it(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[16000];
    uint32_t sent = 0, received = 0, grown;
    static uint8_t in[16000];
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(c->cwnd == 10u * TCP_MSS); /* RFC 6928's initial window */
    for (uint32_t round = 0; round < 200 && sent < sizeof out; round++) {
        int32_t n = tcp_send(c, out + sent, sizeof out - sent);
        sent += n > 0 ? (uint32_t)n : 0u;
        run_ms(50);
        for (int32_t got = tcp_recv(s, in + received, sizeof in - received, false); got > 0;
             got = tcp_recv(s, in + received, sizeof in - received, false)) {
            received += (uint32_t)got;
        }
    }
    grown = c->cwnd;
    KTEST_CHECK(grown > 10u * TCP_MSS); /* it opened as data got through */
    /* Now lose everything until the timer fires: the window must collapse
     * to one segment and the threshold to half of what was outstanding. */
    wire.drop_every = 1; /* nothing gets through from here on */
    (void)tcp_send(c, out, 4000);
    run_ms(5000);
    KTEST_CHECK(c->cwnd == TCP_MSS);
    KTEST_CHECK(c->ssthresh < grown);
    KTEST_CHECK(c->ssthresh >= 2u * TCP_MSS); /* and never below two */
}

static void test_peek_leaves_the_data(void) {
    struct tcp_conn *c, *s, *l;
    uint8_t in[16];
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(tcp_send(c, "abcdef", 6) == 6);
    run_ms(200);
    KTEST_CHECK(tcp_recv(s, in, 3, true) == 3 && memcmp(in, "abc", 3) == 0);
    KTEST_CHECK(tcp_recv(s, in, sizeof in, false) == 6 && memcmp(in, "abcdef", 6) == 0);
    KTEST_CHECK(tcp_recv(s, in, sizeof in, false) == KERR_AGAIN);
}

/* An orderly close from each side, and the end of stream that follows. */
static void test_close_sequence(void) {
    struct tcp_conn *c, *s, *l;
    uint8_t in[8];
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(tcp_send(c, "bye", 3) == 3);
    KTEST_CHECK(tcp_close(c) == KERR_OK);
    run_ms(300);
    KTEST_CHECK(s->state == TCP_CLOSE_WAIT && s->fin_seen);
    KTEST_CHECK(tcp_recv(s, in, sizeof in, false) == 3);   /* the data before the FIN */
    KTEST_CHECK(tcp_recv(s, in, sizeof in, false) == 0);   /* then end of stream */
    KTEST_CHECK(c->state == TCP_FIN_WAIT_2);
    KTEST_CHECK(tcp_close(s) == KERR_OK);
    run_ms(300);
    KTEST_CHECK(s->state == TCP_CLOSED);
    KTEST_CHECK(c->state == TCP_TIME_WAIT);
    run_ms(TCP_TIME_WAIT_MS + 100u);
    KTEST_CHECK(c->state == TCP_CLOSED);
}

static void test_write_after_close_is_refused(void) {
    struct tcp_conn *c, *s, *l;
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(tcp_close(c) == KERR_OK);
    KTEST_CHECK(tcp_send(c, "x", 1) == TCP_EPIPE);
    run_ms(200);
    KTEST_CHECK(tcp_send(s, "y", 1) == 1);  /* the other direction still works */
    run_ms(200);
}

/* Freeing an established connection resets the peer rather than leaving
 * it waiting for a FIN that will never come. */
static void test_free_resets_the_peer(void) {
    struct tcp_conn *c, *s, *l;
    fresh();
    connect_pair(&c, &s, &l);
    tcp_free(c);
    run_ms(200);
    KTEST_CHECK(s->state == TCP_CLOSED && s->error == TCP_ECONNRESET);
}

/* The window closes when the reader does not read, and opens again when
 * it does: flow control, not a dropped stream. */
static void test_flow_control(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[TCP_RX_BUF * 2], in[TCP_RX_BUF];
    uint32_t sent = 0, received = 0;
    fresh();
    connect_pair(&c, &s, &l);
    for (uint32_t i = 0; i < sizeof out; i++) {
        out[i] = (uint8_t)i;
    }
    for (uint32_t round = 0; round < 50; round++) {
        int32_t n = tcp_send(c, out + sent, sizeof out - sent);
        sent += n > 0 ? (uint32_t)n : 0u;
        run_ms(50);
    }
    KTEST_CHECK(s->rx_len == TCP_RX_BUF);           /* the reader's queue is full */
    KTEST_CHECK(c->snd_wnd == 0 || tcp_unsent(c) > 0);  /* so the sender stopped */
    for (uint32_t round = 0; round < 200 && received < sizeof out; round++) {
        int32_t n = tcp_recv(s, in, sizeof in, false);
        received += n > 0 ? (uint32_t)n : 0u;
        n = tcp_send(c, out + sent, sizeof out - sent);
        sent += n > 0 ? (uint32_t)n : 0u;
        run_ms(50);
    }
    KTEST_CHECK(received == sizeof out);
}

static void test_listener_backlog(void) {
    struct tcp_conn *l, *clients[6];
    uint32_t accepted = 0;
    fresh();
    l = tcp_alloc(&host_b.tcp);
    KTEST_CHECK(tcp_bind(l, 0, 80) == KERR_OK && tcp_listen(l, 2) == KERR_OK);
    for (uint32_t i = 0; i < 6; i++) {
        clients[i] = tcp_alloc(&host_a.tcp);
        if (clients[i] != NULL) {
            (void)tcp_connect(clients[i], B_IP, 80);
        }
        run_ms(100);
    }
    while (tcp_accept(l) != NULL) {
        accepted++;
    }
    KTEST_CHECK(accepted <= 2 && accepted >= 1);
    KTEST_CHECK(host_b.tcp.stats.drop_backlog_full > 0);
}

static void test_bind_conflicts(void) {
    struct tcp_conn *a, *b;
    fresh();
    a = tcp_alloc(&host_b.tcp);
    b = tcp_alloc(&host_b.tcp);
    KTEST_CHECK(tcp_bind(a, 0, 80) == KERR_OK && tcp_listen(a, 1) == KERR_OK);
    KTEST_CHECK(tcp_bind(b, 0, 80) == KERR_EXIST);
    KTEST_CHECK(tcp_bind(b, 0, 81) == KERR_OK);
    KTEST_CHECK(tcp_bind(b, 0, 82) == KERR_INVAL);  /* already bound */
}

static void test_table_exhaustion(void) {
    uint32_t n = 0;
    fresh();
    while (tcp_alloc(&host_a.tcp) != NULL) {
        n++;
    }
    KTEST_CHECK(n == TCP_MAX_CONNS);
    KTEST_CHECK(tcp_alloc(&host_a.tcp) == NULL);
}

static void test_poll_bits(void) {
    struct tcp_conn *c, *s, *l;
    uint8_t in[8];
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK((tcp_poll(c) & TCP_POLL_OUT) != 0 && (tcp_poll(c) & TCP_POLL_IN) == 0);
    KTEST_CHECK(tcp_poll(l) == 0);  /* its one connection was accepted */
    KTEST_CHECK(tcp_send(c, "hi", 2) == 2);
    run_ms(200);
    KTEST_CHECK((tcp_poll(s) & TCP_POLL_IN) != 0);
    KTEST_CHECK(tcp_recv(s, in, sizeof in, false) == 2);
    KTEST_CHECK((tcp_poll(s) & TCP_POLL_IN) == 0);
    tcp_free(c);
    run_ms(200);
    KTEST_CHECK((tcp_poll(s) & TCP_POLL_ERR) != 0);
}

static uint32_t notified;

static void count_notify(void *self, struct tcp_conn *c) {
    (void)self;
    (void)c;
    notified++;
}

/* **An acknowledgement that frees send space tells the owner.** A writer
 * that filled the buffer waits for "writable"; nothing else announces it.
 * Without this, a Go server writing a 42 KB stylesheet waited out its
 * poller's timer between every window — 1 to 26 seconds a page. */
static void test_an_ack_that_frees_room_notifies(void) {
    struct tcp_conn *c, *s, *l;
    static uint8_t out[TCP_TX_BUF];
    fresh();
    connect_pair(&c, &s, &l);
    KTEST_CHECK(tcp_send(c, out, sizeof out) == (int32_t)sizeof out);
    KTEST_CHECK((tcp_poll(c) & TCP_POLL_OUT) == 0); /* full: the writer waits */
    c->notify = count_notify;
    notified = 0;
    run_ms(50);
    KTEST_CHECK((tcp_poll(c) & TCP_POLL_OUT) != 0);
    KTEST_CHECK(notified > 0);
}

int main(void) {
    KTEST_RUN(test_handshake);
    KTEST_RUN(test_refused_when_nobody_listens);
    KTEST_RUN(test_connect_times_out_when_silent);
    KTEST_RUN(test_data_both_ways);
    KTEST_RUN(test_stream_larger_than_the_buffer);
    KTEST_RUN(test_stream_survives_loss);
    KTEST_RUN(test_sack_is_negotiated_both_ways);
    KTEST_RUN(test_a_gap_is_reported_as_blocks);
    KTEST_RUN(test_a_block_beyond_what_was_sent_is_ignored);
    KTEST_RUN(test_duplicates_and_reordering);
    KTEST_RUN(test_early_segments_are_held);
    KTEST_RUN(test_the_window_grows_and_a_loss_halves_it);
    KTEST_RUN(test_peek_leaves_the_data);
    KTEST_RUN(test_close_sequence);
    KTEST_RUN(test_write_after_close_is_refused);
    KTEST_RUN(test_free_resets_the_peer);
    KTEST_RUN(test_flow_control);
    KTEST_RUN(test_listener_backlog);
    KTEST_RUN(test_bind_conflicts);
    KTEST_RUN(test_table_exhaustion);
    KTEST_RUN(test_poll_bits);
    KTEST_RUN(test_an_ack_that_frees_room_notifies);
    return ktest_failures();
}
