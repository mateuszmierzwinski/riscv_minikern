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
