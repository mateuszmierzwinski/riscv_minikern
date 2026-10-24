#include "kernel/xfsmount.h"

#include "lib/sha256.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "core/xfs/xfs_internal.h"
#include "arch/riscv/timer.h"
#include "kernel/disk.h"
#include "kernel/mem.h"
#include "kernel/volumes.h"

static struct xfs fs;
static bool mounted;

int xfsmount_attach(void) {
    struct bcache *cache = disk_cache();
    int rc;
    if (cache == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_mount(&fs, cache);
    if (rc != KERR_OK) {
        return rc;
    }
    mounted = true;
    return KERR_OK;
}

int xfsmount_remount(void) {
    struct bcache *cache = disk_cache();
    int rc;
    if (cache == NULL) {
        return KERR_NODEV;
    }
    /* **The cache is dropped first.** A re-mount that read the superblock
     * out of the cache would be reading what this kernel last wrote rather
     * than what is on the disk, and the whole point of re-mounting in a
     * test is to see the filesystem as a fresh boot would. */
    rc = bcache_flush(cache);
    if (rc != KERR_OK) {
        return rc;
    }
    bcache_invalidate(cache);
    mounted = false;
    for (uint32_t i = 0; i < sizeof fs; i++) {
        ((uint8_t *)&fs)[i] = 0;
    }
    rc = xfs_mount(&fs, cache);
    mounted = rc == KERR_OK;
    return rc;
}

void xfsmount_leave_log_dirty(bool on) {
    fs.skip_unmount_record = on;
}

bool xfsmount_log_was_dirty(void) {
    return fs.log_was_dirty;
}

const char *xfsmount_label(void) {
    return fs.label;
}

bool xfsmount_present(void) {
    return mounted;
}

/* What a directory walk reports back. The callback form is the filesystem's,
 * so the counting happens here rather than in it. */
struct listing {
    int32_t count;
};

static int show_entry(void *self, const struct xfs_dirent *e) {
    struct listing *l = self;
    l->count++;
    kprintf("xfs:   %.*s%s\n", (int)e->name_len, e->name,
            e->type == XFS_DIR3_FT_DIR ? "/" : "");
    return KERR_OK;
}

int32_t xfsmount_list_root(void) {
    struct xfs_inode root;
    struct listing l = {0};
    int rc;
    if (!mounted) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(&fs, "/", &root);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_dir_list(&fs, &root, show_entry, &l);
    return rc != KERR_OK ? rc : l.count;
}

/* Reads a file and prints its length and its first line, which is what
 * proves the extent map rather than just the directory. */
/* How much of a file is read at a time. 64 KiB is large enough that the
 * per-read cost disappears and small enough to sit in the heap beside
 * everything else. */
#define HASH_CHUNK 65536u

int xfsmount_hash_file(const char *path, uint8_t digest[32], uint64_t *size) {
    struct xfs_inode ip;
    struct sha256 state;
    uint8_t *buf;
    uint64_t at = 0;
    int rc;
    if (!mounted) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    buf = kzalloc(HASH_CHUNK);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    sha256_init(&state);
    while (at < ip.size) {
        uint64_t left = ip.size - at;
        uint32_t want = left < HASH_CHUNK ? (uint32_t)left : HASH_CHUNK;
        uint32_t got = 0;
        rc = xfs_file_read(&fs, &ip, at, buf, want, &got);
        if (rc != KERR_OK || got == 0) {
            rc = rc != KERR_OK ? rc : KERR_IO;
            break;
        }
        sha256_update(&state, buf, got);
        at += got;
    }
    if (rc == KERR_OK) {
        sha256_final(&state, digest);
        *size = ip.size;
    }
    kfree(buf);
    return rc;
}

/* Which filesystem answers for a path, and the path within it.
 *
 * **The mount table first, and this single mount only as a fallback.** The
 * table is what makes a path mean something — `/volumes/DATA/x` has to
 * reach the DATA volume — and the fallback is what keeps every caller
 * written before the table existed working: a kernel that mounted one
 * filesystem by hand (`minikern.disk=`, the self-tests) still resolves
 * through it. Dropping the fallback would be tidier and would break the
 * board tests, which mount a RAM disk directly. */
static struct xfs *owner_of(const char *path, const char **within) {
    struct xfs *v = volumes_resolve(path, within);
    if (v != NULL) {
        return v;
    }
    *within = path;
    return mounted ? &fs : NULL;
}

int xfsmount_show_file(const char *path) {
    struct xfs_inode ip;
    char *buf;
    uint32_t got = 0;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(vol, within, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    buf = kzalloc(512u);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    rc = xfs_file_read(vol, &ip, 0, buf, 511u, &got);
    if (rc == KERR_OK) {
        /* One line of it: a file's first bytes prove the data path, and the
         * whole file would prove the same thing less legibly. */
        for (uint32_t i = 0; i < got; i++) {
            buf[i] = buf[i] == '\n' ? '\0' : buf[i];
        }
        kprintf("xfs: %s is %llu bytes, begins \"%s\"\n", path, (unsigned long long)ip.size, buf);
    }
    kfree(buf);
    return rc;
}

/* Who owns a path and what its mode is.
 *
 * Reported rather than enforced **here**, deliberately. This layer is the
 * filesystem's and knows nothing about processes; the permission rule needs
 * to know who is asking, which is the process layer's business. Putting the
 * check in this function would have the hexagon reaching outward — and the
 * dependency would be circular besides.
 *
 * So: ownership is parsed (it was not, until there was something to check
 * it against) and handed up, `core/access.c` holds the rule, and the check
 * belongs at whatever opens a file on behalf of a process. There is no such
 * caller yet — regular-file `openat` is §3 of the roadmap — which is stated
 * rather than papered over with a check that only ever sees root. */
int xfsmount_stat(const char *path, uint32_t *uid, uint32_t *gid, uint32_t *mode) {
    struct xfs_inode ip;
    int rc;
    if (!mounted) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (uid != NULL) {
        *uid = ip.uid;
    }
    if (gid != NULL) {
        *gid = ip.gid;
    }
    if (mode != NULL) {
        *mode = ip.mode;
    }
    return KERR_OK;
}

/* Where a file's pages actually live on the device.
 *
 * **What makes a swap *file* possible without a filesystem that can write.**
 * A swapfile is only ever overwritten in place: its blocks are allocated
 * once, by whoever created it, and swapping changes no metadata — no size,
 * no extent map, no allocation. So the kernel resolves the file's blocks
 * once, here, and then reads and writes them as blocks. That is how Linux
 * does it too, and it is why this works on a filesystem this kernel mounts
 * read-only.
 *
 * Every page must be backed by a **written** extent. A hole or an unwritten
 * extent reads as zeroes and has nowhere to put a page, so a sparse file is
 * refused rather than half used — which is also what `swapon` does on
 * Linux, and for the same reason.
 *
 * Reports byte offsets into the device, because that is what the block
 * cache takes. */
int xfsmount_map_file(const char *path, uint64_t *offsets, uint32_t max, uint32_t *pages) {
    struct xfs_inode ip;
    uint32_t want;
    int rc;
    if (!mounted) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (ip.size < XFSMOUNT_PAGE_BYTES) {
        return KERR_INVAL; /* nothing to swap into */
    }
    want = (uint32_t)(ip.size / XFSMOUNT_PAGE_BYTES);
    *pages = want < max ? want : max;
    for (uint32_t i = 0; i < *pages; i++) {
        struct xfs_extent e;
        uint64_t fileblock = (uint64_t)i * XFSMOUNT_PAGE_BYTES / fs.blocksize;
        rc = xfs_bmap(&fs, &ip, fileblock, &e);
        if (rc != KERR_OK) {
            return rc == KERR_NOENT ? KERR_INVAL : rc; /* a hole: not usable */
        }
        if (e.unwritten) {
            return KERR_INVAL; /* allocated but never written: the same */
        }
        offsets[i] = (uint64_t)xfs_fsb_to_block(&fs, e.startblock + (fileblock - e.startoff)) *
                     fs.blocksize;
    }
    return KERR_OK;
}

int xfsmount_read_file(const char *path, uint32_t limit, uint8_t **out, uint32_t *size) {
    struct xfs_inode ip;
    uint8_t *buf;
    uint32_t got = 0, at = 0;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(vol, within, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (ip.size > limit) {
        return KERR_FBIG;
    }
    buf = kzalloc((uint32_t)ip.size);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    /* In whatever pieces the filesystem gives: a read may stop at an extent
     * boundary, and a loop that assumes one call is enough works until the
     * first file that is not contiguous. */
    while (at < ip.size) {
        rc = xfs_file_read(vol, &ip, at, buf + at, (uint32_t)ip.size - at, &got);
        if (rc != KERR_OK || got == 0) {
            kfree(buf);
            return rc != KERR_OK ? rc : KERR_IO;
        }
        at += got;
    }
    *out = buf;
    *size = (uint32_t)ip.size;
    return KERR_OK;
}

void xfsmount_lookup_counts(uint64_t *hashed, uint64_t *walked) {
    *hashed = fs.hash_lookups;
    *walked = fs.walked_lookups;
}

void xfsmount_single_group(bool on) {
    fs.single_group = on;
}

uint64_t xfsmount_alloc_other_group(void) {
    return fs.alloc_other_group;
}

uint64_t xfsmount_inode_chunks_added(void) {
    return fs.inode_chunks_added;
}

void xfsmount_growth_counts(uint64_t *to_block, uint64_t *to_leaf, uint64_t *data_blocks) {
    *to_block = fs.grew_to_block;
    *to_leaf = fs.grew_to_leaf;
    *data_blocks = fs.dir_blocks_added;
}

/* ── writing ───────────────────────────────────────────────────────────── */

/* What goes in a timestamp, given that this board has no clock.
 *
 * **Seconds since boot, and files will look dated 1970.** There is no RTC
 * (`ROADMAP.md` §4), so there is no epoch to count from; the alternative is
 * writing zero, which claims the same wrong thing less informatively. A
 * timestamp that at least orders two writes correctly is worth more than
 * one that does not, and when a clock arrives this is the one line that
 * changes. */
static uint64_t now_seconds(void) {
    return timer_ns() / 1000000000ull;
}

/* The root directory, which is what the write operations act in.
 *
 * Read fresh each time rather than kept: every operation that adds or
 * removes an entry changes the directory's inode, and a cached copy would
 * be the shape it had before. */
static int root_inode(struct xfs_inode *out) {
    if (!mounted) {
        return KERR_NODEV;
    }
    return xfs_path_lookup(&fs, "/", out);
}

bool xfsmount_writable(void) {
    return mounted && xfs_writable(&fs);
}

static uint32_t name_length(const char *name) {
    uint32_t n = 0;
    while (name[n] != '\0') {
        n++;
    }
    return n;
}

int xfsmount_create(const char *name, uint16_t mode) {
    struct xfs_inode root;
    uint64_t ino = 0;
    int rc = root_inode(&root);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_create(&fs, &root, name, name_length(name), mode, now_seconds(), &ino);
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_write(const char *name, uint64_t off, const void *buf, uint32_t len) {
    struct xfs_inode root, ip;
    uint64_t ino = 0;
    uint32_t put = 0;
    int rc = root_inode(&root);
    rc = rc != KERR_OK ? rc : xfs_dir_lookup(&fs, &root, name, name_length(name), &ino);
    rc = rc != KERR_OK ? rc : xfs_inode_read(&fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_file_write(&fs, &ip, off, buf, len, &put);
    if (rc == KERR_OK && put != len) {
        return KERR_IO; /* a short write with no reason is not a success */
    }
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_read_at(const char *name, uint64_t off, void *buf, uint32_t len) {
    struct xfs_inode root, ip;
    uint64_t ino = 0;
    uint32_t got = 0;
    int rc = root_inode(&root);
    rc = rc != KERR_OK ? rc : xfs_dir_lookup(&fs, &root, name, name_length(name), &ino);
    rc = rc != KERR_OK ? rc : xfs_inode_read(&fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_file_read(&fs, &ip, off, buf, len, &got);
    return rc != KERR_OK ? rc : (got == len ? KERR_OK : KERR_IO);
}

int xfsmount_link(const char *existing, const char *newname) {
    struct xfs_inode root;
    uint64_t ino = 0;
    int rc = root_inode(&root);
    rc = rc != KERR_OK ? rc : xfs_dir_lookup(&fs, &root, existing, name_length(existing), &ino);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_link(&fs, &root, newname, name_length(newname), ino, now_seconds());
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_rename(const char *from, const char *to) {
    struct xfs_inode root;
    int rc = root_inode(&root);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_rename(&fs, &root, from, name_length(from), &root, to, name_length(to),
                    now_seconds());
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_unlink(const char *name) {
    struct xfs_inode root;
    int rc = root_inode(&root);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_unlink(&fs, &root, name, name_length(name), now_seconds());
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_inode_of(const char *name, uint64_t *ino, uint32_t *nlink) {
    struct xfs_inode root, ip;
    int rc = root_inode(&root);
    rc = rc != KERR_OK ? rc : xfs_dir_lookup(&fs, &root, name, name_length(name), ino);
    rc = rc != KERR_OK ? rc : xfs_inode_read(&fs, *ino, &ip);
    if (rc == KERR_OK) {
        *nlink = ip.nlink;
    }
    return rc;
}

void xfsmount_root_shape(void) {
    struct xfs_inode root;
    int rc = root_inode(&root);
    if (rc != KERR_OK) {
        kprintf("xfs: the root will not read (rc %d)\n", rc);
        return;
    }
    kprintf("xfs: root mode %u format %u size %llu nextents %u nblocks %llu\n", root.mode,
            root.format, (unsigned long long)root.size, root.nextents,
            (unsigned long long)root.nblocks);
}
