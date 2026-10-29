/* The mount table, and automount by label. See `volumes.h` for the policy. */
#include "kernel/volumes.h"

#include "core/bcache.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/fat/fat.h"
#include "core/xfs/xfs.h"
#include "kernel/disk.h"
#include "kernel/mem.h"

/* One per partition the disk can offer, which is what bounds it. */
#define VOLUMES_MAX 4u
#define VOLUMES_PATH_MAX 48u
#define VOLUMES_CACHE_SLOTS 16u

/* One entry in the table.
 *
 * `use` is what resolution hands back, and it is not always `&own`: the
 * root filesystem is mounted by `xfsmount` — which every caller written
 * before this table existed still goes through — so the table **points at**
 * that mount rather than making a second one of its own.
 *
 * That distinction is the whole reason this struct has two fields for one
 * filesystem. Automounting every candidate *and* letting `xfsmount` mount
 * the one it adopts gave the same device two independent block caches, both
 * able to write: one holding stale metadata while the other changed it.
 * Nothing had gone wrong yet because no path resolved into the duplicate,
 * which is luck rather than design — the same luck as the card that mounted
 * at a wrong log offset because the bytes there happened to be zero. */
struct volume {
    struct xfs own;   /* storage, for a volume this table mounted itself */
    struct fat fat;   /* or, for a FAT volume, this */
    struct bcache cache;
    struct xfs *use;  /* &own, or somebody else's mount; NULL for a FAT volume */
    char at[VOLUMES_PATH_MAX];
    bool is_fat;
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

/* A candidate that is not XFS, tried as FAT: the boot partition U-Boot
 * reads, labelled `BOOT`, goes to `/boot`; any other FAT goes where its
 * label says, under `/volumes`, and `/boot` is given to the first FAT
 * after automount when none is labelled so (`settle_boot`). */
static bool mount_fat(struct volume *v, uint32_t index) {
    int rc;
    bcache_invalidate(&v->cache);
    rc = fat_mount(&v->fat, &v->cache);
    if (rc != KERR_OK) {
        kprintf("volumes: candidate %u is not a filesystem this kernel mounts (rc %d)\n",
                index, rc);
        return false;
    }
    v->use = NULL;
    v->is_fat = true;
    if (same_str(v->fat.label, VOLUMES_BOOT_LABEL)) {
        copy_str(v->at, sizeof v->at, VOLUMES_BOOT_DIR);
    } else {
        mount_point_for(v->fat.label, index, v->at, sizeof v->at);
    }
    v->used = true;
    count++;
    kprintf("volumes: FAT%u \"%s\" mounted at %s%s\n", v->fat.type,
            v->fat.label[0] != '\0' ? v->fat.label : "(no label)", v->at,
            v->fat.writable ? "" : " (read-only)");
    return true;
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
    rc = xfs_mount(&v->own, &v->cache);
    v->use = &v->own;
    v->is_fat = false;
    if (rc != KERR_OK) {
        return mount_fat(v, index);
    }
    mount_point_for(v->own.label, index, v->at, sizeof v->at);
    v->used = true;
    count++;
    kprintf("volumes: \"%s\" mounted at %s%s\n",
            v->own.label[0] != '\0' ? v->own.label : "(no label)", v->at,
            xfs_writable(&v->own) ? "" : " (read-only)");
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

void volumes_register(struct xfs *fs, uint32_t index) {
    struct volume *v;
    if (count >= VOLUMES_MAX || fs == NULL) {
        return;
    }
    v = &table[count];
    v->use = fs;
    v->used = true;
    mount_point_for(fs->label, index, v->at, sizeof v->at);
    count++;
    kprintf("volumes: \"%s\" at %s%s (the root filesystem's own mount)\n",
            fs->label[0] != '\0' ? fs->label : "(no label)", v->at,
            xfs_writable(fs) ? "" : " (read-only)");
}

/* **`/boot` is the boot partition, whatever it is called.** One labelled
 * `BOOT` already went there; otherwise the first FAT volume does — a card
 * formatted for U-Boot need not have labelled its FAT, and the one the
 * firmware loads the kernel from is the one a kernel update must reach. */
static void settle_boot(void) {
    for (uint32_t i = 0; i < count; i++) {
        if (same_str(table[i].at, VOLUMES_BOOT_DIR)) {
            return;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (table[i].is_fat) {
            copy_str(table[i].at, sizeof table[i].at, VOLUMES_BOOT_DIR);
            kprintf("volumes: FAT at %s is the boot partition\n", VOLUMES_BOOT_DIR);
            return;
        }
    }
}

uint32_t volumes_automount(void) {
    uint32_t candidates = disk_volume_count();
    count = 0;
    /* **Every candidate but the root.** The root is the one `disk.c`
     * chose and `xfsmount` mounted, and mounting it here as well would be
     * the same device under two caches. It reaches the table through
     * `volumes_register` instead, pointed at rather than copied. */
    for (uint32_t i = 0; i < candidates && count < VOLUMES_MAX; i++) {
        const struct blockdev *dev = disk_volume(i);
        if (dev != NULL && i != disk_root_volume()) {
            (void)mount_one(i, dev);
        }
    }
    settle_boot();
    return count;
}

/* The volume a path is on — the longest mount point it lies under — and
 * the path inside it; VOLUMES_MAX for none. */
static uint32_t pick_volume(const char *path, const char **within) {
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
        return VOLUMES_MAX;
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
    return pick;
}

/* **NULL with `within` NULL for a path on a FAT volume**, so the caller
 * cannot mistake it for "no volume at all" and fall back to the root — which
 * would have answered for `/boot` out of the root filesystem. */
struct xfs *volumes_resolve(const char *path, const char **within) {
    uint32_t pick = pick_volume(path, within);
    if (pick == VOLUMES_MAX) {
        return NULL;
    }
    if (table[pick].is_fat) {
        *within = NULL;
        return NULL;
    }
    return table[pick].use;
}

struct fat *volumes_resolve_fat(const char *path, const char **within) {
    uint32_t pick = pick_volume(path, within);
    return pick != VOLUMES_MAX && table[pick].is_fat ? &table[pick].fat : NULL;
}

int volumes_flush_fat(void) {
    int rc = KERR_OK;
    for (uint32_t i = 0; i < count; i++) {
        int one = table[i].is_fat ? fat_flush(&table[i].fat) : KERR_OK;
        rc = rc == KERR_OK ? one : rc;
    }
    return rc;
}

uint32_t volumes_count(void) {
    return count;
}

const char *volumes_at(uint32_t i) {
    return i < count ? table[i].at : "";
}

const char *volumes_label(uint32_t i) {
    if (i < count && table[i].is_fat) {
        return table[i].fat.label;
    }
    return i < count && table[i].use != NULL ? table[i].use->label : "";
}

bool volumes_readonly(uint32_t i) {
    if (i < count && table[i].is_fat) {
        return !table[i].fat.writable;
    }
    return i < count && table[i].use != NULL ? !xfs_writable(table[i].use) : true;
}

bool volumes_is_the_dir(const char *path) {
    return same_str(path, VOLUMES_DIR) || same_str(path, VOLUMES_DIR "/");
}
