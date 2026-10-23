#include "kernel/xfsmount.h"

#include "lib/sha256.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "kernel/disk.h"
#include "kernel/mem.h"

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

int xfsmount_show_file(const char *path) {
    struct xfs_inode ip;
    char *buf;
    uint32_t got = 0;
    int rc;
    if (!mounted) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(&fs, path, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    buf = kzalloc(512u);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    rc = xfs_file_read(&fs, &ip, 0, buf, 511u, &got);
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

int xfsmount_read_file(const char *path, uint32_t limit, uint8_t **out, uint32_t *size) {
    struct xfs_inode ip;
    uint8_t *buf;
    uint32_t got = 0, at = 0;
    int rc;
    if (!mounted) {
        return KERR_NODEV;
    }
    rc = xfs_path_lookup(&fs, path, &ip);
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
        rc = xfs_file_read(&fs, &ip, at, buf + at, (uint32_t)ip.size - at, &got);
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
