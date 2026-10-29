/* Directories: walking their slots, putting long names back together, and
 * finding room for new entries.
 *
 * A directory is a run of 32-byte slots — in the fixed region before the
 * data on FAT12/16's root, in a cluster chain everywhere else. A long name
 * is a run of pieces stored **last piece first**, each carrying 13 UTF-16
 * units and the checksum of the short entry that follows them; a piece
 * whose checksum or order does not fit is not part of the name, and the
 * entry is then known by its short name alone, which is what `fsck.fat`
 * and every other reader do with an orphaned piece. */
#include "core/errno.h"
#include "core/fat/fat_internal.h"

static bool is_fixed_root(const struct fat *fs, const struct fat_node *dir) {
    return fs->type != 32 && dir->entry == 0 && dir->first == 0;
}

void fat_walk_start(const struct fat *fs, const struct fat_node *dir, struct fat_walk *w) {
    (void)fs;
    *w = (struct fat_walk){.dir = dir, .cluster = dir->first, .index = 0};
}

int fat_walk_next(struct fat *fs, struct fat_walk *w, uint8_t *slot, uint64_t *off) {
    uint32_t per = fs->cluster / FAT_ENTRY;
    if (is_fixed_root(fs, w->dir)) {
        if (w->index >= fs->root_entries) {
            return KERR_NOENT;
        }
        *off = fs->root_start + (uint64_t)w->index * FAT_ENTRY;
    } else {
        if (w->index > 0 && w->index % per == 0) {
            uint32_t next = 0;
            int rc = fat_get(fs, w->cluster, &next);
            if (rc != KERR_OK || fat_is_end(fs, next)) {
                return rc != KERR_OK ? rc : KERR_NOENT;
            }
            w->cluster = next;
        }
        if (!fat_cluster_ok(fs, w->cluster) || w->index >= 65536u) {
            return w->index >= 65536u ? KERR_NOENT : KERR_IO;
        }
        *off = fat_cluster_offset(fs, w->cluster) + (uint64_t)(w->index % per) * FAT_ENTRY;
    }
    w->index++;
    return fat_pread(fs, *off, slot, FAT_ENTRY);
}

int fat_slot_offset(struct fat *fs, const struct fat_node *dir, uint32_t index, uint64_t *off) {
    uint32_t per = fs->cluster / FAT_ENTRY, c = 0;
    int rc;
    if (is_fixed_root(fs, dir)) {
        *off = fs->root_start + (uint64_t)index * FAT_ENTRY;
        return index < fs->root_entries ? KERR_OK : KERR_INVAL;
    }
    rc = fat_nth_cluster(fs, dir->first, index / per, &c);
    *off = fat_cluster_offset(fs, c) + (uint64_t)(index % per) * FAT_ENTRY;
    return rc;
}

void fat_node_from_entry(const struct fat *fs, const uint8_t *e, uint64_t off,
                         struct fat_node *out) {
    uint32_t hi = fs->type == 32 ? fat_le16(e + FAT_DE_HICLUST) : 0;
    *out = (struct fat_node){.first = (hi << 16) | fat_le16(e + FAT_DE_LOCLUST),
                             .size = fat_le32(e + FAT_DE_SIZE), .attr = e[FAT_DE_ATTR],
                             .entry = off, .mtime = fat_le16(e + FAT_DE_MTIME),
                             .mdate = fat_le16(e + FAT_DE_MDATE)};
    out->dir = (out->attr & FAT_ATTR_DIR) != 0;
    if (out->dir) {
        out->size = 0;
    }
}

/* ── long names ───────────────────────────────────────────────────────── */

/* A long name being gathered from its pieces. */
struct lfn {
    uint16_t units[FAT_NAME_MAX + FAT_LFN_CHARS];
    uint32_t len;       /* units, once complete */
    uint32_t expect;    /* the ordinal the next piece must carry, 0 for none */
    uint32_t first;     /* the slot the name began at */
    uint8_t checksum;
};

static const uint8_t piece_at[FAT_LFN_CHARS] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

/* Copies one piece's thirteen units into their place in the name. */
static void take_piece(struct lfn *l, const uint8_t *e, uint32_t ordinal) {
    for (uint32_t i = 0; i < FAT_LFN_CHARS; i++) {
        uint16_t u = fat_le16(e + piece_at[i]);
        uint32_t at = (ordinal - 1u) * FAT_LFN_CHARS + i;
        l->units[at] = u;
        if (u == 0 && at < l->len) {
            l->len = at;
        }
    }
}

/* Feeds one long-name slot into `l`: a last piece starts a name, the next
 * in order continues it, anything else drops it. */
static void lfn_piece(struct lfn *l, const uint8_t *e, uint32_t index) {
    uint32_t ordinal = e[FAT_LFN_ORD] & 0x1fu;
    if ((e[FAT_LFN_ORD] & FAT_LFN_LAST) != 0 && ordinal != 0 && ordinal <= 20u) {
        l->expect = ordinal;
        l->len = ordinal * FAT_LFN_CHARS;
        l->first = index;
        l->checksum = e[FAT_LFN_CHKSUM];
    } else if (l->expect == 0 || ordinal != l->expect - 1u ||
               e[FAT_LFN_CHKSUM] != l->checksum) {
        l->expect = 0;
        return;
    }
    l->expect = ordinal;
    take_piece(l, e, ordinal);
}

/* The name an entry is known by — its long name when the pieces before it
 * are whole and belong to it, its 8.3 name otherwise — and the slot that
 * name began at. */
static uint32_t entry_name(struct lfn *l, const uint8_t *e, uint32_t index, char *out,
                           uint32_t *first) {
    bool whole = l->expect == 1u && l->checksum == fat_short_checksum(e) && l->len > 0;
    uint32_t n = whole ? fat_utf16_to_utf8(l->units, l->len, out) : fat_short_render(e, out);
    *first = whole ? l->first : index;
    l->expect = 0;
    return n;
}

/* Walks a directory, handing each live entry's name, node and slots to
 * `each`; stops early when it returns anything but KERR_OK, and passes
 * that on. */
typedef int (*entry_fn)(void *self, const char *name, uint32_t len, const struct fat_found *f);

static int walk_entries(struct fat *fs, const struct fat_node *dir, entry_fn each, void *self) {
    static struct lfn l; /* static: 500 bytes, and one walk runs at a time */
    static char name[FAT_NAME_MAX * 3u + 1u];
    struct fat_walk w;
    uint8_t e[FAT_ENTRY];
    uint64_t off;
    int rc;
    l.expect = 0;
    fat_walk_start(fs, dir, &w);
    while ((rc = fat_walk_next(fs, &w, e, &off)) == KERR_OK && e[0] != FAT_DE_END) {
        struct fat_found f;
        uint32_t n;
        if (e[0] == FAT_DE_DELETED) {
            l.expect = 0;
        } else if (e[FAT_DE_ATTR] == FAT_ATTR_LFN) {
            lfn_piece(&l, e, w.index - 1u);
        } else if ((e[FAT_DE_ATTR] & FAT_ATTR_VOLUME) != 0) {
            l.expect = 0;
        } else {
            n = entry_name(&l, e, w.index - 1u, name, &f.first);
            fat_node_from_entry(fs, e, off, &f.node);
            f.slots = w.index - f.first;
            rc = each(self, name, n, &f);
            if (rc != KERR_OK) {
                return rc;
            }
        }
    }
    return rc == KERR_NOENT || rc == KERR_OK ? KERR_OK : rc;
}

/* ── finding and listing ─────────────────────────────────────────────── */

struct finding {
    const char *name;
    uint32_t len;
    struct fat_found *out;
};

/* `KERR_EXIST` stops the walk: it is how a match leaves it. */
static int match(void *self, const char *name, uint32_t len, const struct fat_found *f) {
    struct finding *q = self;
    if (!fat_same_name(name, len, q->name, q->len)) {
        return KERR_OK;
    }
    *q->out = *f;
    return KERR_EXIST;
}

int fat_find(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
             struct fat_found *out) {
    struct finding q = {name, len, out};
    int rc = dir->dir ? walk_entries(fs, dir, match, &q) : KERR_NOTDIR;
    return rc == KERR_EXIST ? KERR_OK : (rc == KERR_OK ? KERR_NOENT : rc);
}

int fat_lookup(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
               struct fat_node *out) {
    struct fat_found f;
    int rc = fat_find(fs, dir, name, len, &f);
    if (rc == KERR_OK) {
        *out = f.node;
    }
    return rc;
}

/* A `..` entry names its parent's first cluster, and 0 for the root —
 * which on FAT32 is not cluster 0. */
static void fix_dotdot(const struct fat *fs, struct fat_node *n) {
    if (n->dir && n->first == 0) {
        (void)fat_root(fs, n);
    }
}

int fat_path_lookup(struct fat *fs, const char *path, struct fat_node *out) {
    uint32_t at = 0;
    int rc = fat_root(fs, out);
    while (rc == KERR_OK && path[at] != '\0') {
        uint32_t start, len = 0;
        while (path[at] == '/') {
            at++;
        }
        start = at;
        while (path[at] != '\0' && path[at] != '/') {
            at++;
            len++;
        }
        if (len > 0) {
            struct fat_node dir = *out;
            rc = fat_lookup(fs, &dir, path + start, len, out);
            fix_dotdot(fs, out);
        }
    }
    return rc;
}

struct listing {
    const struct fat *fs;
    fat_dirent_fn fn;
    void *self;
};

static int hand_over(void *self, const char *name, uint32_t len, const struct fat_found *f) {
    struct listing *l = self;
    static struct fat_dirent d;
    for (uint32_t i = 0; i <= len; i++) {
        d.name[i] = name[i];
    }
    d.node = f->node;
    fix_dotdot(l->fs, &d.node);
    return l->fn(l->self, &d);
}

int fat_list(struct fat *fs, const struct fat_node *dir, fat_dirent_fn fn, void *self) {
    struct listing l = {fs, fn, self};
    return dir->dir ? walk_entries(fs, dir, hand_over, &l) : KERR_NOTDIR;
}

/* ── room for new entries ─────────────────────────────────────────────── */

/* Scans for `slots` free slots in a row; *first is where they start, or
 * the directory's end when it ran out first, with *run how many free slots
 * lead up to that end. */
static int scan_free(struct fat *fs, const struct fat_node *dir, uint32_t slots,
                     uint32_t *first, uint32_t *run) {
    struct fat_walk w;
    uint8_t e[FAT_ENTRY];
    uint64_t off;
    int rc;
    *run = 0;
    fat_walk_start(fs, dir, &w);
    while ((rc = fat_walk_next(fs, &w, e, &off)) == KERR_OK) {
        bool free = e[0] == FAT_DE_END || e[0] == FAT_DE_DELETED;
        *run = free ? *run + 1u : 0;
        if (*run == slots) {
            *first = w.index - slots;
            return KERR_OK;
        }
    }
    *first = w.index - *run;
    return rc == KERR_NOENT ? KERR_NOSPC : rc;
}

/* The last cluster of a chain. */
static int last_cluster(struct fat *fs, uint32_t first, uint32_t *out) {
    uint32_t c = first;
    for (uint32_t hops = 0; hops < fs->clusters; hops++) {
        uint32_t next = 0;
        int rc = fat_get(fs, c, &next);
        if (rc != KERR_OK || fat_is_end(fs, next)) {
            *out = c;
            return rc;
        }
        c = next;
    }
    return KERR_IO;
}

int fat_dir_room(struct fat *fs, const struct fat_node *dir, uint32_t slots, uint32_t *first) {
    uint32_t run = 0, per = fs->cluster / FAT_ENTRY;
    int rc = scan_free(fs, dir, slots, first, &run);
    if (rc != KERR_NOSPC || is_fixed_root(fs, dir)) {
        return rc;
    }
    /* Grown by as many zeroed clusters as the rest of the run needs; a zeroed
     * slot is an end marker, so what follows the new entries reads as the
     * directory's end. */
    for (uint32_t have = run; rc == KERR_NOSPC || have < slots; have += per) {
        uint32_t last = 0, fresh = 0;
        rc = last_cluster(fs, dir->first, &last);
        rc = rc != KERR_OK ? rc : fat_take_cluster(fs, last, &fresh);
        if (rc != KERR_OK) {
            return rc;
        }
        rc = have + per >= slots ? KERR_OK : KERR_NOSPC;
    }
    return KERR_OK;
}
