#include "kernel/socket.h"
#include "kernel/netlink.h"

#include "arch/riscv/timer.h"

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/sched.h"
#include "kernel/file.h"

static int32_t sock_read(struct file *f, void *buf, uint32_t n) {
    struct sock *s = f->priv;
    return s->ops->recv(s, buf, n, NULL, 0, (f->flags & O_NONBLOCK) != 0);
}

static int32_t sock_write(struct file *f, const void *buf, uint32_t n) {
    struct sock *s = f->priv;
    return s->ops->send(s, buf, n, NULL);
}

static uint32_t sock_poll(struct file *f) {
    struct sock *s = f->priv;
    return s->ops->poll(s);
}

static void sock_release(struct file *f) {
    struct sock *s = f->priv;
    s->ops->release(s);
    kfree(s);
}

static const struct file_ops socket_ops = {
    .read = sock_read, .write = sock_write, .poll = sock_poll, .release = sock_release,
};

struct sock *socket_of(struct file *f) {
    return f != NULL && f->ops == &socket_ops ? f->priv : NULL;
}

struct file *socket_file(struct sock *s, uint32_t flags) {
    /* No mode and no inode: this kernel's files are objects with
     * operations, not entries in a filesystem, so there is nothing for a
     * socket to claim to be. */
    struct file *f = file_new(&socket_ops, NULL, 0);
    if (f != NULL) {
        f->priv = s;
        f->flags = O_RDWR | (flags & O_NONBLOCK);
        s->file = f;
    }
    return f;
}

static int protocol_init(struct sock *s, int type, int protocol) {
    /* **AF_UNIX takes no protocol number.** There is one transport for a
     * local socket, so anything but 0 is a caller asking for something that
     * does not exist rather than for a default. */
    if (s->domain == AF_UNIX) {
        return protocol == 0 ? unix_sock_init(s, type) : EPROTONOSUPPORT;
    }
    if (s->domain == AF_NETLINK) {
        return netlink_sock_init(s, type, protocol);
    }
    if (type == SOCK_DGRAM && (protocol == 0 || protocol == 17)) {
        return sock_udp_init(s);
    }
    if (type == SOCK_STREAM && (protocol == 0 || protocol == 6)) {
        return sock_tcp_init(s);
    }
    return EPROTONOSUPPORT;
}

int socket_create(int domain, int type, int protocol, struct file **out) {
    struct sock *s;
    int rc;
    if (domain != AF_INET && domain != AF_UNIX && domain != AF_NETLINK) {
        return EAFNOSUPPORT;
    }
    s = kzalloc(sizeof *s);
    if (s == NULL) {
        return KERR_NOMEM;
    }
    s->domain = domain;
    s->type = type & SOCK_TYPE_MASK;
    rc = protocol_init(s, s->type, protocol);
    if (rc == KERR_OK && (*out = socket_file(s, (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0u)) == NULL) {
        s->ops->release(s);
        rc = KERR_NOMEM;
    }
    if (rc != KERR_OK) {
        kfree(s);
    }
    return rc;
}

/* Wakes both kinds of waiter: a thread blocked in this socket's own call,
 * and **every `epoll_pwait`**, which sleeps on the poller key.
 *
 * The second was missing. A pipe or an eventfd woke the key; a socket only
 * bumped the readiness count, which a sleeping poller reads when it wakes —
 * at its deadline. A Go runtime with no timer pending asks for "for ever",
 * which this kernel cuts to 10 ms, so a small daemon looked fine; one with
 * a timer a second away (Gin's, the blog's cleanups) slept the whole second
 * before seeing a connection. Requests took 0.99, 1.99, 2.99 s — the
 * deadline, every time — and once 21 s. */
void socket_wake(struct sock *s) {
    (void)sched_wake_on(s, SCHED_MAX_THREADS);
    file_readiness_changed(s->file);
    (void)sched_futex_wake(SCHED_POLLER_KEY, 64u);
}

struct netif *socket_netif(void) {
    return knet_interface();
}

uint64_t sock_deadline(const struct sock *s, bool sending) {
    uint32_t ms = sending ? s->send_timeout_ms : s->recv_timeout_ms;
    return ms == 0 ? 0 : timer_ns() + (uint64_t)ms * 1000000ull;
}
