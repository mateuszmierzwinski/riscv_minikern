/* bcache — a buffer cache over one block device.
 *
 * It exists because the device is at the far end of a network: XFS reads a
 * superblock, then an AG header, then a btree root, then a leaf, and each
 * one of those is a round trip. Blocks are kept, so walking a directory
 * twice costs one walk, and a read-modify-write of part of a block does not
 * have to fetch it again.
 *
 * A buffer handed out by bcache_get is pinned until bcache_put: eviction
 * cannot take it away while a caller is parsing it. Writes mark it dirty and
 * go to the device at bcache_flush, or earlier if the buffer is evicted.
 * There is no ordering guarantee between dirty buffers — that is what a
 * journal is for, and the filesystem's business, not the cache's. */
#ifndef MINIKERN_CORE_BCACHE_H
#define MINIKERN_CORE_BCACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/blockdev.h"

#define BCACHE_MAX_SLOTS 64u

/* One cached block. data is the caller's to read, and to write as long as
 * bcache_dirty is called before the put. */
struct bbuf {
    uint64_t lba;
    uint8_t *data;
    uint32_t refs;
    uint64_t used;  /* bcache's clock when last acquired: the LRU order */
    bool valid;
    bool dirty;
};

struct bcache {
    const struct blockdev *dev;
    struct bbuf slots[BCACHE_MAX_SLOTS];
    uint32_t nslots;
    uint64_t clock;
    /* Counters, for the netstat log and for tests that assert a read was
     * served from memory rather than from the network. */
    uint32_t hits;
    uint32_t misses;
    uint32_t writebacks;
};

/* Takes nslots buffers of dev->block_size bytes from mem (which must be at
 * least nslots * block_size and stay alive as long as the cache does).
 * KERR_INVAL on a bad device or slot count. */
int bcache_init(struct bcache *c, const struct blockdev *dev, void *mem, uint32_t nslots);

/* The block at lba, read from the device if it is not already here, pinned.
 * KERR_OK and *out set, or KERR_INVAL/KERR_IO/KERR_NOMEM (every slot
 * pinned). Every successful get needs a put. */
int bcache_get(struct bcache *c, uint64_t lba, struct bbuf **out);

/* The block at lba, pinned, as zeroes and dirty: for a block whose old
 * contents nobody may see — one just allocated — so it is not read first. */
int bcache_get_zeroed(struct bcache *c, uint64_t lba, struct bbuf **out);

/* Releases a pin. A dirty buffer stays dirty and in the cache. */
void bcache_put(struct bcache *c, struct bbuf *b);

/* Says the caller changed b->data, so it must reach the device. */
void bcache_dirty(struct bbuf *b);

/* Writes every dirty buffer, then flushes the device. */
int bcache_flush(struct bcache *c);

/* Forgets every clean, unpinned buffer. For when something else may have
 * changed the device under us; dirty buffers are kept, not discarded. */
void bcache_invalidate(struct bcache *c);

/* Discards count blocks at lba on the device, and forgets any copy of them
 * here, so a later read sees what the device now holds. KERR_NOTSUP when
 * the device cannot discard; KERR_BUSY when one of them is dirty or pinned
 * — a block somebody is writing is not free, and the caller (which flushed
 * first) has made a mistake worth hearing about rather than a write lost. */
int bcache_discard(struct bcache *c, uint64_t lba, uint32_t count);

/* Byte-granular access, for a block device opened as a file: these do the
 * read-modify-write for partial blocks. off and n are bytes, and a transfer
 * that would run past the end of the device is KERR_INVAL. */
int bcache_pread(struct bcache *c, uint64_t off, void *buf, uint32_t n);
int bcache_pwrite(struct bcache *c, uint64_t off, const void *buf, uint32_t n);

#endif
