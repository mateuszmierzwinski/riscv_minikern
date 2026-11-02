/* sock_tcp — SOCK_STREAM sockets over core/net/tcp.h. The protocol's
 * readiness bits are Linux's EPOLL* values by construction (tcp.h), which
 * the asserts below hold the two headers to; everything else here is
 * blocking, waking and the mapping of states to errno. */
#include "core/errno.h"
#include "core/net/tcp.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/sched.h"
#include "kernel/socket.h"
#include "kernel/file.h"

_Static_assert(TCP_POLL_IN == EPOLLIN, "the poll bits are Linux's");
_Static_assert(TCP_POLL_OUT == EPOLLOUT, "the poll bits are Linux's");
_Static_assert(TCP_POLL_ERR == EPOLLERR, "the poll bits are Linux's");
_Static_assert(TCP_POLL_HUP == EPOLLHUP, "the poll bits are Linux's");

/* The connection a socket owns. */
static struct tcp_conn *conn_of(struct sock *s) {
    return s->proto;
}

/* A connection with a peer: everything from ESTABLISHED onwards, since a
 * half-closed connection still has one. The states before it (SYN_SENT and
 * SYN_RECEIVED) do not, and nor do CLOSED and LISTEN. */
static bool has_peer(const struct tcp_conn *c) {
    return c->state >= TCP_ESTABLISHED;
}

/* One end of a connection as a socket address, in its own family. */
static struct sock_addr endpoint(const struct tcp_conn *c, bool local) {
    struct sock_addr a = {.ip = local ? c->local_ip : c->remote_ip,
                          .port = local ? c->local_port : c->remote_port, .v6 = c->v6};
    if (c->v6) {
        for (uint32_t i = 0; i < 16u; i++) {
            a.ip6[i] = local ? c->local_ip6[i] : c->remote_ip6[i];
        }
    }
    return a;
}

static bool same_endpoint(const struct sock_addr *a, const struct sock_addr *b) {
    if (a->port != b->port || a->v6 != b->v6) {
        return false;
    }
    for (uint32_t i = 0; a->v6 && i < 16u; i++) {
        if (a->ip6[i] != b->ip6[i]) {
            return false;
        }
    }
    return a->v6 || a->ip == b->ip;
}

/* The families a socket's listener hears: an AF_INET socket IPv4 alone, an
 * AF_INET6 one both unless V6ONLY — or one address's family, once bound to
 * it. */
static uint8_t families_of(const struct sock *s, const struct sock_addr *bound) {
    if (s->domain != AF_INET6 || (bound->ip != 0 && !bound->v6)) {
        return TCP_FAMILY_V4;
    }
    return s->v6only || bound->v6 ? TCP_FAMILY_V6 : (uint8_t)(TCP_FAMILY_V4 | TCP_FAMILY_V6);
}

/* The TCP stack's notification: the socket's view of the connection follows
 * the protocol's, then the waiters are woken.
 *
 * Keeping s->connected in step matters more than it looks: Go confirms a
 * non-blocking connect with getpeername once the poller reports the socket
 * writable, and a socket that still called itself unconnected answered
 * ENOTCONN, so Go waited for a connection it already had. */
static void on_change(void *self, struct tcp_conn *c) {
    struct sock *s = self;
    s->connected = has_peer(c);
    if (s->connected) {
        s->local = endpoint(c, true);
        s->remote = endpoint(c, false);
    }
    if (c->error != 0) {
        s->error = c->error;
    }
    socket_wake(s);
}

/* Attaches c to s and points its notifications at s. */
static void adopt(struct sock *s, struct tcp_conn *c) {
    s->proto = c;
    c->notify = on_change;
    c->notify_self = s;
}

/* An IPv6 address is bound as the port alone plus the family: this host
 * has one IPv6 address, so "this address" and "any" differ only in that a
 * socket bound to it no longer hears IPv4. */
static int tcp_sock_bind(struct sock *s, struct sock_addr a) {
    struct netif *nif = socket_netif();
    int rc;
    if (a.v6 ? !net_ip6_is_local(nif, a.ip6) : a.ip != 0 && !net_is_local(nif, a.ip)) {
        return EADDRNOTAVAIL;
    }
    rc = tcp_bind(conn_of(s), a.ip, a.port);
    if (rc == KERR_EXIST) {
        return EADDRINUSE;
    }
    if (rc == KERR_OK) {
        conn_of(s)->families = families_of(s, &a);
        a.port = conn_of(s)->local_port;
        s->local = a;
        s->bound = true;
    }
    return rc;
}

/* Waits until the connect finished, or returns EINPROGRESS. */
static int wait_connected(struct sock *s, struct tcp_conn *c, bool nonblock) {
    while (!tcp_connect_done(c)) {
        int rc;
        if (nonblock) {
            return EINPROGRESS;
        }
        rc = sched_wait_on(s, 0);
        if (rc == KERR_INTR) {
            return rc;
        }
    }
    if (c->state != TCP_ESTABLISHED) {
        return c->error != 0 ? c->error : ECONNREFUSED;
    }
    s->connected = true;
    s->local = endpoint(c, true);
    return KERR_OK;
}

static int tcp_sock_connect(struct sock *s, struct sock_addr a) {
    struct tcp_conn *c = conn_of(s);
    int rc;
    if (c->state == TCP_ESTABLISHED) {
        return EISCONN;
    }
    if (c->state == TCP_SYN_SENT) {
        return wait_connected(s, c, (s->file->flags & O_NONBLOCK) != 0) == KERR_OK ? KERR_OK : EALREADY;
    }
    if (!a.v6 && s->v6only) {
        return ENETUNREACH;
    }
    s->remote = a;
    rc = a.v6 ? tcp_connect6(c, a.ip6, a.port) : tcp_connect(c, a.ip, a.port);
    if (rc != KERR_OK) {
        return rc == KERR_EXIST ? EISCONN : rc;
    }
    return wait_connected(s, c, (s->file->flags & O_NONBLOCK) != 0);
}

static int tcp_sock_listen(struct sock *s, int backlog) {
    int rc;
    if (!s->bound && tcp_sock_bind(s, (struct sock_addr){0}) != KERR_OK) {
        return EADDRINUSE;
    }
    rc = tcp_listen(conn_of(s), backlog < 0 ? 1u : (uint32_t)backlog);
    return rc;
}

/* Wraps an accepted connection in a socket of its own. The caller fills
 * the addresses and calls adopt. */
static int wrap_accepted(struct tcp_conn *c, struct sock **out) {
    struct sock *child = kzalloc(sizeof *child);
    if (child == NULL) {
        tcp_free(c);
        return KERR_NOMEM;
    }
    *out = child;
    return KERR_OK;
}

static int tcp_sock_accept(struct sock *s, struct sock **out, bool nonblock) {
    struct tcp_conn *l = conn_of(s);
    struct tcp_conn *c;
    int rc;
    if (l->state != TCP_LISTEN) {
        return KERR_INVAL;
    }
    while ((c = tcp_accept(l)) == NULL) {
        if (nonblock) {
            return KERR_AGAIN;
        }
        rc = sched_wait_on(s, sock_deadline(s, false));
        if (rc == KERR_INTR) {
            return rc;
        }
        if (rc == KERR_TIMEDOUT) {
            return KERR_AGAIN; /* SO_RCVTIMEO applies to accept too */
        }
    }
    rc = wrap_accepted(c, out);
    if (rc != KERR_OK) {
        return rc;
    }
    (*out)->ops = s->ops;
    (*out)->domain = s->domain;
    (*out)->v6only = s->v6only;
    (*out)->type = SOCK_STREAM;
    (*out)->bound = true;
    (*out)->connected = true;
    (*out)->local = endpoint(c, true);
    (*out)->remote = endpoint(c, false);
    adopt(*out, c);
    return KERR_OK;
}

static int32_t tcp_sock_send(struct sock *s, const void *buf, uint32_t n, const struct sock_addr *to) {
    struct tcp_conn *c = conn_of(s);
    bool nonblock = (s->file->flags & O_NONBLOCK) != 0;
    uint32_t done = 0;
    if (to != NULL && !same_endpoint(to, &s->remote)) {
        return EISCONN;  /* a stream socket cannot be redirected per send */
    }
    if (!s->connected && c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        return ENOTCONN;
    }
    while (done < n) {
        int32_t sent = tcp_send(c, (const uint8_t *)buf + done, n - done);
        if (sent < 0) {
            return done > 0 ? (int32_t)done : sent;
        }
        done += (uint32_t)sent;
        if (done < n) {
            int rc;
            if (nonblock) {
                return done > 0 ? (int32_t)done : KERR_AGAIN;
            }
            rc = sched_wait_on(s, sock_deadline(s, true));
            if (rc == KERR_INTR || rc == KERR_TIMEDOUT) {
                /* What was already sent counts: a partial write is not a
                 * failure, and a caller that is told 0 bytes went when some
                 * did will send them twice. */
                return done > 0 ? (int32_t)done : (rc == KERR_INTR ? rc : KERR_AGAIN);
            }
        }
    }
    return (int32_t)done;
}

static int32_t tcp_sock_recv(struct sock *s, void *buf, uint32_t n, struct sock_addr *from, uint32_t flags,
                             bool nonblock) {
    struct tcp_conn *c = conn_of(s);
    bool peek = (flags & MSG_PEEK) != 0;
    if (from != NULL) {
        *from = s->remote;
    }
    for (;;) {
        int32_t got = tcp_recv(c, buf, n, peek);
        int rc;
        if (got != KERR_AGAIN) {
            return got;
        }
        if (nonblock || (flags & MSG_DONTWAIT) != 0) {
            return KERR_AGAIN;
        }
        rc = sched_wait_on(s, sock_deadline(s, false));
        if (rc == KERR_INTR) {
            return rc;
        }
        if (rc == KERR_TIMEDOUT) {
            return KERR_AGAIN;
        }
    }
}

static uint32_t tcp_sock_poll(struct sock *s) {
    return tcp_poll(conn_of(s));
}

/* SHUT_WR (and SHUT_RDWR) send the FIN; SHUT_RD only stops our reading,
 * which this stack does not model, so it is accepted and ignored. */
static int tcp_sock_shutdown(struct sock *s, int how) {
    struct tcp_conn *c = conn_of(s);
    if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) {
        return ENOTCONN;
    }
    return how == 0 ? KERR_OK : tcp_close(c);
}

/* Closing a socket closes its connection **gracefully** and then lets go of
 * it: the FIN exchange finishes without an owner, and the stack reclaims the
 * slot when it is done. Freeing here instead would reset a connection whose
 * peer is still finishing, and the peer reads that as a failed transfer. */
static void tcp_sock_release(struct sock *s) {
    struct tcp_conn *c = conn_of(s);
    if (c != NULL) {
        (void)tcp_close(c);
        tcp_detach(c);
    }
    s->proto = NULL;
}

static const struct sock_ops tcp_ops = {
    .bind = tcp_sock_bind, .connect = tcp_sock_connect, .send = tcp_sock_send, .recv = tcp_sock_recv,
    .poll = tcp_sock_poll, .listen = tcp_sock_listen, .accept = tcp_sock_accept,
    .shutdown = tcp_sock_shutdown, .release = tcp_sock_release,
};

int sock_tcp_init(struct sock *s) {
    struct tcp_conn *c = tcp_alloc(knet_tcp());
    if (c == NULL) {
        return KERR_NOMEM;  /* the connection table is full: ENFILE-ish, EMFILE is closer */
    }
    s->ops = &tcp_ops;
    adopt(s, c);
    return KERR_OK;
}
