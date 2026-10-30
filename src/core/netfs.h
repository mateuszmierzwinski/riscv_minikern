/* netfs — a directory on another machine, as a filesystem.
 *
 * The client of `src/go/tools/netfs`: each operation is one protobuf
 * `Request` answered by one `Response` (netfs.proto), framed on a byte
 * stream as `"MKFS" u32 | length u32 | message`. Pure: it speaks through a
 * transport port, which is TCP in the kernel (kernel/netfs_tcp.c) and a fake
 * in the tests.
 *
 * Nothing the server says is trusted: a frame's magic and length, every
 * field's length, and a read's byte count against what was asked are all
 * checked before anything is copied.
 */
#ifndef MINIKERN_CORE_NETFS_H
#define MINIKERN_CORE_NETFS_H

#include <stdbool.h>
#include <stdint.h>

#define NETFS_MAGIC 0x53464b4du /* "MKFS" */

/* The most one READ or WRITE moves, and the frame buffer that carries it
 * with room for the rest of the message. */
#define NETFS_IO 32768u
#define NETFS_FRAME (NETFS_IO + 2048u)

/* How a request reaches the server. `exchange` sends the encoded Request
 * in buf[0, req_len) and leaves the encoded Response in buf (at most cap
 * bytes) with its length — the framing is the transport's: "MKFS" frames
 * over TCP, or a gRPC call. It returns KERR_OK or a negative KERR_*, and the
 * implementation owns the timeout. lock/unlock make one exchange exclusive,
 * since a caller may sleep in it while another wants the connection; reset
 * drops the connection so the next exchange starts a new one. */
struct netfs_transport {
    int (*exchange)(void *self, uint8_t *buf, uint32_t req_len, uint32_t cap, uint32_t *resp_len);
    void (*lock)(void *self);
    void (*unlock)(void *self);
    void (*reset)(void *self);
    void *self;
};

struct netfs {
    struct netfs_transport t;
    uint8_t *frame; /* NETFS_FRAME bytes, the caller's */
    uint64_t calls;
    uint64_t failures; /* transport or protocol, not a server errno */
    uint64_t retries;  /* exchanges repeated on a fresh stream after one of those */
};

/* A file's status, as the server reports it. */
struct netfs_stat {
    uint32_t mode;
    uint32_t nlink;
    uint64_t size;
    int64_t mtime;
    uint32_t uid;
    uint32_t gid;
};

/* One directory entry; `name` is not NUL-terminated. */
typedef int (*netfs_entry_fn)(void *self, const char *name, uint32_t name_len, uint32_t mode);

void netfs_init(struct netfs *nf, const struct netfs_transport *t, uint8_t *frame);

/* Each returns KERR_OK or a negative errno — the server's (ENOENT for a
 * missing file) or the transport's, or KERR_PROTO for a broken reply. */
int netfs_stat(struct netfs *nf, const char *path, struct netfs_stat *out);
/* A symbolic link's target, without a terminator, cut to `room` bytes;
 * KERR_INVAL for a path that is not a link. */
int netfs_readlink(struct netfs *nf, const char *path, char *out, uint32_t room, uint32_t *got);

int netfs_read(struct netfs *nf, const char *path, uint64_t off, void *buf, uint32_t len,
               uint32_t *got);
int netfs_write(struct netfs *nf, const char *path, uint64_t off, const void *buf, uint32_t len,
                uint32_t *wrote);
int netfs_list(struct netfs *nf, const char *path, netfs_entry_fn fn, void *self);
int netfs_mkdir(struct netfs *nf, const char *path);
int netfs_unlink(struct netfs *nf, const char *path);
int netfs_truncate(struct netfs *nf, const char *path, uint64_t size);
int netfs_rename(struct netfs *nf, const char *from, const char *to);

#endif
