#include "core/netfs.h"

#include <stddef.h>

#include "core/errno.h"
#include "core/pb.h"

/* netfs.proto's numbers. */
enum { OP_STAT = 1, OP_READ, OP_LIST, OP_WRITE, OP_MKDIR, OP_UNLINK, OP_TRUNCATE, OP_RENAME, OP_READLINK };
enum { REQ_OP = 1, REQ_PATH, REQ_TO, REQ_OFFSET, REQ_LENGTH, REQ_DATA };
enum { RESP_STATUS = 1, RESP_DATA, RESP_STAT, RESP_ENTRIES };
enum { STAT_MODE = 1, STAT_NLINK, STAT_SIZE, STAT_MTIME, STAT_UID, STAT_GID };
enum { ENTRY_NAME = 1, ENTRY_MODE };


/* What a request carries beyond its operation and path. */
struct request {
    uint32_t op;
    const char *path;
    const char *to;
    uint64_t offset;
    uint32_t length;
    const void *data;
    uint32_t data_len;
};

/* A decoded response; `data` and `entries` point into the frame buffer. */
struct response {
    int32_t status;
    const uint8_t *data;
    uint32_t data_len;
    const uint8_t *stat;
    uint32_t stat_len;
    struct pb_reader entries; /* the whole message, walked again for field 4 */
};

void netfs_init(struct netfs *nf, const struct netfs_transport *t, uint8_t *frame) {
    *nf = (struct netfs){.t = *t, .frame = frame};
}

/* Encodes req at the start of the frame buffer; its length, or 0 when the
 * message does not fit. */
static uint32_t encode(struct netfs *nf, const struct request *req) {
    struct pb_writer w;
    pb_writer_init(&w, nf->frame, NETFS_FRAME);
    pb_put_varint(&w, REQ_OP, req->op);
    pb_put_string(&w, REQ_PATH, req->path);
    if (req->to != NULL) {
        pb_put_string(&w, REQ_TO, req->to);
    }
    pb_put_varint(&w, REQ_OFFSET, req->offset);
    pb_put_varint(&w, REQ_LENGTH, req->length);
    if (req->data != NULL) {
        pb_put_bytes(&w, REQ_DATA, req->data, req->data_len);
    }
    return w.overflow ? 0u : w.len;
}

static void hold(struct netfs *nf) {
    nf->t.lock(nf->t.self);
}

static void let_go(struct netfs *nf) {
    nf->t.unlock(nf->t.self);
}

static int decode(const uint8_t *msg, uint32_t len, struct response *out) {
    struct pb_field f;
    struct pb_reader r;
    int rc;
    *out = (struct response){0};
    pb_reader_init(&r, msg, len);
    pb_reader_init(&out->entries, msg, len);
    while ((rc = pb_next(&r, &f)) == KERR_OK) {
        if (f.number == RESP_STATUS && f.wire == PB_VARINT) {
            out->status = (int32_t)(int64_t)f.value; /* int32: sign-extended */
        } else if (f.number == RESP_DATA && f.wire == PB_BYTES) {
            out->data = f.data;
            out->data_len = f.data_len;
        } else if (f.number == RESP_STAT && f.wire == PB_BYTES) {
            out->stat = f.data;
            out->stat_len = f.data_len;
        }
    }
    return rc == KERR_NOENT ? KERR_OK : rc;
}

/* One exchange: the encoded request out, the answer back, decoded. */
static int exchange(struct netfs *nf, struct response *resp, uint32_t n) {
    uint32_t len = 0;
    int rc = nf->t.exchange(nf->t.self, nf->frame, n, NETFS_FRAME, &len);
    return rc != KERR_OK ? rc : decode(nf->frame, len, resp);
}

/* An exchange, **once more on a fresh stream** when the stream failed
 * (silence, a broken frame) rather than the server answering: every
 * operation here names its file and offset, so asking twice is safe, and a
 * stalled stream then costs one timeout rather than the call. The caller
 * holds the transport (`hold`) until it has finished with the response,
 * which points into the shared frame buffer. */
/* Whether asking twice is safe: reads, and writes that name their offset.
 * A rename, unlink or mkdir that succeeded and lost its reply would fail
 * the second time and report a failure for what was done. */
static bool idempotent(uint32_t op) {
    return op == OP_STAT || op == OP_READ || op == OP_LIST || op == OP_WRITE || op == OP_TRUNCATE ||
           op == OP_READLINK;
}

/* A server's status as an errno: success, a Linux errno (-4095..-1), or
 * KERR_IO for anything else — which would otherwise reach user space as a
 * "success" with a huge count. */
static int status_of(int32_t status) {
    return status >= 0 ? KERR_OK : (status >= -4095 ? (int)status : KERR_IO);
}

static int call(struct netfs *nf, const struct request *req, struct response *resp) {
    uint32_t n = encode(nf, req);
    int rc;
    if (n == 0) {
        return KERR_NAMETOOLONG;
    }
    nf->calls++;
    rc = exchange(nf, resp, n);
    if (rc != KERR_OK && idempotent(req->op)) {
        nf->failures++;
        nf->retries++;
        nf->t.reset(nf->t.self);
        n = encode(nf, req); /* the frame buffer held the broken reply */
        rc = exchange(nf, resp, n);
    }
    if (rc != KERR_OK) {
        nf->t.reset(nf->t.self); /* the stream's state is unknown either way */
        return rc;
    }
    return status_of(resp->status);
}

static void decode_stat(const uint8_t *msg, uint32_t len, struct netfs_stat *out) {
    struct pb_reader r;
    struct pb_field f;
    *out = (struct netfs_stat){0};
    pb_reader_init(&r, msg, len);
    while (pb_next(&r, &f) == KERR_OK) {
        if (f.wire != PB_VARINT) {
            continue;
        }
        switch (f.number) {
            case STAT_MODE: out->mode = (uint32_t)f.value; break;
            case STAT_NLINK: out->nlink = (uint32_t)f.value; break;
            case STAT_SIZE: out->size = f.value; break;
            case STAT_MTIME: out->mtime = (int64_t)f.value; break;
            case STAT_UID: out->uid = (uint32_t)f.value; break;
            case STAT_GID: out->gid = (uint32_t)f.value; break;
            default: break;
        }
    }
}

static int stat_held(struct netfs *nf, const char *path, struct netfs_stat *out) {
    struct request req = {.op = OP_STAT, .path = path};
    struct response resp;
    int rc = call(nf, &req, &resp);
    if (rc != KERR_OK) {
        return rc;
    }
    if (resp.stat == NULL) {
        return KERR_PROTO;
    }
    decode_stat(resp.stat, resp.stat_len, out);
    return KERR_OK;
}

/* One READ of at most NETFS_IO bytes. */
static int read_once(struct netfs *nf, const char *path, uint64_t off, uint8_t *buf, uint32_t len,
                     uint32_t *got) {
    struct request req = {.op = OP_READ, .path = path, .offset = off, .length = len};
    struct response resp;
    int rc = call(nf, &req, &resp);
    if (rc != KERR_OK) {
        return rc;
    }
    if (resp.data_len > len || (uint32_t)resp.status != resp.data_len) {
        return KERR_PROTO; /* more than was asked, or a count that disagrees */
    }
    for (uint32_t i = 0; i < resp.data_len; i++) {
        buf[i] = resp.data[i];
    }
    *got = resp.data_len;
    return KERR_OK;
}

static int read_held(struct netfs *nf, const char *path, uint64_t off, void *buf, uint32_t len,
                     uint32_t *got) {
    uint8_t *out = buf;
    *got = 0;
    while (*got < len) {
        uint32_t want = len - *got > NETFS_IO ? NETFS_IO : len - *got, n = 0;
        int rc = read_once(nf, path, off + *got, out + *got, want, &n);
        if (rc != KERR_OK) {
            return rc;
        }
        *got += n;
        if (n < want) {
            break; /* the end of the file */
        }
    }
    return KERR_OK;
}

static int write_held(struct netfs *nf, const char *path, uint64_t off, const void *buf,
                      uint32_t len, uint32_t *wrote) {
    const uint8_t *in = buf;
    *wrote = 0;
    do {
        uint32_t n = len - *wrote > NETFS_IO ? NETFS_IO : len - *wrote;
        struct request req = {.op = OP_WRITE, .path = path, .offset = off + *wrote,
                              .data = in + *wrote, .data_len = n, .length = n};
        struct response resp;
        int rc = call(nf, &req, &resp);
        if (rc != KERR_OK) {
            return rc;
        }
        if ((uint32_t)resp.status != n) {
            return KERR_IO;
        }
        *wrote += n;
    } while (*wrote < len);
    return KERR_OK;
}

/* Whether a server's entry name is a single component. */
static bool plain_name(const uint8_t *name, uint32_t len) {
    if (len == 0 || len > 255u) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (name[i] == '/' || name[i] == '\0') {
            return false;
        }
    }
    return true;
}

/* Hands each entry in the response to fn; how many there were. */
static int each_entry(struct response *resp, netfs_entry_fn fn, void *self, uint32_t *seen) {
    struct pb_field f, g;
    while (pb_next(&resp->entries, &f) == KERR_OK) {
        struct pb_reader e;
        const uint8_t *name = NULL;
        uint32_t name_len = 0, mode = 0;
        if (f.number != RESP_ENTRIES || f.wire != PB_BYTES) {
            continue;
        }
        pb_reader_init(&e, f.data, f.data_len);
        while (pb_next(&e, &g) == KERR_OK) {
            if (g.number == ENTRY_NAME && g.wire == PB_BYTES) {
                name = g.data;
                name_len = g.data_len;
            } else if (g.number == ENTRY_MODE && g.wire == PB_VARINT) {
                mode = (uint32_t)g.value;
            }
        }
        if (name == NULL || !plain_name(name, name_len)) {
            return KERR_PROTO; /* "", "/", "a/b" or a NUL: not one entry */
        }
        (*seen)++;
        int rc = fn(self, (const char *)name, name_len, mode);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* The most entries one listing takes from a server: a server that never
 * runs out must not keep the kernel (and the connection) busy for ever. */
#define LIST_MAX_ENTRIES 65536u

static int list_held(struct netfs *nf, const char *path, netfs_entry_fn fn, void *self) {
    uint64_t index = 0;
    for (;;) {
        if (index >= LIST_MAX_ENTRIES) {
            return KERR_FBIG;
        }
        struct request req = {.op = OP_LIST, .path = path, .offset = index, .length = NETFS_IO};
        struct response resp;
        uint32_t seen = 0;
        int rc = call(nf, &req, &resp);
        rc = rc != KERR_OK ? rc : each_entry(&resp, fn, self, &seen);
        if (rc != KERR_OK) {
            return rc;
        }
        if (seen == 0) {
            return KERR_OK;
        }
        index += seen;
    }
}

/* An operation whose answer is only a status. */
static int simple(struct netfs *nf, uint32_t op, const char *path, const char *to, uint64_t offset) {
    struct request req = {.op = op, .path = path, .to = to, .offset = offset};
    struct response resp;
    int rc;
    hold(nf);
    rc = call(nf, &req, &resp);
    let_go(nf);
    return rc;
}

int netfs_mkdir(struct netfs *nf, const char *path) {
    return simple(nf, OP_MKDIR, path, NULL, 0);
}

int netfs_unlink(struct netfs *nf, const char *path) {
    return simple(nf, OP_UNLINK, path, NULL, 0);
}

int netfs_truncate(struct netfs *nf, const char *path, uint64_t size) {
    return simple(nf, OP_TRUNCATE, path, NULL, size);
}

int netfs_rename(struct netfs *nf, const char *from, const char *to) {
    return simple(nf, OP_RENAME, from, to, 0);
}

int netfs_stat(struct netfs *nf, const char *path, struct netfs_stat *out) {
    int rc;
    hold(nf);
    rc = stat_held(nf, path, out);
    let_go(nf);
    return rc;
}

int netfs_read(struct netfs *nf, const char *path, uint64_t off, void *buf, uint32_t len,
               uint32_t *got) {
    int rc;
    hold(nf);
    rc = read_held(nf, path, off, buf, len, got);
    let_go(nf);
    return rc;
}

/* A link's target, cut to `room` bytes as readlink(2) cuts it. */
static int readlink_held(struct netfs *nf, const char *path, char *out, uint32_t room,
                         uint32_t *got) {
    struct request req = {.op = OP_READLINK, .path = path};
    struct response resp;
    int rc = call(nf, &req, &resp);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((uint32_t)resp.status != resp.data_len) {
        return KERR_PROTO;
    }
    *got = resp.data_len < room ? resp.data_len : room;
    for (uint32_t i = 0; i < *got; i++) {
        out[i] = (char)resp.data[i];
    }
    return KERR_OK;
}

int netfs_readlink(struct netfs *nf, const char *path, char *out, uint32_t room, uint32_t *got) {
    int rc;
    hold(nf);
    rc = readlink_held(nf, path, out, room, got);
    let_go(nf);
    return rc;
}

int netfs_write(struct netfs *nf, const char *path, uint64_t off, const void *buf, uint32_t len,
                uint32_t *wrote) {
    int rc;
    hold(nf);
    rc = write_held(nf, path, off, buf, len, wrote);
    let_go(nf);
    return rc;
}

/* **The callback runs with the connection held**: it must not call netfs. */
int netfs_list(struct netfs *nf, const char *path, netfs_entry_fn fn, void *self) {
    int rc;
    hold(nf);
    rc = list_held(nf, path, fn, self);
    let_go(nf);
    return rc;
}
