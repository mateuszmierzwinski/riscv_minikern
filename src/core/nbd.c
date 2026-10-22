#include "core/nbd.h"

#include <stddef.h>

#include "core/errno.h"

/* Wire constants, from the NBD protocol document. They are an ABI. */
#define NBD_MAGIC         0x4e42444d41474943ull  /* "NBDMAGIC" */
#define NBD_OPT_MAGIC     0x49484156454f5054ull  /* "IHAVEOPT" */
#define NBD_REP_MAGIC     0x0003e889045565a9ull
#define NBD_REQUEST_MAGIC 0x25609513u
#define NBD_REPLY_MAGIC   0x67446698u

#define NBD_FLAG_FIXED_NEWSTYLE 0x0001u
#define NBD_FLAG_NO_ZEROES      0x0002u
#define NBD_FLAG_C_FIXED_NEWSTYLE 0x00000001u
#define NBD_FLAG_C_NO_ZEROES      0x00000002u

#define NBD_OPT_GO 7u

#define NBD_REP_ACK  1u
#define NBD_REP_INFO 3u
#define NBD_REP_ERR  0x80000000u
#define NBD_REP_ERR_UNSUP   0x80000001u
#define NBD_REP_ERR_UNKNOWN 0x80000006u

#define NBD_INFO_EXPORT     0u
#define NBD_INFO_BLOCK_SIZE 3u

#define NBD_CMD_READ  0u
#define NBD_CMD_WRITE 1u
#define NBD_CMD_DISC  2u
#define NBD_CMD_FLUSH 3u

#define NBD_REQUEST_BYTES 28u
#define NBD_REPLY_BYTES   16u
#define NBD_OPT_REPLY_BYTES 20u

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

static uint64_t get64(const uint8_t *p) {
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static uint32_t string_len(const char *s) {
    uint32_t n = 0;
    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static int send_all(struct nbd *n, const void *buf, uint32_t len) {
    return n->t.send(n->t.self, buf, len);
}

static int recv_all(struct nbd *n, void *buf, uint32_t len) {
    return n->t.recv(n->t.self, buf, len);
}

/* Reads and throws away len bytes, so one unknown reply does not desync the
 * stream. A hostile length is refused rather than looped over. */
static int skip(struct nbd *n, uint32_t len) {
    uint8_t sink[64];
    if (len > 4096u) {
        return KERR_PROTO;
    }
    while (len > 0) {
        uint32_t chunk = len < sizeof sink ? len : (uint32_t)sizeof sink;
        int rc = recv_all(n, sink, chunk);
        if (rc != KERR_OK) {
            return rc;
        }
        len -= chunk;
    }
    return KERR_OK;
}

/* The server's greeting: magic, IHAVEOPT, its handshake flags. */
static int read_greeting(struct nbd *n, uint16_t *flags) {
    uint8_t hello[18];
    int rc = recv_all(n, hello, sizeof hello);
    if (rc != KERR_OK) {
        return rc;
    }
    if (get64(hello) != NBD_MAGIC || get64(hello + 8) != NBD_OPT_MAGIC) {
        return KERR_PROTO;
    }
    *flags = get16(hello + 16);
    return KERR_OK;
}

/* NBD_OPT_GO <name>: ask to go to transmission on that export. */
static int send_opt_go(struct nbd *n, const char *export_name) {
    uint32_t name_len = string_len(export_name);
    uint8_t head[20];
    int rc;
    if (name_len > 256u) {
        return KERR_NAMETOOLONG;
    }
    put64(head, NBD_OPT_MAGIC);
    put32(head + 8, NBD_OPT_GO);
    put32(head + 12, name_len + 6u);  /* name length + the name + 2-byte request count */
    put32(head + 16, name_len);
    rc = send_all(n, head, sizeof head);
    if (rc == KERR_OK && name_len > 0) {
        rc = send_all(n, export_name, name_len);
    }
    if (rc == KERR_OK) {
        uint8_t nrequests[2] = {0, 0};  /* no NBD_INFO_* requested: send what you like */
        rc = send_all(n, nrequests, sizeof nrequests);
    }
    return rc;
}

/* NBD_INFO_EXPORT carries the size and the transmission flags; the block
 * size reply carries the minimum and preferred I/O. Anything else is
 * skipped: a server may send information we have no use for. */
static int take_info(struct nbd *n, const uint8_t *body, uint32_t len) {
    uint16_t type = len >= 2u ? get16(body) : 0xffffu;
    if (type == NBD_INFO_EXPORT && len >= 12u) {
        n->size = get64(body + 2);
        n->flags = get16(body + 10);
        return KERR_OK;
    }
    if (type == NBD_INFO_BLOCK_SIZE && len >= 14u) {
        n->min_block = get32(body + 2);
        n->pref_block = get32(body + 6);
    }
    return KERR_OK;
}

/* One option reply. Returns KERR_OK on NBD_REP_INFO (more follow),
 * KERR_AGAIN on NBD_REP_ACK (the handshake is done) or an error. */
static int read_opt_reply(struct nbd *n) {
    uint8_t head[NBD_OPT_REPLY_BYTES];
    uint8_t body[32];
    uint32_t type, len;
    int rc = recv_all(n, head, sizeof head);
    if (rc != KERR_OK) {
        return rc;
    }
    type = get32(head + 12);
    len = get32(head + 16);
    if (get64(head) != NBD_REP_MAGIC || get32(head + 8) != NBD_OPT_GO) {
        return KERR_PROTO;
    }
    if (type == NBD_REP_ACK) {
        return len == 0 ? KERR_AGAIN : KERR_PROTO;
    }
    if ((type & NBD_REP_ERR) != 0) {
        (void)skip(n, len);
        return type == NBD_REP_ERR_UNKNOWN ? KERR_NOENT : KERR_NOTSUP;
    }
    if (type != NBD_REP_INFO || len > sizeof body) {
        (void)skip(n, len);
        return KERR_PROTO;
    }
    rc = recv_all(n, body, len);
    return rc == KERR_OK ? take_info(n, body, len) : rc;
}

/* Replies to NBD_OPT_GO until the ACK that ends the handshake. */
static int finish_handshake(struct nbd *n) {
    for (uint32_t i = 0; i < 16u; i++) {
        int rc = read_opt_reply(n);
        if (rc == KERR_AGAIN) {
            return n->size == 0 ? KERR_PROTO : KERR_OK;
        }
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_PROTO;  /* an endless stream of information is not an answer */
}

int nbd_connect(struct nbd *n, const struct nbd_transport *t, const char *export_name) {
    uint8_t client_flags[4];
    uint16_t server_flags = 0;
    int rc;
    *n = (struct nbd){.t = *t, .min_block = 512u, .pref_block = 4096u};
    rc = read_greeting(n, &server_flags);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((server_flags & NBD_FLAG_FIXED_NEWSTYLE) == 0) {
        return KERR_NOTSUP;
    }
    put32(client_flags, NBD_FLAG_C_FIXED_NEWSTYLE | NBD_FLAG_C_NO_ZEROES);
    rc = send_all(n, client_flags, sizeof client_flags);
    if (rc == KERR_OK) {
        rc = send_opt_go(n, export_name);
    }
    if (rc == KERR_OK) {
        rc = finish_handshake(n);
    }
    n->ready = rc == KERR_OK;
    return rc;
}

static int send_request(struct nbd *n, uint16_t cmd, uint64_t off, uint32_t len, uint64_t *handle) {
    uint8_t req[NBD_REQUEST_BYTES];
    *handle = ++n->next_handle;
    put32(req, NBD_REQUEST_MAGIC);
    put16(req + 4, 0);  /* no command flags: no FUA, no DF */
    put16(req + 6, cmd);
    put64(req + 8, *handle);
    put64(req + 16, off);
    put32(req + 24, len);
    return send_all(n, req, sizeof req);
}

/* The server's error numbers are its own; a failed command is reported as
 * KERR_IO unless it named a bad argument. */
static int reply_error(uint32_t err) {
    return err == 22u ? KERR_INVAL : err == 28u ? KERR_NOSPC : err == 30u ? KERR_ROFS : KERR_IO;
}

/* Waits for the reply to handle. A reply for anything else means the stream
 * is out of step — with one request in flight at a time there is nothing to
 * resynchronise to, so the connection is finished. */
static int read_reply(struct nbd *n, uint64_t handle) {
    uint8_t rep[NBD_REPLY_BYTES];
    uint32_t err;
    int rc = recv_all(n, rep, sizeof rep);
    if (rc != KERR_OK) {
        return rc;
    }
    if (get32(rep) != NBD_REPLY_MAGIC || get64(rep + 8) != handle) {
        n->ready = false;
        return KERR_PROTO;
    }
    err = get32(rep + 4);
    return err == 0 ? KERR_OK : reply_error(err);
}

/* True when [off, off+len) is inside the export and len is sane. */
static bool io_ok(const struct nbd *n, uint64_t off, uint32_t len) {
    return n->ready && len > 0 && len <= NBD_MAX_IO && off <= n->size && len <= n->size - off;
}

static int one_read(struct nbd *n, uint64_t off, void *buf, uint32_t len) {
    uint64_t handle;
    int rc = send_request(n, NBD_CMD_READ, off, len, &handle);
    if (rc == KERR_OK) {
        rc = read_reply(n, handle);
    }
    /* The data follows a successful simple reply, and only then. */
    return rc == KERR_OK ? recv_all(n, buf, len) : rc;
}

static int one_write(struct nbd *n, uint64_t off, const void *buf, uint32_t len) {
    uint64_t handle;
    int rc = send_request(n, NBD_CMD_WRITE, off, len, &handle);
    if (rc == KERR_OK) {
        rc = send_all(n, buf, len);
    }
    return rc == KERR_OK ? read_reply(n, handle) : rc;
}

/* The loop both directions share: NBD_MAX_IO at a time (Rule 5.1). */
static int transfer(struct nbd *n, uint64_t off, uint8_t *buf, uint32_t len, bool write) {
    while (len > 0) {
        uint32_t chunk = len < NBD_MAX_IO ? len : NBD_MAX_IO;
        int rc;
        if (!io_ok(n, off, chunk)) {
            return n->ready ? KERR_INVAL : KERR_PROTO;
        }
        rc = write ? one_write(n, off, buf, chunk) : one_read(n, off, buf, chunk);
        if (rc != KERR_OK) {
            return rc;
        }
        off += chunk;
        buf += chunk;
        len -= chunk;
    }
    return KERR_OK;
}

int nbd_pread(struct nbd *n, uint64_t off, void *buf, uint32_t len) {
    return transfer(n, off, buf, len, false);
}

int nbd_pwrite(struct nbd *n, uint64_t off, const void *buf, uint32_t len) {
    if ((n->flags & NBD_FLAG_READ_ONLY) != 0) {
        return KERR_ROFS;
    }
    return transfer(n, off, (uint8_t *)(uintptr_t)buf, len, true);
}

int nbd_flush(struct nbd *n) {
    uint64_t handle;
    int rc;
    if (!n->ready) {
        return KERR_PROTO;
    }
    if ((n->flags & NBD_FLAG_SEND_FLUSH) == 0) {
        return KERR_NOTSUP;
    }
    rc = send_request(n, NBD_CMD_FLUSH, 0, 0, &handle);
    return rc == KERR_OK ? read_reply(n, handle) : rc;
}

int nbd_disconnect(struct nbd *n) {
    uint64_t handle;
    int rc = n->ready ? send_request(n, NBD_CMD_DISC, 0, 0, &handle) : KERR_PROTO;
    n->ready = false;  /* there is no reply to a disconnect */
    return rc;
}

/* The blockdev port over one connection: self is the nbd, and the block
 * size it was built with is kept there, so no wrapper struct is needed. */
static bool dev_range_ok(const struct nbd *n, uint64_t lba, uint32_t count) {
    uint64_t blocks = n->size / n->dev_block;
    return count > 0 && count <= NBD_MAX_IO / n->dev_block && lba <= blocks && count <= blocks - lba;
}

static int dev_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    struct nbd *n = self;
    if (!dev_range_ok(n, lba, count)) {
        return KERR_INVAL;
    }
    return nbd_pread(n, lba * n->dev_block, buf, count * n->dev_block);
}

static int dev_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    struct nbd *n = self;
    if (!dev_range_ok(n, lba, count)) {
        return KERR_INVAL;
    }
    return nbd_pwrite(n, lba * n->dev_block, buf, count * n->dev_block);
}

static int dev_flush(void *self) {
    int rc = nbd_flush(self);
    return rc == KERR_NOTSUP ? KERR_OK : rc;  /* a server with no cache has nothing to flush */
}

int nbd_blockdev(struct nbd *n, uint32_t block_size, struct blockdev *out) {
    if (!n->ready || block_size < 512u || (block_size & (block_size - 1u)) != 0 ||
        block_size < n->min_block || block_size > NBD_MAX_IO) {
        return KERR_INVAL;
    }
    n->dev_block = block_size;
    *out = (struct blockdev){
        .read = dev_read,
        .write = dev_write,
        .flush = dev_flush,
        .self = n,
        .block_size = block_size,
        .blocks = n->size / block_size,
        .read_only = (n->flags & NBD_FLAG_READ_ONLY) != 0,
    };
    return KERR_OK;
}
