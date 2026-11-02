/* sock_udp — SOCK_DGRAM sockets. Each bound socket owns one port in the
 * stack's table; arriving datagrams go into a ring of UDP_QUEUE slots, and
 * a full ring drops the newest (UDP's contract: no promise of delivery). A
 * connected socket accepts datagrams only from its peer.
 *
 * The port table is shared by IPv4 and IPv6, so each socket decides here
 * which family's datagrams are its own (`wants`): an AF_INET socket only
 * IPv4's, an AF_INET6 one both unless V6ONLY, and a socket bound to one
 * address only datagrams sent to it. */
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

static bool same_ip6(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 16u; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* Where a datagram came from, as a socket address. */
static struct sock_addr sender_of(const struct udp_datagram *d) {
    struct sock_addr a = {.ip = d->src, .port = d->sport};
    if (d->src6 != NULL) {
        a.v6 = true;
        for (uint32_t i = 0; i < 16u; i++) {
            a.ip6[i] = d->src6[i];
        }
    }
    return a;
}

/* Whether this socket's family, local address and peer admit `from`, sent
 * to `d`'s destination. */
static bool wants(const struct sock *s, const struct udp_datagram *d, const struct sock_addr *from) {
    if (from->v6 ? s->domain != AF_INET6 : s->v6only) {
        return false;
    }
    if (s->local.v6 ? !from->v6 || !same_ip6(d->dst6, s->local.ip6) : s->local.ip != 0 && from->v6) {
        return false;
    }
    if (!s->connected) {
        return true;
    }
    return from->port == s->remote.port && from->v6 == s->remote.v6 &&
           (from->v6 ? same_ip6(from->ip6, s->remote.ip6) : from->ip == s->remote.ip);
}

/* Interrupt context: queue the datagram and wake the socket's waiters. */
static void deliver(void *self, struct netif *nif, const struct udp_datagram *d) {
    struct sock *s = self;
    struct udp_sock *u = udp(s);
    struct sock_addr from = sender_of(d);
    struct dgram *slot;
    (void)nif;
    if (!wants(s, d, &from)) {
        return;
    }
    if (u->count == UDP_QUEUE || d->len > NET_UDP_MAX) {
        u->dropped++;
        return;
    }
    slot = &u->q[(u->head + u->count++) % UDP_QUEUE];
    slot->from = from;
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
    if (a.v6 ? !net_ip6_is_local(nif, a.ip6) : a.ip != 0 && !net_is_local(nif, a.ip) && a.ip != IP4_BROADCAST) {
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

/* A connect is where an unreachable peer is first told: Go dials each of
 * a name's addresses this way to choose between them (RFC 6724), and an
 * IPv6 address off the link must come back ENETUNREACH, as from a host with
 * no route, or it is ranked as if it could be reached. */
static int udp_connect(struct sock *s, struct sock_addr a) {
    int rc = a.v6 ? (net_ip6_on_link(a.ip6) ? KERR_OK : ENETUNREACH) : KERR_OK;
    if (rc == KERR_OK) {
        rc = autobind(s);
    }
    if (rc == KERR_OK) {
        s->remote = a;
        s->connected = a.v6 || a.ip != 0 || a.port != 0;  /* AF_UNSPEC-style zero disconnects */
    }
    return rc;
}

/* Whether a datagram may go to dst from this socket: the family must have
 * a side for it, and IPv4 needs an address unless it stays on the host. */
static int check_destination(const struct sock *s, const struct netif *nif, const struct sock_addr *dst,
                             uint32_t n) {
    if (dst->v6 ? n > NET_UDP6_MAX : n > NET_UDP_MAX) {
        return EMSGSIZE;
    }
    if (dst->v6 ? s->local.ip != 0 : s->v6only || s->local.v6) {
        return ENETUNREACH; /* bound to the other family's address */
    }
    if (!dst->v6 && nif->ip == 0 && dst->ip != IP4_BROADCAST && (dst->ip >> 24) != 127u) {
        return ENETUNREACH;
    }
    return KERR_OK;
}

static int32_t udp_send(struct sock *s, const void *buf, uint32_t n, const struct sock_addr *to) {
    const struct sock_addr *dst = to != NULL ? to : (s->connected ? &s->remote : NULL);
    struct netif *nif = socket_netif();
    int rc;
    if (dst == NULL) {
        return EDESTADDRREQ;
    }
    rc = check_destination(s, nif, dst, n);
    if (rc == KERR_OK) {
        rc = autobind(s);
    }
    if (rc == KERR_OK) {
        rc = dst->v6 ? net_udp6_send(nif, dst->ip6, s->local.port, dst->port, buf, n)
                     : net_udp_send_from(nif, s->local.ip, dst->ip, s->local.port, dst->port, buf, n);
    }
    return rc == KERR_OK || rc == KERR_AGAIN ? (int32_t)n : rc;  /* AGAIN: held for ARP or ND, sent soon */
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
