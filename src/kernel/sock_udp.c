/* sock_udp — SOCK_DGRAM sockets. Each bound socket owns one port in the
 * stack's table; arriving datagrams go into a ring of UDP_QUEUE slots, and
 * a full ring drops the newest (UDP's contract: no promise of delivery). A
 * connected socket accepts datagrams only from its peer. */
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/sched.h"
#include "kernel/socket.h"
#include "kernel/file.h"

#define UDP_QUEUE 16u

struct dgram {
    struct sock_addr from;
    uint32_t len;
    uint8_t data[NET_UDP_MAX];
};

struct udp_sock {
    struct dgram q[UDP_QUEUE];
    uint32_t head;   /* oldest */
    uint32_t count;
    uint32_t dropped;
};

static struct udp_sock *udp(struct sock *s) {
    return s->proto;
}

/* Interrupt context: queue the datagram and wake the socket's waiters. */
static void deliver(void *self, struct netif *nif, const struct udp_datagram *d) {
    struct sock *s = self;
    struct udp_sock *u = udp(s);
    struct dgram *slot;
    (void)nif;
    if (s->connected && (d->src != s->remote.ip || d->sport != s->remote.port)) {
        return;
    }
    if (u->count == UDP_QUEUE || d->len > NET_UDP_MAX) {
        u->dropped++;
        return;
    }
    slot = &u->q[(u->head + u->count++) % UDP_QUEUE];
    slot->from = (struct sock_addr){.ip = d->src, .port = d->sport};
    slot->len = d->len;
    for (uint32_t i = 0; i < d->len; i++) {
        slot->data[i] = d->data[i];
    }
    socket_wake(s);
}

/* Takes the port (0: an ephemeral one) in the stack's table. */
static int udp_bind(struct sock *s, struct sock_addr a) {
    struct netif *nif = socket_netif();
    struct udp_handler h = {.receive = deliver, .self = s};
    int rc;
    if (s->bound) {
        return KERR_INVAL;
    }
    if (a.ip != 0 && !net_is_local(nif, a.ip) && a.ip != IP4_BROADCAST) {
        return EADDRNOTAVAIL;
    }
    a.port = a.port != 0 ? a.port : net_udp_ephemeral(nif);
    rc = a.port == 0 ? EADDRINUSE : net_udp_bind(nif, a.port, &h);
    if (rc != KERR_OK) {
        return rc == KERR_EXIST ? EADDRINUSE : rc;
    }
    s->local = a;
    s->bound = true;
    return KERR_OK;
}

static int autobind(struct sock *s) {
    return s->bound ? KERR_OK : udp_bind(s, (struct sock_addr){0});
}

static int udp_connect(struct sock *s, struct sock_addr a) {
    int rc = autobind(s);
    if (rc == KERR_OK) {
        s->remote = a;
        s->connected = a.ip != 0 || a.port != 0;  /* AF_UNSPEC-style zero disconnects */
    }
    return rc;
}

static int32_t udp_send(struct sock *s, const void *buf, uint32_t n, const struct sock_addr *to) {
    const struct sock_addr *dst = to != NULL ? to : (s->connected ? &s->remote : NULL);
    struct netif *nif = socket_netif();
    int rc;
    if (dst == NULL) {
        return EDESTADDRREQ;
    }
    if (n > NET_UDP_MAX) {
        return EMSGSIZE;
    }
    if (nif->ip == 0 && dst->ip != IP4_BROADCAST && (dst->ip >> 24) != 127u) {
        return ENETUNREACH;
    }
    rc = autobind(s);
    if (rc == KERR_OK) {
        rc = net_udp_send_from(nif, s->local.ip, dst->ip, s->local.port, dst->port, buf, n);
    }
    return rc == KERR_OK || rc == KERR_AGAIN ? (int32_t)n : rc;  /* AGAIN: held for ARP, sent soon */
}

/* Waits for a datagram; KERR_OK when one is queued. */
static int wait_datagram(struct sock *s, bool nonblock) {
    while (udp(s)->count == 0) {
        int rc;
        if (nonblock) {
            return KERR_AGAIN;
        }
        rc = sched_wait_on(s, sock_deadline(s, false));
        if (rc == KERR_INTR) {
            return rc;
        }
        if (rc == KERR_TIMEDOUT) {
            return KERR_AGAIN; /* what a timed-out socket says, as on Linux */
        }
    }
    return KERR_OK;
}

static int32_t udp_recv(struct sock *s, void *buf, uint32_t n, struct sock_addr *from, uint32_t flags,
                        bool nonblock) {
    struct udp_sock *u = udp(s);
    struct dgram *d;
    uint32_t c;
    int rc = wait_datagram(s, nonblock || (flags & MSG_DONTWAIT));
    if (rc != KERR_OK) {
        return rc;
    }
    d = &u->q[u->head];
    c = d->len < n ? d->len : n;  /* the rest of a longer datagram is lost, as POSIX says */
    for (uint32_t i = 0; i < c; i++) {
        ((uint8_t *)buf)[i] = d->data[i];
    }
    if (from != NULL) {
        *from = d->from;
    }
    if ((flags & MSG_PEEK) == 0) {
        u->head = (u->head + 1u) % UDP_QUEUE;
        u->count--;
    }
    return (int32_t)c;
}

static uint32_t udp_poll(struct sock *s) {
    return EPOLLOUT | (udp(s)->count > 0 ? EPOLLIN : 0u);
}

static int udp_listen(struct sock *s, int backlog) {
    (void)s;
    (void)backlog;
    return EOPNOTSUPP;
}

static int udp_accept(struct sock *s, struct sock **out, bool nonblock) {
    (void)s;
    (void)out;
    (void)nonblock;
    return EOPNOTSUPP;
}

static int udp_shutdown(struct sock *s, int how) {
    (void)how;
    return s->connected ? KERR_OK : ENOTCONN;
}

static void udp_release(struct sock *s) {
    if (s->bound) {
        net_udp_unbind(socket_netif(), s->local.port);
    }
    kfree(s->proto);
}

static const struct sock_ops udp_ops = {
    .bind = udp_bind, .connect = udp_connect, .send = udp_send, .recv = udp_recv, .poll = udp_poll,
    .listen = udp_listen, .accept = udp_accept, .shutdown = udp_shutdown, .release = udp_release,
};

int sock_udp_init(struct sock *s) {
    s->proto = kzalloc(sizeof(struct udp_sock));
    if (s->proto == NULL) {
        return KERR_NOMEM;
    }
    s->ops = &udp_ops;
    return KERR_OK;
}
