/* tcp_internal — what the TCP parts share. Not for users of tcp.h. */
#ifndef MINIKERN_CORE_NET_TCP_INTERNAL_H
#define MINIKERN_CORE_NET_TCP_INTERNAL_H

#include "core/net/net_internal.h"
#include "core/net/tcp.h"

/* Header flags (RFC 9293 §3.1). */
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

/* Field offsets in the TCP header. */
#define TH_SPORT   0u
#define TH_DPORT   2u
#define TH_SEQ     4u
#define TH_ACK     8u
#define TH_OFF     12u
#define TH_FLAGS   13u
#define TH_WINDOW  14u
#define TH_CKSUM   16u
#define TH_URGENT  18u

#define TCP_OPT_END  0u
#define TCP_OPT_NOP  1u
#define TCP_OPT_MSS  2u

/* One parsed segment. */
struct tcp_seg {
    uint32_t seq;
    uint32_t ack;
    uint32_t wnd;
    uint16_t sport;
    uint16_t dport;
    uint8_t flags;
    uint32_t mss;          /* 0 when the option was absent */
    const uint8_t *data;
    uint32_t len;          /* payload bytes */
};

/* Sequence arithmetic, which must wrap: RFC 9293 §3.4. */
static inline bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static inline bool seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

static inline bool seq_gt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static inline bool seq_ge(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) >= 0;
}

/* The window this connection can still take. */
static inline uint32_t tcp_rcv_wnd(const struct tcp_conn *c) {
    return TCP_RX_BUF - c->rx_len;
}

/* Bytes sent and not yet acknowledged, the FIN's sequence number included. */
static inline uint32_t tcp_in_flight(const struct tcp_conn *c) {
    return c->snd_nxt - c->snd_una;
}

/* Bytes queued but not yet sent. It saturates at zero: a sent FIN (or SYN)
 * occupies a sequence number that is not in the send queue, so what is in
 * flight can exceed what the queue holds. */
static inline uint32_t tcp_unsent(const struct tcp_conn *c) {
    uint32_t sent = tcp_in_flight(c);
    return c->tx_len > sent ? c->tx_len - sent : 0u;
}

/* Everything a segment needs that is not a connection's buffers. A reset
 * for a segment that belongs to nothing is built from one of these, so no
 * code path ever puts a struct tcp_conn (16 KiB of queues) on the kernel's
 * 8 KiB stack. */
struct tcp_out {
    struct tcp_stack *stack;
    ip4_t local_ip;
    ip4_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t ack;     /* the acknowledgement field, when TCP_ACK is set */
    uint32_t window;
};

/* tcp_out.c */
int tcp_emit_out(const struct tcp_out *o, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len,
                 bool with_mss);
int tcp_emit(struct tcp_conn *c, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len, bool with_mss);
void tcp_send_ack(struct tcp_conn *c);
void tcp_reset_peer(struct tcp_stack *s, ip4_t to, uint16_t our_port, uint16_t their_port, uint32_t seq,
                    uint32_t ack, bool with_ack);
void tcp_flush(struct tcp_conn *c);
void tcp_arm_timer(struct tcp_conn *c);
void tcp_cancel_timer(struct tcp_conn *c);
void tcp_retransmit(struct tcp_conn *c);

/* tcp_in.c */
void tcp_input_packet(void *self, struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len);

/* tcp.c */
void tcp_changed(struct tcp_conn *c);
void tcp_enter(struct tcp_conn *c, enum tcp_state state);
void tcp_fail(struct tcp_conn *c, int error);

/* Sends the oldest unacknowledged segment again now, without touching the
 * backoff: what fast retransmit does when three duplicate acknowledgements
 * say one segment is missing (net_tcp_in.c). */
void tcp_retransmit_oldest(struct tcp_conn *c);

#endif