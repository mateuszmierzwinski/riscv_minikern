#include "kernel/xfsmount.h"

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
