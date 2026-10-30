/* net_tcp_out — building, sending and retransmitting segments. */
#include "core/errno.h"
#include "core/net/tcp_internal.h"

/* The pseudo-header sum a TCP checksum covers (RFC 9293 §3.1). */
static uint32_t pseudo_sum(ip4_t src, ip4_t dst, uint32_t len) {
    uint8_t ph[12];
    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IPPROTO_TCP;
    put16(ph + 10, (uint16_t)len);
    return net_sum(ph, sizeof ph, 0);
}

/* Writes the fixed header into h. */
static void write_header(uint8_t *h, const struct tcp_out *o, uint32_t seq, uint8_t flags, uint32_t hlen) {
    put16(h + TH_SPORT, o->local_port);
    put16(h + TH_DPORT, o->remote_port);
    put32(h + TH_SEQ, seq);
    put32(h + TH_ACK, (flags & TCP_ACK) ? o->ack : 0u);
    h[TH_OFF] = (uint8_t)((hlen / 4u) << 4);
    h[TH_FLAGS] = flags;
    put16(h + TH_WINDOW, (uint16_t)(o->window > 0xffffu ? 0xffffu : o->window));
    put16(h + TH_CKSUM, 0);
    put16(h + TH_URGENT, 0);
}

/* Writes the options and returns how many bytes they took.
 *
 * **The length must be a multiple of four**, because the header length is
 * measured in 32-bit words — so a SACK option, whose own length is 2 + 8n,
 * is preceded by two NOPs. That is the conventional padding and it is not
 * cosmetic: without it the data offset cannot describe the header and the
 * peer reads the options as payload. */
static uint32_t write_options(uint8_t *h, const struct tcp_out *o, bool with_mss) {
    uint32_t at = TCP_HLEN;
    if (with_mss) {
        h[at] = TCP_OPT_MSS;
        h[at + 1u] = 4;
        put16(h + at + 2u, (uint16_t)TCP_MSS);
        at += 4u;
        /* Offered on every SYN, which is what lets a peer send us SACK
         * blocks; whether we send any of our own is a separate question. */
        h[at] = TCP_OPT_SACK_PERMITTED;
        h[at + 1u] = 2;
        h[at + 2u] = TCP_OPT_NOP;
        h[at + 3u] = TCP_OPT_NOP;
        at += 4u;
    }
    if (o->sack_blocks > 0) {
        h[at] = TCP_OPT_NOP;
        h[at + 1u] = TCP_OPT_NOP;
        h[at + 2u] = TCP_OPT_SACK;
        h[at + 3u] = (uint8_t)(2u + o->sack_blocks * 8u);
        at += 4u;
        for (uint32_t i = 0; i < o->sack_blocks; i++) {
            put32(h + at, o->sack_left[i]);
            put32(h + at + 4u, o->sack_right[i]);
            at += 8u;
        }
    }
    return at - TCP_HLEN;
}

int tcp_emit_out(const struct tcp_out *o, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len,
                 bool with_mss) {
    uint8_t *h = net_l4_buffer(o->stack->nif);
    uint32_t option_bytes;
    uint32_t hlen;
    uint32_t total;
    /* **Written before the header**, because the header's data offset has to
     * describe them and that is only known once they are laid out. */
    option_bytes = write_options(h, o, with_mss);
    hlen = TCP_HLEN + option_bytes;
    total = hlen + len;
    if (total > NET_L4_MAX) {
        return KERR_INVAL;
    }
    write_header(h, o, seq, flags, hlen);
    for (uint32_t i = 0; i < len; i++) {
        h[hlen + i] = data[i];
    }
    put16(h + TH_CKSUM, net_checksum(h, total, pseudo_sum(o->local_ip, o->remote_ip, total)));
    o->stack->stats.segments_out++;
    return net_ip_send_from(o->stack->nif, o->local_ip, o->remote_ip, IPPROTO_TCP, total);
}

/* The SACK blocks to report: what this connection holds out of order.
 *
 * **The held segments are the blocks**, which is why SACK became worth
 * building here — the roadmap entry's own argument against it was that
 * "without the queue there is nothing for a receiver to acknowledge
 * selectively", and the queue now exists. A block is `[left, right)`, the
 * right edge exclusive, as the specification defines it; an inclusive right
 * edge is an off-by-one the peer resolves by retransmitting one byte for
 * ever.
 *
 * Adjacent held segments are **coalesced**, because two segments that touch
 * are one contiguous run the peer should be told about once: reporting them
 * separately wastes a block and can hide a third run behind the limit.
 * Reported only when the peer offered SACK. */
static void fill_sack_blocks(struct tcp_conn *c, struct tcp_out *o) {
    if (!c->sack_ok) {
        return;
    }
    for (uint32_t i = 0; i < TCP_HELD_SEGMENTS; i++) {
        const struct tcp_held *h = &c->held[i];
        uint32_t left, right;
        bool merged = false;
        if (h->len == 0) {
            continue;
        }
        left = h->seq;
        right = h->seq + h->len;
        for (uint32_t b = 0; b < o->sack_blocks; b++) {
            if (o->sack_right[b] == left) {
                o->sack_right[b] = right;
                merged = true;
            } else if (o->sack_left[b] == right) {
                o->sack_left[b] = left;
                merged = true;
            }
        }
        if (!merged && o->sack_blocks < TCP_SACK_BLOCKS) {
            o->sack_left[o->sack_blocks] = left;
            o->sack_right[o->sack_blocks] = right;
            o->sack_blocks++;
            c->stack->stats.sack_blocks_sent++;
        }
    }
}

/* The output description of a connection. */
static struct tcp_out out_of(struct tcp_conn *c) {
    struct tcp_out o = {.stack = c->stack, .local_ip = c->local_ip, .remote_ip = c->remote_ip,
                        .local_port = c->local_port, .remote_port = c->remote_port, .ack = c->rcv_nxt,
                        .window = tcp_rcv_wnd(c)};
    fill_sack_blocks(c, &o);
    return o;
}

int tcp_emit(struct tcp_conn *c, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len, bool with_mss) {
    struct tcp_out o = out_of(c);
    return tcp_emit_out(&o, seq, flags, data, len, with_mss);
}

void tcp_send_ack(struct tcp_conn *c) {
    if (tcp_emit(c, c->snd_nxt, TCP_ACK, NULL, 0, false) != KERR_OK) {
        c->stack->stats.acks_unsent++;
    }
}

/* A reset for a segment that belongs to no connection: built from the
 * addresses alone, with no connection state and nothing large on the stack. */
void tcp_reset_peer(struct tcp_stack *s, ip4_t to, uint16_t our_port, uint16_t their_port, uint32_t seq,
                    uint32_t ack, bool with_ack) {
    struct tcp_out o = {.stack = s, .local_ip = net_source_for(s->nif, to), .remote_ip = to,
                        .local_port = our_port, .remote_port = their_port, .ack = ack, .window = 0};
    s->stats.resets_sent++;
    (void)tcp_emit_out(&o, seq, (uint8_t)(TCP_RST | (with_ack ? TCP_ACK : 0u)), NULL, 0, false);
}

void tcp_arm_timer(struct tcp_conn *c) {
    if (c->rto_at_ms == 0) {
        c->rto_ms = c->rto_ms < TCP_RTO_MIN_MS ? TCP_RTO_MIN_MS : c->rto_ms;
        c->rto_at_ms = c->stack->now_ms + c->rto_ms;
    }
}

void tcp_cancel_timer(struct tcp_conn *c) {
    c->rto_at_ms = 0;
    c->rto_ms = TCP_RTO_MIN_MS;
    c->retries = 0;
}

/* How much may be sent now: what is queued, bounded by the peer's window
 * and one segment. A zero window still allows nothing but the FIN. */
/* How much may be in flight: the smaller of what the peer will hold and
 * what the path will carry. Sending the first alone is right on a cable
 * with nothing else on it and wrong anywhere else (tcp.h). */
static uint32_t allowed(const struct tcp_conn *c) {
    return c->snd_wnd < c->cwnd ? c->snd_wnd : c->cwnd;
}

static uint32_t sendable(const struct tcp_conn *c) {
    uint32_t unsent = tcp_unsent(c);
    uint32_t limit = allowed(c);
    uint32_t room = limit > tcp_in_flight(c) ? limit - tcp_in_flight(c) : 0u;
    uint32_t n = unsent < room ? unsent : room;
    return n < TCP_MSS ? n : TCP_MSS;
}

/* True when the FIN may go now: everything queued has been sent. */
static bool fin_ready(const struct tcp_conn *c) {
    return c->fin_queued && tcp_unsent(c) == 0 &&
           (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT);
}

/* Sends as much of the queue as the window allows, then the FIN. */
void tcp_flush(struct tcp_conn *c) {
    for (uint32_t n = sendable(c); n > 0; n = sendable(c)) {
        uint32_t offset = tcp_in_flight(c);
        uint8_t flags = TCP_ACK | (tcp_unsent(c) == n ? TCP_PSH : 0u);
        if (tcp_emit(c, c->snd_nxt, flags, c->tx + offset, n, false) != KERR_OK) {
            return;
        }
        c->snd_nxt += n;
        tcp_arm_timer(c);
    }
    if (fin_ready(c)) {
        if (tcp_emit(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0, false) != KERR_OK) {
            return;
        }
        c->snd_nxt++;
        tcp_arm_timer(c);
        tcp_enter(c, c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT_1 : TCP_LAST_ACK);
    }
}

/* Resends the oldest outstanding segment: the SYN, the first unacknowledged
 * MSS of data, or the FIN. snd_nxt is left alone, so new data still goes out
 * once the peer acknowledges; a middle segment that was lost is resent from
 * the head until it lands, which is slower than selective repeat and just
 * as correct. */
/* How much of `want` bytes from `from` is **not** already held by the peer.
 *
 * A SACK block is `[left, right)` above the cumulative acknowledgement. If
 * one begins inside the run about to be retransmitted, the retransmission
 * stops there: everything from that point is data the peer has. Returns
 * `want` unchanged when there are no blocks, which is every connection whose
 * peer did not offer SACK and every one that has lost nothing.
 *
 * Only the *start* of a hole is needed, not a full scoreboard: this sender
 * retransmits from `snd_una` forwards, so the first gap is the only one it
 * can be working on. A scoreboard would let it skip *over* a block to the
 * next hole, which is the rest of RFC 6675 and is not built — what is built
 * is the part that stops the same bytes being sent twice. */
static uint32_t sack_hole_end(const struct tcp_conn *c, uint32_t from, uint32_t want) {
    uint32_t limit = want;
    for (uint32_t i = 0; i < c->sacked_blocks; i++) {
        uint32_t left = c->sacked_left[i];
        if (seq_gt(left, from) && left - from < limit) {
            limit = left - from;
        }
    }
    return limit > 0 ? limit : want;
}

static void resend_oldest(struct tcp_conn *c) {
    if (c->state == TCP_SYN_SENT) {
        (void)tcp_emit(c, c->snd_una, TCP_SYN, NULL, 0, true);
        return;
    }
    if (c->state == TCP_SYN_RECEIVED) {
        (void)tcp_emit(c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0, true);
        return;
    }
    if (c->tx_len > 0) {
        /* **Shortened to stop where the peer's first SACK block begins.**
         * The peer has told us it already holds that run, so resending into
         * it is work whose result is discarded — and on a lossy path it is
         * the difference between recovering one segment per round trip and
         * resending a whole window. Without SACK this is `TCP_MSS` and
         * behaves exactly as before, which is the property that matters:
         * nothing changes for a peer that did not offer the option. */
        uint32_t len = c->tx_len < TCP_MSS ? c->tx_len : TCP_MSS;
        uint32_t stop = sack_hole_end(c, c->snd_una, len);
        (void)tcp_emit(c, c->snd_una, TCP_ACK | TCP_PSH, c->tx, stop, false);
        return;
    }
    (void)tcp_emit(c, c->snd_una, TCP_FIN | TCP_ACK, NULL, 0, false);  /* only the FIN is outstanding */
}

/* The timer fired: resend the oldest unacknowledged segment, back off, and
 * give up after TCP_RETRIES. */
void tcp_retransmit_oldest(struct tcp_conn *c) {
    if (tcp_in_flight(c) > 0) {
        c->stack->stats.fast_retransmits++;
        resend_oldest(c);
        c->rto_at_ms = c->stack->now_ms + c->rto_ms;
    }
}

void tcp_retransmit(struct tcp_conn *c) {
    if (tcp_in_flight(c) == 0) {
        tcp_cancel_timer(c);
        return;
    }
    if (++c->retries > TCP_RETRIES) {
        tcp_fail(c, TCP_ETIMEDOUT);
        return;
    }
    c->stack->stats.retransmits++;
    /* A timeout is the strong signal: not one segment missing but nothing
     * getting through, so the window goes back to one segment and slow
     * start begins again from half of where it was. Anything gentler here
     * is what turns a congested path into a collapsed one. */
    c->ssthresh = tcp_in_flight(c) / 2u;
    c->ssthresh = c->ssthresh < 2u * TCP_MSS ? 2u * TCP_MSS : c->ssthresh;
    c->cwnd = TCP_MSS;
    c->in_recovery = false;
    c->dup_acks = 0;
    resend_oldest(c);
    c->rto_ms = c->rto_ms * 2u > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : c->rto_ms * 2u;
    c->rto_at_ms = c->stack->now_ms + c->rto_ms;
}
