#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

/* Reading a file's bytes.
 *
 * File data is not metadata: its blocks carry no magic, no owner and no
 * checksum, so they are read through the cache without verification. What
 * is verified is the path that led here — the inode and every directory
 * block — and the extent list's arithmetic, which is checked against the
 * filesystem's size before any block number is used. */

static void zero_fill(uint8_t *at, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        at[i] = 0;
    }
}

/* A file held inside its own inode: a symbolic link's target, or a
 * directory's shortform content. */
static int read_local(const struct xfs_inode *ip, uint64_t off, uint8_t *buf, uint32_t len,
                      uint32_t *got) {
    uint32_t have = ip->size < ip->fork_len ? (uint32_t)ip->size : ip->fork_len;
    if (off >= have) {
        return KERR_OK;
    }
    *got = have - (uint32_t)off < len ? have - (uint32_t)off : len;
    xfs_copy_bytes(buf, ip->fork + off, *got);
    return KERR_OK;
}

/* One block's worth: from the file's data, from a hole, or from an extent
 * that was allocated but never written — the last two read as zeroes,
 * which is what they are. */
static int read_chunk(struct xfs *fs, const struct xfs_inode *ip, uint64_t at, uint8_t *out,
                      uint32_t within, uint32_t chunk) {
    struct xfs_extent e;
    struct bbuf *b;
    int rc = xfs_bmap(fs, ip, at >> fs->blocklog, &e);
    if (rc == KERR_NOENT || (rc == KERR_OK && e.unwritten)) {
        zero_fill(out, chunk);
        return KERR_OK;
    }
    if (rc != KERR_OK) {
        return rc;
    }
    /* **Which device, decided by the inode and not by the extent.**
     *
     * A realtime file's extent records are indistinguishable from any
     * other file's: the same `startoff`, `startblock` and `blockcount`. The
     * only thing that says they address the realtime device is the inode's
     * flag — so a reader that ignored it would fetch perfectly valid block
     * numbers from the wrong device and hand back whatever the data device
     * holds there, which is somebody else's file rather than an error.
     *
     * The realtime device also needs no `xfs_fsb_to_block`: a realtime
     * block number is linear, with no allocation group packed into its high
     * bits, because the realtime subvolume has no allocation groups. Running
     * it through that conversion is the same class of mistake as the log's
     * `logstart`, which this tree has already paid for once — and in the
     * opposite direction, which is why it is said here. */
    {
        uint64_t block = e.startblock + ((at >> fs->blocklog) - e.startoff);
        if (ip->realtime) {
            if (fs->rt_cache == NULL) {
                return KERR_NODEV; /* the realtime device is not attached */
            }
            rc = bcache_get(fs->rt_cache, block, &b);
        } else {
            rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, block), &b);
        }
    }
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(out, b->data + within, chunk);
    bcache_put(ip->realtime ? fs->rt_cache : fs->cache, b);
    return KERR_OK;
}

int xfs_file_read(struct xfs *fs, const struct xfs_inode *ip, uint64_t off, void *buf, uint32_t len,
                  uint32_t *got) {
    uint8_t *out = buf;
    uint32_t done = 0;
    *got = 0;
    /* A directory's bytes are structure rather than content: handing them
     * back as a file's contents is how a reader turns a directory into
     * rubbish for whoever asked. */
    if (xfs_is_dir(ip)) {
        return KERR_ISDIR;
    }
    if (!xfs_is_reg(ip) && !xfs_is_link(ip)) {
        return KERR_INVAL;
    }
    if (ip->format == XFS_DINODE_FMT_LOCAL) {
        return read_local(ip, off, out, len, got);
    }
    if (ip->format != XFS_DINODE_FMT_EXTENTS && ip->format != XFS_DINODE_FMT_BTREE) {
        return KERR_NOTSUP;
    }
    if (off >= ip->size) {
        return KERR_OK; /* at or past the end of the file */
    }
    len = ip->size - off < len ? (uint32_t)(ip->size - off) : len;
    while (done < len) {
        uint64_t at = off + done;
        uint32_t within = (uint32_t)(at & (fs->blocksize - 1u));
        uint32_t chunk = fs->blocksize - within < len - done ? fs->blocksize - within : len - done;
        int rc = read_chunk(fs, ip, at, out + done, within, chunk);
        if (rc != KERR_OK) {
            return rc;
        }
        done += chunk;
    }
    *got = done;
    return KERR_OK;
}

/* ── writing ───────────────────────────────────────────────────────────── */

bool xfs_writable(const struct xfs *fs) {
    return fs->mounted && fs->cache != NULL && !fs->cache->dev->read_only &&
           !fs->write_refused;
}

int xfs_flush(struct xfs *fs) {
    return fs->mounted ? bcache_flush(fs->cache) : KERR_INVAL;
}

/* The one kind of write that changes no metadata: into a block the file
 * already has, and has already been written to. Anything else — a hole, an
 * extent allocated but never written, a block past the end of the file —
 * would mean changing the inode, the free-space trees or the extent list,
 * and all of that has to go through the log first (P9.4). */
static void put_be64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (56u - 8u * i));
    }
}

static int write_chunk(struct xfs *fs, const struct xfs_inode *ip, uint64_t at, const uint8_t *in,
                       uint32_t within, uint32_t chunk) {
    struct xfs_extent e;
    struct bbuf *b;
    int rc = xfs_bmap(fs, ip, at >> fs->blocklog, &e);
    if (rc == KERR_NOENT || (rc == KERR_OK && e.unwritten)) {
        return KERR_NOTSUP; /* a hole or an unwritten extent: metadata */
    }
    if (rc != KERR_OK) {
        return rc;
    }
    /* The same choice the read path makes, for the same reason: a realtime
     * file's extents address the realtime device, and writing them to the
     * data device would overwrite whatever lives at those block numbers —
     * which is other files' contents, silently. */
    {
        uint64_t block = e.startblock + ((at >> fs->blocklog) - e.startoff);
        if (ip->realtime) {
            if (fs->rt_cache == NULL) {
                return KERR_NODEV;
            }
            rc = bcache_get(fs->rt_cache, block, &b);
        } else {
            rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, block), &b);
        }
    }
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(b->data + within, in, chunk);
    bcache_dirty(b);
    bcache_put(ip->realtime ? fs->rt_cache : fs->cache, b);
    return KERR_OK;
}

/* How many bytes the file's existing blocks could hold, which is where a
 * write may reach without anything being allocated. A file's last block is
 * usually only partly used — a 10-byte file still owns 4096 — and growing
 * into that room is a metadata change (the recorded size) but not an
 * allocation. */
static uint64_t allocated_bytes(const struct xfs *fs, const struct xfs_inode *ip) {
    uint64_t end = 0;
    for (uint64_t block = 0;; block++) {
        struct xfs_extent e;
        if (xfs_bmap(fs, ip, block, &e) != KERR_OK) {
            break;
        }
        end = (e.startoff + e.blockcount) << fs->blocklog;
        block = e.startoff + e.blockcount - 1u;
    }
    return end > ip->size ? end : ip->size;
}

/* One more realtime extent for a realtime file.
 *
 * **A realtime block number is linear**: the realtime subvolume has no
 * allocation groups, so there is no group number packed into the high bits
 * and none of the `agblklog` shifting the data device needs. Applying that
 * shift here would produce a block number far outside the device — the
 * mirror image of the log's `logstart`, which this tree got wrong in the
 * other direction and has a scar for.
 *
 * `rextsize` is the allocation granularity in blocks, so extent `n` begins
 * at block `n * rextsize`. With the common `rextsize` of 1 those are the
 * same number, which is exactly the coincidence that would hide a missing
 * multiplication — so it is written out rather than assumed away. */
/* A realtime block, zeroed for the reason `xfs_zero_block` gives. */
static int zero_realtime(struct xfs *fs, uint64_t block) {
    struct bbuf *b;
    int rc = bcache_get_zeroed(fs->rt_cache, block, &b);
    if (rc == KERR_OK) {
        bcache_put(fs->rt_cache, b);
    }
    return rc;
}

static int grow_realtime(struct xfs *fs, struct xfs_fork_map *m, uint64_t logical) {
    uint64_t extent = 0;
    struct xfs_extent e;
    int rc;
    if (fs->rt_cache == NULL) {
        return KERR_NODEV; /* nowhere to put the data once it is allocated */
    }
    if (fs->rextsize == 0) {
        return KERR_INVAL;
    }
    rc = xfs_rt_alloc(fs, 1u, &extent);
    if (rc != KERR_OK) {
        return rc;
    }
    e = (struct xfs_extent){.startoff = logical, .startblock = extent * fs->rextsize,
                            .blockcount = 1};
    rc = zero_realtime(fs, e.startblock);
    rc = rc != KERR_OK ? rc : xfs_fork_map_add(m, &e);
    if (rc != KERR_OK) {
        /* The extent was taken but the map would not hold it: give it back,
         * or the bitmap says used and no inode refers to it — a leak only a
         * repair tool could find. */
        (void)xfs_rt_release(fs, extent, 1u);
    }
    return rc;
}

/* One block from an allocation group, next to the file's last one when that
 * is free, recorded in the reverse map and then in the map.
 *
 * **The reverse map before the fork**, so that each failure has one thing
 * to undo: a block the map refused goes back to the allocator, and one the
 * fork refused leaves the map as well. */
static int grow_from_group(struct xfs *fs, const struct xfs_inode *ip, struct xfs_fork_map *m,
                           uint64_t logical) {
    uint32_t want = 0, got = 0, agno = xfs_fsb_ag(fs, ip->ino >> fs->inopblog);
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u, fsb;
    struct xfs_extent e;
    int rc;
    if (m->count > 0) {
        const struct xfs_extent *last = &m->e[m->count - 1u];
        agno = xfs_fsb_ag(fs, last->startblock);
        want = (uint32_t)((last->startblock + last->blockcount) & mask);
    }
    rc = xfs_alloc_somewhere(fs, &agno, 1u, want, &got);
    if (rc != KERR_OK) {
        return rc;
    }
    fsb = ((uint64_t)agno << fs->agblklog) | got;
    e = (struct xfs_extent){.startoff = logical, .startblock = fsb, .blockcount = 1};
    rc = xfs_zero_block(fs, fsb);
    rc = rc != KERR_OK ? rc : xfs_rmap_map(fs, fsb, 1u, ip->ino, logical);
    if (rc == KERR_OK && (rc = xfs_fork_map_add(m, &e)) != KERR_OK) {
        (void)xfs_rmap_unmap(fs, fsb, 1u, ip->ino, logical);
    }
    if (rc != KERR_OK) {
        (void)xfs_free_extent(fs, agno, got, 1u);
    }
    return rc;
}

/* The logical block just past the map's last extent. */
static uint64_t map_end(const struct xfs_fork_map *m) {
    const struct xfs_extent *last = m->count > 0 ? &m->e[m->count - 1u] : NULL;
    return last == NULL ? 0 : last->startoff + last->blockcount;
}

/* One more block, at `logical`, charged to the owner's quota first.
 *
 * **The quota is asked before the block is taken.** Charging afterwards
 * would mean an owner over their limit still consumed the space, and
 * giving it back needs the allocator's free path — which is more work than
 * not allocating. A filesystem with no quotas pays one comparison for this. */
static int add_block(struct xfs *fs, const struct xfs_inode *ip, struct xfs_fork_map *m,
                     uint64_t logical) {
    int rc = xfs_quota_charge_all(fs, ip, 1, 0);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = ip->realtime ? grow_realtime(fs, m, logical) : grow_from_group(fs, ip, m, logical);
    if (rc != KERR_OK) {
        (void)xfs_quota_charge_all(fs, ip, -1, 0);
    }
    return rc;
}

/* Adds blocks from the map's end — or from `from`, past a gap that stays a
 * hole — until the file reaches `upto`; *added says how many. */
static int add_blocks(struct xfs *fs, const struct xfs_inode *ip, struct xfs_fork_map *m,
                      uint64_t from, uint64_t upto, uint64_t *added) {
    int rc = KERR_OK;
    *added = 0;
    for (uint64_t next = map_end(m); rc == KERR_OK && next < upto; next = map_end(m)) {
        rc = add_block(fs, ip, m, next > from ? next : from);
        *added += rc == KERR_OK ? 1u : 0u;
    }
    return rc;
}

/* Gives the file enough blocks to hold `end` bytes.
 *
 * One block at a time, each asked for immediately after the file's last
 * one: when that block is free the file's last extent simply gets longer,
 * and only when it is not does the file collect another extent. A file
 * written in order therefore stays a single extent, which is what the
 * reader walks quickest and what `xfs_repair` expects to see.
 *
 * Each new block goes at the next logical block past the file's last
 * extent, or at the block the write begins in when that is further on: the
 * gap between them stays a hole, which is what a write past the end of a
 * file leaves on any filesystem. */
static int grow_file(struct xfs *fs, struct xfs_inode *ip, uint64_t off, uint64_t end) {
    uint64_t block, added = 0, upto = (end + fs->blocksize - 1u) >> fs->blocklog;
    struct xfs_fork_map m;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc, stored;
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    rc = xfs_fork_load(fs, di, &m);
    rc = rc != KERR_OK ? rc : add_blocks(fs, ip, &m, off >> fs->blocklog, upto, &added);
    /* **Stored whenever a block was added, even if a later one failed.**
     * The allocator and the reverse map already have it; an inode left
     * saying otherwise is a leak, while blocks past the end of a file are
     * an ordinary thing for one to own. */
    if (added > 0) {
        xfs_put_be64(di + XFS_DI_NBLOCKS, xfs_be64(di + XFS_DI_NBLOCKS) + added);
        stored = xfs_fork_store(fs, ip->ino, di, &m);
        rc = rc != KERR_OK ? rc : stored;
        xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    /* The caller's copy follows the disk: the fork may have changed shape. */
    return added > 0 && xfs_inode_read(fs, ip->ino, ip) != KERR_OK ? KERR_IO : rc;
}

/* Whether this file can be written at all. The blocks it touches are made
 * writable as the write reaches them — a hole filled, an unwritten or a
 * shared block dealt with (xfs_fill.c) — so a write can now stop part way,
 * on a full disk, and reports how far it got as any write does. */
static int write_is_possible(struct xfs *fs, const struct xfs_inode *ip) {
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (xfs_is_dir(ip)) {
        return KERR_ISDIR;
    }
    if (!xfs_is_reg(ip)) {
        return KERR_INVAL;
    }
    if (ip->format != XFS_DINODE_FMT_EXTENTS && ip->format != XFS_DINODE_FMT_BTREE) {
        return KERR_NOTSUP; /* a file held inside its inode would change it */
    }
    return KERR_OK;
}

/* Records a file's new size: in the log first, then in its place.
 *
 * This is the only metadata a data write changes, and it is where the log
 * earns its place in the order of things — a crash between the two leaves
 * the change in the log rather than half done. What this kernel will not do
 * is **replay** one (xfs_logwrite.c), so it also writes an unmount record
 * afterwards and the reader goes on refusing a dirty log: Linux's
 * `xfs_repair` stays the recovery tool.
 *
 * The inode's own checksum is recomputed, because a v5 inode carries one
 * and an inode whose sum does not match is an inode nothing will read —
 * including this reader, which is how it would be noticed. */
static int record_new_size(struct xfs *fs, const struct xfs_inode *ip, uint64_t size) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    put_be64(di + XFS_DI_SIZE, size);
    /* **`xfs_reseal`, not a hand-rolled checksum.** These two lines used to
     * compute the sum here, which is the same operation written twice — and
     * when v4 arrived only the shared one learned that a v4 inode has no
     * checksum. `XFS_DI_CRC` is offset 100, which on a v4 inode is the
     * *first four bytes of the literal area*: the first extent record's
     * first word. So this wrote a checksum over the extent of every file it
     * had just grown, `nextents` still said one, and the file read back as
     * zeroes — a write that reported success and lost the data. */
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    struct xfs_log_inode_item item = {.ino = ip->ino, .chunk_block = block, .boffset = boffset, .core = di};
    rc = xfs_log_inodes(fs, &item, 1);
    if (rc == KERR_OK) {
        bcache_dirty(b);
    }
    bcache_put(fs->cache, b);
    return rc;
}

int xfs_file_write(struct xfs *fs, struct xfs_inode *ip, uint64_t off, const void *buf,
                   uint32_t len, uint32_t *put) {
    const uint8_t *in = buf;
    uint32_t done = 0;
    int rc;
    *put = 0;
    if (len == 0) {
        return KERR_OK;
    }
    if ((rc = write_is_possible(fs, ip)) != KERR_OK) {
        return rc;
    }
    if (off + len > allocated_bytes(fs, ip) && (rc = grow_file(fs, ip, off, off + len)) != KERR_OK) {
        return rc;
    }
    while (done < len) {
        uint64_t at = off + done;
        uint32_t within = (uint32_t)(at & (fs->blocksize - 1u));
        uint32_t chunk = fs->blocksize - within < len - done ? fs->blocksize - within : len - done;
        /* A hole, an unwritten block or a shared one is made writable
         * first; a block that already holds this file's data is left. */
        if ((rc = xfs_file_prepare_block(fs, ip, at >> fs->blocklog)) != KERR_OK ||
            (rc = write_chunk(fs, ip, at, in + done, within, chunk)) != KERR_OK) {
            *put = done; /* the range was checked, so this is a device error */
            return rc;
        }
        done += chunk;
    }
    *put = done;
    if (off + done <= ip->size) {
        return KERR_OK;
    }
    rc = record_new_size(fs, ip, off + done);
    if (rc == KERR_OK) {
        /* The in-memory inode follows the disk, or nothing above this ever
         * learns the file grew: a stat would keep reporting the old size
         * and a read would stop at it. */
        ip->size = off + done;
    }
    return rc;
}
