/* sys_socket — the socket syscalls: socket, bind, connect, listen, accept(4),
 * getsockname, getpeername, sendto, recvfrom, sendmsg, recvmsg, shutdown,
 * setsockopt, getsockopt. Addresses cross the boundary as the socket's own
 * family's shape: sockaddr_in, sockaddr_in6, sockaddr_un or sockaddr_nl. */
#include "core/errno.h"
#include "kernel/uspace.h"
#include "kernel/socket.h"
#include "kernel/netlink.h"
#include "kernel/syscall.h"
#include "kernel/file.h"

#include <stddef.h>

#define SOCKADDR_IN_LEN 16u
#define SOCKADDR_IN6_LEN 28u
#define SOCKADDR_IN6_MIN 24u   /* RFC 2133's, without the scope id */
#define IPV6_V6ONLY     26u
/* The interface index a link-local address is scoped to: the real
 * interface's in the netlink dump (sock_netlink.c), so Go names the zone. */
#define ETH_SCOPE_ID    2u
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
 * for: carried on the new file into `fd_install`, which moves it onto the
 * descriptor (where it lives, so a dup'd copy does not inherit it). */
static int32_t install_descriptor(struct file *f, bool cloexec) {
    f->flags |= cloexec ? O_CLOEXEC : 0u;
    return (int32_t)fd_install(f);
}

/* `sockaddr_un`: the family, then up to 108 bytes of name.
 *
 * **The name's length comes from the caller's `socklen_t`, not from a NUL.**
 * An abstract name — the kind whose first byte is NUL, which is how a
 * program asks for a name with no filesystem entry — would otherwise measure
 * as zero bytes long, and every abstract socket in the system would have the
 * same empty name and collide with every other. So the length is
 * `len - 2` for an abstract name, and for a path it is the NUL-terminated
 * string, which is what a caller that passed `sizeof(struct sockaddr_un)`
 * expects.
 *
 * `create` distinguishes bind from connect: a connect must not intern a name
 * nobody has bound, or it would be handed a valid index for a listener that
 * does not exist and get ECONNREFUSED one layer too late. */
static int get_unix_addr(virt_t uaddr, uint32_t len, struct sock_addr *a, bool create) {
    char name[UNIX_NAME_MAX];
    uint32_t room;
    if (len < 3u || len > 2u + UNIX_NAME_MAX) {
        return KERR_INVAL;
    }
    room = len - 2u;
    if (uspace_copy_in(name, uaddr + 2u, room) != KERR_OK) {
        return KERR_FAULT;
    }
    if (name[0] != '\0') {
        /* A path: as long as its text, so that a caller passing the whole
         * structure and one passing the exact length agree. */
        uint32_t n = 0;
        while (n < room && name[n] != '\0') {
            n++;
        }
        room = n;
    }
    a->unix_name = unix_name_intern(name, room, create);
    if (a->unix_name == 0) {
        /* Either the table is full, or this is a connect to a name nobody
         * holds. The second is far more likely and is what a caller acts on. */
        return create ? KERR_NOSPC : ECONNREFUSED;
    }
    return KERR_OK;
}

/* `sockaddr_nl`: the family, two bytes of padding, a 32-bit port and a
 * 32-bit group mask. The port is the caller's own identity, which it chooses
 * — zero asks the kernel to pick, and the port is echoed back in every reply
 * so a caller can tell its own answers from another's. */
static int get_netlink_addr(virt_t uaddr, uint32_t len, struct sock_addr *a) {
    uint8_t b[12] = {0};
    if (len < 8u || len > 12u) {
        return KERR_INVAL;
    }
    if (uspace_copy_in(b, uaddr, len) != KERR_OK) {
        return KERR_FAULT;
    }
    if ((b[0] | (b[1] << 8)) != AF_NETLINK) {
        return EAFNOSUPPORT;
    }
    /* Host order, unlike a `sockaddr_in`'s port: netlink's fields are native. */
    a->port = (uint16_t)((uint32_t)b[4] | ((uint32_t)b[5] << 8));
    return KERR_OK;
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
    /* **Every field, not only the two an IPv4 address has.** `sendto`'s
     * address lives on the stack uninitialised, so a `v6` left as it was
     * sent an IPv4 datagram down the IPv6 path whenever the stack held a
     * nonzero byte there: `sendto` on an AF_INET UDP socket answered
     * ENETUNREACH, run after run, for a peer on the link. */
    *a = (struct sock_addr){0};
    if ((b[0] | (b[1] << 8)) == AF_UNSPEC) {
        return KERR_OK;
    }
    if ((b[0] | (b[1] << 8)) != AF_INET) {
        return EAFNOSUPPORT;
    }
    a->port = (uint16_t)((b[2] << 8) | b[3]);
    a->ip = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    return KERR_OK;
}

/* ::ffff:0:0/96, the prefix that marks an IPv4 address in an IPv6 one. */
static bool is_v4_mapped(const uint8_t *a) {
    for (uint32_t i = 0; i < 10u; i++) {
        if (a[i] != 0) {
            return false;
        }
    }
    return a[10] == 0xff && a[11] == 0xff;
}

static bool is_unspecified6(const uint8_t *a) {
    for (uint32_t i = 0; i < 16u; i++) {
        if (a[i] != 0) {
            return false;
        }
    }
    return true;
}

/* `sockaddr_in6`: family, port (network order), flow label, address, scope.
 *
 * **The mapping is undone here**, once, so nothing below has two spellings
 * of one IPv4 peer: ::ffff:a.b.c.d becomes the IPv4 address and :: becomes
 * 0, the wildcard of both families. Only a real IPv6 address sets `v6`. A
 * V6ONLY socket has no IPv4 side, so a mapped address is refused there. */
static int get_addr6(const struct sock *s, virt_t uaddr, uint32_t len, struct sock_addr *a) {
    uint8_t b[SOCKADDR_IN6_LEN] = {0};
    if (len < 2u || len > 128u) {
        return KERR_INVAL;
    }
    if (uspace_copy_in(b, uaddr, len < sizeof b ? len : sizeof b) != KERR_OK) {
        return KERR_FAULT;
    }
    *a = (struct sock_addr){0};
    if ((b[0] | (b[1] << 8)) == AF_UNSPEC) {
        return KERR_OK;
    }
    if ((b[0] | (b[1] << 8)) != AF_INET6) {
        return EAFNOSUPPORT;
    }
    if (len < SOCKADDR_IN6_MIN) {
        return KERR_INVAL;
    }
    a->port = (uint16_t)((b[2] << 8) | b[3]);
    if (is_v4_mapped(b + 8)) {
        a->ip = ((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23];
        return s->v6only ? KERR_INVAL : KERR_OK;
    }
    if (!is_unspecified6(b + 8)) {
        a->v6 = true;
        for (uint32_t i = 0; i < 16u; i++) {
            a->ip6[i] = b[8u + i];
        }
    }
    return KERR_OK;
}

/* Reads the address a call was given, in the shape the socket's own family
 * uses. **The socket decides, not the bytes**: every shape begins with a
 * family, and reading one as another produces an address that is nobody's
 * rather than an error. `create` is bind's (see get_unix_addr). */
static int get_sockaddr(const struct sock *s, virt_t uaddr, uint32_t len, struct sock_addr *a, bool create) {
    switch (s->domain) {
    case AF_UNIX:
        return get_unix_addr(uaddr, len, a, create);
    case AF_NETLINK:
        return get_netlink_addr(uaddr, len, a);
    case AF_INET6:
        return get_addr6(s, uaddr, len, a);
    default:
        return get_addr(uaddr, len, a);
    }
}

/* A `sockaddr_in6` for an endpoint: its IPv6 address, or an IPv4 one mapped
 * back (:: for the wildcard), which is how a dual-stack caller sees an IPv4
 * peer. A link-local address carries the interface's scope. */
static uint32_t fill_sockaddr_in6(uint8_t *b, struct sock_addr a) {
    b[0] = AF_INET6;
    b[2] = (uint8_t)(a.port >> 8);
    b[3] = (uint8_t)a.port;
    if (a.v6) {
        for (uint32_t i = 0; i < 16u; i++) {
            b[8u + i] = a.ip6[i];
        }
        b[24] = a.ip6[0] == 0xfe && (a.ip6[1] & 0xc0u) == 0x80u ? (uint8_t)ETH_SCOPE_ID : 0u;
    } else if (a.ip != 0) {
        b[18] = 0xff;
        b[19] = 0xff;
        b[20] = (uint8_t)(a.ip >> 24);
        b[21] = (uint8_t)(a.ip >> 16);
        b[22] = (uint8_t)(a.ip >> 8);
        b[23] = (uint8_t)a.ip;
    }
    return SOCKADDR_IN6_LEN;
}

/* Writes the endpoint to uaddr in the family's shape, honouring and
 * updating *ulen (socklen_t). */
/* A `sockaddr_un`: the family, then the path, NUL-terminated.
 *
 * **This existed only as `sockaddr_in` and it broke every Unix listener.**
 * `getsockname` wrote an `AF_INET` address whatever the socket was, so a
 * `net.Listen("unix", ...)` in Go — which asks for its own name after
 * binding, to fill the listener's `Addr` — got an IPv4 address full of
 * zeros, failed to parse it as a path, and dereferenced the nil that came
 * back. The listener that failed was QuokkaOS init's control socket, which
 * is how `rc` reaches it.
 *
 * The length reported is the family plus the path plus its terminator,
 * which is what Linux reports for a bound filesystem name and what tells a
 * caller how much of `sun_path` is real. Reporting the whole 110-byte array
 * would have a caller read 109 bytes of nothing as part of the name. */
static uint32_t fill_sockaddr_un(uint8_t *b, uint32_t room, struct sock_addr a) {
    uint32_t len = 0;
    const char *name = unix_name_at(a.unix_name, &len);
    uint32_t at = 2u;
    b[0] = AF_UNIX;
    b[1] = 0;
    if (name == NULL) {
        return 2u; /* bound to nothing: the family alone, as Linux does */
    }
    for (uint32_t i = 0; i < len && at + 1u < room; i++) {
        b[at++] = (uint8_t)name[i];
    }
    b[at++] = 0;
    return at;
}

#define SOCKADDR_UN_LEN 110u

static int put_addr(int domain, virt_t uaddr, virt_t ulen_addr, struct sock_addr a) {
    uint8_t b[SOCKADDR_UN_LEN] = {AF_INET, 0, (uint8_t)(a.port >> 8), (uint8_t)a.port, (uint8_t)(a.ip >> 24),
                                  (uint8_t)(a.ip >> 16), (uint8_t)(a.ip >> 8), (uint8_t)a.ip};
    uint32_t len, want = SOCKADDR_IN_LEN;
    /* **A unix address is told apart by its interned name**, which is
     * one-based precisely so that zero means "not a unix address" — so this
     * needs no extra field and leaves AF_INET untouched. */
    if (a.unix_name != 0) {
        for (uint32_t i = 0; i < sizeof b; i++) {
            b[i] = 0;
        }
        want = fill_sockaddr_un(b, sizeof b, a);
    } else if (domain == AF_INET6) {
        for (uint32_t i = 0; i < sizeof b; i++) {
            b[i] = 0;
        }
        want = fill_sockaddr_in6(b, a);
    }
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
    err = get_sockaddr(s, sysarg(tf, 1), sysarg(tf, 2), &a, !connect);
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
    (void)put_addr(c->domain, sysarg(tf, 1), sysarg(tf, 2), c->remote);
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
    return put_addr(s->domain, sysarg(tf, 1), sysarg(tf, 2), peer ? s->remote : s->local);
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
    struct sock_addr to = {0};
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL) {
        return err;
    }
    if (sysarg(tf, 4) == 0) {
        return send_user(s, sysarg(tf, 1), sysarg(tf, 2), NULL);
    }
    err = get_sockaddr(s, sysarg(tf, 4), sysarg(tf, 5), &to, false);
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
    if (r >= 0 &&
        put_addr(s->domain, sysarg(tf, 4), sysarg(tf, 5), s->type == SOCK_STREAM ? s->remote : from) != KERR_OK) {
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
    struct sock_addr to = {0};
    virt_t base = 0;
    uint64_t len = 0;
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL || (err = get_msghdr(sysarg(tf, 1), &m)) != KERR_OK) {
        return err;
    }
    if (m.name != 0 && (err = get_sockaddr(s, m.name, m.namelen, &to, false)) != KERR_OK) {
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
    (void)put_addr(s->domain, m.name, m.name != 0 ? sysarg(tf, 1) + MSGHDR_NAMELEN : 0, from);
    (void)uspace_copy_out(sysarg(tf, 1) + MSGHDR_CONTROLLEN, &zero, 8);
    (void)uspace_copy_out(sysarg(tf, 1) + MSGHDR_FLAGS, &zero, 4);
    return r;
}

static int64_t sys_shutdown(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    return s == NULL ? err : s->ops->shutdown(s, (int)sysarg(tf, 1));
}

/* The options that are not ignored: how long a blocking receive or send may
 * wait, and IPV6_V6ONLY. Everything else is accepted and has nothing to
 * tune; IPv6 options on an IPv4 socket say ENOPROTOOPT, as Linux does. */
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

/* IPV6_V6ONLY, which only means something before bind: after it the port
 * is held for both families or one, and changing that is EINVAL on Linux. */
static int32_t set_ipv6_option(struct sock *s, uint32_t name, virt_t uaddr) {
    uint32_t v;
    if (s->domain != AF_INET6) {
        return ENOPROTOOPT;
    }
    if (name != IPV6_V6ONLY) {
        return KERR_OK;
    }
    if (uspace_copy_in(&v, uaddr, 4) != KERR_OK) {
        return KERR_FAULT;
    }
    if (s->bound) {
        return KERR_INVAL;
    }
    s->v6only = v != 0;
    return KERR_OK;
}

static int64_t sys_setsockopt(struct trapframe *tf) {
    int err;
    struct sock *s = sock_fd((int32_t)sysarg(tf, 0), &err);
    if (s == NULL) {
        return err;
    }
    if (sysarg(tf, 1) == IPPROTO_IPV6) {
        return set_ipv6_option(s, sysarg(tf, 2), sysarg(tf, 3));
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
    if (level == IPPROTO_IPV6 && name == IPV6_V6ONLY && s->domain == AF_INET6) {
        return s->v6only ? 1u : 0u;
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
