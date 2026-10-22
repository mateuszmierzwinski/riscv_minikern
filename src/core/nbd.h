/* nbd — a Network Block Device client: the volume is a file on a server and
 * the board reaches it over TCP.
 *
 * NBD rather than a protocol of our own, for one reason: everything else
 * already speaks it. `qemu-nbd` serves an image file, `nbdinfo` and
 * `nbdcopy` consume one, and Linux can mount what we wrote — so each end of
 * ours can be tested against a reference implementation instead of only
 * against the other end of ours.
 *
 * This is the "newstyle fixed" handshake with NBD_OPT_GO, and simple
 * replies in the transmission phase (structured replies are never
 * negotiated, so a server must not send them). The protocol is pure: it
 * speaks through a transport port, which is TCP in the kernel
 * (kernel/nbd.h) and a host socket or a fake in the tests.
 *
 * It trusts nothing the server says: every length, offset and reply magic
 * is checked against what was asked for, because a block device that lies
 * about where data belongs is a filesystem that corrupts itself.
 */
#ifndef MINIKERN_CORE_NBD_H
#define MINIKERN_CORE_NBD_H

#include <stdbool.h>
#include <stdint.h>

#include "core/blockdev.h"

/* The most one request moves. Servers may refuse more than 32 MiB; this is
 * well under that and bounds how long one round trip can take. */
#define NBD_MAX_IO 65536u

/* Transmission flags the server sends (NBD_FLAG_*), the ones we read. */
#define NBD_FLAG_READ_ONLY  0x0002u
#define NBD_FLAG_SEND_FLUSH 0x0004u

/* A connected byte stream. send delivers all n bytes; recv fills exactly n.
 * Both return KERR_OK or a negative KERR_*, and **the implementation owns
 * the timeout**: a server that stops mid-reply must not wedge the kernel,
 * so the kernel's transport has a deadline and the tests use one too. */
struct nbd_transport {
    int (*send)(void *self, const void *buf, uint32_t n);
    int (*recv)(void *self, void *buf, uint32_t n);
    void *self;
};

struct nbd {
    struct nbd_transport t;
    uint64_t size;        /* the export's size in bytes */
    uint32_t min_block;   /* smallest I/O the server will accept */
    uint32_t pref_block;  /* the size it would rather have */
    uint16_t flags;       /* NBD_FLAG_* from the server */
    uint64_t next_handle;
    uint32_t dev_block;   /* the block size nbd_blockdev was asked for */
    bool ready;           /* the handshake finished: transmission phase */
};

/* Runs the handshake over t and asks for the named export ("" is the
 * default one). KERR_OK, or KERR_PROTO when the server breaks the protocol,
 * KERR_NOENT when it does not have that export, KERR_NOTSUP when it will
 * not speak fixed-newstyle, or whatever the transport returned. An export
 * that reports no size at all is refused too: there is nothing to do with
 * a block device of zero blocks. */
int nbd_connect(struct nbd *n, const struct nbd_transport *t, const char *export_name);

/* Reads or writes len bytes at off, in NBD_MAX_IO-sized requests. A write
 * to a read-only export is KERR_ROFS; past the end of it, KERR_INVAL. A
 * zero-length transfer sends nothing and succeeds. */
int nbd_pread(struct nbd *n, uint64_t off, void *buf, uint32_t len);
int nbd_pwrite(struct nbd *n, uint64_t off, const void *buf, uint32_t len);

/* Makes everything written durable on the server. KERR_NOTSUP when the
 * export did not offer NBD_FLAG_SEND_FLUSH. */
int nbd_flush(struct nbd *n);

/* Says goodbye so the server can close down cleanly. The connection is
 * unusable afterwards. */
int nbd_disconnect(struct nbd *n);

/* Presents the export as a block device of block_size bytes (a power of
 * two, at least the server's minimum). The export's tail is dropped when it
 * is not a whole number of blocks. KERR_INVAL on a bad size. The blockdev
 * borrows n, which must outlive it. */
int nbd_blockdev(struct nbd *n, uint32_t block_size, struct blockdev *out);

#endif
