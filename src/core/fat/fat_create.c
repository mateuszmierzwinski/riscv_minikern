/* Names in and out of directories: create, mkdir, unlink, rename.
 *
 * An entry is its long-name pieces, if it needs any, then its short entry,
 * in consecutive slots; removing one marks every slot it took as deleted
 * (`0xe5`), which is the format's way and what `fsck.fat` reads. A new
 * directory starts with `.` and `..` in its first cluster, and moving one
 * to another parent repoints its `..` — the entry a `cd ..` follows. */
#include "core/errno.h"
#include "core/fat/fat_internal.h"

/* A name a long name may hold: not empty, not `.` or `..`, no character
 * FAT forbids, and no control character. */
static bool name_allowed(const char *name, uint32_t len) {
    static const char forbidden[] = "\\/:*?\"<>|";
    if (len == 0 || (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if ((uint8_t)name[i] < 0x20u) {
            return false;
        }
        for (uint32_t j = 0; forbidden[j] != '\0'; j++) {
            if (name[i] == forbidden[j]) {
                return false;
            }
        }
    }
    return name[len - 1u] != ' ' && name[len - 1u] != '.'; /* Windows strips both */
}

/* What a new entry is to hold, as a short entry would. */
struct entry_spec {
    uint8_t short_name[11];
    uint8_t attr;
    uint32_t first;
    uint32_t size;
    uint16_t time;
    uint16_t date;
};

static void put_short(const struct fat *fs, uint8_t *e, const struct entry_spec *s) {
    for (uint32_t i = 0; i < FAT_ENTRY; i++) {
        e[i] = 0;
    }
    for (uint32_t i = 0; i < 11u; i++) {
        e[i] = s->short_name[i];
    }
    e[FAT_DE_ATTR] = s->attr;
    fat_put16(e + FAT_DE_CTIME, s->time);
    fat_put16(e + FAT_DE_CDATE, s->date);
    fat_put16(e + FAT_DE_ADATE, s->date);
    fat_put16(e + FAT_DE_HICLUST, fs->type == 32 ? (uint16_t)(s->first >> 16) : 0);
    fat_put16(e + FAT_DE_MTIME, s->time);
    fat_put16(e + FAT_DE_MDATE, s->date);
    fat_put16(e + FAT_DE_LOCLUST, (uint16_t)s->first);
    fat_put32(e + FAT_DE_SIZE, s->size);
}

/* Long-name piece `ordinal` (from 1) of `units`, with the end-of-name
 * terminator and the 0xffff padding after it that the format specifies. */
static void put_piece(uint8_t *e, const uint16_t *units, uint32_t count, uint32_t ordinal,
                      bool last, uint8_t checksum) {
    static const uint8_t at[FAT_LFN_CHARS] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (uint32_t i = 0; i < FAT_ENTRY; i++) {
        e[i] = 0;
    }
    e[FAT_LFN_ORD] = (uint8_t)(ordinal | (last ? FAT_LFN_LAST : 0u));
    e[FAT_DE_ATTR] = FAT_ATTR_LFN;
    e[FAT_LFN_CHKSUM] = checksum;
    for (uint32_t i = 0; i < FAT_LFN_CHARS; i++) {
        uint32_t u = (ordinal - 1u) * FAT_LFN_CHARS + i;
        fat_put16(e + at[i], u < count ? units[u] : (u == count ? 0u : 0xffffu));
    }
}

/* Writes a name's pieces and short entry into slots from `first` of `dir`. */
static int write_entry(struct fat *fs, const struct fat_node *dir, uint32_t first,
                       const uint16_t *units, uint32_t count, const struct entry_spec *s,
                       uint64_t *entry_off) {
    uint32_t pieces = count == 0 ? 0 : (count + FAT_LFN_CHARS - 1u) / FAT_LFN_CHARS;
    uint8_t e[FAT_ENTRY], sum = fat_short_checksum(s->short_name);
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && i < pieces; i++) {
        uint64_t off = 0;
        put_piece(e, units, count, pieces - i, i == 0, sum);
        rc = fat_slot_offset(fs, dir, first + i, &off);
        rc = rc != KERR_OK ? rc : fat_pwrite(fs, off, e, sizeof e);
    }
    put_short(fs, e, s);
    rc = rc != KERR_OK ? rc : fat_slot_offset(fs, dir, first + pieces, entry_off);
    return rc != KERR_OK ? rc : fat_pwrite(fs, *entry_off, e, sizeof e);
}

/* The short name for `name` and, when it needs one, its long name. */
static int names_for(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
                     uint8_t short_name[11], uint16_t *units, uint32_t *count) {
    int rc = name_allowed(name, len) ? fat_utf8_to_utf16(name, len, units, count) : KERR_INVAL;
    if (rc != KERR_OK) {
        return rc;
    }
    if (fat_short_exact(name, len, short_name)) {
        *count = 0; /* stored as itself: no long name */
        return KERR_OK;
    }
    return fat_short_for(fs, dir, name, len, short_name);
}

/* Adds an entry for `name` holding what `s` says, failing on a name that
 * is taken; `out` is the new entry's node. */
static int add_entry(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
                     struct entry_spec *s, struct fat_node *out) {
    static uint16_t units[FAT_NAME_MAX];
    struct fat_found found;
    uint32_t count = 0, first = 0;
    uint64_t at = 0;
    int rc = fs->writable ? fat_find(fs, dir, name, len, &found) : KERR_ROFS;
    if (rc != KERR_NOENT) {
        return rc == KERR_OK ? KERR_EXIST : rc;
    }
    rc = names_for(fs, dir, name, len, s->short_name, units, &count);
    rc = rc != KERR_OK ? rc
                       : fat_dir_room(fs, dir, 1u + (count + FAT_LFN_CHARS - 1u) / FAT_LFN_CHARS,
                                      &first);
    rc = rc != KERR_OK ? rc : write_entry(fs, dir, first, units, count, s, &at);
    if (rc == KERR_OK) {
        uint8_t e[FAT_ENTRY];
        put_short(fs, e, s);
        fat_node_from_entry(fs, e, at, out);
    }
    return rc;
}

/* The `.` and `..` of a new directory, in its first cluster. A `..` that
 * points at the root names cluster 0, on FAT32 as everywhere. */
static int write_dots(struct fat *fs, uint32_t self, const struct fat_node *parent,
                      const struct entry_spec *s) {
    struct entry_spec dot = *s;
    uint8_t e[FAT_ENTRY];
    int rc;
    for (uint32_t i = 0; i < 11u; i++) {
        dot.short_name[i] = i == 0 ? '.' : ' ';
    }
    dot.first = self;
    put_short(fs, e, &dot);
    rc = fat_pwrite(fs, fat_cluster_offset(fs, self), e, sizeof e);
    dot.short_name[1] = '.';
    dot.first = parent->entry == 0 ? 0 : parent->first;
    put_short(fs, e, &dot);
    return rc != KERR_OK ? rc : fat_pwrite(fs, fat_cluster_offset(fs, self) + FAT_ENTRY, e, sizeof e);
}

int fat_create(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
               bool is_dir, uint64_t now_s, struct fat_node *out) {
    struct entry_spec s = {.attr = is_dir ? FAT_ATTR_DIR : FAT_ATTR_ARCH};
    int rc = dir->dir ? KERR_OK : KERR_NOTDIR;
    fat_dos_time(now_s, &s.time, &s.date);
    if (rc == KERR_OK && is_dir) {
        rc = fs->writable ? fat_take_cluster(fs, 0, &s.first) : KERR_ROFS;
        rc = rc != KERR_OK ? rc : write_dots(fs, s.first, dir, &s);
    }
    rc = rc != KERR_OK ? rc : add_entry(fs, dir, name, len, &s, out);
    if (rc != KERR_OK && is_dir && s.first != 0) {
        (void)fat_free_chain(fs, s.first); /* the name was refused: give its cluster back */
    }
    return rc;
}

/* Marks every slot a name took as deleted. */
static int erase_slots(struct fat *fs, const struct fat_node *dir, const struct fat_found *f) {
    static const uint8_t deleted = FAT_DE_DELETED;
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && i < f->slots; i++) {
        uint64_t off = 0;
        rc = fat_slot_offset(fs, dir, f->first + i, &off);
        rc = rc != KERR_OK ? rc : fat_pwrite(fs, off, &deleted, 1);
    }
    return rc;
}

/* Whether a directory holds nothing but `.` and `..`. */
static int is_empty_dir(struct fat *fs, const struct fat_node *dir, bool *empty) {
    struct fat_walk w;
    uint8_t e[FAT_ENTRY];
    uint64_t off;
    int rc;
    *empty = true;
    fat_walk_start(fs, dir, &w);
    while ((rc = fat_walk_next(fs, &w, e, &off)) == KERR_OK && e[0] != FAT_DE_END) {
        bool dots = e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '));
        if (e[0] != FAT_DE_DELETED && e[FAT_DE_ATTR] != FAT_ATTR_LFN && !dots) {
            *empty = false;
            return KERR_OK;
        }
    }
    return rc == KERR_NOENT || rc == KERR_OK ? KERR_OK : rc;
}

int fat_unlink(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len) {
    struct fat_found f;
    bool empty = true;
    int rc = fs->writable ? fat_find(fs, dir, name, len, &f) : KERR_ROFS;
    if (rc == KERR_OK && f.node.dir) {
        rc = is_empty_dir(fs, &f.node, &empty);
        rc = rc != KERR_OK ? rc : (empty ? KERR_OK : KERR_NOTEMPTY);
    }
    rc = rc != KERR_OK ? rc : erase_slots(fs, dir, &f);
    return rc != KERR_OK || f.node.first == 0 ? rc : fat_free_chain(fs, f.node.first);
}

/* Whether `dir` is `moved` or lies inside it — walking up through `..`. A
 * directory moved into its own subtree would be a loop nothing could reach. */
static int inside(struct fat *fs, const struct fat_node *dir, uint32_t moved, bool *in) {
    struct fat_node at = *dir;
    *in = false;
    for (uint32_t hops = 0; at.entry != 0 && hops < 4096u; hops++) {
        int rc;
        if (at.first == moved) {
            *in = true;
            return KERR_OK;
        }
        rc = fat_lookup(fs, &at, "..", 2u, &at);
        if (rc != KERR_OK) {
            return rc;
        }
        if (at.first == 0) {
            return KERR_OK; /* reached the root */
        }
    }
    return KERR_OK;
}

/* Repoints a moved directory's `..` at its new parent. */
static int repoint_dotdot(struct fat *fs, const struct fat_node *moved, const struct fat_node *to) {
    uint8_t e[FAT_ENTRY];
    uint32_t parent = to->entry == 0 ? 0 : to->first;
    uint64_t off = fat_cluster_offset(fs, moved->first) + FAT_ENTRY;
    int rc = fat_pread(fs, off, e, sizeof e);
    if (rc != KERR_OK || e[0] != '.' || e[1] != '.') {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    fat_put16(e + FAT_DE_HICLUST, fs->type == 32 ? (uint16_t)(parent >> 16) : 0);
    fat_put16(e + FAT_DE_LOCLUST, (uint16_t)parent);
    return fat_pwrite(fs, off, e, sizeof e);
}

/* Clears the destination for a rename: a file there is removed, anything
 * else stops it. *same says the destination is the source's own entry — a
 * change of case only, which FAT's names cannot otherwise tell apart. */
static int clear_destination(struct fat *fs, const struct fat_node *to_dir, const char *to,
                             uint32_t to_len, const struct fat_found *src, bool *same) {
    struct fat_found dst;
    int rc = fat_find(fs, to_dir, to, to_len, &dst);
    *same = false;
    if (rc == KERR_NOENT) {
        return KERR_OK;
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (dst.node.entry == src->node.entry) {
        *same = true;
        return KERR_OK;
    }
    return dst.node.dir || src->node.dir ? KERR_EXIST : fat_unlink(fs, to_dir, to, to_len);
}

/* Moves the entry: the new one written **before** the old one is erased,
 * so that a failure leaves the file under its old name rather than under
 * none — except for a change of case, where the new name would find the
 * old entry and the old one has to go first. */
static int move_entry(struct fat *fs, const struct fat_node *from_dir, struct fat_found *src,
                      const struct fat_node *to_dir, const char *to, uint32_t to_len,
                      bool same, uint64_t now_s, struct fat_node *moved) {
    struct entry_spec s = {.attr = src->node.attr, .first = src->node.first,
                           .size = src->node.size};
    int rc;
    fat_dos_time(now_s, &s.time, &s.date);
    if (same) {
        rc = erase_slots(fs, from_dir, src);
        return rc != KERR_OK ? rc : add_entry(fs, to_dir, to, to_len, &s, moved);
    }
    rc = add_entry(fs, to_dir, to, to_len, &s, moved);
    if (rc != KERR_OK) {
        return rc;
    }
    return erase_slots(fs, from_dir, src); /* a slot does not move when another is added */
}

int fat_rename(struct fat *fs, const struct fat_node *from_dir, const char *from,
               uint32_t from_len, const struct fat_node *to_dir, const char *to,
               uint32_t to_len, uint64_t now_s) {
    struct fat_found src;
    struct fat_node moved;
    bool in = false, same = false;
    int rc = fs->writable ? fat_find(fs, from_dir, from, from_len, &src) : KERR_ROFS;
    rc = rc != KERR_OK || !src.node.dir ? rc : inside(fs, to_dir, src.node.first, &in);
    rc = rc != KERR_OK ? rc
                       : (in ? KERR_INVAL : clear_destination(fs, to_dir, to, to_len, &src, &same));
    /* Found again: removing a file at the destination may have been in the
     * same directory, and the walk's slots are what erasing uses. */
    rc = rc != KERR_OK ? rc : fat_find(fs, from_dir, from, from_len, &src);
    rc = rc != KERR_OK ? rc
                       : move_entry(fs, from_dir, &src, to_dir, to, to_len, same, now_s, &moved);
    return rc != KERR_OK || !moved.dir || from_dir->first == to_dir->first
               ? rc
               : repoint_dotdot(fs, &moved, to_dir);
}
