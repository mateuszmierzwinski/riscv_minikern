/* socket — BSD sockets over the kernel's IPv4 stack, for Go's net package.
 *
 * A socket is a file whose ops forward to a per-protocol table
 * (struct sock_ops): UDP (sock_udp.c) and TCP (sock_tcp.c). Only AF_INET
 * exists; asking for AF_INET6 is EAFNOSUPPORT, which is how Go learns to
 * use IPv4 alone. Blocking calls sleep on the socket and wake on arrival,
 * a signal, or never (no SO_RCVTIMEO); non-blocking ones return EAGAIN and
 * readiness reaches epoll through vfs_poll_notify. */
#ifndef MINIKERN_KERNEL_SOCKET_H
#define MINIKERN_KERNEL_SOCKET_H

#include <stdbool.h>
#include <stdint.h>

#include "core/net/net.h"

/* **AF_UNIX, which addresses a name rather than a host.** The name is
 * interned in a kernel-side table and `sock_addr.unix_name` carries its
 * index, so the one `sock_ops` interface serves all three protocols and
 * neither UDP nor TCP has to learn what a path is. */
/* `SOCK_RAW` exists only for netlink here: every netlink caller asks for it
 * and nothing else in this kernel offers a raw socket. */
#define SOCK_RAW      3

#define AF_UNIX       1
#define AF_INET       2
#define AF_INET6      10
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define MSG_PEEK      0x2u
#define MSG_DONTWAIT  0x40u
#define EAFNOSUPPORT  (-97)
#define EADDRINUSE    (-98)
#define EADDRNOTAVAIL (-99)
#define ENETUNREACH   (-101)
#define ECONNRESET    (-104)
#define EISCONN       (-106)
#define ENOTCONN      (-107)
#define ECONNREFUSED  (-111)
#define EINPROGRESS   (-115)
#define EALREADY      (-114)
#define EPIPE         (-32)
#define ENOTSOCK      (-88)
#define EDESTADDRREQ  (-89)
#define EMSGSIZE      (-90)
#define EPROTONOSUPPORT (-93)
#define EOPNOTSUPP    (-95)
#define ENOPROTOOPT   (-92)

struct sock;
struct file;

/* An endpoint: address and port, host order. */
struct sock_addr {
    ip4_t ip;
    uint16_t port;
    /* **AF_UNIX only**: the index of an interned name, one-based so that zero
     * means "not a unix address". Inert for AF_INET, which never sets it, so
     * UDP and TCP are unchanged by its existence. */
    uint16_t unix_name;
};

struct sock_ops {
    int (*bind)(struct sock *s, struct sock_addr a);
    int (*connect)(struct sock *s, struct sock_addr a);
    /* to is NULL for a connected send. Returns bytes sent or -errno. */
    int32_t (*send)(struct sock *s, const void *buf, uint32_t n, const struct sock_addr *to);
    /* Fills *from when not NULL. May sleep unless nonblock. */
    int32_t (*recv)(struct sock *s, void *buf, uint32_t n, struct sock_addr *from, uint32_t flags, bool nonblock);
    uint32_t (*poll)(struct sock *s);
    int (*listen)(struct sock *s, int backlog);
    int (*accept)(struct sock *s, struct sock **out, bool nonblock);
    int (*shutdown)(struct sock *s, int how);
    void (*release)(struct sock *s);
};

struct sock {
    const struct sock_ops *ops;
    /* Which family this socket was created in, so the syscall layer knows
     * whether a `sockaddr` it is handed is a `sockaddr_in` or a
     * `sockaddr_un`. Those are different shapes, and reading one as the
     * other silently produces an address that is nobody's. */
    int domain;
    struct file *file;      /* the open file it belongs to (for readiness) */
    int type;
    struct sock_addr local;
    struct sock_addr remote;
    bool bound;
    bool connected;
    int error;              /* SO_ERROR: the last asynchronous failure */
    /* SO_RCVTIMEO and SO_SNDTIMEO, in milliseconds, 0 for "wait for ever".
     *
     * Go never sets these — it has its own deadlines — but anything else
     * does, and a receive with no timeout is how a program written in C
     * ends up hung on a socket that will never speak again. */
    uint32_t recv_timeout_ms;
    uint32_t send_timeout_ms;
    void *proto;            /* the protocol's own state */
};

/* How long a blocking operation on s may wait, as a deadline in monotonic
 * nanoseconds: 0 means for ever, which is what sched_sleep understands. */
uint64_t sock_deadline(const struct sock *s, bool sending);

/* ── AF_UNIX's name table ──────────────────────────────────────────────── */

/* The longest name a unix socket may have, which is `sockaddr_un`'s own
 * limit. An abstract name (one whose first byte is NUL) uses the same room. */
#define UNIX_NAME_MAX 108u
/* How many names may be bound at once. */
#define UNIX_NAMES 16u

/* Interns a name, returning its one-based index, or 0 when the table is full.
 * `create` is false for a lookup, which must not invent a name a listener
 * never bound — a connect to a name nobody holds is ECONNREFUSED, and a
 * table that interned on lookup would hand back a valid index for it. */
uint16_t unix_name_intern(const char *name, uint32_t len, bool create);

/* The name at an index, for reporting an address back to user space. */
const char *unix_name_at(uint16_t index, uint32_t *len);

/* Gives up a name, so that a bound socket's close frees it for rebinding. */
void unix_name_release(uint16_t index);

/* Fits a unix socket to s. */
int unix_sock_init(struct sock *s, int type);

/* Fits a netlink socket to s. */
int netlink_sock_init(struct sock *s, int type, int protocol);

/* Makes a socket file. KERR_OK with *out, or -errno. */
int socket_create(int domain, int type, int protocol, struct file **out);

/* The socket behind f, or NULL when f is not one. */
struct sock *socket_of(struct file *f);

/* A new file around an existing socket (accept). */
struct file *socket_file(struct sock *s, uint32_t flags);

/* Tells everyone waiting on s (blocking callers and epoll) to look again. */
void socket_wake(struct sock *s);

/* Protocol constructors: fill s->ops and s->proto. */
int sock_udp_init(struct sock *s);
int sock_tcp_init(struct sock *s);

/* The interface sockets run on. */
struct netif *socket_netif(void);

#endif
