/* sys_socket — the socket syscalls: socket, bind, connect, listen, accept(4),
 * getsockname, getpeername, sendto, recvfrom, sendmsg, recvmsg, shutdown,
 * setsockopt, getsockopt. Addresses cross the boundary as sockaddr_in. */
#include "core/errno.h"
#include "kernel/uspace.h"
#include "kernel/socket.h"
#include "kernel/syscall.h"
#include "kernel/file.h"

#include <stddef.h>

#define SOCKADDR_IN_LEN 16u
#define AF_UNSPEC       0
#define SOL_SOCKET      1u
#define SO_TYPE         3u
#define SO_ERROR        4u
#define SO_SNDBUF       7u
#define SO_RCVBUF       8u
#define SO_RCVTIMEO     20u
#define SO_SNDTIMEO     21u

/* A timeval as setsockopt takes it. */
struct user_timeval {
    uint32_t sec;
    uint32_t usec;
};
#define SO_ACCEPTCONN   30u
#define IPPROTO_IPV6    41u
#define SOCK_BUF_SIZE   65536u
#define MSGHDR_LEN      56u
#define BOUNCE          2048u
#define IOV_LIMIT       64u

/* Installing a descriptor, with the close-on-exec flag the caller asked
 * for. The ARM kernel had this in its file-descriptor syscalls; here it is a
 * two-line adapter onto `fd_install`, because this kernel's flag lives on
 * the file rather than beside the descriptor. */
static int32_t install_descriptor(struct file *f, bool cloexec) {
    f->flags |= cloexec ? O_CLOEXEC : 0u;
    return (int32_t)fd_install(f);
}

/* sockaddr_in: family (host order), port and address (network order). */
static int get_addr(virt_t uaddr, uint32_t len, struct sock_addr *a) {
    uint8_t b[SOCKADDR_IN_LEN];
    if (len < 8u || len > 128u) {
        return KERR_INVAL;
    }
    if (uspace_copy_in(b, uaddr, len < sizeof b ? len : sizeof b) != KERR_OK) {
        return KERR_FAULT;
    }
    if ((b[0] | (b[1] << 8)) == AF_UNSPEC) {
        *a = (struct sock_addr){0};
        return KERR_OK;
    }
    if ((b[0] | (b[1] << 8)) != AF_INET) {
        return EAFNOSUPPORT;
    }
    a->port = (uint16_t)((b[2] << 8) | b[3]);
    a->ip = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    return KERR_OK;
}

/* Writes a sockaddr_in to uaddr, honouring and updating *ulen (socklen_t). */
static int put_addr(virt_t uaddr, virt_t ulen_addr, struct sock_addr a) {
    uint8_t b[SOCKADDR_IN_LEN] = {AF_INET, 0, (uint8_t)(a.port >> 8), (uint8_t)a.port, (uint8_t)(a.ip >> 24),
                                  (uint8_t)(a.ip >> 16), (uint8_t)(a.ip >> 8), (uint8_t)a.ip};
    uint32_t len, want = SOCKADDR_IN_LEN;
    if (uaddr == 0 || ulen_addr == 0) {
        return KERR_OK;
    }
    if (uspace_copy_in(&len, ulen_addr, 4) != KERR_OK || (int32_t)len < 0) {
        return KERR_FAULT;
    }
    if (uspace_copy_out(uaddr, b, len < want ? len : want) != KERR_OK) {
        return KERR_FAULT;
    }
    return uspace_copy_out(ulen_addr, &want, 4);
}

static struct sock *sock_fd(int32_t fd, int *err) {
    struct file *f = fd_get(fd);
    struct sock *s = socket_of(f);
    *err = f == NULL ? KERR_BADF : (s == NULL ? ENOTSOCK : KERR_OK);
    return s;
}

static int64_t sys_socket(struct trapframe *tf) {
    struct file *f;
    int32_t fd;
    int rc = socket_create((int)sysarg(tf, 0), (int)sysarg(tf, 1), (int)sysarg(tf, 2), &f);
    if (rc != KERR_OK) {
        return rc;
    }
    fd = install_descriptor(f, (sysarg(tf, 1) & SOCK_CLOEXEC) != 0);
    if (fd < 0) {
        file_put(f);
    }
    return fd;
}

/* bind and connect share a shape: a socket and an address. */
static int32_t with_addr(struct trapframe *tf, bool connect) {
    struct sock_addr a = {0};
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL) {
        return err;
    }
    err = get_addr(sysarg(tf, 1), sysarg(tf, 2), &a);
    if (err != KERR_OK) {
        return err;
    }
    return connect ? s->ops->connect(s, a) : s->ops->bind(s, a);
}

static int64_t sys_bind(struct trapframe *tf) {
    return with_addr(tf, false);
}

static int64_t sys_connect(struct trapframe *tf) {
    return with_addr(tf, true);
}

static int64_t sys_listen(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    return s == NULL ? err : s->ops->listen(s, (int)sysarg(tf, 1));
}

/* accept4(fd, addr, addrlen, flags). */
static int64_t sys_accept4(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err), *c;
    struct file *f;
    int32_t fd;
    if (s == NULL) {
        return err;
    }
    err = s->ops->accept(s, &c, (s->file->flags & O_NONBLOCK) != 0);
    if (err != KERR_OK) {
        return err;
    }
    f = socket_file(c, (sysarg(tf, 3) & SOCK_NONBLOCK) ? O_NONBLOCK : 0u);
    if (f == NULL) {
        c->ops->release(c);
        return KERR_NOMEM;
    }
    fd = install_descriptor(f, (sysarg(tf, 3) & SOCK_CLOEXEC) != 0);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    (void)put_addr(sysarg(tf, 1), sysarg(tf, 2), c->remote);
    return fd;
}

/* `accept` is `accept4` with no flags. On this architecture both numbers
 * exist and mean that; on the one this came from `accept4` was a late
 * addition stranded at 366. The copy sets the flags argument to zero —
 * `a3`, which is `x13`. */
static int64_t sys_accept(struct trapframe *tf) {
    struct trapframe t = *tf;
    t.x[13] = 0;
    return sys_accept4(&t);
}

static int32_t name_of(struct trapframe *tf, bool peer) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL) {
        return err;
    }
    if (peer && !s->connected) {
        return ENOTCONN;
    }
    return put_addr(sysarg(tf, 1), sysarg(tf, 2), peer ? s->remote : s->local);
}

static int64_t sys_getsockname(struct trapframe *tf) {
    return name_of(tf, false);
}

static int64_t sys_getpeername(struct trapframe *tf) {
    return name_of(tf, true);
}

/* One send of n bytes from user memory, optionally to an address. */
static int32_t send_user(struct sock *s, virt_t ubuf, uint32_t n, const struct sock_addr *to) {
    static uint8_t bounce[NET_UDP_MAX + 64u];
    uint32_t done = 0;
    if (s->type == SOCK_DGRAM && n > NET_UDP_MAX) {
        return EMSGSIZE;
    }
    while (done < n || (n == 0 && done == 0)) {
        uint32_t c = n - done < sizeof bounce ? n - done : (uint32_t)sizeof bounce;
        int32_t r;
        if (uspace_copy_in(bounce, ubuf + done, c) != KERR_OK) {
            return done > 0 ? (int32_t)done : KERR_FAULT;
        }
        r = s->ops->send(s, bounce, c, to);
        if (r < 0) {
            return done > 0 ? (int32_t)done : r;
        }
        done += (uint32_t)r;
        if (n == 0 || (uint32_t)r < c) {
            break;
        }
    }
    return (int32_t)done;
}

/* One receive into user memory (a datagram, or what a stream has). */
static int32_t recv_user(struct sock *s, virt_t ubuf, uint32_t n, struct sock_addr *from, uint32_t flags) {
    static uint8_t bounce[BOUNCE];
    uint32_t want = n < sizeof bounce ? n : (uint32_t)sizeof bounce;
    int32_t r = s->ops->recv(s, bounce, want, from, flags, (s->file->flags & O_NONBLOCK) != 0);
    if (r > 0 && uspace_copy_out(ubuf, bounce, (uint32_t)r) != KERR_OK) {
        return KERR_FAULT;
    }
    return r;
}

/* sendto(fd, buf, len, flags, addr, addrlen). */
static int64_t sys_sendto(struct trapframe *tf) {
    struct sock_addr to;
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL) {
        return err;
    }
    if (sysarg(tf, 4) == 0) {
        return send_user(s, sysarg(tf, 1), sysarg(tf, 2), NULL);
    }
    err = get_addr(sysarg(tf, 4), sysarg(tf, 5), &to);
    return err != KERR_OK ? err : send_user(s, sysarg(tf, 1), sysarg(tf, 2), &to);
}

/* recvfrom(fd, buf, len, flags, addr, addrlen). */
static int64_t sys_recvfrom(struct trapframe *tf) {
    struct sock_addr from = {0};
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    int32_t r;
    if (s == NULL) {
        return err;
    }
    r = recv_user(s, sysarg(tf, 1), sysarg(tf, 2), &from, sysarg(tf, 3));
    if (r >= 0 && put_addr(sysarg(tf, 4), sysarg(tf, 5), s->type == SOCK_STREAM ? s->remote : from) != KERR_OK) {
        return KERR_FAULT;
    }
    return r;
}

/* `struct msghdr`, which is **not the 32-bit one with wider fields**: on
 * LP64 the pointers are 8 bytes, `msg_namelen` is still 4 and is followed by
 * 4 bytes of padding, and `msg_iovlen` and `msg_controllen` are 8 rather
 * than 4. 56 bytes in all against 28, and every field after the first at a
 * different offset. The 32-bit layout read `msg_iov` out of the middle of
 * `msg_name`.
 *
 * It is read field by field at explicit offsets rather than by copying a
 * struct, because the padding is the part that is easy to get wrong and a
 * named offset is checkable against the ABI. */
#define MSGHDR_NAME 0u
#define MSGHDR_NAMELEN 8u
#define MSGHDR_IOV 16u
#define MSGHDR_IOVLEN 24u
#define MSGHDR_CONTROL 32u
#define MSGHDR_CONTROLLEN 40u
#define MSGHDR_FLAGS 48u

struct user_msghdr {
    virt_t name;
    uint32_t namelen;
    virt_t iov;
    uint64_t iovlen;
    virt_t control;
    uint64_t controllen;
    uint32_t flags;
};

/* One `struct iovec`: a pointer and a length, **16 bytes** here against 8. */
#define IOVEC_BYTES 16u

static int get_msghdr(virt_t uaddr, struct user_msghdr *m) {
    uint8_t raw[MSGHDR_LEN];
    if (uspace_copy_in(raw, uaddr, MSGHDR_LEN) != KERR_OK) {
        return KERR_FAULT;
    }
    m->name = *(const uint64_t *)&raw[MSGHDR_NAME];
    m->namelen = *(const uint32_t *)&raw[MSGHDR_NAMELEN];
    m->iov = *(const uint64_t *)&raw[MSGHDR_IOV];
    m->iovlen = *(const uint64_t *)&raw[MSGHDR_IOVLEN];
    m->control = *(const uint64_t *)&raw[MSGHDR_CONTROL];
    m->controllen = *(const uint64_t *)&raw[MSGHDR_CONTROLLEN];
    m->flags = *(const uint32_t *)&raw[MSGHDR_FLAGS];
    return m->iovlen > IOV_LIMIT ? KERR_INVAL : KERR_OK;
}

/* The first iovec's base and length. A datagram socket here sends and
 * receives one buffer, which is what Go's single-buffer calls pass. */
static int get_first_iovec(const struct user_msghdr *m, virt_t *base, uint64_t *len) {
    uint8_t raw[IOVEC_BYTES];
    if (m->iovlen == 0) {
        return KERR_INVAL;
    }
    if (uspace_copy_in(raw, m->iov, IOVEC_BYTES) != KERR_OK) {
        return KERR_FAULT;
    }
    *base = *(const uint64_t *)&raw[0];
    *len = *(const uint64_t *)&raw[8];
    return KERR_OK;
}

/* `sendmsg`: the first non-empty buffer, which is what Go's `WriteMsgUDP`
 * passes and all a datagram socket here needs. */
static int64_t sys_sendmsg(struct trapframe *tf) {
    struct user_msghdr m;
    struct sock_addr to;
    virt_t base = 0;
    uint64_t len = 0;
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL || (err = get_msghdr(sysarg(tf, 1), &m)) != KERR_OK) {
        return err;
    }
    if (m.name != 0 && (err = get_addr(m.name, m.namelen, &to)) != KERR_OK) {
        return err;
    }
    if ((err = get_first_iovec(&m, &base, &len)) != KERR_OK) {
        return err;
    }
    return send_user(s, base, (uint32_t)len, m.name != 0 ? &to : NULL);
}

/* recvmsg: into the first iovec; no control messages. */
static int64_t sys_recvmsg(struct trapframe *tf) {
    struct user_msghdr m;
    struct sock_addr from = {0};
    virt_t base = 0;
    uint64_t len = 0, zero = 0;
    int err;
    int32_t r;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL || (err = get_msghdr(sysarg(tf, 1), &m)) != KERR_OK) {
        return err;
    }
    if ((err = get_first_iovec(&m, &base, &len)) != KERR_OK) {
        return err;
    }
    r = recv_user(s, base, (uint32_t)len, &from, (uint32_t)sysarg(tf, 2));
    if (r < 0) {
        return r;
    }
    /* The sender's address, and the two fields a caller reads afterwards.
     * The offsets are the LP64 ones: `msg_namelen` at 8, `msg_controllen` at
     * 40, `msg_flags` at 48 — not 4, 20 and 24. */
    (void)put_addr(m.name, m.name != 0 ? sysarg(tf, 1) + MSGHDR_NAMELEN : 0, from);
    (void)uspace_copy_out(sysarg(tf, 1) + MSGHDR_CONTROLLEN, &zero, 8);
    (void)uspace_copy_out(sysarg(tf, 1) + MSGHDR_FLAGS, &zero, 4);
    return r;
}

static int64_t sys_shutdown(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    return s == NULL ? err : s->ops->shutdown(s, (int)sysarg(tf, 1));
}

/* The two options that are not ignored: how long a blocking receive or send
 * may wait. Everything else is accepted and has nothing to tune; IPv6
 * options say ENOPROTOOPT, as on a kernel without IPv6. */
static int32_t set_timeout(struct sock *s, uint32_t name, virt_t uaddr) {
    struct user_timeval tv;
    uint32_t ms;
    if (uspace_copy_in(&tv, uaddr, sizeof tv) != KERR_OK) {
        return KERR_FAULT;
    }
    /* A zero timeval means "wait for ever", which is what 0 means inside
     * (socket.h), so the two agree without a special case. */
    ms = tv.sec * 1000u + (tv.usec + 999u) / 1000u;
    if (name == SO_RCVTIMEO) {
        s->recv_timeout_ms = ms;
    } else {
        s->send_timeout_ms = ms;
    }
    return KERR_OK;
}

static int64_t sys_setsockopt(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL) {
        return err;
    }
    if (sysarg(tf, 1) == IPPROTO_IPV6) {
        return ENOPROTOOPT;
    }
    if (sysarg(tf, 1) == SOL_SOCKET && (sysarg(tf, 2) == SO_RCVTIMEO || sysarg(tf, 2) == SO_SNDTIMEO)) {
        return set_timeout(s, sysarg(tf, 2), sysarg(tf, 3));
    }
    return KERR_OK;
}

static uint32_t option_value(struct sock *s, uint32_t level, uint32_t name, int *err) {
    *err = KERR_OK;
    if (level == SOL_SOCKET && name == SO_ERROR) {
        uint32_t e = (uint32_t)-s->error;
        s->error = 0;
        return e;
    }
    if (level == SOL_SOCKET && name == SO_TYPE) {
        return (uint32_t)s->type;
    }
    if (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF)) {
        return SOCK_BUF_SIZE;
    }
    if (level == SOL_SOCKET && name == SO_ACCEPTCONN) {
        return 0;
    }
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
        /* Reported in milliseconds rather than as a timeval, which is a
         * deliberate narrowing: no caller here reads it back, and the
         * alternative is a second shape for one number. */
        return name == SO_RCVTIMEO ? s->recv_timeout_ms : s->send_timeout_ms;
    }
    *err = ENOPROTOOPT;
    return 0;
}

/* getsockopt(fd, level, name, val, len). */
static int64_t sys_getsockopt(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    uint32_t v, four = 4;
    if (s == NULL) {
        return err;
    }
    v = option_value(s, sysarg(tf, 1), sysarg(tf, 2), &err);
    if (err != KERR_OK) {
        return err;
    }
    if (uspace_copy_out(sysarg(tf, 3), &v, 4) != KERR_OK ||
        uspace_copy_out(sysarg(tf, 4), &four, 4) != KERR_OK) {
        return KERR_FAULT;
    }
    return KERR_OK;
}

/* The **generic** numbers, which is the whole difference from the
 * architecture this came from: there, sockets were 281..297 with `accept4`
 * stranded at 366, because they were added to a 32-bit ABI one at a time
 * over a decade. Here they are a contiguous block from 198, and there is no
 * `socketcall` multiplexer and no `accept` separate from `accept4` — the
 * numbering is the only thing that had to change, which is the point of
 * having carried the logic across unaltered. */
const struct syscall_entry sys_socket_table[] = {
    {198, "socket", sys_socket},
    {200, "bind", sys_bind},
    {201, "listen", sys_listen},
    {202, "accept", sys_accept},
    {203, "connect", sys_connect},
    {204, "getsockname", sys_getsockname},
    {205, "getpeername", sys_getpeername},
    {206, "sendto", sys_sendto},
    {207, "recvfrom", sys_recvfrom},
    {208, "setsockopt", sys_setsockopt},
    {209, "getsockopt", sys_getsockopt},
    {210, "shutdown", sys_shutdown},
    {211, "sendmsg", sys_sendmsg},
    {212, "recvmsg", sys_recvmsg},
    {242, "accept4", sys_accept4},
    {0, NULL, NULL},
};
