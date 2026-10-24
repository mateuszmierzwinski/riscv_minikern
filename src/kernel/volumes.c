/* The mount table, and automount by label. See `volumes.h` for the policy. */
#include "kernel/volumes.h"

#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "kernel/disk.h"
#include "kernel/mem.h"

/* One per partition the disk can offer, which is what bounds it. */
#define VOLUMES_MAX 4u
#define VOLUMES_PATH_MAX 48u
#define VOLUMES_CACHE_SLOTS 16u

struct volume {
    struct xfs fs;
    struct bcache cache;
    char at[VOLUMES_PATH_MAX];
    bool used;
};

static struct volume table[VOLUMES_MAX];
static uint32_t count;

static void copy_str(char *to, uint32_t room, const char *from) {
    uint32_t n = 0;
    while (from[n] != '\0' && n + 1u < room) {
        to[n] = from[n];
        n++;
    }
    to[n] = '\0';
}

static bool same_str(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/* Whether `path` begins with `prefix` at a path boundary.
 *
 * The boundary is the point: `/volumes/DATA` must not be matched by a
 * volume mounted at `/volumes/DAT`, and a plain character comparison would
 * match it. So the character after the prefix has to be the end of the path
 * or a separator. */
static bool begins_with(const char *path, const char *prefix, uint32_t *len) {
    uint32_t i = 0;
    while (prefix[i] != '\0') {
        if (path[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    *len = i;
    return path[i] == '\0' || path[i] == '/';
}

/* Where this filesystem belongs, from its label.
 *
 * An unlabelled volume still needs a name, because a path has to be able to
 * reach it — so it gets its index. Naming it after the device would be more
 * informative and would change when a disk is moved between slots, which is
 * exactly what a mount point must not do. */
static void mount_point_for(const char *label, uint32_t index, char *out, uint32_t room) {
    if (same_str(label, VOLUMES_ROOT_LABEL)) {
        copy_str(out, room, "/");
        return;
    }
    copy_str(out, room, VOLUMES_DIR "/");
    if (label[0] == '\0') {
        uint32_t n = 0;
        while (out[n] != '\0') {
            n++;
        }
        if (n + 3u < room) {
            out[n] = 'v';
            out[n + 1u] = (char)('0' + (index % 10u));
            out[n + 2u] = '\0';
        }
        return;
    }
    for (uint32_t n = 0; out[n] != '\0'; n++) {
        if (out[n + 1u] == '\0') {
            copy_str(out + n + 1u, room - n - 1u, label);
            return;
        }
    }
}

/* A cache for one volume, from whole pages.
 *
 * Page-aligned for the reason `disk.c` records at length: a slot is handed
 * to a DMA engine, and a buffer straddling a page boundary needs a second
 * pointer the drivers do not supply. */
static int cache_for(struct volume *v, const struct blockdev *dev) {
    uint32_t bytes = VOLUMES_CACHE_SLOTS * dev->block_size;
    uint32_t npages = (bytes + 4095u) / 4096u;
    phys_t pages = 0;
    void *mem;
    if (mem_alloc_pages(npages, 1, &pages) != KERR_OK) {
        return KERR_NOMEM;
    }
    mem = (void *)(uintptr_t)pages;
    for (uint32_t i = 0; i < npages * 4096u; i++) {
        ((volatile uint8_t *)mem)[i] = 0;
    }
    return bcache_init(&v->cache, dev, mem, VOLUMES_CACHE_SLOTS);
}

/* Mounts one candidate and records where it went, or says why it did not.
 *
 * A candidate that is not a filesystem this kernel reads is **not** a
 * failure of automount: a disk carrying a swap partition or somebody else's
 * filesystem is an ordinary disk. It is reported at a lower volume than a
 * filesystem that is readable and failed its check, because the two mean
 * different things to whoever is reading the boot log. */
static bool mount_one(uint32_t index, const struct blockdev *dev) {
    struct volume *v = &table[count];
    int rc = cache_for(v, dev);
    if (rc != KERR_OK) {
        kprintf("volumes: no memory for volume %u\n", index);
        return false;
    }
    rc = xfs_mount(&v->fs, &v->cache);
    if (rc != KERR_OK) {
        kprintf("volumes: candidate %u is not a filesystem this kernel mounts (rc %d)\n",
                index, rc);
        return false;
    }
    mount_point_for(v->fs.label, index, v->at, sizeof v->at);
    v->used = true;
    count++;
    kprintf("volumes: \"%s\" mounted at %s%s\n", v->fs.label[0] != '\0' ? v->fs.label : "(no label)",
            v->at, xfs_writable(&v->fs) ? "" : " (read-only)");
    return true;
}

/* How many characters of a mount point a path matches, or zero.
 *
 * The length is what orders the table, rather than the table being sorted.
 * The first version did sort it — moving the root volume to the front and
 * walking backwards — and that meant copying a `struct volume` to swap two
 * entries: three kilobytes on the stack, which the frame guard refused, and
 * a block cache moved out from under whatever held a buffer in it. A length
 * comparison costs a few instructions per lookup and cannot do either. */
static uint32_t prefix_match(const char *path, const char *at) {
    uint32_t len = 0;
    if (same_str(at, "/")) {
        return 1u; /* matches everything, and is therefore the weakest match */
    }
    return begins_with(path, at, &len) ? len : 0u;
}

uint32_t volumes_automount(void) {
    uint32_t candidates = disk_volume_count();
    count = 0;
    for (uint32_t i = 0; i < candidates && count < VOLUMES_MAX; i++) {
        const struct blockdev *dev = disk_volume(i);
        if (dev != NULL) {
            (void)mount_one(i, dev);
        }
    }
    if (count == 0) {
        kprintf("volumes: nothing mounted\n");
    }
    return count;
}

struct xfs *volumes_resolve(const char *path, const char **within) {
    uint32_t best = 0;
    uint32_t pick = VOLUMES_MAX;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = table[i].used ? prefix_match(path, table[i].at) : 0u;
        if (len > best) {
            best = len;
            pick = i;
        }
    }
    if (pick == VOLUMES_MAX) {
        return NULL;
    }
    /* The path inside the volume. `/` matched as one character and owns the
     * whole path; any other mount point is stripped, and a path that named
     * the mount point itself becomes `/` rather than the empty string,
     * which no lookup would accept. */
    if (same_str(table[pick].at, "/")) {
        *within = path;
    } else {
        *within = path[best] == '\0' ? "/" : path + best;
    }
    return &table[pick].fs;
}

uint32_t volumes_count(void) {
    return count;
}

const char *volumes_at(uint32_t i) {
    return i < count ? table[i].at : "";
}

const char *volumes_label(uint32_t i) {
    return i < count ? table[i].fs.label : "";
}

bool volumes_readonly(uint32_t i) {
    return i < count ? !xfs_writable(&table[i].fs) : true;
}

bool volumes_is_the_dir(const char *path) {
    return same_str(path, VOLUMES_DIR) || same_str(path, VOLUMES_DIR "/");
}
