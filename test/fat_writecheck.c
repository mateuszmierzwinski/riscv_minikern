/* fat_writecheck — reads and writes an image `mkfs.vfat` made, so that
 * `fsck.fat -n` and mtools can judge the result (test/fat_write_test.sh).
 *
 * Usage: fat_writecheck <image> <step>...
 *   read     — /HELLO.TXT and /long name file.txt (put there by mcopy) read back
 *   write    — create, write, grow, cut, rename, remove; a deep directory
 *   many     — enough names that a directory outgrows one cluster
 * Exits 0 when every step succeeded. The files it writes are what the
 * script then reads with mtools.
 *
 * `fat_writecheck --payload <bytes>` writes the payload to stdout instead,
 * so the script can put the same bytes in with mcopy and compare them out. */
#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/fat/fat.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BS 4096u
#define SLOTS 64u
#define NOW 1761609600u /* 2025-10-28 */

static int image_fd = -1;
static uint8_t cache_mem[SLOTS * BS];
static uint8_t payload[300000];

static int img_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    return pread(image_fd, buf, (size_t)count * BS, (off_t)(lba * BS)) == (ssize_t)(count * BS)
               ? KERR_OK
               : KERR_IO;
}

static int img_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    return pwrite(image_fd, buf, (size_t)count * BS, (off_t)(lba * BS)) == (ssize_t)(count * BS)
               ? KERR_OK
               : KERR_IO;
}

static int img_flush(void *self) {
    (void)self;
    return fsync(image_fd) == 0 ? KERR_OK : KERR_IO;
}

static void to_stderr(void *self, const char *buf, size_t len) {
    (void)self;
    (void)fwrite(buf, 1, len, stderr);
}

static const struct console_port console = {.write = to_stderr, .self = NULL};

static void must(int rc, const char *what) {
    if (rc != KERR_OK) {
        fprintf(stderr, "fat_writecheck: %s failed: %d\n", what, rc);
        exit(1);
    }
}

static struct fat_node at(struct fat *fs, const char *path) {
    struct fat_node n;
    must(fat_path_lookup(fs, path, &n), path);
    return n;
}

/* Reads a whole file and compares it with `want`. */
static void must_hold(struct fat *fs, const char *path, const uint8_t *want, uint32_t len) {
    static uint8_t got[sizeof payload];
    struct fat_node n = at(fs, path);
    uint32_t r = 0;
    must(fat_read(fs, &n, 0, got, sizeof got, &r), path);
    if (r != len || memcmp(got, want, len) != 0) {
        fprintf(stderr, "fat_writecheck: %s holds %u bytes, not the %u expected\n", path, r, len);
        exit(1);
    }
}

static void step_read(struct fat *fs) {
    must_hold(fs, "/HELLO.TXT", (const uint8_t *)"hello from mtools\n", 18u);
    must_hold(fs, "/long name file.txt", payload, 100000u);
    must_hold(fs, "/sub/inner.bin", payload, 5000u);
}

/* Creates `name` in `dir` holding `len` bytes of the payload, written in
 * `chunk`-byte pieces. */
static void make(struct fat *fs, const char *dir, const char *name, uint32_t len, uint32_t chunk) {
    struct fat_node d = at(fs, dir), f;
    uint32_t put;
    must(fat_create(fs, &d, name, (uint32_t)strlen(name), false, NOW, &f), name);
    for (uint32_t off = 0; off < len; off += chunk) {
        uint32_t n = len - off < chunk ? len - off : chunk;
        must(fat_write(fs, &f, off, payload + off, n, &put), "write");
    }
}

static void mkdir_at(struct fat *fs, const char *dir, const char *name) {
    struct fat_node d = at(fs, dir), f;
    must(fat_create(fs, &d, name, (uint32_t)strlen(name), true, NOW, &f), name);
}

static void rename_at(struct fat *fs, const char *fd, const char *from, const char *td,
                      const char *to) {
    struct fat_node a = at(fs, fd), b = at(fs, td);
    must(fat_rename(fs, &a, from, (uint32_t)strlen(from), &b, to, (uint32_t)strlen(to), NOW),
         "rename");
}

static void unlink_at(struct fat *fs, const char *dir, const char *name) {
    struct fat_node d = at(fs, dir);
    must(fat_unlink(fs, &d, name, (uint32_t)strlen(name)), "unlink");
}

/* Truncates the file at `path` to `size`. */
static void cut(struct fat *fs, const char *path, uint32_t size) {
    struct fat_node n = at(fs, path);
    must(fat_truncate(fs, &n, size, NOW), "truncate");
}

static void step_write(struct fat *fs) {
    static uint8_t want[sizeof payload];
    struct fat_node d;
    make(fs, "/", "KERNEL.BIN", 250000u, 4096u);
    make(fs, "/", "a file with a long name.txt", 70000u, 1000u);
    make(fs, "/", "zażółć.txt", 10u, 10u);
    mkdir_at(fs, "/", "boot dir");
    mkdir_at(fs, "/boot dir", "deeper");
    make(fs, "/boot dir/deeper", "x.cfg", 300u, 300u);
    make(fs, "/", "shrinks.dat", 20000u, 20000u);
    cut(fs, "/shrinks.dat", 5000u);
    cut(fs, "/shrinks.dat", 9000u); /* grown back: the gap must read as zeroes */
    memcpy(want, payload, 5000u);
    memset(want + 5000u, 0, 4000u);
    must_hold(fs, "/shrinks.dat", want, 9000u);
    rename_at(fs, "/", "a file with a long name.txt", "/boot dir", "moved.txt");
    rename_at(fs, "/", "KERNEL.BIN", "/", "kernel.bin"); /* a change of case only */
    rename_at(fs, "/boot dir", "deeper", "/", "deeper"); /* a directory to another parent */
    make(fs, "/", "doomed", 9000u, 9000u);
    unlink_at(fs, "/", "doomed");
    mkdir_at(fs, "/", "empty");
    unlink_at(fs, "/", "empty");
    d = at(fs, "/deeper");
    if (fat_unlink(fs, &d, "nothing", 7u) != KERR_NOENT) {
        fprintf(stderr, "fat_writecheck: removing a missing name did not say so\n");
        exit(1);
    }
    must_hold(fs, "/kernel.bin", payload, 250000u);
    must_hold(fs, "/boot dir/moved.txt", payload, 70000u);
    must_hold(fs, "/deeper/x.cfg", payload, 300u);
}

static void step_many(struct fat *fs) {
    char name[40];
    mkdir_at(fs, "/", "many");
    for (uint32_t i = 0; i < 300u; i++) {
        snprintf(name, sizeof name, "entry number %03u with a long name", i);
        make(fs, "/many", name, i % 5u == 0 ? 3000u : 0u, 3000u);
    }
    for (uint32_t i = 0; i < 300u; i += 3u) {
        snprintf(name, sizeof name, "entry number %03u with a long name", i);
        unlink_at(fs, "/many", name);
    }
}

int main(int argc, char **argv) {
    static struct blockdev dev;
    static struct bcache cache;
    static struct fat fs;
    struct stat st;
    console_attach(&console);
    for (uint32_t i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }
    if (argc == 3 && strcmp(argv[1], "--payload") == 0) {
        size_t n = (size_t)strtoul(argv[2], NULL, 10);
        return fwrite(payload, 1, n < sizeof payload ? n : sizeof payload, stdout) == n ? 0 : 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <image> <step>...\n", argv[0]);
        return 2;
    }
    image_fd = open(argv[1], O_RDWR);
    if (image_fd < 0 || fstat(image_fd, &st) != 0) {
        perror(argv[1]);
        return 2;
    }
    dev = (struct blockdev){.read = img_read, .write = img_write, .flush = img_flush,
                            .block_size = BS, .blocks = (uint64_t)st.st_size / BS};
    must(bcache_init(&cache, &dev, cache_mem, SLOTS), "bcache_init");
    must(fat_mount(&fs, &cache), "mount");
    fs.now_s = NOW;
    for (int a = 2; a < argc; a++) {
        if (strcmp(argv[a], "read") == 0) {
            step_read(&fs);
        } else if (strcmp(argv[a], "write") == 0) {
            step_write(&fs);
        } else if (strcmp(argv[a], "many") == 0) {
            step_many(&fs);
        }
    }
    must(fat_flush(&fs), "flush");
    printf("fat_writecheck: FAT%u, every step succeeded\n", fs.type);
    return 0;
}
