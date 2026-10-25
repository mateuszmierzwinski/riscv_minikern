#include "kernel/xfsmount.h"

#include "kernel/wallclock.h"

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

/* The wall-clock port, defined below beside the functions it holds. The
 * mount runs above them, and a forward declaration is cheaper than moving a
 * port's definition away from its documentation. */
const struct xfs_clock *xfsmount_clock(void);
static bool mounted;

/* **Policy, which must survive a re-mount; the filesystem's state must
 * not.** `xfsmount_remount` zeroes the whole `struct xfs` so that the
 * filesystem is read as a fresh boot would read it — and that wiped these
 * two along with it, including the one that was supposed to govern the very
 * mount being started. The control that proves the read-only fallback still
 * works was the thing that found it: the knob was set, cleared by the
 * zeroing, and replay ran anyway.
 *
 * They are both test instruments (`-refuse-replay`, `-leave-log-dirty`),
 * which is why they live here rather than being passed to `xfs_mount`: a
 * mount argument for something no real caller sets would be worse. */
static bool policy_no_replay;
static bool policy_skip_unmount;

int xfsmount_attach(void) {
    struct bcache *cache = disk_cache();
    int rc;
    if (cache == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_mount(&fs, cache);
    /* **Installed right after the mount**, before anything can charge a
     * quota — a port installed late is a port that was not installed, which
     * this tree has paid for twice. */
    xfs_set_clock(&fs, xfsmount_clock());
    if (rc != KERR_OK) {
        return rc;
    }
    mounted = true;
    /* **Into the table, pointed at rather than mounted again.** This mount
     * is the root filesystem and `volumes_automount` deliberately skips the
     * candidate it came from, so this is what puts it where its label asks
     * — `/` for `ROOT`, `/volumes/<label>` otherwise. Without it the table
     * would describe every filesystem on the machine except the one the
     * machine is running from. */
    volumes_register(&fs, 0);
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
    /* `xfs_mount` resets the struct itself, keeping exactly the two policy
     * flags set here — so zeroing it a second time from this side would
     * only wipe them again, which is what it used to do. */
    fs.no_replay = policy_no_replay;
    fs.skip_unmount_record = policy_skip_unmount;
    rc = xfs_mount(&fs, cache);
    mounted = rc == KERR_OK;
    return rc;
}

void xfsmount_leave_log_dirty(bool on) {
    policy_skip_unmount = on;
    fs.skip_unmount_record = on;
}

bool xfsmount_log_was_dirty(void) {
    return fs.log_was_dirty;
}

bool xfsmount_log_replayed(void) {
    return fs.log_replayed;
}

void xfsmount_refuse_replay(bool on) {
    policy_no_replay = on;
    fs.no_replay = on;
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

int xfsmount_stat_full(const char *path, uint32_t *mode, uint64_t *size, uint32_t *nlink) {
    struct xfs_inode ip;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(vol, within[0] == '\0' ? "/" : within, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    *mode = ip.mode;
    *size = ip.size;
    *nlink = ip.nlink;
    return KERR_OK;
}

int xfsmount_list(const char *path, xfs_dirent_fn fn, void *self) {
    struct xfs_inode dir;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(vol, within[0] == '\0' ? "/" : within, &dir);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!xfs_is_dir(&dir)) {
        return KERR_NOTDIR;
    }
    return xfs_dir_list(vol, &dir, fn, self);
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

void xfsmount_growth_counts(uint64_t *to_block, uint64_t *to_leaf, uint64_t *to_node,
                            uint64_t *data_blocks) {
    *to_block = fs.grew_to_block;
    *to_leaf = fs.grew_to_leaf;
    *to_node = fs.grew_to_node;
    *data_blocks = fs.dir_blocks_added;
}

uint64_t xfsmount_leaf_splits(void) {
    return fs.leaf_splits;
}

uint64_t xfsmount_attrs_to_leaf(void) {
    return fs.attrs_to_leaf;
}

/* ── writing ───────────────────────────────────────────────────────────── */

/* What goes in a timestamp, given that this board has no clock.
 *
 * **The clock arrived, and this is the line that changed.** It used to read
 * seconds since boot, with a note that the board has no RTC and that files
 * would therefore be dated 1970 — true, and it stopped being the only
 * option. The board has no clock of its own, but the network has one, and
 * `minikern.time=<unix seconds>` sets the epoch at boot; the monotonic
 * counter carries it from there. Without that option this still returns
 * seconds since boot, and `wallclock_is_real()` is how a caller that must
 * not act on a guessed date tells which it has. */
static uint64_t now_seconds(void) {
    return wallclock_seconds();
}

/* The filesystem's wall-clock port.
 *
 * Two functions rather than one, because the filesystem needs to know not
 * only what time it is but **whether to believe it**: a quota's grace
 * deadline is a wall-clock time written to disk, and comparing it against
 * seconds-since-boot would expire every grace period instantly on a board
 * with no real clock. */
static uint64_t clock_seconds(void *self) {
    (void)self;
    return wallclock_seconds();
}

static bool clock_is_real(void *self) {
    (void)self;
    return wallclock_is_real();
}

static const struct xfs_clock the_clock = {
    .seconds = clock_seconds,
    .is_real = clock_is_real,
    .self = NULL,
};

const struct xfs_clock *xfsmount_clock(void) {
    return &the_clock;
}

/* The mounted filesystem, for reporting things only it knows. Read-only to
 * the caller by convention: nothing outside this file mounts or unmounts.
 *
 * (This was inserted between `now_seconds` and its own documentation, which
 * left that comment describing the wrong function. Comments belong to the
 * thing below them, and an insertion has to look at what it is splitting.) */
const struct xfs *xfsmount_fs(void) {
    return &fs;
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
        /* **Say how short.** "A short write with no reason" was the whole
         * of this message, and a reader who meets it learns only that the
         * numbers disagreed — not which way, which is the first thing that
         * narrows it. */
        kprintf("xfs: short write to %s: %u of %u byte(s) at %llu\n", name, put, len,
                (unsigned long long)off);
        return KERR_IO;
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

/* The inode a name in the root directory refers to, for the attribute
 * calls below — all of which act on an inode rather than a path, because
 * that is the shape the filesystem's own interface takes. */
static int inode_for(const char *name, struct xfs_inode *ip) {
    struct xfs_inode root;
    uint64_t ino = 0;
    int rc = root_inode(&root);
    rc = rc != KERR_OK ? rc : xfs_dir_lookup(&fs, &root, name, name_length(name), &ino);
    return rc != KERR_OK ? rc : xfs_inode_read(&fs, ino, ip);
}

int xfsmount_attr_set(const char *name, const char *attr, const void *value,
                      uint32_t valuelen) {
    struct xfs_inode ip;
    int rc = inode_for(name, &ip);
    rc = rc != KERR_OK ? rc
                       : xfs_attr_set(&fs, &ip, attr, name_length(attr), value, valuelen,
                                      now_seconds());
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_attr_get(const char *name, const char *attr, void *out, uint32_t room,
                      uint32_t *valuelen) {
    struct xfs_inode ip;
    int rc = inode_for(name, &ip);
    return rc != KERR_OK ? rc
                         : xfs_attr_get(&fs, &ip, attr, name_length(attr), out, room,
                                        valuelen);
}

int xfsmount_attr_remove(const char *name, const char *attr) {
    struct xfs_inode ip;
    int rc = inode_for(name, &ip);
    rc = rc != KERR_OK ? rc
                       : xfs_attr_remove(&fs, &ip, attr, name_length(attr), now_seconds());
    return rc != KERR_OK ? rc : xfs_flush(&fs);
}

int xfsmount_attr_list(const char *name, xfs_attr_fn fn, void *self) {
    struct xfs_inode ip;
    int rc = inode_for(name, &ip);
    return rc != KERR_OK ? rc : xfs_attr_list(&fs, &ip, fn, self);
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

/* Removes a name anywhere, not only in the root.
 *
 * `xfsmount_unlink` takes a name and resolves it in `/`, which is all the
 * write self-test ever needed. A path needs its parent found first, so the
 * final component is split off and the rest is looked up — and the split is
 * on the **last** separator, because every component before it is the
 * directory.
 *
 * **It removes a broken directory too.** `/var` on the operator's card has
 * mode 0 — the inode's mode was never written while its parent's entry says
 * it is a directory — so `xfs_is_dir` says no and `xfs_unlink` will take it,
 * which is exactly what is wanted for clearing it. A *correct* directory is
 * refused by `xfs_unlink` with `EISDIR`, which is right: removing one is
 * `rmdir`, and this is not it. */
int xfsmount_unlink_path(const char *path) {
    struct xfs_inode dir;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    char parent[128];
    uint32_t cut = 0, n = 0;
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    for (uint32_t i = 0; within[i] != '\0'; i++) {
        if (within[i] == '/') {
            cut = i;
        }
        n = i + 1u;
    }
    if (n == 0 || within[n - 1u] == '/') {
        return KERR_INVAL; /* no final component to remove */
    }
    if (cut + 1u >= sizeof parent) {
        return KERR_NAMETOOLONG;
    }
    for (uint32_t i = 0; i < cut; i++) {
        parent[i] = within[i];
    }
    parent[cut] = '\0';
    rc = xfs_path_lookup(vol, cut == 0 ? "/" : parent, &dir);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_unlink(vol, &dir, within + cut + 1u, n - cut - 1u, now_seconds());
    return rc != KERR_OK ? rc : xfs_flush(vol);
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

/* Creates `leaf` inside the directory `dir` names, and writes `bytes` into
 * it — the operation the kernel log needs to put a boot on disk.
 *
 * **It will not make the directory.** This writer creates files, and
 * `mkdir` is not implemented: a directory needs its own shortform fork
 * written, its parent's link count raised and `..` pointed back, and that
 * is a separate piece of work from what is here. So a missing `/var/log` is
 * reported rather than worked around — writing the log to the root
 * directory instead would put kernel files where a user's are, which is a
 * worse answer than saying where it would have gone.
 */
/* Makes `path` a directory, and every directory above it.
 *
 * **Each component in turn, because `mkdir` makes one.** `/var/log` on a
 * filesystem with neither needs two, and a caller that had to know that
 * would be doing this loop itself. An existing component is not an error —
 * that is what makes this safe to call on every boot. */
int xfsmount_mkdir_p(const char *path) {
    struct xfs_inode here;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    char so_far[128];
    uint32_t at = 0;
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    while (within[at] != '\0') {
        uint32_t start;
        uint32_t n = 0;
        uint64_t ino = 0;
        while (within[at] == '/') {
            at++;
        }
        start = at;
        while (within[at + n] != '\0' && within[at + n] != '/') {
            n++;
        }
        if (n == 0) {
            break;
        }
        at += n;
        if (start + n >= sizeof so_far) {
            return KERR_NAMETOOLONG;
        }
        /* The path so far, so that the parent can be looked up by name
         * rather than carried as an inode across a creation that may have
         * rewritten it. */
        for (uint32_t i = 0; i < n; i++) {
            so_far[i] = within[start + i];
        }
        so_far[n] = '\0';
        {
            char full[160];
            uint32_t k = 0;
            for (uint32_t i = 0; i < start && k + 1u < sizeof full; i++) {
                full[k++] = within[i];
            }
            for (uint32_t i = 0; i < n && k + 1u < sizeof full; i++) {
                full[k++] = within[start + i];
            }
            full[k] = '\0';
            if (xfs_path_lookup(vol, full, &here) == KERR_OK) {
                /* **Already there — but it has to be a directory.** This
                 * said "already there, which is not a failure" and did not
                 * look at *what* was there, so a regular file in the place
                 * of a directory was skipped over and the next component
                 * failed with `ENOTDIR` — an error about the parent,
                 * reported against the child. */
                if (!xfs_is_dir(&here)) {
                    return KERR_NOTDIR;
                }
                continue;
            }
            /* The parent is everything before this component, **without the
             * separator**.
             *
             * The NUL went *at* `start`, which is the first character of the
             * component — so the separator before it survived and the parent
             * of `/var/log` was looked up as `"/var/"`. The root case has to
             * stay `"/"`, which is why the index is clamped at 1 rather than
             * simply decremented: a path's leading slash is the only one
             * that is part of the name. */
            full[start > 1u ? start - 1u : 1u] = '\0';
            rc = xfs_path_lookup(vol, start == 0 ? "/" : full, &here);
            if (rc != KERR_OK) {
                return rc;
            }
            rc = xfs_mkdir(vol, &here, so_far, n, 0, &ino);
            if (rc != KERR_OK) {
                /* **Which component, and what the parent looked like.** The
                 * caller only ever saw the whole path and one errno, so
                 * three board runs went into narrowing "`/var/log` cannot
                 * be made (rc -20)" by elimination. The parent's mode and
                 * format are what the refusal is actually about. */
                kprintf("mkdir: \"%s\" under parent mode %06o format %u: rc %d\n", so_far,
                        here.mode, here.format, rc);
                return rc;
            }
        }
    }
    return xfs_flush(vol);
}

/* Writes `len` bytes at `off` into `dir/leaf`, creating the file if it is
 * not there.
 *
 * **Write-through, so a descriptor does not have to hold the whole file.**
 * The first writable `openat` buffered everything and committed on close,
 * which meant a 16 MiB allocation per open descriptor — absurd for pushing a
 * binary, and it wedged the service after one file. An offset-taking writer
 * already existed; it was only root-relative, which is why it was not used.
 *
 * `put != len` is reported as an I/O error **with the numbers**, because a
 * short write that says only "it failed" leaves the next reader unable to
 * tell a full disk from a bad offset. */
/* The leaf's inode number inside `dir`, created if it is not there.
 *
 * **Its own frame, never inlined**, because it holds the directory's inode
 * and its caller holds the file's: two on one frame is 960 bytes before any
 * locals, and the limit every function here is held to is 1 KiB. This is the
 * fourth place in this tree where that split was needed, and the rule has
 * settled into a shape — a function holds *one* inode and hands its caller a
 * number. */
__attribute__((noinline)) static int find_or_make_leaf(struct xfs *vol, const char *within,
                                                        const char *leaf, uint32_t leaflen,
                                                        uint64_t *ino) {
    struct xfs_inode parent;
    int rc = xfs_path_lookup(vol, within[0] == '\0' ? "/" : within, &parent);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!xfs_is_dir(&parent)) {
        return KERR_NOTDIR;
    }
    rc = xfs_dir_lookup(vol, &parent, leaf, leaflen, ino);
    if (rc == KERR_NOENT) {
        rc = xfs_create(vol, &parent, leaf, leaflen, 0100644u, now_seconds(), ino);
    }
    return rc;
}

int xfsmount_write_at(const char *dir, const char *leaf, uint64_t off, const void *bytes,
                      uint32_t len) {
    struct xfs_inode file;
    uint64_t ino = 0;
    uint32_t put = 0;
    const char *within = dir;
    struct xfs *vol = owner_of(dir, &within);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = find_or_make_leaf(vol, within, leaf, name_length(leaf), &ino);
    rc = rc != KERR_OK ? rc : xfs_inode_read(vol, ino, &file);
    if (rc != KERR_OK) {
        return rc;
    }
    if (len == 0) {
        return xfs_flush(vol); /* an empty file still exists */
    }
    rc = xfs_file_write(vol, &file, off, bytes, len, &put);
    if (rc == KERR_OK && put != len) {
        kprintf("xfs: short write to %s/%s: %u of %u byte(s) at %llu\n", dir, leaf, put, len,
                (unsigned long long)off);
        return KERR_IO;
    }
    return rc != KERR_OK ? rc : xfs_flush(vol);
}

int xfsmount_write_into(const char *dir, const char *leaf, const void *bytes, uint32_t len) {
    struct xfs_inode parent, file;
    uint64_t ino = 0;
    uint32_t put = 0;
    const char *within = dir;
    struct xfs *vol = owner_of(dir, &within);
    uint32_t leaflen = name_length(leaf);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(vol, within, &parent);
    if (rc != KERR_OK) {
        return rc; /* the directory is not there: the caller says so */
    }
    if (!xfs_is_dir(&parent)) {
        return KERR_NOTDIR;
    }
    rc = xfs_dir_lookup(vol, &parent, leaf, leaflen, &ino);
    if (rc == KERR_NOENT) {
        rc = xfs_create(vol, &parent, leaf, leaflen, 0100644u, 0, &ino);
    }
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_inode_read(vol, ino, &file);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_file_write(vol, &file, 0, bytes, len, &put);
    if (rc == KERR_OK && put != len) {
        return KERR_IO;
    }
    return rc != KERR_OK ? rc : xfs_flush(vol);
}

/* ── symbolic links ────────────────────────────────────────────────────── */

/* Splits a path into the directory holding it and the final component.
 *
 * Into a caller-provided buffer rather than returning a pointer, because the
 * directory part is a *prefix* of the path and there is nowhere to put the
 * terminator without writing into the caller's string. A path with no slash
 * belongs to the root of its volume, which is the case `"/"` covers.
 */
static int split_parent(const char *within, char *dir, uint32_t dirmax, const char **leaf) {
    uint32_t last = 0;
    uint32_t n = 0;
    bool found = false;
    for (n = 0; within[n] != '\0'; n++) {
        if (within[n] == '/') {
            last = n;
            found = true;
        }
    }
    if (!found || within[last + 1] == '\0') {
        return KERR_INVAL; /* no leaf to create */
    }
    if (last == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    } else {
        if (last >= dirmax) {
            return KERR_NAMETOOLONG;
        }
        xfs_copy_bytes((uint8_t *)dir, (const uint8_t *)within, last);
        dir[last] = '\0';
    }
    *leaf = within + last + 1;
    return KERR_OK;
}

int xfsmount_symlink(const char *path, const char *target) {
    struct xfs_inode dir;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    char dirpath[128];
    const char *leaf = NULL;
    uint32_t namelen = 0;
    uint32_t targetlen = 0;
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = split_parent(within, dirpath, sizeof dirpath, &leaf);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_path_lookup(vol, dirpath, &dir);
    if (rc != KERR_OK) {
        return rc;
    }
    while (leaf[namelen] != '\0') {
        namelen++;
    }
    while (target[targetlen] != '\0') {
        targetlen++;
    }
    return xfs_symlink(vol, &dir, leaf, namelen, target, targetlen, now_seconds(), NULL);
}

int xfsmount_readlink(const char *path, char *out, uint32_t max, uint32_t *got) {
    struct xfs_inode ip;
    const char *within = path;
    struct xfs *vol = owner_of(path, &within);
    int rc;
    if (vol == NULL) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(vol, within[0] == '\0' ? "/" : within, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **The check is the point of a separate call.** `readlink` on anything
     * that is not a link is `EINVAL`, and a caller that got the file's
     * contents instead would have no way to tell a symlink to "hello" from
     * a file containing the word. */
    if (!xfs_is_link(&ip)) {
        return KERR_INVAL;
    }
    *got = 0;
    return xfs_file_read(vol, &ip, 0, (uint8_t *)out, max, got);
}

/* The existing file's inode number, on a frame of its own.
 *
 * `noinline` load-bearingly: a `struct xfs_inode` is large, and two of them
 * on one frame is 1136 bytes against the kernel's 1024-byte limit. Inlining
 * this would put them back together and the guard would refuse the build —
 * which is the guard working, and the fifth time this tree has answered it
 * the same way. */
static __attribute__((noinline)) int ino_of(struct xfs *vol, const char *within, uint64_t *ino) {
    struct xfs_inode ip;
    int rc = xfs_path_lookup(vol, within, &ip);
    if (rc == KERR_OK) {
        *ino = ip.ino;
    }
    return rc;
}

int xfsmount_link_path(const char *from, const char *to) {
    uint64_t existing = 0;
    struct xfs_inode dir;
    const char *fromwithin = from;
    const char *towithin = to;
    struct xfs *fromvol = owner_of(from, &fromwithin);
    struct xfs *tovol = owner_of(to, &towithin);
    char dirpath[128];
    const char *leaf = NULL;
    uint32_t namelen = 0;
    int rc;
    if (fromvol == NULL || tovol == NULL) {
        return KERR_NODEV;
    }
    /* **A hard link cannot cross a volume**, because the second name is an
     * inode number and an inode number means nothing outside the
     * filesystem that issued it. EXDEV is what the caller expects. */
    if (fromvol != tovol) {
        return KERR_XDEV;
    }
    rc = ino_of(fromvol, fromwithin, &existing);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = split_parent(towithin, dirpath, sizeof dirpath, &leaf);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_path_lookup(tovol, dirpath, &dir);
    if (rc != KERR_OK) {
        return rc;
    }
    while (leaf[namelen] != '\0') {
        namelen++;
    }
    return xfs_link(tovol, &dir, leaf, namelen, existing, now_seconds());
}

uint32_t xfsmount_symlink_limit(void) {
    const struct xfs *vol = xfsmount_fs();
    return vol == NULL ? 0 : xfs_symlink_max(vol);
}

/* Moves a name, which is what log rotation is made of.
 *
 * `xfs_rename` has been here since the write path was tested on the board;
 * like hard links it was simply never exposed to a program. Rotation needs
 * exactly this and nothing else: there is no `ftruncate` either, so moving
 * the file aside is the only mechanism a log can use to stay bounded.
 *
 * Two directory inodes are live at once here, which the frame guard would
 * refuse at 1136 bytes, so each lookup stands on its own frame through
 * `ino_of`-style helpers. The source directory is resolved to an inode
 * because `xfs_rename` needs it; the destination's likewise.
 */
static __attribute__((noinline)) int dir_and_leaf(struct xfs *vol, const char *within, char *dir,
                                                  uint32_t dirmax, const char **leaf,
                                                  uint32_t *namelen) {
    int rc = split_parent(within, dir, dirmax, leaf);
    if (rc != KERR_OK) {
        return rc;
    }
    *namelen = 0;
    while ((*leaf)[*namelen] != '\0') {
        (*namelen)++;
    }
    (void)vol;
    return KERR_OK;
}

/* The two directory inodes, on a frame of their own for the frame guard's
 * sake — two of them plus the caller's buffers is past the kernel's 1024
 * bytes, and `noinline` is what keeps them from being summed back together. */
static __attribute__((noinline)) int rename_resolved(struct xfs *vol, const char *fromdir,
                                                     const char *fromleaf, uint32_t fromlen,
                                                     const char *todir, const char *toleaf,
                                                     uint32_t tolen) {
    struct xfs_inode olddir;
    struct xfs_inode newdir;
    int rc = xfs_path_lookup(vol, fromdir, &olddir);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_path_lookup(vol, todir, &newdir);
    if (rc != KERR_OK) {
        return rc;
    }
    return xfs_rename(vol, &olddir, fromleaf, fromlen, &newdir, toleaf, tolen, now_seconds());
}

int xfsmount_rename_path(const char *from, const char *to) {
    const char *fromwithin = from;
    const char *towithin = to;
    struct xfs *fromvol = owner_of(from, &fromwithin);
    struct xfs *tovol = owner_of(to, &towithin);
    char fromdir[128];
    char todir[128];
    const char *fromleaf = NULL;
    const char *toleaf = NULL;
    uint32_t fromlen = 0;
    uint32_t tolen = 0;
    int rc;
    if (fromvol == NULL || tovol == NULL) {
        return KERR_NODEV;
    }
    if (fromvol != tovol) {
        return KERR_XDEV; /* a rename is not a copy */
    }
    rc = dir_and_leaf(fromvol, fromwithin, fromdir, sizeof fromdir, &fromleaf, &fromlen);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = dir_and_leaf(tovol, towithin, todir, sizeof todir, &toleaf, &tolen);
    if (rc != KERR_OK) {
        return rc;
    }
    return rename_resolved(fromvol, fromdir, fromleaf, fromlen, todir, toleaf, tolen);
}
