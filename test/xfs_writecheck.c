/* xfs_writecheck — applies this kernel's XFS writes to an image `mkfs.xfs`
 * made, so that `xfs_repair -n` can judge them (test/xfs_write_test.sh).
 *
 * The host tests build their filesystems by hand, which proves the writer
 * agrees with this project's reading of the format. Only the vendor's checker
 * proves it agrees with the format, and it needs a real filesystem: so this
 * mounts the image file, does one of everything the writer does, flushes,
 * and leaves the verdict to `xfs_repair`.
 *
 * Usage: xfs_writecheck <image> [step...]; every step when none is named.
 * Exits 0 when every operation succeeded. */
#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "core/xfs/xfs_internal.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BS 4096u
#define SLOTS 64u
#define NOW 1761609600u /* 2025-10-28, any fixed instant */

static int image_fd = -1;
static int rt_fd = -1; /* the realtime device's image, when XFS_WRITECHECK_RT names one */

static void must(int rc, const char *what);
static uint8_t cache_mem[SLOTS * BS];
static uint8_t payload[4u * 1024u * 1024u];

static int img_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    ssize_t n = pread(image_fd, buf, (size_t)count * BS, (off_t)(lba * BS));
    return n == (ssize_t)((size_t)count * BS) ? KERR_OK : KERR_IO;
}

static int img_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    ssize_t n = pwrite(image_fd, buf, (size_t)count * BS, (off_t)(lba * BS));
    return n == (ssize_t)((size_t)count * BS) ? KERR_OK : KERR_IO;
}

static int img_flush(void *self) {
    (void)self;
    return fsync(image_fd) == 0 ? KERR_OK : KERR_IO;
}

static int rt_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    ssize_t n = pread(rt_fd, buf, (size_t)count * BS, (off_t)(lba * BS));
    return n == (ssize_t)((size_t)count * BS) ? KERR_OK : KERR_IO;
}

static int rt_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    ssize_t n = pwrite(rt_fd, buf, (size_t)count * BS, (off_t)(lba * BS));
    return n == (ssize_t)((size_t)count * BS) ? KERR_OK : KERR_IO;
}

static int rt_flush(void *self) {
    (void)self;
    return fsync(rt_fd) == 0 ? KERR_OK : KERR_IO;
}

/* Attaches the realtime device XFS_WRITECHECK_RT names, if any. */
static void attach_realtime(struct xfs *fs) {
    static uint8_t rt_mem[16u * BS];
    static struct blockdev rtdev;
    static struct bcache rtcache;
    struct stat st;
    const char *path = getenv("XFS_WRITECHECK_RT");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    rt_fd = open(path, O_RDWR);
    if (rt_fd < 0 || fstat(rt_fd, &st) != 0) {
        perror(path);
        exit(2);
    }
    rtdev = (struct blockdev){.read = rt_read, .write = rt_write, .flush = rt_flush,
                              .block_size = BS, .blocks = (uint64_t)st.st_size / BS};
    must(bcache_init(&rtcache, &rtdev, rt_mem, 16u), "bcache_init (realtime)");
    must(xfs_attach_rt(fs, &rtcache), "attaching the realtime device");
}

/* A discard poisons the blocks rather than zeroing them: this device says
 * nothing of what a discarded block reads as, so a file or a tree block
 * trimmed by mistake comes back as 0xdb — which `must_match` and
 * `xfs_repair` both see — rather than as plausible zeroes. */
static uint64_t discarded_blocks;

static int img_discard(void *self, uint64_t lba, uint32_t count) {
    static uint8_t poison[BS];
    (void)self;
    memset(poison, 0xdb, sizeof poison);
    for (uint32_t i = 0; i < count; i++) {
        if (pwrite(image_fd, poison, BS, (off_t)((lba + i) * BS)) != (ssize_t)BS) {
            return KERR_IO;
        }
    }
    discarded_blocks += count;
    return KERR_OK;
}

/* The kernel's own messages, which say why a step refused. */
static void to_stderr(void *self, const char *buf, size_t len) {
    (void)self;
    (void)fwrite(buf, 1, len, stderr);
}

static const struct console_port console = {.write = to_stderr, .self = NULL};

/* Stops at the first failure, naming the step: the point is the image
 * `xfs_repair` is shown afterwards, and one built past a failed step is not
 * the one the steps describe. */
static void must(int rc, const char *what) {
    if (rc != KERR_OK) {
        fprintf(stderr, "xfs_writecheck: %s failed: %d\n", what, rc);
        exit(1);
    }
}

static void read_inode(struct xfs *fs, uint64_t ino, struct xfs_inode *out) {
    must(xfs_inode_read(fs, ino, out), "reading an inode back");
}

/* A file of `len` bytes, written in `chunk`-byte pieces so that the file
 * grows block by block the way a program's writes make it grow. */
static uint64_t make_file(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                          uint32_t len, uint32_t chunk) {
    struct xfs_inode ip;
    uint64_t ino;
    uint32_t put;
    must(xfs_create(fs, dir, name, (uint32_t)strlen(name), 0100644, NOW, &ino), name);
    read_inode(fs, ino, &ip);
    for (uint32_t off = 0; off < len; off += chunk) {
        uint32_t n = len - off < chunk ? len - off : chunk;
        must(xfs_file_write(fs, &ip, off, payload + off, n, &put), "a file write");
        read_inode(fs, ino, &ip);
    }
    return ino;
}

/* Enough names to push a directory from shortform through block and leaf
 * format, each of which allocates directory blocks of its own. */
static void fill_directory(struct xfs *fs, uint64_t dirino, uint32_t names) {
    char name[48];
    for (uint32_t i = 0; i < names; i++) {
        struct xfs_inode dir;
        read_inode(fs, dirino, &dir);
        snprintf(name, sizeof name, "entry-with-a-long-name-%04u", i);
        (void)make_file(fs, &dir, name, i % 7u == 0 ? 5000u : 0u, 4096u);
    }
}

/* The steps, each one kind of write, so that a damaged image can be traced
 * to the step that damaged it by running that step alone. */
static void step_files(struct xfs *fs) {
    struct xfs_inode root;
    read_inode(fs, fs->rootino, &root);
    (void)make_file(fs, &root, "small", 100u, 100u);
    read_inode(fs, fs->rootino, &root);
    (void)make_file(fs, &root, "large", sizeof payload, 8192u);
    read_inode(fs, fs->rootino, &root);
    (void)make_file(fs, &root, "doomed", 40000u, 4096u);
}

static uint64_t lookup(struct xfs *fs, const char *path) {
    struct xfs_inode ip;
    must(xfs_path_lookup(fs, path, &ip), path);
    return ip.ino;
}

static void step_dir(struct xfs *fs) {
    struct xfs_inode root;
    uint64_t subino;
    read_inode(fs, fs->rootino, &root);
    must(xfs_mkdir(fs, &root, "sub", 3u, NOW, &subino), "mkdir");
    fill_directory(fs, subino, 300u);
}

static void step_many(struct xfs *fs) {
    fill_directory(fs, fs->rootino, 200u);
}

static void step_truncate(struct xfs *fs) {
    struct xfs_inode ip;
    read_inode(fs, lookup(fs, "/large"), &ip);
    must(xfs_truncate(fs, &ip, 50000u, NOW), "truncate");
}

static void step_unlink(struct xfs *fs) {
    struct xfs_inode root;
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "doomed", 6u, NOW), "unlink");
}

static void step_attr(struct xfs *fs) {
    struct xfs_inode ip;
    uint8_t big[3000];
    uint64_t ino = lookup(fs, "/small");
    read_inode(fs, ino, &ip);
    must(xfs_attr_set(fs, &ip, "user.short", 10u, (const uint8_t *)"v", 1u, NOW), "attr");
    memset(big, 'x', sizeof big);
    read_inode(fs, ino, &ip);
    must(xfs_attr_set(fs, &ip, "user.big", 8u, big, sizeof big, NOW), "remote attr");
}

static void step_rename(struct xfs *fs) {
    struct xfs_inode root, sub;
    read_inode(fs, fs->rootino, &root);
    read_inode(fs, lookup(fs, "/sub"), &sub);
    must(xfs_rename(fs, &root, "small", 5u, &sub, "moved", 5u, NOW), "rename");
}

static void step_symlink(struct xfs *fs) {
    struct xfs_inode root;
    uint64_t ino;
    read_inode(fs, fs->rootino, &root);
    must(xfs_symlink(fs, &root, "link", 4u, "sub/moved", 9u, NOW, &ino), "symlink");
}

/* Reads a file back whole and compares it with what was written. */
static void must_read_back(struct xfs *fs, uint64_t ino, uint32_t len) {
    static uint8_t got[sizeof payload];
    struct xfs_inode ip;
    uint32_t n = 0;
    read_inode(fs, ino, &ip);
    must(xfs_file_read(fs, &ip, 0, got, len, &n), "reading back");
    if (n != len || memcmp(got, payload, len) != 0) {
        fprintf(stderr, "xfs_writecheck: inode %llu read back wrong\n", (unsigned long long)ino);
        exit(1);
    }
}

/* A file that has an attribute in blocks of its own, then grows: its new
 * data must land where it was written, not past the attribute's blocks.
 * Then removed, which must free the attribute's blocks as well. */
static void step_attrfile(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint8_t big[3000];
    uint32_t put;
    uint64_t ino;
    read_inode(fs, fs->rootino, &root);
    ino = make_file(fs, &root, "attrfile", 4096u, 4096u);
    memset(big, 'y', sizeof big);
    read_inode(fs, ino, &ip);
    must(xfs_attr_set(fs, &ip, "user.big", 8u, big, sizeof big, NOW), "remote attr");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 4096u, payload + 4096u, 20000u, &put), "growing write");
    must_read_back(fs, ino, 24096u);
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "attrfile", 8u, NOW), "unlink with attributes");
}

/* Enough one-block files, each its own owner so no two records merge, to
 * grow the reverse-map tree past one leaf (168 records) into several and a
 * root above them; then most of them removed again, which has to borrow,
 * merge and finally collapse that tree. */
#define STRESS_FILES 1800u
#define STRESS_KEEP  18u /* every eighteenth file survives the drain */

static void stress_name(char *out, size_t room, uint32_t i) {
    snprintf(out, room, "f%05u", i);
}

static void step_fill(struct xfs *fs) {
    struct xfs_inode root, dir;
    uint64_t dirino;
    char name[16];
    read_inode(fs, fs->rootino, &root);
    must(xfs_mkdir(fs, &root, "stress", 6u, NOW, &dirino), "mkdir stress");
    for (uint32_t i = 0; i < STRESS_FILES; i++) {
        read_inode(fs, dirino, &dir);
        stress_name(name, sizeof name, i);
        (void)make_file(fs, &dir, name, 4096u, 4096u);
    }
}

static void step_drain(struct xfs *fs) {
    struct xfs_inode dir;
    uint64_t dirino = lookup(fs, "/stress");
    char name[16];
    for (uint32_t i = 0; i < STRESS_FILES; i++) {
        if (i % STRESS_KEEP == 0) {
            continue;
        }
        read_inode(fs, dirino, &dir);
        stress_name(name, sizeof name, i);
        must(xfs_unlink(fs, &dir, name, (uint32_t)strlen(name), NOW), "unlink stress");
    }
}

/* Free space cut into a thousand pieces: one-block files side by side, and
 * then every other one removed, so a group holds more separate free extents
 * than one leaf of either free-space tree can (505) — the trees have to
 * split on the way in and merge again as the survivors go. */
#define FRAGMENT_FILES 2400u

/* Removes every other file of the fragment set, from `first`. */
static void unlink_alternate(struct xfs *fs, uint32_t first) {
    struct xfs_inode dir;
    uint64_t dirino = lookup(fs, "/frag");
    char name[16];
    for (uint32_t i = first; i < FRAGMENT_FILES; i += 2u) {
        read_inode(fs, dirino, &dir);
        snprintf(name, sizeof name, "g%05u", i);
        must(xfs_unlink(fs, &dir, name, (uint32_t)strlen(name), NOW), "unlink frag");
    }
}

static void step_fragment(struct xfs *fs) {
    struct xfs_inode root, dir;
    uint64_t dirino;
    char name[16];
    read_inode(fs, fs->rootino, &root);
    must(xfs_mkdir(fs, &root, "frag", 4u, NOW, &dirino), "mkdir frag");
    for (uint32_t i = 0; i < FRAGMENT_FILES; i++) {
        read_inode(fs, dirino, &dir);
        snprintf(name, sizeof name, "g%05u", i);
        (void)make_file(fs, &dir, name, 4096u, 4096u);
    }
    unlink_alternate(fs, 0);
}

/* The other half, which joins every hole back into a run. */
static void step_defragment(struct xfs *fs) {
    unlink_alternate(fs, 1u);
}

/* A file that has to become a b-tree: written after `fragment`, so each of
 * its blocks lands in a one-block hole and is an extent of its own — far
 * more than the inode holds. Read back whole, cut to a quarter and read
 * again, then a second one written and removed, which must give back both
 * its blocks and its tree's leaves. */
#define BTREE_FILE_BYTES (4u * 1024u * 1024u)

/* The b-tree step on inodes over 512 bytes, whose data fork is larger than
 * this reader caches: a file may not go past the extents that cache holds
 * (`xfs_data_extents_cap`), so a fragmenting write must stop with
 * KERR_NOTSUP — before allocating, or `xfs_repair` finds the block it took
 * — and what was written must read back. The file is then removed. */
static void btree_refused(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint64_t ino, done = 0;
    uint32_t put = 0;
    int rc = KERR_OK;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "fragmented", 10u, 0100644, NOW, &ino), "create fragmented");
    for (; rc == KERR_OK && done < BTREE_FILE_BYTES; done += put) {
        read_inode(fs, ino, &ip);
        put = 0;
        rc = xfs_file_write(fs, &ip, done, payload + done % (sizeof payload - 65536u), 65536u, &put);
    }
    if (rc != KERR_OK && rc != KERR_NOTSUP) {
        fprintf(stderr, "xfs_writecheck: a fragmenting write answered %d, want ok or %d\n", rc,
                KERR_NOTSUP);
        exit(1);
    }
    printf("xfs_writecheck: large inodes: %llu bytes written before %s\n",
           (unsigned long long)done, rc == KERR_NOTSUP ? "the extent cap" : "the end");
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "fragmented", 10u, NOW), "unlink fragmented");
}

static void step_btreefile(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint64_t ino;
    if (fs->inodesize - fs->dinode_size > XFS_FORK_MAX) {
        btree_refused(fs);
        return;
    }
    read_inode(fs, fs->rootino, &root);
    ino = make_file(fs, &root, "fragmented", BTREE_FILE_BYTES, 65536u);
    read_inode(fs, ino, &ip);
    printf("xfs_writecheck: the fragmented file has %u extents, format %u\n", ip.nextents,
           ip.format);
    must_read_back(fs, ino, BTREE_FILE_BYTES);
    must(xfs_truncate(fs, &ip, BTREE_FILE_BYTES / 4u, NOW), "truncate a b-tree file");
    must_read_back(fs, ino, BTREE_FILE_BYTES / 4u);
    read_inode(fs, fs->rootino, &root);
    (void)make_file(fs, &root, "fragmented2", BTREE_FILE_BYTES / 2u, 65536u);
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "fragmented2", 11u, NOW), "unlink a b-tree file");
}

/* A power cut straight after a commit: the transaction is in the log and
 * the unmount record is not, so the next mount finds the log dirty and
 * replays it — from the tail the newest record names, round the ring. The
 * mount here is the kernel's own, so the replay is too; what follows it is
 * `xfs_repair`'s verdict on the result. */
static void step_replay(struct xfs *fs) {
    static struct xfs again;
    struct xfs_inode root;
    read_inode(fs, fs->rootino, &root);
    fs->skip_unmount_record = true;
    (void)make_file(fs, &root, "after-the-crash", 9000u, 4096u);
    must(xfs_flush(fs), "flush before the crash");
    bcache_invalidate(fs->cache);
    must(xfs_mount(&again, fs->cache), "mount after the crash");
    if (!xfs_writable(&again)) {
        fprintf(stderr, "xfs_writecheck: the replayed filesystem is not writable\n");
        exit(1);
    }
    must_read_back(&again, lookup(&again, "/after-the-crash"), 9000u);
    *fs = again;
}

/* Logs a size change for `ino` WITHOUT writing it in place, committed or
 * not: after a crash the log is the only place the change exists, so what
 * the next mount does with it is the whole of what is being judged. */
static void log_size_only(struct xfs *fs, uint64_t ino, uint64_t size, bool committed) {
    static uint8_t core[2048];
    struct xfs_log_inode_item item = {.ino = ino, .core = core};
    struct bbuf *b;
    uint64_t block;
    xfs_inode_place(fs, ino, &block, &item.boffset);
    item.chunk_block = block;
    must(bcache_get(fs->cache, block, &b), "reading the inode to log");
    memcpy(core, b->data + item.boffset, fs->inodesize);
    bcache_put(fs->cache, b);
    xfs_put_be64(core + XFS_DI_SIZE, size);
    xfs_reseal(fs, core, fs->inodesize, XFS_DI_CRC);
    fs->skip_commit_record = !committed;
    must(xfs_log_inodes(fs, &item, 1u), "logging the size change");
}

static uint64_t size_of(struct xfs *fs, uint64_t ino) {
    struct xfs_inode ip;
    read_inode(fs, ino, &ip);
    return ip.size;
}

/* Logs a size change (not written in place), crashes, remounts, and
 * returns the file's size as the replayed filesystem sees it. */
static uint64_t crash_after_logging(struct xfs *fs, uint64_t ino, uint64_t size, bool committed) {
    static struct xfs again;
    uint64_t seen;
    must(xfs_flush(fs), "flush before the logging");
    fs->skip_unmount_record = true;
    log_size_only(fs, ino, size, committed);
    must(xfs_flush(fs), "flush before the crash");
    bcache_invalidate(fs->cache);
    must(xfs_mount(&again, fs->cache), "mount after the crash");
    seen = size_of(&again, ino);
    *fs = again;
    return seen;
}

/* A crash after a transaction was written to the log but before its
 * COMMIT_TRANS — a record cut off mid-transaction. Replay must leave that
 * change out; the same change committed must be applied, which is the
 * control that shows the first result is the commit's doing. One crash
 * each, so each time the transaction judged is the newest in the log. */
static void step_uncommitted(struct xfs *fs) {
    struct xfs_inode root;
    uint64_t cut, kept, kept_size, cut_size;
    read_inode(fs, fs->rootino, &root);
    cut = make_file(fs, &root, "cut-off", 9000u, 4096u);
    read_inode(fs, fs->rootino, &root);
    kept = make_file(fs, &root, "committed", 9000u, 4096u);
    kept_size = crash_after_logging(fs, kept, 4000u, true);
    cut_size = crash_after_logging(fs, cut, 1u, false);
    if (kept_size != 4000u || cut_size != 9000u) {
        fprintf(stderr, "xfs_writecheck: after replay the committed file is %llu bytes (want 4000), "
                "the cut-off one %llu (want 9000)\n", (unsigned long long)kept_size,
                (unsigned long long)cut_size);
        exit(1);
    }
    must(xfs_log_mark_clean(fs), "closing the log after the last replay");
}

/* Appends to one file a byte at a time — each a logged size change — until
 * the log's head has gone round the ring and some way past, so that records
 * were written across its end and the cycle moved on. `mkfs.xfs` will not
 * make a log under 64 MiB, so going round it takes tens of thousands of
 * these; a host run does them in seconds. The replay step after it then has
 * to find its tail behind the wrap. */
#define WRAP_PAST 200u /* records past the wrap, so the head is well clear of it */

static void step_wrap(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint32_t start = fs->log_cycle, past = 0, put;
    uint64_t ino;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "ring", 4u, 0100644, NOW, &ino), "ring");
    read_inode(fs, ino, &ip);
    for (uint32_t i = 0; past < WRAP_PAST; i++) {
        must(xfs_file_write(fs, &ip, i, payload + (i % sizeof payload), 1u, &put), "ring write");
        past += fs->log_cycle != start ? 1u : 0u;
    }
}

/* Reads back the board fixtures' files — `/p1` to `/pN`, each the 64 KiB
 * payload of bytes `i % 256` (test/hil/fixtures.py) — for an image made the
 * way the suite makes them. Reads only; a step for finding read bugs on the
 * host instead of on the board. */
static void step_fixture(struct xfs *fs) {
    static uint8_t got[65536];
    char path[16];
    for (uint32_t i = 1;; i++) {
        struct xfs_inode ip;
        uint32_t n = 0;
        snprintf(path, sizeof path, "/p%u", i);
        if (xfs_path_lookup(fs, path, &ip) != KERR_OK) {
            printf("xfs_writecheck: %u fixture files read back\n", i - 1u);
            return;
        }
        must(xfs_file_read(fs, &ip, 0, got, sizeof got, &n), path);
        for (uint32_t j = 0; j < n; j++) {
            if (got[j] != (uint8_t)j) {
                fprintf(stderr, "xfs_writecheck: %s differs at byte %u\n", path, j);
                exit(1);
            }
        }
    }
}

/* Removes every `/p<N>` an image was made with. `mkfs.xfs -p` fills its
 * chunks as Linux does, and a full chunk has **no** free-inode record — so
 * freeing an inode in one has to put a record back, which was refused
 * until the inode trees were whole B+trees. */
static void step_unlinkproto(struct xfs *fs) {
    char name[16];
    uint32_t removed = 0;
    for (uint32_t i = 1;; i++) {
        struct xfs_inode root;
        int rc;
        snprintf(name, sizeof name, "p%u", i);
        read_inode(fs, fs->rootino, &root);
        rc = xfs_unlink(fs, &root, name, (uint32_t)strlen(name), NOW);
        if (rc == KERR_NOENT) {
            break;
        }
        must(rc, "unlink a file mkfs made");
        removed++;
    }
    printf("xfs_writecheck: %u files mkfs made were removed\n", removed);
}

/* Enough inodes in one group to grow the inode tree past a leaf (252
 * chunks of 64), and then all of them removed again. */
#define MANY_INODES 17000u

static void step_inodes(struct xfs *fs) {
    struct xfs_inode root, dir;
    uint64_t dirino, ino;
    char name[16];
    read_inode(fs, fs->rootino, &root);
    must(xfs_mkdir(fs, &root, "ino", 3u, NOW, &dirino), "mkdir ino");
    for (uint32_t i = 0; i < MANY_INODES; i++) {
        read_inode(fs, dirino, &dir);
        snprintf(name, sizeof name, "i%05u", i);
        must(xfs_create(fs, &dir, name, (uint32_t)strlen(name), 0100644, NOW, &ino), name);
    }
    printf("xfs_writecheck: %u inodes made\n", MANY_INODES);
}

static void step_noinodes(struct xfs *fs) {
    struct xfs_inode dir;
    uint64_t dirino = lookup(fs, "/ino");
    char name[16];
    for (uint32_t i = 0; i < MANY_INODES; i++) {
        read_inode(fs, dirino, &dir);
        snprintf(name, sizeof name, "i%05u", i);
        must(xfs_unlink(fs, &dir, name, (uint32_t)strlen(name), NOW), "unlink ino");
    }
}

/* Compares part of a file with `want`. */
static void must_match(struct xfs *fs, uint64_t ino, uint64_t off, const uint8_t *want,
                       uint32_t len, const char *what) {
    static uint8_t got[1u << 20];
    struct xfs_inode ip;
    uint32_t n = 0;
    read_inode(fs, ino, &ip);
    must(xfs_file_read(fs, &ip, off, got, len, &n), what);
    if (n != len || memcmp(got, want, len) != 0) {
        fprintf(stderr, "xfs_writecheck: %s reads back wrong\n", what);
        exit(1);
    }
}

/* A reflink copy: a file made to share every block of another, as
 * `cp --reflink` makes one — and `xfs_repair` has to accept that structure
 * before anything is done to it. Then a write into the copy, which must
 * leave the original as it was; the copy cut short; the original removed,
 * which must leave the copy whole. */
#define REFLINK_BYTES 200000u

static void step_reflink(struct xfs *fs) {
    static const uint8_t patch[16] = "COPY-ON-WRITE!!";
    struct xfs_inode root, a, b;
    uint64_t orig, copy;
    uint32_t put;
    if (!fs->has_reflink) {
        return; /* a filesystem made without reflink has nothing to share with */
    }
    read_inode(fs, fs->rootino, &root);
    orig = make_file(fs, &root, "orig", REFLINK_BYTES, 65536u);
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "copy", 4u, 0100644, NOW, &copy), "create copy");
    read_inode(fs, orig, &a);
    read_inode(fs, copy, &b);
    must(xfs_reflink_clone(fs, &a, &b), "reflink clone");
    must_read_back(fs, copy, REFLINK_BYTES);
    read_inode(fs, copy, &b);
    must(xfs_file_write(fs, &b, 50000u, patch, sizeof patch, &put), "write into the copy");
    must_match(fs, copy, 50000u, patch, sizeof patch, "the written copy");
    must_read_back(fs, orig, REFLINK_BYTES); /* the original did not change */
    read_inode(fs, copy, &b);
    must(xfs_truncate(fs, &b, 120000u, NOW), "truncate the copy");
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "orig", 4u, NOW), "unlink the original");
    must_match(fs, copy, 0, payload, 50000u, "the copy's head");
    must_match(fs, copy, 50016u, payload + 50016u, 120000u - 50016u, "the copy's tail");
}

/* Holes: a write a megabyte past the end, one into the hole it left, and a
 * file made longer by truncate then written in the middle of the gap. */
static void step_holes(struct xfs *fs) {
    static uint8_t zeroes[300000];
    struct xfs_inode root, ip;
    uint64_t ino;
    uint32_t put;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "sparse", 6u, 0100644, NOW, &ino), "create sparse");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 1000000u, payload, 100u, &put), "write past the end");
    must_match(fs, ino, 0, zeroes, sizeof zeroes, "the hole before it");
    must_match(fs, ino, 1000000u, payload, 100u, "the write past the end");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 300000u, payload, 9000u, &put), "write into the hole");
    must_match(fs, ino, 300000u, payload, 9000u, "the filled hole");
    must_match(fs, ino, 309000u, zeroes, 4000u, "the rest of the hole");
    read_inode(fs, ino, &ip);
    must(xfs_truncate(fs, &ip, 3000000u, NOW), "truncate upward");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 2500000u, payload, 5000u, &put), "write into the new hole");
    must_match(fs, ino, 2500000u, payload, 5000u, "the write into the new hole");
}

/* The board self-test's directory growth, name for name: `g` and two bytes
 * from `'0' + i / 10` and `'0' + i % 10`, which run past `'9'` into
 * punctuation and bytes above 0x7f, twelve hundred of them in the root,
 * added, found and removed, twice. */
static void boardgrow_name(char *name, uint32_t i) {
    name[0] = 'g';
    name[1] = (char)('0' + i / 10u);
    name[2] = (char)('0' + i % 10u);
    name[3] = '\0';
}

static void step_boardgrow(struct xfs *fs) {
    char name[4];
    uint64_t ino;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t i = 0; i < 1200u; i++) {
            struct xfs_inode root;
            read_inode(fs, fs->rootino, &root);
            boardgrow_name(name, i);
            must(xfs_create(fs, &root, name, 3u, 0100644, NOW, &ino), "boardgrow create");
        }
        for (uint32_t i = 0; i < 1200u; i++) {
            struct xfs_inode root;
            read_inode(fs, fs->rootino, &root);
            boardgrow_name(name, i);
            if (xfs_dir_lookup(fs, &root, name, 3u, &ino) != KERR_OK) {
                fprintf(stderr, "boardgrow: pass %u: name %u not found\n", pass, i);
                exit(1);
            }
        }
        for (uint32_t i = 0; i < 1200u; i++) {
            struct xfs_inode root;
            int rc;
            read_inode(fs, fs->rootino, &root);
            boardgrow_name(name, i);
            rc = xfs_unlink(fs, &root, name, 3u, NOW);
            if (rc != KERR_OK) {
                fprintf(stderr, "boardgrow: pass %u: unlink of name %u failed: %d\n", pass, i, rc);
                (void)xfs_flush(fs); /* left as it was, for xfs_db */
                exit(1);
            }
        }
    }
}

/* Adds up a group's free blocks, as the trim's own walker sees them. */
static int count_free(void *self, uint32_t agno, uint32_t start, uint32_t count) {
    (void)agno;
    (void)start;
    *(uint64_t *)self += count;
    return KERR_OK;
}

static uint64_t free_blocks(struct xfs *fs) {
    uint64_t n = 0;
    for (uint32_t agno = 0; agno < fs->agcount; agno++) {
        must(xfs_free_extents_each(fs, agno, count_free, &n), "walk the free extents");
    }
    return n;
}

/* A file kept and a megabyte written and removed: free space the
 * filesystem once used. Returns the kept file's inode. */
static uint64_t trim_setup(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint64_t keep, gone;
    uint32_t put;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "trimkeep", 8u, 0100644, NOW, &keep), "create trimkeep");
    read_inode(fs, keep, &ip);
    must(xfs_file_write(fs, &ip, 0, payload, 300000u, &put), "write trimkeep");
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "trimgone", 8u, 0100644, NOW, &gone), "create trimgone");
    read_inode(fs, gone, &ip);
    must(xfs_file_write(fs, &ip, 0, payload, 1000000u, &put), "write trimgone");
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "trimgone", 8u, NOW), "unlink trimgone");
    return keep;
}

/* Everything free discarded, and nothing else: the walker's count, the
 * trim's and the device's agree. */
static void trim_everything(struct xfs *fs) {
    struct xfs_trim_range all = {.start = 0, .len = fs->dblocks, .minlen = 1};
    uint64_t trimmed = 0;
    must(xfs_trim(fs, &all, &trimmed), "trim");
    if (trimmed == 0 || trimmed != free_blocks(fs) || trimmed != discarded_blocks) {
        fprintf(stderr, "trim: %llu trimmed, %llu free, %llu discarded\n",
                (unsigned long long)trimmed, (unsigned long long)free_blocks(fs),
                (unsigned long long)discarded_blocks);
        exit(1);
    }
}

/* A minimum longer than any free extent trims nothing; a range past the
 * end is refused. */
static void trim_limits(struct xfs *fs) {
    struct xfs_trim_range none = {.start = 0, .len = fs->dblocks, .minlen = fs->dblocks};
    struct xfs_trim_range past = {.start = fs->dblocks, .len = 1, .minlen = 1};
    uint64_t trimmed = 0;
    must(xfs_trim(fs, &none, &trimmed), "trim with a long minimum");
    if (trimmed != 0 || xfs_trim(fs, &past, &trimmed) != KERR_INVAL) {
        fprintf(stderr, "trim: a long minimum trimmed %llu, or a range past the end ran\n",
                (unsigned long long)trimmed);
        exit(1);
    }
}

/* A file written over trimmed blocks, with part of one block left
 * unwritten between two writes: zeroes there, not the poison. */
static void write_over_trimmed(struct xfs *fs) {
    static const uint8_t zeroes[4000];
    struct xfs_inode root, ip;
    uint64_t ino;
    uint32_t put;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "trimnew", 7u, 0100644, NOW, &ino), "create trimnew");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 0, payload, 5000u, &put), "write trimnew");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 9000u, payload, 5000u, &put), "write trimnew past a gap");
    must_match(fs, ino, 5000u, zeroes, sizeof zeroes, "the gap over trimmed blocks");
    must_match(fs, ino, 9000u, payload, 5000u, "the write past the gap");
}

/* Trim, as `fstrim` asks for it: every free block discarded and nothing
 * else. The kept file is read back from the device (the cache forgotten),
 * the limits are honoured, and a file written afterwards over trimmed
 * blocks reads zeroes where it was not written. Then `xfs_repair` judges
 * every tree. */
static void step_trim(struct xfs *fs) {
    uint64_t keep = trim_setup(fs);
    trim_everything(fs);
    bcache_invalidate(fs->cache);
    must_match(fs, keep, 0, payload, 300000u, "the kept file after the trim");
    trim_limits(fs);
    write_over_trimmed(fs);
}

/* What lies past the end of a file is not the file's: a truncate into the
 * middle of a block, then a write further on, must read zeroes in between,
 * not the bytes the truncate cut off. */
static void step_eofzero(struct xfs *fs) {
    static const uint8_t zeroes[4000];
    struct xfs_inode root, ip;
    uint64_t ino;
    uint32_t put;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "eofzero", 7u, 0100644, NOW, &ino), "create eofzero");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 0, payload, 12000u, &put), "write eofzero");
    read_inode(fs, ino, &ip);
    must(xfs_truncate(fs, &ip, 5000u, NOW), "truncate into a block");
    read_inode(fs, ino, &ip);
    must(xfs_file_write(fs, &ip, 9000u, payload, 100u, &put), "write past the cut");
    must_match(fs, ino, 0, payload, 5000u, "what the truncate kept");
    must_match(fs, ino, 5000u, zeroes, sizeof zeroes, "what the truncate cut off");
}

struct step {
    const char *name;
    void (*run)(struct xfs *fs);
};

/* Marks an empty file realtime, as `chattr +r` does: the flag is only ever
 * set on a file with no blocks, since its blocks would then be on the wrong
 * device. */
static void make_realtime(struct xfs *fs, uint64_t ino) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    xfs_inode_place(fs, ino, &block, &boffset);
    must(bcache_get(fs->cache, block, &b), "reading the inode to mark it realtime");
    di = b->data + boffset;
    xfs_put_be16(di + XFS_DI_FLAGS, (uint16_t)(xfs_be16(di + XFS_DI_FLAGS) | XFS_DIFLAG_REALTIME));
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
}

static uint64_t realtime_file(struct xfs *fs, struct xfs_inode *root, const char *name) {
    uint64_t ino;
    must(xfs_create(fs, root, name, (uint32_t)strlen(name), 0100644, NOW, &ino), "create realtime");
    make_realtime(fs, ino);
    return ino;
}

/* Realtime files, which only a filesystem with a realtime device has (run
 * with XFS_WRITECHECK_RT): grown block by block, written past the end and
 * into the hole left, truncated, and one removed — so the bitmap, the
 * summary and `sb_frextents` all change and `xfs_repair -n` judges them. */
static void step_realtime(struct xfs *fs) {
    static uint8_t zeroes[65536];
    struct xfs_inode root, ip;
    uint64_t keep, gone;
    uint32_t put;
    if (fs->rt_cache == NULL) {
        return; /* no realtime device given: nothing to judge */
    }
    read_inode(fs, fs->rootino, &root);
    keep = realtime_file(fs, &root, "rtkeep");
    read_inode(fs, keep, &ip);
    for (uint32_t i = 0; i < 5u; i++) {
        must(xfs_file_write(fs, &ip, (uint64_t)i * 3000u, payload + i * 3000u, 3000u, &put),
             "realtime append");
        read_inode(fs, keep, &ip);
    }
    must(xfs_file_write(fs, &ip, 200000u, payload, 5000u, &put), "realtime write past the end");
    read_inode(fs, keep, &ip);
    must(xfs_file_write(fs, &ip, 90000u, payload, 9000u, &put), "realtime write into the hole");
    must_match(fs, keep, 0, payload, 15000u, "the realtime appends");
    must_match(fs, keep, 90000u, payload, 9000u, "the filled realtime hole");
    must_match(fs, keep, 30000u, zeroes, 60000u, "the realtime hole before it");
    read_inode(fs, keep, &ip);
    must(xfs_truncate(fs, &ip, 100000u, NOW), "realtime truncate");
    read_inode(fs, fs->rootino, &root);
    gone = realtime_file(fs, &root, "rtgone");
    read_inode(fs, gone, &ip);
    must(xfs_file_write(fs, &ip, 0, payload, 70000u, &put), "realtime file to remove");
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "rtgone", 6u, NOW), "realtime unlink");
}

/* A file of more extents than a root and one level of leaves can name —
 * 20 x 251 with 512-byte v5 inodes, 25 x 254 with 512-byte v4 ones — so
 * its map needs a level of nodes between them. Every other block is
 * written, which makes each block an extent of its own wherever the
 * allocator puts it. */
#define TALL_EXTENTS 7000u

/* Block `i` of the tall file holds payload block `i % 1024`. */
static const uint8_t *tall_block(uint32_t i) {
    return payload + (size_t)(i % 1024u) * BS;
}

/* The tall file's written blocks read back, and the holes between them as
 * zeroes, for the first `extents` of them. */
static void must_read_tall(struct xfs *fs, uint64_t ino, uint32_t extents) {
    static uint8_t got[2u * BS], zero[BS];
    struct xfs_inode ip;
    read_inode(fs, ino, &ip);
    for (uint32_t i = 0; i < extents; i++) {
        uint32_t n = 0;
        must(xfs_file_read(fs, &ip, (uint64_t)i * 2u * BS, got, 2u * BS, &n), "tall read");
        if (memcmp(got, tall_block(i), BS) != 0 ||
            (n == 2u * BS && memcmp(got + BS, zero, BS) != 0)) {
            fprintf(stderr, "xfs_writecheck: tall file extent %u read back wrong\n", i);
            exit(1);
        }
    }
}

/* The root's level in the inode: 1 for leaves under it, 2 for nodes. */
static uint32_t tall_root_level(struct xfs *fs, uint64_t ino) {
    struct xfs_inode ip;
    read_inode(fs, ino, &ip);
    return ip.format == XFS_DINODE_FMT_BTREE ? (uint32_t)ip.fork[0] << 8 | ip.fork[1] : 0;
}

/* Truncates the tall file to its first `extents` extents and checks them. */
static void cut_tall(struct xfs *fs, uint64_t ino, uint32_t extents) {
    struct xfs_inode ip;
    read_inode(fs, ino, &ip);
    must(xfs_truncate(fs, &ip, (uint64_t)extents * 2u * BS - BS, NOW), "truncate the tall file");
    must_read_tall(fs, ino, extents);
    printf("xfs_writecheck: tall file cut to %u extents, root level %u\n", extents,
           tall_root_level(fs, ino));
}

/* Grows the tall file to three levels, reads it, cuts it back through two
 * levels to extents in the inode, and removes it. Skipped on inodes whose
 * fork is past what this reader caches (`btreefile` covers those). */
static void step_tallfork(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint64_t ino;
    uint32_t put;
    if (fs->inodesize - fs->dinode_size > XFS_FORK_MAX) {
        return;
    }
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "tall", 4u, 0100644, NOW, &ino), "create tall");
    for (uint32_t i = 0; i < TALL_EXTENTS; i++) {
        read_inode(fs, ino, &ip);
        must(xfs_file_write(fs, &ip, (uint64_t)i * 2u * BS, tall_block(i), BS, &put), "tall write");
    }
    read_inode(fs, ino, &ip);
    printf("xfs_writecheck: tall file has %u extents, root level %u\n", ip.nextents,
           tall_root_level(fs, ino));
    if (ip.nextents != TALL_EXTENTS || tall_root_level(fs, ino) != 2u) {
        fprintf(stderr, "xfs_writecheck: the tall file is not a three-level tree\n");
        exit(1);
    }
    must_read_tall(fs, ino, TALL_EXTENTS);
    cut_tall(fs, ino, 900u);
    cut_tall(fs, ino, 5u);
    read_inode(fs, fs->rootino, &root);
    must(xfs_unlink(fs, &root, "tall", 4u, NOW), "unlink tall");
}

/* A file with more attributes than one leaf block holds: 1,500 names with
 * 40-byte values (about thirty leaves, so the fork becomes node format and
 * its leaves split), every tenth with a 3,000-byte value in blocks of its
 * own. A block is appended to a second file after each set, so the
 * fork's blocks are never neighbours and its extent map outgrows the inode
 * and becomes a b-tree. All are read back and listed; every other one is
 * removed and both halves checked; then the rest, which empties the leaves
 * and gives every remote value's blocks back. */
#define MANY_ATTRS 1500u

static void attr_name(char *out, size_t room, uint32_t i) {
    snprintf(out, room, "user.attribute-number-%05u", i);
}

/* Attribute `i`'s value: 40 bytes, or 3,000 for every tenth. */
static uint32_t attr_value(uint32_t i, uint8_t *out) {
    uint32_t len = i % 10u == 0 ? 3000u : 40u;
    for (uint32_t k = 0; k < len; k++) {
        out[k] = (uint8_t)(i * 13u + k);
    }
    return len;
}

/* Attribute `i` of `ino` is there with its value, or is not, as wanted. */
static void attr_expect(struct xfs *fs, uint64_t ino, uint32_t i, bool present) {
    static uint8_t want[3000], got[3000];
    struct xfs_inode ip;
    char name[48];
    uint32_t len = attr_value(i, want), vlen = 0;
    int rc;
    attr_name(name, sizeof name, i);
    read_inode(fs, ino, &ip);
    rc = xfs_attr_get(fs, &ip, name, (uint32_t)strlen(name), got, sizeof got, &vlen);
    if (present ? rc != KERR_OK || vlen != len || memcmp(got, want, len) != 0
                : rc != KERR_NOENT) {
        fprintf(stderr, "xfs_writecheck: %s: rc %d, %u bytes (want %s)\n", name, rc, vlen,
                present ? "its value" : "gone");
        exit(1);
    }
}

static int count_attr(void *self, const uint8_t *name, uint32_t namelen, uint8_t flags,
                      uint32_t valuelen) {
    (void)name;
    (void)namelen;
    (void)flags;
    (void)valuelen;
    (*(uint32_t *)self)++;
    return KERR_OK;
}

static uint32_t attr_count(struct xfs *fs, uint64_t ino) {
    struct xfs_inode ip;
    uint32_t n = 0;
    read_inode(fs, ino, &ip);
    must(xfs_attr_list(fs, &ip, count_attr, &n), "list attributes");
    return n;
}

static void attrs_remove(struct xfs *fs, uint64_t ino, uint32_t first, uint32_t step) {
    char name[48];
    for (uint32_t i = first; i < MANY_ATTRS; i += step) {
        struct xfs_inode ip;
        read_inode(fs, ino, &ip);
        attr_name(name, sizeof name, i);
        must(xfs_attr_remove(fs, &ip, name, (uint32_t)strlen(name), NOW), "remove attribute");
    }
}

/* Whether block 0 of the attribute fork is a node (rather than a leaf). */
static bool attr_root_is_node(struct xfs *fs, uint64_t ino) {
    struct xfs_inode ip;
    struct xfs_extent e;
    struct bbuf *b;
    bool node;
    read_inode(fs, ino, &ip);
    must(xfs_attr_bmap(fs, &ip, 0, &e), "attribute block 0");
    must(bcache_get(fs->cache, xfs_fsb_to_block(fs, e.startblock), &b), "attribute block 0");
    node = xfs_be16(b->data + XFS_DA3_MAGIC_OFF) == xfs_magic_for(fs, XFS_BLK_DA_NODE);
    bcache_put(fs->cache, b);
    return node;
}

/* Sets every attribute, appending a block to `spacer` after each. */
static void attrs_set(struct xfs *fs, uint64_t ino, uint64_t spacer) {
    static uint8_t value[3000];
    char name[48];
    uint32_t put = 0;
    for (uint32_t i = 0; i < MANY_ATTRS; i++) {
        struct xfs_inode ip;
        uint32_t len = attr_value(i, value);
        read_inode(fs, ino, &ip);
        attr_name(name, sizeof name, i);
        must(xfs_attr_set(fs, &ip, name, (uint32_t)strlen(name), value, len, NOW), name);
        read_inode(fs, spacer, &ip);
        must(xfs_file_write(fs, &ip, (uint64_t)i * BS, payload, BS, &put), "spacer");
    }
}

/* A file named `name` with every attribute set, checked: a node-format
 * fork whose extent map is a b-tree, all listed, a sample read back. */
static uint64_t many_attrs_file(struct xfs *fs, const char *name, uint64_t spacer) {
    struct xfs_inode root, ip;
    uint64_t ino;
    read_inode(fs, fs->rootino, &root);
    ino = make_file(fs, &root, name, 4096u, 4096u);
    attrs_set(fs, ino, spacer);
    read_inode(fs, ino, &ip);
    printf("xfs_writecheck: %s: %u attributes, fork format %u, %u listed, root %s\n", name,
           MANY_ATTRS, ip.aformat, attr_count(fs, ino),
           attr_root_is_node(fs, ino) ? "a node" : "a leaf");
    if (ip.aformat != XFS_DINODE_FMT_BTREE || attr_count(fs, ino) != MANY_ATTRS ||
        !attr_root_is_node(fs, ino)) {
        fprintf(stderr, "xfs_writecheck: not a node-format fork in a b-tree, of all of them\n");
        exit(1);
    }
    for (uint32_t i = 0; i < MANY_ATTRS; i += 7u) {
        attr_expect(fs, ino, i, true);
    }
    return ino;
}

/* `kept` keeps half its attributes, for `xfs_repair` to judge a populated
 * node fork; `emptied` loses all of them, every leaf and remote value. */
static void step_manyattrs(struct xfs *fs) {
    struct xfs_inode root;
    uint64_t kept, emptied, spacer;
    read_inode(fs, fs->rootino, &root);
    must(xfs_create(fs, &root, "spacer", 6u, 0100644, NOW, &spacer), "create spacer");
    kept = many_attrs_file(fs, "attrs-kept", spacer);
    attrs_remove(fs, kept, 0, 2u);
    for (uint32_t i = 0; i < MANY_ATTRS; i += 7u) {
        attr_expect(fs, kept, i, i % 2u == 1u);
    }
    emptied = many_attrs_file(fs, "attrs-emptied", spacer);
    attrs_remove(fs, emptied, 0, 1u);
    printf("xfs_writecheck: %u attributes kept, %u left on the emptied file\n",
           attr_count(fs, kept), attr_count(fs, emptied));
    must_read_back(fs, emptied, 4096u);
}

/* A directory past what one node and one free block describe: 200,000
 * names of 40 bytes, which is ~2,800 data blocks (a second free block past
 * 2,016) and a hash index whose root fills at ~177,000 names (a level of
 * nodes under it). Every other name is then removed and both halves are
 * looked up, and then the rest go, emptying leaves and nodes on the way.
 * A minute and a 1 GiB image, so it runs only when asked for
 * (`XFS_WRITECHECK_BIGDIR`); `xfs_repair` judges the result. */
#define BIGDIR_NAMES 200000u

static void bigdir_name(char *out, size_t room, uint32_t i) {
    snprintf(out, room, "a-directory-entry-with-a-long-name-%06u", i);
}

/* Whether name `i` is in /bigdir, as wanted. */
static void bigdir_expect(struct xfs *fs, uint64_t dirino, uint32_t i, bool present) {
    struct xfs_inode dir;
    uint64_t ino = 0;
    char name[48];
    bigdir_name(name, sizeof name, i);
    read_inode(fs, dirino, &dir);
    if ((xfs_dir_lookup(fs, &dir, name, (uint32_t)strlen(name), &ino) == KERR_OK) != present) {
        fprintf(stderr, "xfs_writecheck: %s is %s\n", name, present ? "missing" : "still there");
        exit(1);
    }
}

/* Removes names from `first` on, `step` apart. */
static void bigdir_remove(struct xfs *fs, uint64_t dirino, uint32_t first, uint32_t step) {
    char name[48];
    for (uint32_t i = first; i < BIGDIR_NAMES; i += step) {
        struct xfs_inode dir;
        read_inode(fs, dirino, &dir);
        bigdir_name(name, sizeof name, i);
        must(xfs_unlink(fs, &dir, name, (uint32_t)strlen(name), NOW), "unlink bigdir");
    }
}

/* The hash index root's level: 1 when it names leaves, 2 with nodes under. */
static uint32_t bigdir_index_level(struct xfs *fs, uint64_t dirino) {
    struct xfs_inode dir;
    uint64_t fsb = 0;
    read_inode(fs, dirino, &dir);
    must(xfs_dir_raw_load(fs, &dir, fs->dir_leaf_block, &fsb), "the index root");
    return xfs_be16(fs->dirblock + fs->da_level);
}

/* Whether the free index has its second block (data blocks past 2,016). */
static bool bigdir_has_second_free_block(struct xfs *fs, uint64_t dirino) {
    struct xfs_inode dir;
    uint64_t fsb = 0;
    read_inode(fs, dirino, &dir);
    return xfs_dir_raw_load(fs, &dir, fs->dir_free_block + fs->dirblocks, &fsb) == KERR_OK &&
           xfs_be32(fs->dirblock) == xfs_magic_for(fs, XFS_BLK_DIR_FREE);
}

static void step_bigdir(struct xfs *fs) {
    struct xfs_inode root, dir;
    uint64_t dirino, ino;
    char name[48];
    if (getenv("XFS_WRITECHECK_BIGDIR") == NULL) {
        return;
    }
    read_inode(fs, fs->rootino, &root);
    must(xfs_mkdir(fs, &root, "bigdir", 6u, NOW, &dirino), "mkdir bigdir");
    for (uint32_t i = 0; i < BIGDIR_NAMES; i++) {
        read_inode(fs, dirino, &dir);
        bigdir_name(name, sizeof name, i);
        must(xfs_create(fs, &dir, name, (uint32_t)strlen(name), 0100644, NOW, &ino), name);
    }
    printf("xfs_writecheck: %u names, index root level %u\n", BIGDIR_NAMES,
           bigdir_index_level(fs, dirino));
    if (bigdir_index_level(fs, dirino) < 2u || !bigdir_has_second_free_block(fs, dirino)) {
        fprintf(stderr, "xfs_writecheck: the index never grew a level of nodes, or the free "
                        "index a second block\n");
        exit(1);
    }
    bigdir_remove(fs, dirino, 0, 2u);
    for (uint32_t i = 0; i < BIGDIR_NAMES; i += 997u) {
        bigdir_expect(fs, dirino, i, i % 2u == 1u);
    }
    bigdir_remove(fs, dirino, 1u, 2u);
    bigdir_expect(fs, dirino, BIGDIR_NAMES - 1u, false);
    printf("xfs_writecheck: every name removed again\n");
}

/* On a `-i size=1024` image made with /big (a shortform directory whose
 * fork is past XFS_FORK_MAX) and /beside: the big directory is refused —
 * per inode, KERR_NOTSUP — and everything else on the filesystem still
 * reads and writes. Skipped on images without /big. */
static void step_bigfork(struct xfs *fs) {
    struct xfs_inode ip, root;
    int rc = xfs_path_lookup(fs, "/big", &ip);
    if (rc == KERR_NOENT) {
        return;
    }
    if (rc != KERR_NOTSUP) {
        fprintf(stderr, "xfs_writecheck: /big (a fork over %u bytes) answered %d, want %d\n",
                XFS_FORK_MAX, rc, KERR_NOTSUP);
        exit(1);
    }
    must(xfs_path_lookup(fs, "/beside/s1", &ip), "a small directory beside the big one");
    read_inode(fs, fs->rootino, &root);
    (void)make_file(fs, &root, "after-big", 9000u, 4096u);
    must_read_back(fs, lookup(fs, "/after-big"), 9000u);
}

static const struct step steps[] = {
    {"files", step_files},     {"dir", step_dir},       {"many", step_many},
    {"truncate", step_truncate}, {"unlink", step_unlink}, {"attr", step_attr},
    {"rename", step_rename},   {"symlink", step_symlink}, {"attrfile", step_attrfile},
    {"fill", step_fill},       {"drain", step_drain},     {"wrap", step_wrap},
    {"fragment", step_fragment}, {"btreefile", step_btreefile},
    {"defragment", step_defragment}, {"replay", step_replay},
    {"fixture", step_fixture},  {"unlinkproto", step_unlinkproto},
    {"inodes", step_inodes},    {"noinodes", step_noinodes},
    {"reflink", step_reflink},  {"holes", step_holes},     {"boardgrow", step_boardgrow},
    {"trim", step_trim},        {"eofzero", step_eofzero},  {"realtime", step_realtime},
    {"uncommitted", step_uncommitted}, {"bigfork", step_bigfork}, {"tallfork", step_tallfork}, {"manyattrs", step_manyattrs},
    {"bigdir", step_bigdir},
};

/* Runs the named steps, or every one when none is named. */
static void exercise(struct xfs *fs, int argc, char **argv) {
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        bool wanted = argc <= 2;
        for (int a = 2; a < argc; a++) {
            wanted = wanted || strcmp(argv[a], steps[i].name) == 0;
        }
        if (wanted) {
            steps[i].run(fs);
        }
    }
}

int main(int argc, char **argv) {
    static struct blockdev dev;
    static struct bcache cache;
    static struct xfs fs;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image> [step...]\n", argv[0]);
        return 2;
    }
    console_attach(&console);
    image_fd = open(argv[1], O_RDWR);
    struct stat st;
    if (image_fd < 0 || fstat(image_fd, &st) != 0) {
        perror(argv[1]);
        return 2;
    }
    for (uint32_t i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i * 31u + 7u);
    }
    dev = (struct blockdev){.read = img_read, .write = img_write, .flush = img_flush,
                            .discard = img_discard, .block_size = BS,
                            .blocks = (uint64_t)st.st_size / BS};
    must(bcache_init(&cache, &dev, cache_mem, SLOTS), "bcache_init");
    must(xfs_mount(&fs, &cache), "mount");
    if (!xfs_writable(&fs)) {
        fprintf(stderr, "xfs_writecheck: the filesystem mounted read-only\n");
        return 1;
    }
    attach_realtime(&fs);
    exercise(&fs, argc, argv);
    must(xfs_flush(&fs), "flush");
    printf("xfs_writecheck: every write succeeded\n");
    return 0;
}
