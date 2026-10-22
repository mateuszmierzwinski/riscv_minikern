/* net_tcp — the connection table, the state machine and the input path.
 * Segment output and the timers are in net_tcp_out.c. */
#include "core/errno.h"
#include "core/net/tcp_internal.h"

/* Tells the owner (a socket) that something changed. */
void tcp_changed(struct tcp_conn *c) {
    if (c->notify != NULL) {
        c->notify(c->notify_self, c);
    }
}

void tcp_enter(struct tcp_conn *c, enum tcp_state state) {
    c->state = state;
    if (state == TCP_TIME_WAIT) {
        c->linger_at_ms = c->stack->now_ms + TCP_TIME_WAIT_MS;
    }
    tcp_changed(c);
}

/* Ends a connection with an error a blocked caller can see. */
void tcp_fail(struct tcp_conn *c, int error) {
    c->error = error;
    c->reset = true;
    tcp_cancel_timer(c);
    tcp_enter(c, TCP_CLOSED);
}

int tcp_init(struct tcp_stack *s, struct netif *nif, uint32_t seed) {
    struct ip_handler h;
    net_zero(s, sizeof *s);
    s->nif = nif;
    s->iss = seed;
    s->now_ms = nif->now_ms;
    h = (struct ip_handler){.input = tcp_input_packet, .self = s};
    return net_ip_register(nif, IPPROTO_TCP, &h);
}

struct tcp_conn *tcp_alloc(struct tcp_stack *s) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        if (!s->conns[i].used) {
            struct tcp_conn *c = &s->conns[i];
            net_zero(c, sizeof *c);
            c->used = true;
            c->stack = s;
            c->state = TCP_CLOSED;
            c->rto_ms = TCP_RTO_MIN_MS;
            c->snd_wnd = TCP_MSS;  /* until the peer tells us its window */
            /* Ten segments to start with, as RFC 6928 settled on: one was
             * chosen when a round trip cost a second, and on anything
             * modern it spends three round trips discovering that the path
             * is fine. The threshold starts out of the way, so the first
             * loss is what sets it. */
            c->cwnd = 10u * TCP_MSS;
            c->ssthresh = 0xffffffffu;
            return c;
        }
    }
    return NULL;
}

/* Drops a connection from its listener's backlog, if it is in one. */
static void unlink_backlog(struct tcp_conn *c) {
    struct tcp_conn *l = c->listener;
    if (l == NULL) {
        return;
    }
    for (uint32_t i = 0; i < l->backlog_len; i++) {
        if (l->backlog[i] == c) {
            l->backlog[i] = l->backlog[--l->backlog_len];
            break;
        }
    }
    c->listener = NULL;
}

void tcp_free(struct tcp_conn *c) {
    if (c == NULL || !c->used) {
        return;
    }
    if (c->state != TCP_CLOSED && c->state != TCP_LISTEN && c->state != TCP_TIME_WAIT) {
        (void)tcp_emit(c, c->snd_nxt, TCP_RST | TCP_ACK, NULL, 0, false);  /* abort what is left */
    }
    for (uint32_t i = 0; i < c->backlog_len; i++) {
        c->backlog[i]->listener = NULL;
        tcp_free(c->backlog[i]);
    }
    unlink_backlog(c);
    c->used = false;
    c->state = TCP_CLOSED;
    c->notify = NULL;
}

/* True when some other connection already holds this local port. */
static bool port_taken(const struct tcp_stack *s, const struct tcp_conn *self, ip4_t ip, uint16_t port) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        const struct tcp_conn *c = &s->conns[i];
        if (c == self || !c->used || c->local_port != port) {
            continue;
        }
        if (c->state == TCP_LISTEN && (c->local_ip == 0 || ip == 0 || c->local_ip == ip)) {
            return true;
        }
    }
    return false;
}

int tcp_bind(struct tcp_conn *c, ip4_t ip, uint16_t port) {
    if (c->state != TCP_CLOSED || c->local_port != 0) {
        return KERR_INVAL;
    }
    if (port != 0 && port_taken(c->stack, c, ip, port)) {
        return KERR_EXIST;
    }
    c->local_ip = ip;
    c->local_port = port != 0 ? port : net_udp_ephemeral(c->stack->nif);
    return c->local_port == 0 ? KERR_NOMEM : KERR_OK;
}

int tcp_connect(struct tcp_conn *c, ip4_t ip, uint16_t port) {
    if (c->state != TCP_CLOSED) {
        return c->state == TCP_ESTABLISHED ? KERR_EXIST : KERR_INVAL;
    }
    if (ip == 0 || port == 0) {
        return KERR_INVAL;
    }
    if (c->local_port == 0 && tcp_bind(c, 0, 0) != KERR_OK) {
        return KERR_NOMEM;
    }
    if (c->local_ip == 0) {
        c->local_ip = net_source_for(c->stack->nif, ip);  /* loopback keeps a loopback source */
    }
    c->remote_ip = ip;
    c->remote_port = port;
    c->snd_una = c->stack->iss;
    c->snd_nxt = c->snd_una + 1u;  /* the SYN occupies one sequence number */
    c->stack->iss += 0x9e3779b9u;  /* a different space for the next connection */
    c->stack->stats.connections++;
    tcp_enter(c, TCP_SYN_SENT);
    tcp_arm_timer(c);
    /* The SYN's fate does not decide the connect's: a send that is waiting
     * for ARP, or a ring that is briefly full, is retried by the timer. */
    (void)tcp_emit(c, c->snd_una, TCP_SYN, NULL, 0, true);
    return KERR_OK;
}

int tcp_listen(struct tcp_conn *c, uint32_t backlog) {
    if (c->local_port == 0 || (c->state != TCP_CLOSED && c->state != TCP_LISTEN)) {
        return KERR_INVAL;
    }
    c->backlog_max = backlog == 0 ? 1u : (backlog < TCP_BACKLOG ? backlog : TCP_BACKLOG);
    tcp_enter(c, TCP_LISTEN);
    return KERR_OK;
}

struct tcp_conn *tcp_accept(struct tcp_conn *listener) {
    struct tcp_conn *c;
    if (listener->state != TCP_LISTEN || listener->backlog_len == 0) {
        return NULL;
    }
    c = listener->backlog[0];
    for (uint32_t i = 1; i < listener->backlog_len; i++) {
        listener->backlog[i - 1] = listener->backlog[i];
    }
    listener->backlog_len--;
    c->listener = NULL;
    return c;
}

int32_t tcp_send(struct tcp_conn *c, const void *data, uint32_t n) {
    const uint8_t *p = data;
    uint32_t room = TCP_TX_BUF - c->tx_len;
    uint32_t take = n < room ? n : room;
    if (c->reset || c->error != 0) {
        return c->error != 0 ? c->error : TCP_ECONNRESET;
    }
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        return TCP_EPIPE;
    }
    if (c->fin_queued) {
        return TCP_EPIPE;
    }
    for (uint32_t i = 0; i < take; i++) {
        c->tx[c->tx_len + i] = p[i];
    }
    c->tx_len += take;
    tcp_flush(c);
    return (int32_t)take;
}

int32_t tcp_recv(struct tcp_conn *c, void *data, uint32_t n, bool peek) {
    uint8_t *p = data;
    uint32_t take = n < c->rx_len ? n : c->rx_len;
    if (c->rx_len == 0) {
        if (c->error != 0) {
            return c->error;
        }
        return (c->fin_seen || c->state == TCP_CLOSED) ? 0 : KERR_AGAIN;
    }
    for (uint32_t i = 0; i < take; i++) {
        p[i] = c->rx[i];
    }
    if (!peek) {
        for (uint32_t i = take; i < c->rx_len; i++) {
            c->rx[i - take] = c->rx[i];
        }
        c->rx_len -= take;
        tcp_send_ack(c);  /* the window just opened */
    }
    return (int32_t)take;
}

int tcp_close(struct tcp_conn *c) {
    if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) {
        tcp_enter(c, TCP_CLOSED);
        return KERR_OK;
    }
    if (c->state == TCP_SYN_SENT) {
        tcp_cancel_timer(c);
        tcp_enter(c, TCP_CLOSED);
        return KERR_OK;
    }
    c->fin_queued = true;
    tcp_flush(c);
    return KERR_OK;
}

uint32_t tcp_poll(const struct tcp_conn *c) {
    uint32_t events = 0;
    if (c->state == TCP_LISTEN) {
        return c->backlog_len > 0 ? TCP_POLL_IN : 0u;
    }
    if (c->rx_len > 0 || c->fin_seen || c->state == TCP_CLOSED) {
        events |= TCP_POLL_IN;
    }
    if ((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) && c->tx_len < TCP_TX_BUF) {
        events |= TCP_POLL_OUT;
    }
    if (c->fin_seen && c->tx_len == 0) {
        events |= TCP_POLL_HUP;
    }
    if (c->error != 0) {
        events |= TCP_POLL_ERR | TCP_POLL_IN;
    }
    return events;
}

bool tcp_connect_done(const struct tcp_conn *c) {
    return c->state != TCP_SYN_SENT && c->state != TCP_SYN_RECEIVED;
}

void tcp_tick(struct tcp_stack *s, uint64_t now_ms) {
    s->now_ms = now_ms;
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn *c = &s->conns[i];
        if (!c->used) {
            continue;
        }
        if (c->state == TCP_TIME_WAIT && now_ms >= c->linger_at_ms) {
            tcp_enter(c, TCP_CLOSED);
            continue;
        }
        if (c->rto_at_ms != 0 && now_ms >= c->rto_at_ms) {
            tcp_retransmit(c);
        }
        tcp_flush(c);
    }
}
