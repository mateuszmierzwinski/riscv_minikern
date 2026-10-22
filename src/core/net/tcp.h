/* tcp — TCP (RFC 9293) for one interface, enough for Go's net package:
 * active and passive open, in-order data both ways, retransmission, and an
 * orderly close. Pure C over struct netif, so the whole state machine is
 * host-tested against crafted and hostile segments.
 *
 * What it implements: the state machine, sequence and acknowledgement
 * arithmetic with wraparound, a send queue with one retransmission timer
 * and exponential backoff, a receive queue with a real advertised window,
 * the MSS option, RST generation and validation, a bounded accept backlog,
 * **congestion control** in Reno's shape — slow start, congestion
 * avoidance, fast retransmit and fast recovery — and **reassembly** of
 * segments that arrive before their turn.
 *
 * What it deliberately does not: selective acknowledgement, window scaling,
 * timestamps and urgent data. SACK is the one worth wanting, and it is an
 * improvement on reassembly rather than a substitute for it: without the
 * queue there is nothing for a receiver to acknowledge selectively, and
 * with the queue a loss already costs one round trip instead of a window's
 * worth. TIME_WAIT lasts TCP_TIME_WAIT_MS rather than two maximum segment
 * lifetimes, because a board with TCP_MAX_CONNS connections cannot hold
 * them for minutes. Every one of these is a throughput limit, never a
 * correctness shortcut on the wire. */
#ifndef MINIKERN_CORE_NET_TCP_H
#define MINIKERN_CORE_NET_TCP_H

#include <stdbool.h>
#include <stdint.h>

#include "core/net/net.h"

#define TCP_MAX_CONNS   16u
#define TCP_HELD_SEGMENTS 4u
#define TCP_BACKLOG     8u      /* established connections one listener may hold */
#define TCP_RX_BUF      8192u
#define TCP_TX_BUF      8192u
#define TCP_MSS         1460u   /* 1500 MTU - 20 IP - 20 TCP */
#define TCP_RTO_MIN_MS  200u
#define TCP_RTO_MAX_MS  8000u
#define TCP_RETRIES     7u      /* ~25 s of backoff before a connection dies */
#define TCP_TIME_WAIT_MS 2000u
#define TCP_HLEN        20u
#define IPPROTO_TCP     6u

/* The states of RFC 9293 §3.3.2. */
enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_stack;

/* One connection, or a listener. */
/* One segment that arrived before its turn. */
struct tcp_held {
    uint32_t seq;
    uint32_t len; /* 0 when the slot is free */
    uint8_t data[TCP_MSS];
};

struct tcp_conn {
    /* Nobody owns this any more, but its shutdown is unfinished: the tick
     * reclaims it when it reaches CLOSED. See `tcp_detach`. */
    bool detached;
    struct tcp_stack *stack;
    enum tcp_state state;
    ip4_t local_ip, remote_ip;
    uint16_t local_port, remote_port;

    uint32_t snd_una;   /* oldest unacknowledged byte */
    uint32_t snd_nxt;   /* next byte to send */
    uint32_t snd_wnd;   /* what the peer will accept */
    /* Congestion control, Reno's shape (RFC 5681): the peer's window says
     * what it can *receive*, and these say what the path between us can
     * *carry*. Sending the first without the second is right on a cable
     * with nothing on it and wrong on anything shared, where it means
     * filling a queue until it drops and then doing it again.
     *
     * `cwnd` and `ssthresh` are in bytes. Below the threshold each
     * acknowledgement opens the window by a segment (slow start, which
     * doubles per round trip); above it, by a segment per round trip
     * (congestion avoidance). */
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t dup_acks;      /* repeats of the same ACK: three mean a loss */
    uint32_t recover_to;    /* snd_nxt when fast recovery began */
    bool in_recovery;
    uint32_t rcv_nxt;   /* next byte expected */
    uint32_t irs;       /* the peer's initial sequence number */

    uint8_t rx[TCP_RX_BUF];
    /* Segments that arrived early, held until the gap before them is
     * filled. Without this a single lost segment throws away everything
     * sent after it, and the peer resends the lot — which on a path that
     * loses one packet in a thousand costs far more than the packet.
     *
     * Four of them, which is a judgement rather than a law: it covers the
     * common case (one segment lost out of a burst) and bounds what one
     * connection can make the kernel hold. A fifth early segment is
     * dropped, and the peer resends it, which is the behaviour this
     * replaces — so the limit degrades into the old behaviour rather than
     * into a failure. */
    struct tcp_held held[TCP_HELD_SEGMENTS];
    uint32_t rx_len;
    uint8_t tx[TCP_TX_BUF];
    uint32_t tx_len;    /* queued from snd_una onwards, sent or not */

    uint64_t rto_at_ms; /* 0 when no timer is running */
    uint32_t rto_ms;
    uint32_t retries;
    uint64_t linger_at_ms;  /* TIME_WAIT's expiry */

    bool fin_queued;    /* close() was called; send FIN after the data */
    bool fin_seen;      /* the peer's FIN arrived in order */
    bool reset;         /* the connection was reset */
    bool used;
    int error;          /* what a blocked caller should be told */

    struct tcp_conn *listener;              /* the listener this came from */
    struct tcp_conn *backlog[TCP_BACKLOG];  /* listeners only: ready connections */
    uint32_t backlog_len;
    uint32_t backlog_max;

    /* Told when anything changed, so a socket can wake its waiters. */
    void (*notify)(void *self, struct tcp_conn *c);
    void *notify_self;
};

/* Why a connection failed, as Linux's errno values. */
#define TCP_ECONNREFUSED (-111)
#define TCP_ECONNRESET   (-104)
#define TCP_ETIMEDOUT    (-110)
#define TCP_EPIPE        (-32)

/* Readiness bits. The values are Linux's EPOLL* ones, which is the same
 * kind of ABI fact as the errno values in core/errno.h: the core may not
 * include the kernel's headers, and the syscall layer passes these
 * straight out to user space. */
#define TCP_POLL_IN  0x001u
#define TCP_POLL_OUT 0x004u
#define TCP_POLL_ERR 0x008u
#define TCP_POLL_HUP 0x010u

struct tcp_stats {
    uint32_t segments_in;
    uint32_t segments_out;
    uint32_t drop_checksum;
    uint32_t drop_malformed;
    uint32_t drop_no_socket;
    uint32_t drop_out_of_window;
    uint32_t held_out_of_order; /* early segments kept until their turn */
    uint32_t drop_out_of_order;
    uint32_t drop_backlog_full;
    uint32_t drop_bad_reset;
    uint32_t resets_sent;
    uint32_t retransmits;
    uint32_t fast_retransmits; /* losses caught by three duplicate ACKs */
    uint32_t connections;
};

struct tcp_stack {
    struct netif *nif;
    struct tcp_conn conns[TCP_MAX_CONNS];
    uint32_t iss;        /* the next initial sequence number */
    uint64_t now_ms;
    struct tcp_stats stats;
};

/* Registers TCP on nif. seed starts the initial-sequence-number sequence
 * and should be unpredictable. */
int tcp_init(struct tcp_stack *s, struct netif *nif, uint32_t seed);

/* A fresh, unbound connection, or NULL when the table is full. */
struct tcp_conn *tcp_alloc(struct tcp_stack *s);

/* Gives a connection back, **aborting** it if it was still open: anything
 * not CLOSED, LISTEN or TIME_WAIT gets a reset, because the caller has said
 * it is finished with the connection whatever state it was in. That is the
 * right primitive for an abort and the wrong one for an ordinary close —
 * see `tcp_detach`. */
void tcp_free(struct tcp_conn *c);

/* The owner is gone, but the connection is not: finish the shutdown and
 * reclaim the slot when it reaches CLOSED.
 *
 * This is what closing a socket should do after `tcp_close`. Freeing instead
 * sends a reset, and the peer — which is still completing the FIN exchange,
 * or still has data in flight — sees "connection reset by peer" at the end
 * of a transfer that had in fact succeeded. That was a real failure here:
 * Go's `io.ReadAll` on a half-closed connection reported a reset, and the
 * stack's counters showed five segments arriving for a connection that no
 * longer existed and five resets going back.
 *
 * A connection already CLOSED or LISTEN has nothing to finish, so this frees
 * it at once. */
void tcp_detach(struct tcp_conn *c);

/* Binds the local address. KERR_OK, KERR_INVAL or KERR_EXIST. */
int tcp_bind(struct tcp_conn *c, ip4_t ip, uint16_t port);

/* Starts an active open. KERR_OK means the SYN went out; the caller waits
 * for the state to become ESTABLISHED or for c->error. */
int tcp_connect(struct tcp_conn *c, ip4_t ip, uint16_t port);

/* Makes a bound connection a listener. */
int tcp_listen(struct tcp_conn *c, uint32_t backlog);

/* Takes the oldest established connection off a listener, or NULL. */
struct tcp_conn *tcp_accept(struct tcp_conn *listener);

/* Queues data. Returns what was taken (possibly less than n, zero when
 * the queue is full) or a negative errno. */
int32_t tcp_send(struct tcp_conn *c, const void *data, uint32_t n);

/* Takes up to n received bytes. Returns the count, 0 at a clean end of
 * stream, or a negative errno. peek leaves the data queued. */
int32_t tcp_recv(struct tcp_conn *c, void *data, uint32_t n, bool peek);

/* Half-closes: the queued data, then FIN. */
int tcp_close(struct tcp_conn *c);

/* Timers: retransmission, TIME_WAIT, and sending what the window allows. */
void tcp_tick(struct tcp_stack *s, uint64_t now_ms);

/* The TCP_POLL_* bits that describe this connection now. */
uint32_t tcp_poll(const struct tcp_conn *c);

/* True once a connect has finished, either way. */
bool tcp_connect_done(const struct tcp_conn *c);

#endif
