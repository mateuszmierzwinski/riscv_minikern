/* The mount table, and automount by label. See `volumes.h` for the policy. */
#include "kernel/volumes.h"

#include "core/bcache.h"
#include "core/console.h"
#include "arch/riscv/mmu.h"
#include "core/errno.h"
#include "core/fat/fat.h"
#include "core/vfs.h"
#include "kernel/fatmount.h"
#include "core/xfs/xfs.h"
#include "kernel/disk.h"
#include "kernel/mem.h"

/* The root disk's partitions and a few more that `mount(2)` brought in. */
#define VOLUMES_MAX 8u
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
    struct blockdev ro; /* the volume behind a refusing writer, for a read-only mount */
    phys_t pages;       /* the cache's memory, kept for the slot's next mount */
    bool gathering;     /* its run-writing buffer is given (once per slot) */
    uint32_t npages;
    uint32_t vol;       /* which `disk_volume`, or NO_VOLUME */
    char at[VOLUMES_PATH_MAX];
    bool is_fat;
    bool used;
};

#define NO_VOLUME 0xffffffffu

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
    void *mem;
    /* **Kept for the slot, not freed at unmount**: a slot mounted and
     * unmounted again and again must not cost a cache each time. */
    if (v->pages != 0 && v->npages < npages) {
        return KERR_NOMEM;
    }
    if (v->pages == 0 && mem_alloc_pages(npages, 1, &v->pages) != KERR_OK) {
        v->pages = 0;
        return KERR_NOMEM;
    }
    v->npages = v->npages > npages ? v->npages : npages;
    mem = mmu_phys_to_virt(v->pages); /* never the identity alias (step C) */
    for (uint32_t i = 0; i < v->npages * 4096u; i++) {
        ((volatile uint8_t *)mem)[i] = 0;
    }
    {
        uint8_t *gather = v->cache.gather;
        uint32_t gather_slots = v->cache.gather_slots;
        int rc = bcache_init(&v->cache, dev, mem, VOLUMES_CACHE_SLOTS);
        if (rc == KERR_OK && v->gathering) {
            bcache_set_gather(&v->cache, gather, gather_slots); /* kept across mounts */
        } else if (rc == KERR_OK) {
            give_the_cache_a_gather_buffer(&v->cache, dev->block_size);
            v->gathering = v->cache.gather != NULL;
        }
        return rc;
    }
}

/* The first free slot, claimed; NULL when the table is full. `count` is the
 * high-water mark the loops below walk, holes included. */
static struct volume *take_slot(void) {
    for (uint32_t i = 0; i < VOLUMES_MAX; i++) {
        if (!table[i].used) {
            table[i].used = true;
            table[i].vol = NO_VOLUME;
            count = i + 1u > count ? i + 1u : count;
            return &table[i];
        }
    }
    return NULL;
}

/* Gives a slot back, keeping its cache memory for the next mount. */
static void release_slot(struct volume *v) {
    v->used = false;
    v->is_fat = false;
    v->use = NULL;
    v->vol = NO_VOLUME;
    v->at[0] = '\0';
    while (count > 0 && !table[count - 1u].used) {
        count--;
    }
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
    if (v->at[0] != '\0') {
        /* the caller's mount point (mount(2)) */
    } else if (same_str(v->fat.label, VOLUMES_BOOT_LABEL)) {
        copy_str(v->at, sizeof v->at, VOLUMES_BOOT_DIR);
    } else {
        mount_point_for(v->fat.label, index, v->at, sizeof v->at);
    }
    if (vfs_mount(v->at, &fatmount_vfs_ops, &v->fat) != KERR_OK) {
        kprintf("volumes: the mount table refused %s\n", v->at);
    }
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
static bool mount_into(struct volume *v, uint32_t index, const struct blockdev *dev) {
    int rc = cache_for(v, dev);
    if (rc != KERR_OK) {
        kprintf("volumes: no memory for volume %u\n", index);
        return false;
    }
    v->vol = index;
    rc = xfs_mount(&v->own, &v->cache);
    v->use = &v->own;
    v->is_fat = false;
    if (rc != KERR_OK) {
        return mount_fat(v, index);
    }
    if (v->at[0] == '\0') {
        mount_point_for(v->own.label, index, v->at, sizeof v->at);
    }
    kprintf("volumes: \"%s\" mounted at %s%s\n",
            v->own.label[0] != '\0' ? v->own.label : "(no label)", v->at,
            xfs_writable(&v->own) ? "" : " (read-only)");
    return true;
}

/* Automount's: a slot, the label's mount point, given back on failure. */
static bool mount_one(uint32_t index, const struct blockdev *dev) {
    struct volume *v = take_slot();
    if (v == NULL) {
        return false;
    }
    v->at[0] = '\0';
    if (!mount_into(v, index, dev)) {
        release_slot(v);
        return false;
    }
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
    v = take_slot();
    if (v == NULL) {
        return;
    }
    v->use = fs;
    v->vol = index;
    mount_point_for(fs->label, index, v->at, sizeof v->at);
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
        if (table[i].used && same_str(table[i].at, VOLUMES_BOOT_DIR)) {
            return;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (table[i].used && table[i].is_fat) {
            (void)vfs_umount(table[i].at);
            copy_str(table[i].at, sizeof table[i].at, VOLUMES_BOOT_DIR);
            (void)vfs_mount(table[i].at, &fatmount_vfs_ops, &table[i].fat);
            kprintf("volumes: FAT at %s is the boot partition\n", VOLUMES_BOOT_DIR);
            return;
        }
    }
}

/* Takes the previous automount's FAT volumes out of the mount table, since
 * the table entries they point at are about to be reused. */
static void forget_fat_mounts(void) {
    for (uint32_t i = 0; i < count; i++) {
        if (table[i].used && table[i].is_fat) {
            (void)vfs_umount(table[i].at);
        }
        table[i].used = false;
    }
}

uint32_t volumes_automount(void) {
    uint32_t candidates = disk_root_disk_count();
    forget_fat_mounts();
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
        int one = table[i].used && table[i].is_fat ? fat_flush(&table[i].fat) : KERR_OK;
        rc = rc == KERR_OK ? one : rc;
    }
    return rc;
}

/* The i-th mounted entry, holes skipped; NULL past the end. */
static const struct volume *nth(uint32_t i) {
    for (uint32_t s = 0; s < count; s++) {
        if (table[s].used && i-- == 0u) {
            return &table[s];
        }
    }
    return NULL;
}

static bool entry_readonly(const struct volume *v) {
    if (v->is_fat) {
        return !v->fat.writable;
    }
    return v->use != NULL ? !xfs_writable(v->use) : true;
}

uint32_t volumes_count(void) {
    uint32_t n = 0;
    for (uint32_t s = 0; s < count; s++) {
        n += table[s].used ? 1u : 0u;
    }
    return n;
}

const char *volumes_at(uint32_t i) {
    const struct volume *v = nth(i);
    return v != NULL ? v->at : "";
}

const char *volumes_label(uint32_t i) {
    const struct volume *v = nth(i);
    if (v != NULL && v->is_fat) {
        return v->fat.label;
    }
    return v != NULL && v->use != NULL ? v->use->label : "";
}

bool volumes_readonly(uint32_t i) {
    const struct volume *v = nth(i);
    return v != NULL ? entry_readonly(v) : true;
}

const char *volumes_mount_point_of(uint32_t vol, bool *readonly) {
    for (uint32_t s = 0; s < count; s++) {
        if (table[s].used && table[s].vol == vol) {
            *readonly = entry_readonly(&table[s]);
            return table[s].at;
        }
    }
    return NULL;
}

bool volumes_is_the_dir(const char *path) {
    return same_str(path, VOLUMES_DIR) || same_str(path, VOLUMES_DIR "/");
}

/* ── mount(2) and umount2(2) of a block volume ─────────────────────────── */

/* What decides "in use": the composition root's, because the descriptor
 * tables live above this layer. NULL: nothing is ever in use. */
static bool (*busy_check)(const char *at);

void volumes_set_busy_check(bool (*fn)(const char *at)) {
    busy_check = fn;
}

static int ro_write(void *self, uint64_t lba, uint32_t n, const void *buf) {
    (void)self;
    (void)lba;
    (void)n;
    (void)buf;
    return KERR_ROFS;
}

/* Nothing was written, so there is nothing to make durable. */
static int ro_flush(void *self) {
    (void)self;
    return KERR_OK;
}

/* The volume behind a writer that refuses, for a read-only mount. **The
 * refusal is the device's, not the filesystem's**: a filesystem that
 * decided to write anyway — a log replay, an access time — still cannot. */
static const struct blockdev *read_only_view(struct volume *v, const struct blockdev *dev) {
    v->ro = *dev;
    v->ro.write = ro_write;
    v->ro.flush = ro_flush;
    v->ro.discard = NULL;
    v->ro.read_only = true;
    return &v->ro;
}

/* A mount point a mount(2) may use: `/volumes/<name>`, the name one
 * component of letters, digits, `_`, `.` and `-`, and not `.` or `..`. */
static bool valid_mount_point(const char *at) {
    const char *prefix = VOLUMES_DIR "/";
    uint32_t i = 0, n = 0;
    while (prefix[i] != '\0') {
        if (at[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    for (; at[i + n] != '\0'; n++) {
        char c = at[i + n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-';
        if (!ok || i + n + 1u >= VOLUMES_PATH_MAX) {
            return false;
        }
    }
    return n > 0u && !same_str(at + i, ".") && !same_str(at + i, "..");
}

static struct volume *entry_at(const char *at) {
    for (uint32_t s = 0; s < count; s++) {
        if (table[s].used && same_str(table[s].at, at)) {
            return &table[s];
        }
    }
    return NULL;
}

/* Why volume `vol` may not be mounted at `at` as `fstype`, or KERR_OK. */
static int mount_refusal(uint32_t vol, const char *at, const char *fstype) {
    const struct volinfo *info = disk_volume_info(vol);
    bool ro = false;
    if (info == NULL) {
        return KERR_NOENT;
    }
    if (same_str(info->fstype, "unknown") ||
        !(same_str(fstype, info->fstype) || same_str(fstype, "auto"))) {
        return KERR_INVAL; /* Linux's answer to a type the volume is not */
    }
    if (!valid_mount_point(at)) {
        return KERR_INVAL;
    }
    if (volumes_mount_point_of(vol, &ro) != NULL || entry_at(at) != NULL) {
        return KERR_BUSY;
    }
    return KERR_OK;
}

int volumes_mount_named(const char *name, const char *at, const char *fstype, bool readonly) {
    uint32_t vol = disk_volume_named(name);
    int rc = mount_refusal(vol, at, fstype);
    struct volume *v;
    const struct blockdev *dev;
    if (rc != KERR_OK) {
        return rc;
    }
    v = take_slot();
    if (v == NULL) {
        return KERR_NOMEM;
    }
    copy_str(v->at, sizeof v->at, at);
    dev = disk_volume(vol);
    dev = readonly ? read_only_view(v, dev) : dev;
    if (!mount_into(v, vol, dev)) {
        release_slot(v);
        return KERR_INVAL;
    }
    kprintf("volumes: %s mounted at %s by request%s\n", name, at, readonly ? " (read-only)" : "");
    return KERR_OK;
}

bool volumes_is_mount_point(const char *at) {
    return entry_at(at) != NULL;
}

int volumes_umount(const char *at) {
    struct volume *v = entry_at(at);
    int rc;
    if (v == NULL) {
        return KERR_INVAL;
    }
    /* The root, and anything mounted by somebody else's code (`use` not
     * this slot's own), stay: the root has nowhere to be unmounted to. */
    if (same_str(at, "/") || (!v->is_fat && v->use != &v->own)) {
        return KERR_BUSY;
    }
    if (busy_check != NULL && busy_check(at)) {
        return KERR_BUSY;
    }
    rc = v->is_fat ? fat_flush(&v->fat) : xfs_flush(&v->own);
    if (rc != KERR_OK && rc != KERR_ROFS) {
        return rc;
    }
    if (v->is_fat) {
        (void)vfs_umount(at);
    }
    kprintf("volumes: %s unmounted\n", at);
    release_slot(v);
    return KERR_OK;
}

uint32_t volumes_xfs_claim(const char *path) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = table[i].used && !table[i].is_fat ? prefix_match(path, table[i].at) : 0u;
        best = len > best ? len : best;
    }
    return best;
}
