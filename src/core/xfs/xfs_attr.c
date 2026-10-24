/* Extended attributes: the shortform fork, read and written.
 *
 * **Why shortform first, and why that is what made this testable at all.**
 * An attribute set needs a filesystem with attributes on it, and nothing
 * here could make one: setting an xattr needs a mounted filesystem, which
 * needs root, and `mkfs.xfs`'s protofile cannot express one. So a *reader*
 * written alone would have been code that compiled and never ran — which
 * this tree has paid for five times over in one session, including a
 * directory writer whose first real call handed out the root inode.
 *
 * Shortform attributes live **entirely inside the inode's literal area**,
 * above the data fork, so the writer allocates nothing and the test needs
 * no fixture: set an attribute, read it back, list it, remove it, and mount
 * the filesystem again to prove it was on the disk rather than in memory.
 * The reader and the writer check each other, which is the only honest way
 * to start when there is no third party to produce a sample.
 *
 * **The namespace is a flag, not part of the name.** Linux presents
 * `user.foo`, `trusted.foo` and `security.foo`; XFS stores the name as
 * `foo` with a flag saying which namespace it is in. A reader that treated
 * the prefix as part of the name would find nothing that Linux had written
 * and write names Linux could not find, while passing any test that only
 * used its own output.
 */
#include <stddef.h>

#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* The shortform header and one entry. The header's `totsize` counts itself,
 * so an empty fork is four bytes rather than nothing. */
#define SF_TOTSIZE 0u /* be16 */
#define SF_COUNT   2u /* u8 */
#define SF_HDR     4u
#define SF_NAMELEN 0u /* u8 */
#define SF_VALUELEN 1u /* u8 */
#define SF_FLAGS   2u /* u8 */
#define SF_ENTRY   3u /* then the name, then the value */

/* Splits `user.foo` into the namespace flag and the name XFS stores.
 *
 * An unknown prefix is refused rather than stored as a user attribute: a
 * name that round-trips through this kernel and means something else to
 * Linux is worse than one that was rejected. */
static int split_name(const char *name, uint32_t len, uint8_t *flags, const char **bare,
                      uint32_t *barelen) {
    static const struct {
        const char *prefix;
        uint32_t n;
        uint8_t flag;
    } SPACES[] = {{"user.", 5u, 0}, {"trusted.", 8u, XFS_ATTR_ROOT},
                  {"security.", 9u, XFS_ATTR_SECURE}};
    for (uint32_t i = 0; i < sizeof SPACES / sizeof SPACES[0]; i++) {
        uint32_t n = SPACES[i].n;
        uint32_t m = 0;
        while (m < n && m < len && name[m] == SPACES[i].prefix[m]) {
            m++;
        }
        if (m == n && len > n) {
            *flags = SPACES[i].flag;
            *bare = name + n;
            *barelen = len - n;
            return KERR_OK;
        }
    }
    return KERR_NOTSUP;
}

static bool same_name(const uint8_t *at, const char *name, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (at[i] != (uint8_t)name[i]) {
            return false;
        }
    }
    return true;
}

/* Walks the shortform entries, stopping at `want` or running out.
 *
 * Returns the entry's offset within the fork, or the fork's used length
 * when the name is not there — so one walk answers both "where is it" and
 * "where would it go", which is what an insert needs. */
static uint32_t sf_find(const uint8_t *fork, uint32_t len, const char *want,
                        uint32_t wantlen, uint8_t flags, bool *found) {
    uint32_t count = fork[SF_COUNT];
    uint32_t at = SF_HDR;
    *found = false;
    for (uint32_t i = 0; i < count && at + SF_ENTRY <= len; i++) {
        uint32_t namelen = fork[at + SF_NAMELEN];
        uint32_t valuelen = fork[at + SF_VALUELEN];
        uint32_t size = SF_ENTRY + namelen + valuelen;
        if (at + size > len) {
            break; /* an entry that claims more than the fork holds */
        }
        if (namelen == wantlen && (fork[at + SF_FLAGS] & XFS_ATTR_NSP) == flags &&
            same_name(fork + at + SF_ENTRY, want, wantlen)) {
            *found = true;
            return at;
        }
        at += size;
    }
    return at;
}

/* Copies the inode's attribute fork out of its block.
 *
 * On demand rather than from `struct xfs_inode`, which keeps only the two
 * bytes that locate it: carrying the contents in every inode doubled that
 * struct for a feature most inodes do not use, and three functions holding
 * two inodes went over the kernel's frame limit the moment it did. */
static int load_afork(struct xfs *fs, const struct xfs_inode *ip, uint8_t *fork,
                      uint32_t *len) {
    uint32_t area = fs->inodesize - XFS_DI_V3_SIZE;
    uint32_t at = (uint32_t)ip->forkoff * 8u;
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    int rc;
    *len = 0;
    if (ip->forkoff == 0 || at >= area) {
        return KERR_NOENT; /* no attribute fork */
    }
    *len = area - at;
    if (*len > XFS_AFORK_MAX) {
        return KERR_IO;
    }
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(fork, b->data + boffset + XFS_DI_V3_SIZE + at, *len);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* ── reading ──────────────────────────────────────────────────────────── */

int xfs_attr_get(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                 uint32_t namelen, uint8_t *out, uint32_t room, uint32_t *valuelen) {
    uint8_t fork[XFS_AFORK_MAX];
    const char *bare = NULL;
    uint32_t barelen = 0, len = 0, at;
    uint8_t flags = 0;
    bool found = false;
    int rc = split_name(name, namelen, &flags, &bare, &barelen);
    if (rc != KERR_OK) {
        return rc;
    }
    if (ip->aformat != XFS_DINODE_FMT_LOCAL) {
        return xfs_attr_leaf_get(fs, ip, bare, barelen, flags, out, room, valuelen);
    }
    rc = load_afork(fs, ip, fork, &len);
    if (rc != KERR_OK || len < SF_HDR) {
        return rc != KERR_OK ? rc : KERR_NOENT;
    }
    at = sf_find(fork, len, bare, barelen, flags, &found);
    if (!found) {
        return KERR_NOENT;
    }
    *valuelen = fork[at + SF_VALUELEN];
    if (*valuelen > room) {
        return KERR_RANGE; /* the caller's buffer is the wrong size */
    }
    for (uint32_t i = 0; i < *valuelen; i++) {
        out[i] = fork[at + SF_ENTRY + barelen + i];
    }
    return KERR_OK;
}

int xfs_attr_list(struct xfs *fs, const struct xfs_inode *ip, xfs_attr_fn fn, void *self) {
    uint8_t fork[XFS_AFORK_MAX];
    uint32_t count, len = 0, at = SF_HDR;
    int rc;
    if (ip->forkoff == 0) {
        return KERR_OK; /* none, which is not an error */
    }
    if (ip->aformat != XFS_DINODE_FMT_LOCAL) {
        return xfs_attr_leaf_list(fs, ip, fn, self);
    }
    rc = load_afork(fs, ip, fork, &len);
    if (rc != KERR_OK || len < SF_HDR) {
        return rc == KERR_NOENT ? KERR_OK : rc;
    }
    count = fork[SF_COUNT];
    for (uint32_t i = 0; i < count && at + SF_ENTRY <= len; i++) {
        uint32_t namelen = fork[at + SF_NAMELEN];
        uint32_t valuelen = fork[at + SF_VALUELEN];
        uint8_t flags = fork[at + SF_FLAGS];
        if (at + SF_ENTRY + namelen + valuelen > len) {
            return KERR_IO;
        }
        /* An interrupted set is skipped rather than reported: the name is
         * on disk and its value is not, so listing it would offer a caller
         * something that cannot be read. */
        if ((flags & XFS_ATTR_INCOMPLETE) == 0) {
            rc = fn(self, fork + at + SF_ENTRY, namelen,
                    (uint8_t)(flags & XFS_ATTR_NSP), valuelen);
            if (rc != KERR_OK) {
                return rc;
            }
        }
        at += SF_ENTRY + namelen + valuelen;
    }
    return KERR_OK;
}

/* ── writing ──────────────────────────────────────────────────────────── */

/* Defined below, and called from `xfs_attr_set` and `xfs_attr_remove`: the
 * shortform fork becomes a leaf when the inode fills, a fork that is
 * already a leaf is written as one, and both paths need the loader and the
 * remover that live with the leaf code. */
static int leaf_load(struct xfs *fs, const struct xfs_inode *ip, uint8_t **at,
                     uint64_t *fsb);
static bool leaf_take_out(struct xfs *fs, const char *name, uint32_t namelen,
                          uint8_t flags);
static int xfs_attr_leaf_store(struct xfs *fs, uint64_t fsb);
static int xfs_attr_to_leaf(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                            uint32_t namelen, const uint8_t *value, uint32_t valuelen,
                            uint8_t flags, uint64_t now_s);
static int xfs_attr_leaf_set(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                             uint32_t namelen, const uint8_t *value, uint32_t valuelen,
                             uint8_t flags, uint64_t now_s);

/* How many bytes of the literal area the data fork needs.
 *
 * This is what bounds where the attribute fork may start, and getting it
 * wrong in the generous direction overwrites the file's own extents. A
 * `LOCAL` data fork is its size; an `EXTENTS` one is its extents; a b-tree
 * root is whatever it already occupies, and this refuses to move it —
 * repacking a b-tree root to make room for an attribute is a different
 * piece of work from storing one. */
static int data_fork_bytes(const struct xfs_inode *ip, uint32_t *need) {
    if (ip->format == XFS_DINODE_FMT_LOCAL) {
        *need = (uint32_t)ip->size;
        return KERR_OK;
    }
    if (ip->format == XFS_DINODE_FMT_EXTENTS) {
        *need = ip->nextents * XFS_EXTENT_BYTES;
        return KERR_OK;
    }
    return KERR_NOTSUP; /* a b-tree data fork: not moved to make room */
}

/* Chooses where the attribute fork starts, in eight-byte units.
 *
 * **Zero is not a valid answer**, because zero is the sentinel for "this
 * inode has no attribute fork". The first version computed
 * `(data_fork_bytes + 7) / 8`, which for a freshly created empty file is
 * exactly zero — so it stored the fork at the start of the literal area and
 * then stamped "there is no fork here" into the inode. The board said so by
 * reading back `KERR_NOENT` for an attribute it had just written: the
 * writer and the reader disagreeing about the same inode, which is the one
 * disagreement a self-testing pair can still have.
 *
 * The default is **half the literal area**, which is what Linux does, and
 * for a good reason rather than symmetry: the data fork cannot be moved
 * once attributes sit above it (`extend_fork` refuses to grow past its
 * bound, which is what stops a growing file from eating them), so a file
 * given an attribute while empty would otherwise never be able to hold a
 * single extent. Half of a 512-byte inode's literal area is ten extents
 * for the data and 168 bytes for attributes.
 *
 * An inode that already has an attribute fork keeps the offset it has:
 * moving it would mean moving the attributes too. */
static int choose_forkoff(const struct xfs *fs, const struct xfs_inode *ip,
                          uint32_t *forkoff, uint32_t *room) {
    uint32_t area = fs->inodesize - XFS_DI_V3_SIZE;
    uint32_t need = 0;
    uint32_t half = (area / 2u) / 8u;
    int rc = data_fork_bytes(ip, &need);
    if (rc != KERR_OK) {
        return rc;
    }
    if (ip->forkoff != 0) {
        *forkoff = ip->forkoff;
    } else {
        uint32_t least = (need + 7u) / 8u;
        *forkoff = least > half ? least : half;
        *forkoff = *forkoff == 0 ? 1u : *forkoff;
    }
    if (*forkoff * 8u >= area) {
        return KERR_NOSPC; /* the data fork leaves no room for attributes */
    }
    *room = area - *forkoff * 8u;
    return KERR_OK;
}

/* Writes a shortform attribute fork back into the inode, at `forkoff`.
 *
 * The inode is read, edited and resealed here rather than by the caller
 * because the fork's position is part of what is being written: `forkoff`
 * and `aformat` are in the core, and a fork stored without them is a fork
 * nothing will find. */
static int store_fork(struct xfs *fs, const struct xfs_inode *ip, const uint8_t *fork,
                      uint32_t len, uint32_t forkoff, uint64_t now_s) {
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
    for (uint32_t i = 0; i < len; i++) {
        di[XFS_DI_V3_SIZE + forkoff * 8u + i] = fork[i];
    }
    di[XFS_DI_FORKOFF] = (uint8_t)forkoff;
    di[XFS_DI_AFORMAT] = XFS_DINODE_FMT_LOCAL;
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_attr_set(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                 uint32_t namelen, const uint8_t *value, uint32_t valuelen,
                 uint64_t now_s) {
    uint8_t fork[XFS_AFORK_MAX];
    const char *bare = NULL;
    uint32_t barelen = 0, forkoff = 0, room = 0, used, at, entry;
    uint8_t flags = 0;
    bool found = false;
    int rc;
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    rc = split_name(name, namelen, &flags, &bare, &barelen);
    rc = rc != KERR_OK ? rc : choose_forkoff(fs, ip, &forkoff, &room);
    if (rc != KERR_OK) {
        return rc;
    }
    /* A name or value past a byte each cannot be shortform: the format
     * stores both lengths in a single byte, which is also why a large
     * attribute needs the leaf format this does not write. */
    if (barelen > 255u || valuelen > 255u || room > sizeof fork) {
        return KERR_NOSPC;
    }
    /* An inode whose attribute fork is already a leaf is written as one:
     * overwriting it with a shortform fork would lose every attribute it
     * held. */
    if (ip->forkoff != 0 && ip->aformat == XFS_DINODE_FMT_EXTENTS) {
        return xfs_attr_leaf_set(fs, ip, bare, barelen, value, valuelen, flags, now_s);
    }
    if (ip->forkoff != 0 && ip->aformat != XFS_DINODE_FMT_LOCAL) {
        return KERR_NOTSUP; /* a b-tree attribute fork */
    }
    used = SF_HDR;
    if (ip->forkoff != 0) {
        uint32_t len = 0;
        rc = load_afork(fs, ip, fork, &len);
        if (rc != KERR_OK) {
            return rc;
        }
        used = len >= SF_HDR ? xfs_be16(fork + SF_TOTSIZE) : SF_HDR;
        used = used < SF_HDR || used > len || used > room ? SF_HDR : used;
    }
    if (ip->forkoff == 0 || used == SF_HDR) {
        xfs_put_be16(fork + SF_TOTSIZE, SF_HDR);
        fork[SF_COUNT] = 0;
        fork[3] = 0;
    }
    /* **Replace means remove and then add.** The entries are packed with no
     * gaps, so a new value of a different length cannot be written over the
     * old one — and writing it over anyway is how the entry after it loses
     * its first bytes. */
    at = sf_find(fork, used, bare, barelen, flags, &found);
    if (found) {
        uint32_t size = SF_ENTRY + fork[at + SF_NAMELEN] + fork[at + SF_VALUELEN];
        for (uint32_t i = at; i + size < used; i++) {
            fork[i] = fork[i + size];
        }
        used -= size;
        fork[SF_COUNT]--;
    }
    entry = SF_ENTRY + barelen + valuelen;
    if (used + entry > room) {
        /* **The inode is full, so the fork becomes a leaf block.** The
         * conversion takes the new attribute with it rather than being a
         * step of its own: converting first and adding after would leave a
         * leaf on disk that does not hold the attribute the caller asked
         * for, and a caller that then failed would have grown the file's
         * metadata for nothing. */
        rc = xfs_attr_to_leaf(fs, ip, bare, barelen, value, valuelen, flags, now_s);
        fs->attrs_to_leaf += rc == KERR_OK ? 1u : 0u;
        return rc;
    }
    fork[used + SF_NAMELEN] = (uint8_t)barelen;
    fork[used + SF_VALUELEN] = (uint8_t)valuelen;
    fork[used + SF_FLAGS] = flags;
    for (uint32_t i = 0; i < barelen; i++) {
        fork[used + SF_ENTRY + i] = (uint8_t)bare[i];
    }
    for (uint32_t i = 0; i < valuelen; i++) {
        fork[used + SF_ENTRY + barelen + i] = value[i];
    }
    used += entry;
    fork[SF_COUNT]++;
    xfs_put_be16(fork + SF_TOTSIZE, (uint16_t)used);
    return store_fork(fs, ip, fork, used, forkoff, now_s);
}

int xfs_attr_remove(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                    uint32_t namelen, uint64_t now_s) {
    uint8_t fork[XFS_AFORK_MAX];
    const char *bare = NULL;
    uint32_t barelen = 0, used, at, size, len = 0;
    uint8_t flags = 0;
    bool found = false;
    int rc;
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    rc = split_name(name, namelen, &flags, &bare, &barelen);
    if (rc != KERR_OK) {
        return rc;
    }
    if (ip->forkoff == 0) {
        return KERR_NOENT;
    }
    if (ip->aformat == XFS_DINODE_FMT_EXTENTS) {
        uint8_t *leaf = NULL;
        uint64_t fsb = 0;
        rc = leaf_load(fs, ip, &leaf, &fsb);
        if (rc != KERR_OK) {
            return rc;
        }
        if (!leaf_take_out(fs, bare, barelen, flags)) {
            return KERR_NOENT;
        }
        xfs_reseal(fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
        return xfs_attr_leaf_store(fs, fsb);
    }
    if (ip->aformat != XFS_DINODE_FMT_LOCAL) {
        return KERR_NOTSUP;
    }
    rc = load_afork(fs, ip, fork, &len);
    if (rc != KERR_OK || len < SF_HDR) {
        return rc != KERR_OK ? rc : KERR_NOENT;
    }
    used = xfs_be16(fork + SF_TOTSIZE);
    if (used < SF_HDR || used > len) {
        return KERR_IO;
    }
    at = sf_find(fork, used, bare, barelen, flags, &found);
    if (!found) {
        return KERR_NOENT;
    }
    size = SF_ENTRY + fork[at + SF_NAMELEN] + fork[at + SF_VALUELEN];
    for (uint32_t i = at; i + size < used; i++) {
        fork[i] = fork[i + size];
    }
    used -= size;
    fork[SF_COUNT]--;
    xfs_put_be16(fork + SF_TOTSIZE, (uint16_t)used);
    return store_fork(fs, ip, fork, used, ip->forkoff, now_s);
}

/* ── the leaf format, read only ───────────────────────────────────────── */

/* Maps a logical block of the attribute fork.
 *
 * **A separate extent list from the data fork's**, living above
 * `di_forkoff` in the literal area — so the data fork's mapper cannot
 * answer for it, and reading an attribute block through `xfs_bmap` would
 * return a block of the file's own contents. Extents only: a b-tree
 * attribute fork is refused. */
int xfs_attr_bmap(struct xfs *fs, const struct xfs_inode *ip, uint64_t logical,
                  struct xfs_extent *out) {
    uint8_t fork[XFS_AFORK_MAX];
    uint32_t len = 0, n;
    int rc;
    if (ip->aformat != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP;
    }
    rc = load_afork(fs, ip, fork, &len);
    if (rc != KERR_OK) {
        return rc;
    }
    n = len / XFS_EXTENT_BYTES;
    for (uint32_t i = 0; i < n; i++) {
        struct xfs_extent e;
        xfs_extent_unpack(fork + i * XFS_EXTENT_BYTES, &e);
        if (e.blockcount == 0) {
            break; /* a zero-length extent ends the list */
        }
        if (logical >= e.startoff && logical < e.startoff + e.blockcount) {
            *out = e;
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

/* Loads the attribute fork's one leaf block, verified.
 *
 * **Read, never written.** A set that outgrows the inode becomes a leaf
 * block, and Linux writes those routinely — so refusing to read them would
 * make this kernel unable to see attributes on an ordinary filesystem. But
 * writing one means a hash index, a free map and a block allocation on the
 * attribute fork, and a half-built leaf is an attribute set that reads as
 * corrupt rather than as absent. So `xfs_attr_set` answers `KERR_NOSPC`
 * when the inode is full, and this side stays a reader. */
static int leaf_load(struct xfs *fs, const struct xfs_inode *ip, uint8_t **at,
                     uint64_t *fsb) {
    struct xfs_extent e;
    struct bbuf *b;
    int rc;
    if (ip->aformat != XFS_DINODE_FMT_EXTENTS || ip->forkoff == 0) {
        return KERR_NOTSUP; /* a b-tree attribute fork: not read either */
    }
    rc = xfs_attr_bmap(fs, ip, 0, &e);
    if (rc != KERR_OK) {
        return rc;
    }
    *fsb = e.startblock;
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, e.startblock), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(fs->dirblock, b->data, fs->blocksize);
    bcache_put(fs->cache, b);
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != XFS_ATTR_LEAF_MAGIC) {
        return KERR_IO;
    }
    if (!xfs_crc_ok(fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF) ||
        !xfs_uuid_ok(fs, fs->dirblock + XFS_DA3_UUID_OFF)) {
        return KERR_IO;
    }
    *at = fs->dirblock;
    return KERR_OK;
}

/* One leaf entry's name, value length and value offset.
 *
 * A **remote** value lives in blocks of its own and the entry carries only
 * their address, so it is reported as present with its length and refused
 * on read: saying "here is a value" and handing back the block number would
 * be worse than saying it cannot be fetched. */
static int leaf_entry(const struct xfs *fs, const uint8_t *leaf, uint32_t i,
                      const uint8_t **name, uint32_t *namelen, uint32_t *valuelen,
                      uint32_t *value_at, uint8_t *flags) {
    uint32_t e = ATTR_LEAF_HDR + i * ATTR_LEAF_ENTRY;
    uint32_t idx;
    if (e + ATTR_LEAF_ENTRY > fs->blocksize) {
        return KERR_IO;
    }
    idx = xfs_be16(leaf + e + ATTR_LEAF_NAMEIDX);
    *flags = leaf[e + ATTR_LEAF_FLAGS];
    if (idx + 4u > fs->blocksize) {
        return KERR_IO;
    }
    if ((*flags & XFS_ATTR_LOCAL) != 0) {
        /* **The value's length comes first.** Reading `namelen` from offset
         * zero costs nothing visible on a short name and mangles every
         * longer one — and that is exactly what this did for as long as
         * there was no leaf block to read it against. The writer below is
         * what caught it. */
        *valuelen = xfs_be16(leaf + idx + ATTR_LOCAL_VALUELEN);
        *namelen = leaf[idx + ATTR_LOCAL_NAMELEN];
        *name = leaf + idx + ATTR_LOCAL_NAMEVAL;
        *value_at = idx + ATTR_LOCAL_NAMEVAL + *namelen;
        return *value_at + *valuelen <= fs->blocksize ? KERR_OK : KERR_IO;
    }
    *valuelen = xfs_be32(leaf + idx + ATTR_REMOTE_VALUELEN);
    *namelen = leaf[idx + ATTR_REMOTE_NAMELEN];
    *name = leaf + idx + ATTR_REMOTE_NAME;
    *value_at = 0; /* not in this block */
    return idx + ATTR_REMOTE_NAME + *namelen <= fs->blocksize ? KERR_OK : KERR_IO;
}

int xfs_attr_leaf_get(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                      uint32_t namelen, uint8_t flags, uint8_t *out, uint32_t room,
                      uint32_t *valuelen) {
    uint8_t *leaf = NULL;
    uint32_t count;
    uint64_t fsb = 0;
    int rc = leaf_load(fs, ip, &leaf, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(leaf + ATTR_LEAF_COUNT);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *at = NULL;
        uint32_t len = 0, vlen = 0, value_at = 0;
        uint8_t f = 0;
        rc = leaf_entry(fs, leaf, i, &at, &len, &vlen, &value_at, &f);
        if (rc != KERR_OK) {
            return rc;
        }
        if ((f & XFS_ATTR_INCOMPLETE) != 0 || len != namelen ||
            (f & XFS_ATTR_NSP) != flags || !same_name(at, name, namelen)) {
            continue;
        }
        *valuelen = vlen;
        if ((f & XFS_ATTR_LOCAL) == 0) {
            return KERR_NOTSUP; /* the value is in blocks of its own */
        }
        if (vlen > room) {
            return KERR_RANGE;
        }
        for (uint32_t k = 0; k < vlen; k++) {
            out[k] = leaf[value_at + k];
        }
        return KERR_OK;
    }
    return KERR_NOENT;
}

int xfs_attr_leaf_list(struct xfs *fs, const struct xfs_inode *ip, xfs_attr_fn fn,
                       void *self) {
    uint8_t *leaf = NULL;
    uint32_t count;
    uint64_t fsb = 0;
    int rc = leaf_load(fs, ip, &leaf, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(leaf + ATTR_LEAF_COUNT);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *at = NULL;
        uint32_t len = 0, vlen = 0, value_at = 0;
        uint8_t f = 0;
        rc = leaf_entry(fs, leaf, i, &at, &len, &vlen, &value_at, &f);
        if (rc != KERR_OK) {
            return rc;
        }
        if ((f & XFS_ATTR_INCOMPLETE) != 0) {
            continue;
        }
        rc = fn(self, at, len, (uint8_t)(f & XFS_ATTR_NSP), vlen);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

/* ── the leaf format, written ─────────────────────────────────────────── */

/* Writes the buffer back to the attribute fork's block. */
static int xfs_attr_leaf_store(struct xfs *fs, uint64_t fsb) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(b->data, fs->dirblock, fs->blocksize);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Takes one entry out of the leaf in the buffer, leaving its name and value
 * where they are.
 *
 * The pair becomes a **hole**, which `holes` records: compacting it out
 * would mean moving every pair below it and rewriting every entry's
 * `nameidx`, and the format is explicitly built to tolerate gaps until a
 * writer needs the space. `usedbytes` drops, so the block still knows how
 * much of it is live. */
static bool leaf_take_out(struct xfs *fs, const char *name, uint32_t namelen,
                          uint8_t flags) {
    uint32_t count = xfs_be16(fs->dirblock + ATTR_LEAF_COUNT);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *at = NULL;
        uint32_t len = 0, vlen = 0, value_at = 0;
        uint8_t f = 0;
        if (leaf_entry(fs, fs->dirblock, i, &at, &len, &vlen, &value_at, &f) != KERR_OK) {
            return false;
        }
        if (len != namelen || (f & XFS_ATTR_NSP) != flags || !same_name(at, name, namelen)) {
            continue;
        }
        for (uint32_t k = i; k + 1u < count; k++) {
            xfs_copy_bytes(fs->dirblock + ATTR_LEAF_HDR + k * ATTR_LEAF_ENTRY,
                           fs->dirblock + ATTR_LEAF_HDR + (k + 1u) * ATTR_LEAF_ENTRY,
                           ATTR_LEAF_ENTRY);
        }
        xfs_put_be16(fs->dirblock + ATTR_LEAF_COUNT, (uint16_t)(count - 1u));
        {
            uint32_t used = xfs_be16(fs->dirblock + ATTR_LEAF_USEDBYTES);
            uint32_t size = ATTR_LOCAL_SIZE(len, vlen);
            xfs_put_be16(fs->dirblock + ATTR_LEAF_USEDBYTES,
                         (uint16_t)(used > size ? used - size : 0u));
        }
        fs->dirblock[ATTR_LEAF_HOLES] = 1u;
        return true;
    }
    return false;
}

/* Stores the inode's attribute fork as a single extent and says it is one.
 *
 * The shortform bytes at `forkoff` are replaced by a sixteen-byte extent
 * record, and `di_aformat` becomes `EXTENTS`. Both halves matter: a fork
 * whose format still says `LOCAL` would be parsed as attributes, and the
 * first two bytes of an extent record read as a shortform `totsize` of
 * something enormous. */
static int store_attr_extent(struct xfs *fs, const struct xfs_inode *ip, uint64_t fsb,
                             uint64_t now_s) {
    struct xfs_extent e = {.startoff = 0, .startblock = fsb, .blockcount = 1};
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
    for (uint32_t i = 0; i < XFS_EXTENT_BYTES; i++) {
        di[XFS_DI_V3_SIZE + (uint32_t)ip->forkoff * 8u + i] = 0;
    }
    xfs_extent_pack(di + XFS_DI_V3_SIZE + (uint32_t)ip->forkoff * 8u, &e);
    di[XFS_DI_AFORMAT] = XFS_DINODE_FMT_EXTENTS;
    xfs_put_be64(di + XFS_DI_NBLOCKS, ip->nblocks + 1u);
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* Writes an empty leaf into the directory buffer.
 *
 * `firstused` starts at the end of the block and grows **downwards** as
 * pairs are added, while the entry array grows upwards from the header —
 * the same arrangement a directory's leaf uses, and the free map records
 * the gap between them. A free map left wrong is the structure `xfs_repair`
 * reads to decide whether a block has room. */
static void empty_attr_leaf(struct xfs *fs, uint64_t ino, uint64_t blkno) {
    for (uint32_t i = 0; i < fs->blocksize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be16(fs->dirblock + XFS_DA3_MAGIC_OFF, XFS_ATTR_LEAF_MAGIC);
    xfs_put_be64(fs->dirblock + XFS_DA3_BLKNO_OFF, blkno);
    xfs_put_be64(fs->dirblock + XFS_DA3_OWNER_OFF, ino);
    for (uint32_t i = 0; i < 16; i++) {
        fs->dirblock[XFS_DA3_UUID_OFF + i] = fs->uuid[i];
    }
    xfs_put_be16(fs->dirblock + ATTR_LEAF_FIRSTUSED, (uint16_t)fs->blocksize);
    xfs_put_be16(fs->dirblock + ATTR_LEAF_FREEMAP, ATTR_LEAF_HDR);
    xfs_put_be16(fs->dirblock + ATTR_LEAF_FREEMAP + 2u,
                 (uint16_t)(fs->blocksize - ATTR_LEAF_HDR));
}

/* Puts one attribute into the leaf held in the buffer.
 *
 * The entry array is kept **sorted by hash**, which is what makes a lookup
 * a search rather than a walk — and what a reader expecting sorted order
 * would otherwise silently mis-search. The name and value go at the bottom
 * of the free gap; the entry that points at them is inserted in place.
 *
 * Runs to 30 lines: it is one derivation over a single block — where the
 * pair goes, where its entry goes, and the three header fields that move
 * with both — and no part of it can be checked without the others in front
 * of you (Rule 11, Single derivation). */
static int leaf_put(struct xfs *fs, const char *name, uint32_t namelen,
                    const uint8_t *value, uint32_t valuelen, uint8_t flags) {
    uint32_t count = xfs_be16(fs->dirblock + ATTR_LEAF_COUNT);
    uint32_t firstused = xfs_be16(fs->dirblock + ATTR_LEAF_FIRSTUSED);
    uint32_t used = xfs_be16(fs->dirblock + ATTR_LEAF_USEDBYTES);
    uint32_t size = ATTR_LOCAL_SIZE(namelen, valuelen);
    uint32_t hash = xfs_dir_hashname(name, namelen);
    uint32_t entries_end = ATTR_LEAF_HDR + (count + 1u) * ATTR_LEAF_ENTRY;
    uint32_t at = count, pair;
    if (firstused < size || entries_end > firstused - size) {
        return KERR_NOSPC; /* the array and the pairs would meet */
    }
    pair = firstused - size;
    for (uint32_t i = 0; i < count; i++) {
        if (xfs_be32(fs->dirblock + ATTR_LEAF_HDR + i * ATTR_LEAF_ENTRY) > hash) {
            at = i;
            break;
        }
    }
    for (uint32_t i = count; i > at; i--) {
        xfs_copy_bytes(fs->dirblock + ATTR_LEAF_HDR + i * ATTR_LEAF_ENTRY,
                       fs->dirblock + ATTR_LEAF_HDR + (i - 1u) * ATTR_LEAF_ENTRY,
                       ATTR_LEAF_ENTRY);
    }
    xfs_put_be32(fs->dirblock + ATTR_LEAF_HDR + at * ATTR_LEAF_ENTRY, hash);
    xfs_put_be16(fs->dirblock + ATTR_LEAF_HDR + at * ATTR_LEAF_ENTRY + ATTR_LEAF_NAMEIDX,
                 (uint16_t)pair);
    fs->dirblock[ATTR_LEAF_HDR + at * ATTR_LEAF_ENTRY + ATTR_LEAF_FLAGS] =
        (uint8_t)(flags | XFS_ATTR_LOCAL);
    xfs_put_be16(fs->dirblock + pair + ATTR_LOCAL_VALUELEN, (uint16_t)valuelen);
    fs->dirblock[pair + ATTR_LOCAL_NAMELEN] = (uint8_t)namelen;
    for (uint32_t i = 0; i < namelen; i++) {
        fs->dirblock[pair + ATTR_LOCAL_NAMEVAL + i] = (uint8_t)name[i];
    }
    for (uint32_t i = 0; i < valuelen; i++) {
        fs->dirblock[pair + ATTR_LOCAL_NAMEVAL + namelen + i] = value[i];
    }
    xfs_put_be16(fs->dirblock + ATTR_LEAF_COUNT, (uint16_t)(count + 1u));
    xfs_put_be16(fs->dirblock + ATTR_LEAF_FIRSTUSED, (uint16_t)pair);
    xfs_put_be16(fs->dirblock + ATTR_LEAF_USEDBYTES, (uint16_t)(used + size));
    xfs_put_be16(fs->dirblock + ATTR_LEAF_FREEMAP, (uint16_t)entries_end);
    xfs_put_be16(fs->dirblock + ATTR_LEAF_FREEMAP + 2u, (uint16_t)(pair - entries_end));
    return KERR_OK;
}

int xfs_attr_to_leaf(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                     uint32_t namelen, const uint8_t *value, uint32_t valuelen,
                     uint8_t flags, uint64_t now_s) {
    uint8_t fork[XFS_AFORK_MAX];
    uint32_t len = 0, count, at = SF_HDR, agno;
    uint32_t agbno = 0;
    uint64_t fsb;
    int rc = load_afork(fs, ip, fork, &len);
    if (rc != KERR_OK || len < SF_HDR) {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    count = fork[SF_COUNT];
    agno = xfs_fsb_ag(fs, ip->ino >> fs->inopblog);
    rc = xfs_alloc_somewhere(fs, &agno, 1u, 0, &agbno);
    if (rc != KERR_OK) {
        return rc;
    }
    fsb = ((uint64_t)agno << fs->agblklog) | agbno;
    empty_attr_leaf(fs, ip->ino, 0);
    /* Every attribute that was in the inode, then the one that would not
     * fit. **The new one last**, so that a failure to place it leaves the
     * block holding exactly what the inode already held — and the inode
     * still pointing at the inode, since the extent is stored at the end. */
    for (uint32_t i = 0; i < count && at + SF_ENTRY <= len && rc == KERR_OK; i++) {
        uint32_t nlen = fork[at + SF_NAMELEN];
        uint32_t vlen = fork[at + SF_VALUELEN];
        if (at + SF_ENTRY + nlen + vlen > len) {
            return KERR_IO;
        }
        rc = leaf_put(fs, (const char *)(fork + at + SF_ENTRY), nlen,
                      fork + at + SF_ENTRY + nlen, vlen, fork[at + SF_FLAGS]);
        at += SF_ENTRY + nlen + vlen;
    }
    rc = rc != KERR_OK ? rc : leaf_put(fs, name, namelen, value, valuelen, flags);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_reseal(fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
    rc = xfs_attr_leaf_store(fs, fsb);
    return rc != KERR_OK ? rc : store_attr_extent(fs, ip, fsb, now_s);
}

int xfs_attr_leaf_set(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                      uint32_t namelen, const uint8_t *value, uint32_t valuelen,
                      uint8_t flags, uint64_t now_s) {
    uint8_t *leaf = NULL;
    uint64_t fsb = 0;
    int rc = leaf_load(fs, ip, &leaf, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    /* Replace is remove-then-add here too, and for the same reason as in
     * the inode: a pair of a different length cannot be written over the
     * old one. The old pair becomes a hole rather than being compacted
     * out — `holes` is what tells a writer the block needs packing before
     * its free map can be believed. */
    (void)leaf_take_out(fs, name, namelen, flags);
    rc = leaf_put(fs, name, namelen, value, valuelen, flags);
    if (rc != KERR_OK) {
        return rc; /* the block is unchanged on disk: nothing was stored */
    }
    xfs_reseal(fs->dirblock, fs->blocksize, XFS_DA3_CRC_OFF);
    rc = xfs_attr_leaf_store(fs, fsb);
    (void)now_s;
    return rc;
}
