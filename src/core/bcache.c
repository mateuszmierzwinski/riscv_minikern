#include "core/bcache.h"

#include <stddef.h>

#include "core/errno.h"

static void copy_bytes(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

int bcache_init(struct bcache *c, const struct blockdev *dev, void *mem, uint32_t nslots) {
    uint8_t *at = mem;
    if (dev == NULL || mem == NULL || nslots == 0 || nslots > BCACHE_MAX_SLOTS ||
        dev->block_size < 512u || (dev->block_size & (dev->block_size - 1u)) != 0) {
        return KERR_INVAL;
    }
    *c = (struct bcache){.dev = dev, .nslots = nslots};
    for (uint32_t i = 0; i < nslots; i++) {
        c->slots[i].data = at + (uint64_t)i * dev->block_size;
    }
    return KERR_OK;
}

/* Writes one buffer out and leaves it clean. */
static int write_back(struct bcache *c, struct bbuf *b) {
    int rc = c->dev->write(c->dev->self, b->lba, 1, b->data);
    if (rc != KERR_OK) {
        return rc;
    }
    b->dirty = false;
    c->writebacks++;
    return KERR_OK;
}

static struct bbuf *find(struct bcache *c, uint64_t lba) {
    for (uint32_t i = 0; i < c->nslots; i++) {
        if (c->slots[i].valid && c->slots[i].lba == lba) {
            return &c->slots[i];
        }
    }
    return NULL;
}

/* The least recently acquired unpinned slot: a free one first, so a cold
 * cache never writes anything back. NULL when every slot is pinned. */
static struct bbuf *victim(struct bcache *c) {
    struct bbuf *best = NULL;
    for (uint32_t i = 0; i < c->nslots; i++) {
        struct bbuf *b = &c->slots[i];
        if (b->refs > 0) {
            continue;
        }
        if (!b->valid) {
            return b;
        }
        if (best == NULL || b->used < best->used) {
            best = b;
        }
    }
    return best;
}

/* A slot for block lba: the cached one, or a victim given to it — read
 * from the device when `fill` says so, left as it is when the caller is
 * about to overwrite every byte. */
static int claim(struct bcache *c, uint64_t lba, bool fill, struct bbuf **out) {
    struct bbuf *b = find(c, lba);
    int rc;
    if (!blockdev_in_range(c->dev, lba, 1)) {
        return KERR_INVAL;
    }
    if (b == NULL) {
        b = victim(c);
        if (b == NULL) {
            return KERR_NOMEM;
        }
        if (b->valid && b->dirty && (rc = write_back(c, b)) != KERR_OK) {
            return rc;
        }
        if (fill && (rc = c->dev->read(c->dev->self, lba, 1, b->data)) != KERR_OK) {
            b->valid = false;
            return rc;
        }
        *b = (struct bbuf){.lba = lba, .data = b->data, .valid = true};
        c->misses++;
    } else {
        c->hits++;
    }
    b->refs++;
    b->used = ++c->clock;
    *out = b;
    return KERR_OK;
}

int bcache_get(struct bcache *c, uint64_t lba, struct bbuf **out) {
    return claim(c, lba, true, out);
}

int bcache_get_zeroed(struct bcache *c, uint64_t lba, struct bbuf **out) {
    int rc = claim(c, lba, false, out);
    if (rc == KERR_OK) {
        for (uint32_t i = 0; i < c->dev->block_size; i++) {
            (*out)->data[i] = 0;
        }
        bcache_dirty(*out);
    }
    return rc;
}

void bcache_put(struct bcache *c, struct bbuf *b) {
    (void)c;
    if (b != NULL && b->refs > 0) {
        b->refs--;
    }
}

void bcache_dirty(struct bbuf *b) {
    b->dirty = true;
}

int bcache_flush(struct bcache *c) {
    int first = KERR_OK;
    for (uint32_t i = 0; i < c->nslots; i++) {
        struct bbuf *b = &c->slots[i];
        int rc = b->valid && b->dirty ? write_back(c, b) : KERR_OK;
        first = first == KERR_OK ? rc : first;
    }
    if (c->dev->read_only) {
        return first;
    }
    return first != KERR_OK ? first : c->dev->flush(c->dev->self);
}

void bcache_invalidate(struct bcache *c) {
    for (uint32_t i = 0; i < c->nslots; i++) {
        if (c->slots[i].refs == 0 && !c->slots[i].dirty) {
            c->slots[i].valid = false;
        }
    }
}

/* True when a buffer in [lba, lba+count) is in use. */
static bool busy_in(const struct bcache *c, uint64_t lba, uint32_t count) {
    for (uint32_t i = 0; i < c->nslots; i++) {
        const struct bbuf *b = &c->slots[i];
        if (b->valid && b->lba - lba < count && (b->dirty || b->refs != 0)) {
            return true;
        }
    }
    return false;
}

int bcache_discard(struct bcache *c, uint64_t lba, uint32_t count) {
    if (c->dev->discard == NULL || c->dev->read_only) {
        return KERR_NOTSUP;
    }
    if (!blockdev_in_range(c->dev, lba, count)) {
        return KERR_INVAL;
    }
    if (busy_in(c, lba, count)) {
        return KERR_BUSY;
    }
    for (uint32_t i = 0; i < c->nslots; i++) {
        if (c->slots[i].valid && c->slots[i].lba - lba < count) {
            c->slots[i].valid = false;
        }
    }
    return c->dev->discard(c->dev->self, lba, count);
}

/* True when [off, off+n) is inside the device. */
static bool byte_range_ok(const struct blockdev *d, uint64_t off, uint32_t n) {
    uint64_t bytes = d->blocks * d->block_size;
    return n == 0 || (off <= bytes && n <= bytes - off);
}

/* One block's worth of a byte range: copies in the given direction and
 * marks the buffer dirty when writing. */
static int transfer_one(struct bcache *c, uint64_t off, uint8_t *buf, uint32_t n, bool write) {
    uint32_t bs = c->dev->block_size;
    uint32_t at = (uint32_t)(off % bs);
    struct bbuf *b;
    int rc = bcache_get(c, off / bs, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    if (write) {
        copy_bytes(b->data + at, buf, n);
        bcache_dirty(b);
    } else {
        copy_bytes(buf, b->data + at, n);
    }
    bcache_put(c, b);
    return KERR_OK;
}

/* The shared walk behind bcache_pread and bcache_pwrite (Rule 5.1): one
 * block at a time, because the first and last may be partial. */
static int transfer(struct bcache *c, uint64_t off, void *buf, uint32_t n, bool write) {
    uint8_t *at = buf;
    uint32_t bs = c->dev->block_size;
    if (!byte_range_ok(c->dev, off, n)) {
        return KERR_INVAL;
    }
    if (write && c->dev->read_only) {
        return KERR_ROFS;
    }
    while (n > 0) {
        uint32_t chunk = bs - (uint32_t)(off % bs);
        int rc;
        chunk = chunk < n ? chunk : n;
        if ((rc = transfer_one(c, off, at, chunk, write)) != KERR_OK) {
            return rc;
        }
        off += chunk;
        at += chunk;
        n -= chunk;
    }
    return KERR_OK;
}

int bcache_pread(struct bcache *c, uint64_t off, void *buf, uint32_t n) {
    return transfer(c, off, buf, n, false);
}

int bcache_pwrite(struct bcache *c, uint64_t off, const void *buf, uint32_t n) {
    return transfer(c, off, (void *)(uintptr_t)buf, n, true);
}
