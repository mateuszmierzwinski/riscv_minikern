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

static void step_btreefile(struct xfs *fs) {
    struct xfs_inode root, ip;
    uint64_t ino;
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
    {"trim", step_trim},        {"eofzero", step_eofzero},
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
    exercise(&fs, argc, argv);
    must(xfs_flush(&fs), "flush");
    printf("xfs_writecheck: every write succeeded\n");
    return 0;
}
