/* Unix-domain sockets: two programs on this machine talking over a socket.
 *
 * **Why there is a third protocol rather than a loopback shortcut.** The
 * stack already carries traffic to 127.0.0.1, so a local conversation was
 * *possible* — over IP, with checksums, a port space shared with the
 * network, and a listener reachable by anything that can reach this host. A
 * unix socket is a different guarantee: the name is not on the network at
 * all, so nothing outside the machine can connect to it, and the bytes never
 * touch the IP stack.
 *
 * Both kinds are here. `SOCK_STREAM` is a pair of rings, one per direction,
 * which is what makes a half-close meaningful: the writer's end closing must
 * let the reader drain what is already buffered and *then* see end of file.
 * `SOCK_DGRAM` keeps whole messages with their sender's name, because a
 * datagram socket that joined its messages together would be a stream with
 * extra steps.
 *
 * **The name is interned, not a filesystem node.** Linux makes a socket
 * inode, so the path is visible to `ls` and removed with `unlink`. This
 * kernel's root filesystem is frequently read-only — it is an SD card
 * mounted at boot — so a bind that needed to create a file would fail on
 * exactly the machine this is for. Names therefore live in a kernel table,
 * which is **fully correct for the abstract namespace** (a name whose first
 * byte is NUL, which never had a filesystem entry even on Linux) and a
 * documented approximation for a path: the socket works, and `stat` on the
 * path does not find it. The deviation is in CLAUDE.md rather than only
 * here.
 */
#include "kernel/socket.h"

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/file.h"
#include "kernel/sched.h"

#include <stddef.h>

/* How much one direction of a stream holds before a writer blocks, and how
 * many datagrams a message socket queues.
 *
 * A stream's buffer is the whole of its flow control: a writer that fills it
 * waits for the reader, which is the back pressure a local socket is
 * expected to have. 4 KiB is a page, which is the unit everything else here
 * allocates in. */
#define STREAM_BYTES 4096u
#define DGRAM_QUEUE 8u
#define DGRAM_BYTES 2048u

/* One direction of a stream: a ring, and whether the end that writes into it
 * has gone. `shut` is what separates "nothing to read yet" from "nothing
 * more will ever arrive", and a reader that could not tell them apart would
 * either block for ever at end of file or report end of file on an idle
 * connection. */
struct pipe_half {
    uint8_t data[STREAM_BYTES];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    bool shut;
};

/* A queued datagram, with its sender so that `recvfrom` can say who sent it.
 * An unbound sender has no name, which is ordinary — a client need not bind
 * — and is reported as an empty address rather than as a failure. */
struct dgram {
    uint8_t data[DGRAM_BYTES];
    uint32_t len;
    uint16_t from;
};

/* A unix socket's own state.
 *
 * `peer` is the other end of a connected stream, and the two point at each
 * other. The pair is what makes a close visible to the far side: releasing
 * one marks the other's incoming half shut and wakes it, so the peer learns
 * at once rather than on a timeout. */
struct unix_sock {
    struct sock *peer;
    /* Which half this socket reads from and which it writes into. A stream
     * pair allocates two and crosses them over, so neither end needs to know
     * which of the two it was given. */
    struct pipe_half *in;
    struct pipe_half *out;
    /* A listener's queue of sockets that have connected and not yet been
     * accepted. */
    struct sock *pending[8];
    uint32_t pending_count;
    uint32_t backlog;
    bool listening;
    /* Datagrams waiting, for SOCK_DGRAM. */
    struct dgram *queue;
    uint32_t queue_count;
    uint32_t queue_head;
};

static struct unix_sock *un(struct sock *s) {
    return s->proto;
}

/* ── the name table ─────────────────────────────────────────────────────── */

/* Names are compared by bytes and length, never as C strings: an abstract
 * name begins with NUL, so `strcmp` would call every one of them equal. */
struct unix_name_slot {
    char name[UNIX_NAME_MAX];
    uint32_t len;
    struct sock *owner;
    bool used;
};

static struct unix_name_slot names[UNIX_NAMES];

/* The table slot an index names, or NULL.
 *
 * **Every use of an index goes through this.** `owner_of` used to bounds
 * check and return NULL, and a caller then tested that against NULL and
 * indexed the array itself — so an index past the end passed the test (there
 * is no owner) and the write that followed landed past `names`. The static
 * analyser found it, which is the second real defect that suite has caught
 * in this tree. Returning the slot rather than a field is what makes the
 * check impossible to skip: there is no index arithmetic outside this
 * function to get wrong. */
static struct unix_name_slot *slot_of(uint16_t index) {
    if (index == 0 || index > UNIX_NAMES || !names[index - 1u].used) {
        return NULL;
    }
    return &names[index - 1u];
}

/* The socket listening on a name, or NULL. */
static struct sock *owner_of(uint16_t index) {
    struct unix_name_slot *slot = slot_of(index);
    return slot != NULL ? slot->owner : NULL;
}

static bool same_name(uint32_t i, const char *name, uint32_t len) {
    if (!names[i].used || names[i].len != len) {
        return false;
    }
    for (uint32_t k = 0; k < len; k++) {
        if (names[i].name[k] != name[k]) {
            return false;
        }
    }
    return true;
}

uint16_t unix_name_intern(const char *name, uint32_t len, bool create) {
    if (name == NULL || len == 0 || len > UNIX_NAME_MAX) {
        return 0;
    }
    for (uint32_t i = 0; i < UNIX_NAMES; i++) {
        if (same_name(i, name, len)) {
            return (uint16_t)(i + 1u);
        }
    }
    if (!create) {
        return 0;
    }
    for (uint32_t i = 0; i < UNIX_NAMES; i++) {
        if (names[i].used) {
            continue;
        }
        for (uint32_t k = 0; k < len; k++) {
            names[i].name[k] = name[k];
        }
        names[i].len = len;
        names[i].used = true;
        names[i].owner = NULL;
        return (uint16_t)(i + 1u);
    }
    return 0;
}

const char *unix_name_at(uint16_t index, uint32_t *len) {
    struct unix_name_slot *slot = slot_of(index);
    if (slot == NULL) {
        return NULL;
    }
    if (len != NULL) {
        *len = slot->len;
    }
    return slot->name;
}

void unix_name_release(uint16_t index) {
    struct unix_name_slot *slot = slot_of(index);
    if (slot != NULL) {
        slot->used = false;
        slot->owner = NULL;
    }
}


/* ── the rings ──────────────────────────────────────────────────────────── */

static uint32_t half_write(struct pipe_half *h, const uint8_t *src, uint32_t n) {
    uint32_t room = STREAM_BYTES - h->count;
    uint32_t take = n < room ? n : room;
    for (uint32_t i = 0; i < take; i++) {
        h->data[h->tail] = src[i];
        h->tail = (h->tail + 1u) % STREAM_BYTES;
    }
    h->count += take;
    return take;
}

static uint32_t half_read(struct pipe_half *h, uint8_t *dst, uint32_t n) {
    uint32_t take = n < h->count ? n : h->count;
    for (uint32_t i = 0; i < take; i++) {
        dst[i] = h->data[h->head];
        h->head = (h->head + 1u) % STREAM_BYTES;
    }
    h->count -= take;
    return take;
}

/* ── the operations ─────────────────────────────────────────────────────── */

static int unix_bind(struct sock *s, struct sock_addr a) {
    if (s->bound) {
        return KERR_INVAL;
    }
    if (a.unix_name == 0) {
        return KERR_INVAL; /* no name: the syscall layer could not intern one */
    }
    {
        struct unix_name_slot *slot = slot_of(a.unix_name);
        if (slot == NULL) {
            return KERR_INVAL; /* an index naming nothing: never indexed blind */
        }
        if (slot->owner != NULL) {
            return EADDRINUSE;
        }
        slot->owner = s;
    }
    s->local = a;
    s->bound = true;
    return KERR_OK;
}

static int unix_listen(struct sock *s, int backlog) {
    if (s->type != SOCK_STREAM) {
        return EOPNOTSUPP;
    }
    if (!s->bound) {
        return KERR_INVAL; /* nothing could reach it: a listener needs a name */
    }
    un(s)->backlog = backlog <= 0 ? 1u : (uint32_t)backlog;
    if (un(s)->backlog > 8u) {
        un(s)->backlog = 8u;
    }
    un(s)->listening = true;
    return KERR_OK;
}

/* Crosses two halves over so each end reads what the other writes. Allocated
 * once per connection and owned by the pair; whichever end is released last
 * frees them, which is why the owner is recorded on both. */
static int pair_them_up(struct sock *a, struct sock *b) {
    struct pipe_half *one = kzalloc(sizeof *one);
    struct pipe_half *two = kzalloc(sizeof *two);
    if (one == NULL || two == NULL) {
        kfree(one);
        kfree(two);
        return KERR_NOMEM;
    }
    un(a)->out = one;
    un(b)->in = one;
    un(b)->out = two;
    un(a)->in = two;
    un(a)->peer = b;
    un(b)->peer = a;
    a->connected = true;
    b->connected = true;
    return KERR_OK;
}

/* The accepted side of a connection: a socket with no name of its own.
 *
 * It is **not** the listener. A listener that handed itself to `accept`
 * could serve one client and would lose its own name on the first close,
 * which is the bug this allocation exists to avoid. */
static struct sock *make_the_other_end(int type) {
    struct sock *s = kzalloc(sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    s->domain = AF_UNIX;
    s->type = type;
    if (unix_sock_init(s, type) != KERR_OK) {
        kfree(s);
        return NULL;
    }
    return s;
}

static int unix_connect(struct sock *s, struct sock_addr a) {
    struct sock *listener = owner_of(a.unix_name);
    struct sock *server_end;
    if (s->type == SOCK_DGRAM) {
        /* A datagram socket records where to send and nothing more: there is
         * no handshake, and a connect to a name nobody holds yet is allowed
         * to succeed because the name may be bound before the first send. */
        s->remote = a;
        s->connected = true;
        return KERR_OK;
    }
    if (listener == NULL || !un(listener)->listening) {
        return ECONNREFUSED;
    }
    if (un(listener)->pending_count >= un(listener)->backlog) {
        return ECONNREFUSED; /* the backlog is full, as a listener's queue is */
    }
    server_end = make_the_other_end(s->type);
    if (server_end == NULL) {
        return KERR_NOMEM;
    }
    if (pair_them_up(s, server_end) != KERR_OK) {
        kfree(server_end);
        return KERR_NOMEM;
    }
    server_end->local = listener->local;
    un(listener)->pending[un(listener)->pending_count++] = server_end;
    s->remote = a;
    socket_wake(listener);
    return KERR_OK;
}

static int unix_accept(struct sock *s, struct sock **out, bool nonblock) {
    if (!un(s)->listening) {
        return KERR_INVAL;
    }
    while (un(s)->pending_count == 0) {
        int rc;
        if (nonblock) {
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
    *out = un(s)->pending[0];
    for (uint32_t i = 1; i < un(s)->pending_count; i++) {
        un(s)->pending[i - 1u] = un(s)->pending[i];
    }
    un(s)->pending_count--;
    return KERR_OK;
}

/* Queues one datagram on the socket bound to `to`. */
static int32_t post_datagram(struct sock *from, struct sock_addr to, const uint8_t *buf, uint32_t n) {
    struct sock *target = owner_of(to.unix_name);
    struct unix_sock *u;
    uint32_t slot;
    if (target == NULL) {
        return ECONNREFUSED; /* nobody is bound to that name */
    }
    if (n > DGRAM_BYTES) {
        return EMSGSIZE; /* a datagram is not split: that would make it a stream */
    }
    u = un(target);
    if (u->queue_count == DGRAM_QUEUE) {
        /* **Dropped, not blocked.** A full datagram queue on Linux blocks a
         * unix sender, but blocking here would let one unresponsive reader
         * stop a writer for ever with no deadline — the one case this
         * kernel's scheduler cannot wake. Reported as EAGAIN so a caller
         * retries rather than believing it was sent. */
        return KERR_AGAIN;
    }
    slot = (u->queue_head + u->queue_count) % DGRAM_QUEUE;
    for (uint32_t i = 0; i < n; i++) {
        u->queue[slot].data[i] = buf[i];
    }
    u->queue[slot].len = n;
    u->queue[slot].from = from->bound ? from->local.unix_name : 0u;
    u->queue_count++;
    socket_wake(target);
    return (int32_t)n;
}

static int32_t unix_send(struct sock *s, const void *buf, uint32_t n, const struct sock_addr *to) {
    struct unix_sock *u = un(s);
    uint32_t done = 0;
    if (s->type == SOCK_DGRAM) {
        struct sock_addr dst = to != NULL ? *to : s->remote;
        if (dst.unix_name == 0) {
            return EDESTADDRREQ;
        }
        return post_datagram(s, dst, buf, n);
    }
    if (!s->connected || u->out == NULL) {
        return ENOTCONN;
    }
    while (done < n) {
        uint32_t took;
        if (u->peer == NULL || u->out->shut) {
            /* The reader has gone. The first such write is an error rather
             * than a short count, which is what a caller can act on. */
            return done > 0 ? (int32_t)done : EPIPE;
        }
        took = half_write(u->out, (const uint8_t *)buf + done, n - done);
        done += took;
        if (took > 0) {
            socket_wake(u->peer);
        }
        if (done < n) {
            int rc = sched_wait_on(s, sock_deadline(s, true));
            if (rc == KERR_INTR) {
                return done > 0 ? (int32_t)done : rc;
            }
            if (rc == KERR_TIMEDOUT) {
                return done > 0 ? (int32_t)done : KERR_AGAIN;
            }
        }
    }
    return (int32_t)done;
}

/* Hands back one queued datagram. */
static int32_t take_datagram(struct sock *s, uint8_t *buf, uint32_t n, struct sock_addr *from) {
    struct unix_sock *u = un(s);
    struct dgram *d = &u->queue[u->queue_head];
    uint32_t take = n < d->len ? n : d->len;
    for (uint32_t i = 0; i < take; i++) {
        buf[i] = d->data[i];
    }
    if (from != NULL) {
        *from = (struct sock_addr){.unix_name = d->from};
    }
    /* **The whole datagram is consumed even when the buffer was short**,
     * which is what a message socket does: the remainder is discarded rather
     * than left to appear as the next message. */
    u->queue_head = (u->queue_head + 1u) % DGRAM_QUEUE;
    u->queue_count--;
    return (int32_t)take;
}

static int32_t unix_recv(struct sock *s, void *buf, uint32_t n, struct sock_addr *from, uint32_t flags,
                         bool nonblock) {
    struct unix_sock *u = un(s);
    (void)flags;
    if (s->type == SOCK_DGRAM) {
        while (u->queue_count == 0) {
            int rc;
            if (nonblock) {
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
        return take_datagram(s, buf, n, from);
    }
    if (u->in == NULL) {
        return ENOTCONN;
    }
    while (u->in->count == 0) {
        int rc;
        /* **Drained first, then end of file.** The order of these two tests
         * is the half-close: a peer that wrote and closed leaves bytes in
         * the ring, and reporting end of file while they are unread would
         * lose the last of every conversation. */
        if (u->in->shut) {
            return 0;
        }
        if (nonblock) {
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
    {
        int32_t got = (int32_t)half_read(u->in, buf, n);
        if (u->peer != NULL) {
            socket_wake(u->peer); /* room freed: a blocked writer may continue */
        }
        return got;
    }
}

static uint32_t unix_poll(struct sock *s) {
    struct unix_sock *u = un(s);
    uint32_t ready = 0;
    if (s->type == SOCK_DGRAM) {
        return (u->queue_count > 0 ? EPOLLIN : 0u) | EPOLLOUT;
    }
    if (un(s)->listening) {
        /* A listener is readable when it has something to accept, which is
         * what `epoll` on a server socket is waiting for. */
        return u->pending_count > 0 ? EPOLLIN : 0u;
    }
    if (u->in != NULL && (u->in->count > 0 || u->in->shut)) {
        /* Shut counts as readable: the read that follows returns 0, and a
         * poll that said otherwise would spin a program for ever at end of
         * file. */
        ready |= EPOLLIN;
    }
    if (u->out != NULL && !u->out->shut && u->out->count < STREAM_BYTES) {
        ready |= EPOLLOUT;
    }
    return ready;
}

static int unix_shutdown(struct sock *s, int how) {
    struct unix_sock *u = un(s);
    if (!s->connected) {
        return ENOTCONN;
    }
    /* `how` is 0 read, 1 write, 2 both. Shutting the *write* side marks the
     * half the peer reads, which is the end the peer will see. */
    if (how != 0 && u->out != NULL) {
        u->out->shut = true;
    }
    if (how != 1 && u->in != NULL) {
        u->in->shut = true;
    }
    if (u->peer != NULL) {
        socket_wake(u->peer);
    }
    return KERR_OK;
}

static void unix_release(struct sock *s) {
    struct unix_sock *u = un(s);
    if (s->bound) {
        unix_name_release(s->local.unix_name);
    }
    /* **The peer learns at once.** Marking the half it reads and waking it is
     * what turns a close into an end of file rather than a silence the peer
     * discovers on a timeout it may not have set. */
    if (u->peer != NULL) {
        struct unix_sock *p = un(u->peer);
        if (p->in != NULL) {
            p->in->shut = true;
        }
        p->peer = NULL;
        socket_wake(u->peer);
        /* The halves belong to the pair, and the peer is still reading one
         * of them, so they are freed by whichever end goes last. */
    } else {
        kfree(u->in);
        kfree(u->out);
    }
    /* A listener still holding unaccepted connections must not leave them
     * believing they are connected: each is released, which tells its
     * client. */
    for (uint32_t i = 0; i < u->pending_count; i++) {
        struct sock *waiting = u->pending[i];
        waiting->ops->release(waiting);
        kfree(waiting);
    }
    kfree(u->queue);
    kfree(u);
    s->proto = NULL;
}

static const struct sock_ops unix_ops = {
    .bind = unix_bind,
    .connect = unix_connect,
    .send = unix_send,
    .recv = unix_recv,
    .poll = unix_poll,
    .listen = unix_listen,
    .accept = unix_accept,
    .shutdown = unix_shutdown,
    .release = unix_release,
};

int unix_sock_init(struct sock *s, int type) {
    struct unix_sock *u;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        return EPROTONOSUPPORT;
    }
    u = kzalloc(sizeof *u);
    if (u == NULL) {
        return KERR_NOMEM;
    }
    if (type == SOCK_DGRAM) {
        u->queue = kzalloc(sizeof *u->queue * DGRAM_QUEUE);
        if (u->queue == NULL) {
            kfree(u);
            return KERR_NOMEM;
        }
    }
    s->proto = u;
    s->ops = &unix_ops;
    return KERR_OK;
}
