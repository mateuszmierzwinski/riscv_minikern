#include "kernel/overlayfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/errno.h"
#include "core/overlay.h"
#include "core/vfs.h"
#include "kernel/mem.h"
#include "kernel/xfsmount.h"

#define OVERLAY_COPY_MAX (64u << 20)    /* the largest file copied up whole */
#define OVERLAY_SEEN_BYTES 32768u       /* names one merged listing remembers */
#define S_IFMT_BITS 0170000u
#define S_IFDIR_BITS 0040000u

struct overlay {
    char lower[OVERLAY_PATH_MAX];
    char upper[OVERLAY_PATH_MAX];
};

/* Three paths, off the stack: each is 256 bytes and the frame limit is 1 KiB. */
struct scratch {
    char a[OVERLAY_PATH_MAX];
    char b[OVERLAY_PATH_MAX];
    char c[OVERLAY_PATH_MAX];
};

static struct overlay overlays[OVERLAY_MOUNTS];
static const struct vfs_ops overlay_ops;

static int layer(const struct overlay *o, bool upper, const char *in, char out[OVERLAY_PATH_MAX]) {
    return overlay_join(upper ? o->upper : o->lower, in, out);
}

static bool exists(const char *path, uint32_t *mode) {
    uint32_t nlink = 0, m = 0;
    uint64_t size = 0;
    bool there = xfsmount_stat_full(path, &m, &size, &nlink) == KERR_OK;
    if (mode != NULL) {
        *mode = m;
    }
    return there;
}

static bool is_dir(uint32_t mode) {
    return (mode & S_IFMT_BITS) == S_IFDIR_BITS;
}

/* Whether a whiteout in the upper layer hides `in` or any directory above
 * it: a removed directory hides everything the lower layer has under it. */
static bool hidden(const struct overlay *o, const char *in, struct scratch *s) {
    for (uint32_t end = 1; in[end - 1u] != '\0'; end++) {
        if (in[end] != '/' && in[end] != '\0') {
            continue;
        }
        for (uint32_t i = 0; i < end; i++) {
            s->a[i] = in[i];
        }
        s->a[end] = '\0';
        if (overlay_whiteout_of(s->a, s->b) == KERR_OK && layer(o, true, s->b, s->c) == KERR_OK &&
            exists(s->c, NULL)) {
            return true;
        }
    }
    return false;
}

/* The real path `in` is seen at — upper first — into s->a; KERR_NOENT when
 * neither layer has it or a whiteout hides it. *upper says which layer. */
static int visible(const struct overlay *o, const char *in, struct scratch *s, bool *upper) {
    if (in[0] != '\0' && !(in[0] == '/' && in[1] == '\0') && hidden(o, in, s)) {
        return KERR_NOENT;
    }
    for (int up = 1; up >= 0; up--) {
        if (layer(o, up == 1, in, s->a) == KERR_OK && exists(s->a, NULL)) {
            *upper = up == 1;
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

/* Runs fn on the path `in` is seen at, with a scratch area to hold it. */
static int on_visible(void *self, const char *in, int (*fn)(const char *path, void *arg),
                      void *arg) {
    struct scratch *s = kzalloc(sizeof *s);
    bool upper = false;
    int rc = s == NULL ? KERR_NOMEM : visible(self, in, s, &upper);
    rc = rc != KERR_OK ? rc : fn(s->a, arg);
    kfree(s);
    return rc;
}

struct stat_args {
    uint32_t *mode;
    uint64_t *size;
    uint32_t *nlink;
};

static int stat_at(const char *path, void *arg) {
    struct stat_args *a = arg;
    return xfsmount_stat_full(path, a->mode, a->size, a->nlink);
}

static int stat_op(void *self, const char *in, uint32_t *mode, uint64_t *size, uint32_t *nlink) {
    struct stat_args a = {.mode = mode, .size = size, .nlink = nlink};
    return on_visible(self, in, stat_at, &a);
}

struct owner_args {
    uint32_t *uid;
    uint32_t *gid;
};

static int owner_at(const char *path, void *arg) {
    struct owner_args *a = arg;
    return xfsmount_owner_of(path, a->uid, a->gid);
}

static int owner_op(void *self, const char *in, uint32_t *uid, uint32_t *gid) {
    struct owner_args a = {.uid = uid, .gid = gid};
    return on_visible(self, in, owner_at, &a);
}

struct read_args {
    uint32_t limit;
    uint8_t **out;
    uint32_t *size;
};

static int read_at(const char *path, void *arg) {
    struct read_args *a = arg;
    return xfsmount_read_file(path, a->limit, a->out, a->size);
}

static int read_op(void *self, const char *in, uint32_t limit, uint8_t **out, uint32_t *size) {
    struct read_args a = {.limit = limit, .out = out, .size = size};
    return on_visible(self, in, read_at, &a);
}

struct times_args {
    uint64_t *atime, *mtime, *ctime;
};

static int times_at(const char *path, void *arg) {
    struct times_args *a = arg;
    return xfsmount_times(path, a->atime, a->mtime, a->ctime);
}

/* The times of the copy that is seen, upper or lower. */
static int times_op(void *self, const char *in, uint64_t *atime, uint64_t *mtime,
                    uint64_t *ctime) {
    struct times_args a = {.atime = atime, .mtime = mtime, .ctime = ctime};
    return on_visible(self, in, times_at, &a);
}

/* ── a merged listing ─────────────────────────────────────────────────── */

/* The names the upper layer showed or whited out, so the lower layer's
 * listing leaves them out: NUL-separated in one bounded buffer. */
struct merge {
    xfs_dirent_fn fn;
    void *ctx;
    char *names;
    uint32_t used;
    bool full;
};

static bool seen(const struct merge *m, const char *name, uint32_t len) {
    uint32_t at = 0;
    while (at < m->used) {
        uint32_t n = 0;
        while (m->names[at + n] != '\0') {
            n++;
        }
        bool same = n == len;
        for (uint32_t i = 0; same && i < n; i++) {
            same = m->names[at + i] == name[i];
        }
        if (same) {
            return true;
        }
        at += n + 1u;
    }
    return false;
}

static void remember(struct merge *m, const char *name, uint32_t len) {
    if (m->used + len + 1u > OVERLAY_SEEN_BYTES) {
        m->full = true;
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        m->names[m->used + i] = name[i];
    }
    m->names[m->used + len] = '\0';
    m->used += len + 1u;
}

static int upper_entry(void *self, const struct xfs_dirent *e) {
    struct merge *m = self;
    bool whiteout = overlay_is_whiteout(e->name, e->name_len);
    uint32_t skip = whiteout ? 4u : 0u;
    remember(m, e->name + skip, e->name_len - skip);
    return whiteout ? KERR_OK : m->fn(m->ctx, e);
}

static int lower_entry(void *self, const struct xfs_dirent *e) {
    struct merge *m = self;
    return seen(m, e->name, e->name_len) ? KERR_OK : m->fn(m->ctx, e);
}

/* Lists one layer's copy of `in`; *found is set when that layer has it. */
static int list_layer(const struct overlay *o, bool upper, const char *in, struct merge *m,
                      struct scratch *s, bool *found) {
    int rc = layer(o, upper, in, s->a);
    if (rc != KERR_OK || !exists(s->a, NULL)) {
        return rc;
    }
    *found = true;
    return xfsmount_list(s->a, upper ? upper_entry : lower_entry, m);
}

/* Upper entries first, then the lower ones the upper layer neither shows
 * nor whited out. A directory with more names than the buffer holds is
 * refused (KERR_FBIG) rather than listed with lower files reappearing. */
static int list_op(void *self, const char *in, xfs_dirent_fn fn, void *ctx) {
    struct scratch *s = kzalloc(sizeof *s);
    struct merge m = {.fn = fn, .ctx = ctx, .names = kzalloc(OVERLAY_SEEN_BYTES)};
    bool found = false;
    int rc = s == NULL || m.names == NULL ? KERR_NOMEM : KERR_OK;
    rc = rc != KERR_OK || !hidden(self, in, s) ? rc : KERR_NOENT;
    rc = rc != KERR_OK ? rc : list_layer(self, true, in, &m, s, &found);
    rc = rc != KERR_OK ? rc : (m.full ? KERR_FBIG : list_layer(self, false, in, &m, s, &found));
    kfree(m.names);
    kfree(s);
    return rc == KERR_OK && !found ? KERR_NOENT : rc;
}

/* ── changes, all in the upper layer ─────────────────────────────────── */

/* The directory `in` is in ("/a/b" -> "/a", "/a" -> "/"), and its last name. */
static const char *split(const char *in, char parent[OVERLAY_PATH_MAX]) {
    uint32_t slash = 0, n = 0;
    while (in[n] != '\0' && n + 1u < OVERLAY_PATH_MAX) {
        parent[n] = in[n];
        slash = in[n] == '/' ? n : slash;
        n++;
    }
    parent[slash == 0 ? 1u : slash] = '\0';
    return in + slash + 1u;
}

/* Gives the upper copy its permission bits, where the upper layer keeps
 * any: a FAT or netfs layer serves its own modes and refuses chmod, and a
 * copy-up must not fail for a bit it could never have stored. */
static int keep_mode(const char *to, uint32_t mode) {
    const char *within = NULL;
    return vfs_resolve(to, &within) != NULL ? KERR_OK : xfsmount_chmod_path(to, mode & 07777u);
}

/* Copies a lower regular file at `from` to the upper path `to`. */
static int copy_file(const char *from, const char *to, uint32_t mode, struct scratch *s) {
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    const char *leaf = split(to, s->c);
    int rc = xfsmount_read_file(from, OVERLAY_COPY_MAX, &bytes, &size);
    rc = rc != KERR_OK ? rc : xfsmount_write_at(s->c, leaf, 0, bytes, size);
    kfree(bytes);
    return rc != KERR_OK ? rc : keep_mode(to, mode);
}

/* Copies a lower symbolic link to the upper path `to`. */
static int copy_link(const char *from, const char *to, struct scratch *s) {
    uint32_t got = 0;
    int rc = xfsmount_readlink(from, s->c, OVERLAY_PATH_MAX - 1u, &got);
    if (rc != KERR_OK) {
        return rc;
    }
    s->c[got] = '\0';
    return xfsmount_symlink(to, s->c);
}

/* Makes `in` exist in the upper layer: its directories, and its lower copy
 * if there is one. Nothing to copy is not an error — a create follows. */
static int copy_up(const struct overlay *o, const char *in, struct scratch *s) {
    uint32_t mode = 0;
    int rc = layer(o, true, in, s->b);
    if (rc != KERR_OK || exists(s->b, NULL)) {
        return rc;
    }
    split(in, s->a);
    rc = layer(o, true, s->a, s->c);
    rc = rc != KERR_OK ? rc : xfsmount_mkdir_p(s->c);
    rc = rc != KERR_OK ? rc : layer(o, false, in, s->a);
    if (rc != KERR_OK || !exists(s->a, &mode)) {
        return rc;
    }
    switch (mode & S_IFMT_BITS) {
        case S_IFDIR_BITS: return xfsmount_mkdir_p(s->b);
        case 0120000u: return copy_link(s->a, s->b, s);
        default: return copy_file(s->a, s->b, mode, s);
    }
}

/* Takes away a whiteout of `in`: a name created again is seen again. */
static int clear_whiteout(const struct overlay *o, const char *in, struct scratch *s) {
    int rc = overlay_whiteout_of(in, s->a);
    rc = rc != KERR_OK ? rc : layer(o, true, s->a, s->b);
    if (rc != KERR_OK || !exists(s->b, NULL)) {
        return rc == KERR_INVAL ? KERR_OK : rc;
    }
    return xfsmount_unlink_path(s->b);
}

/* Leaves a whiteout of `in` in the upper layer, so the lower copy is no
 * longer seen. */
static int whiteout(const struct overlay *o, const char *in, struct scratch *s) {
    int rc = overlay_whiteout_of(in, s->a);
    rc = rc != KERR_OK ? rc : layer(o, true, s->a, s->b);
    if (rc != KERR_OK) {
        return rc;
    }
    const char *leaf = split(s->b, s->c);
    rc = xfsmount_mkdir_p(s->c);
    return rc != KERR_OK ? rc : xfsmount_write_at(s->c, leaf, 0, NULL, 0);
}

/* Whether the LOWER layer has `in`, and what it is. */
static bool in_lower(const struct overlay *o, const char *in, struct scratch *s, uint32_t *mode) {
    return layer(o, false, in, s->a) == KERR_OK && exists(s->a, mode);
}

/* A change's working space: the scratch paths, and the paths it is about. */
struct change {
    struct scratch s;
    char in[OVERLAY_PATH_MAX];
    char upper[OVERLAY_PATH_MAX];
};

/* `dir` and `leaf` as one path inside the overlay. */
static int joined(const char *dir, const char *leaf, char out[OVERLAY_PATH_MAX]) {
    char slashed[OVERLAY_PATH_MAX];
    uint32_t n = 0;
    slashed[n++] = '/';
    while (leaf[n - 1u] != '\0' && n + 1u < OVERLAY_PATH_MAX) {
        slashed[n] = leaf[n - 1u];
        n++;
    }
    slashed[n] = '\0';
    return leaf[n - 1u] != '\0' ? KERR_NAMETOOLONG : overlay_join(dir, slashed, out);
}

/* Copies `in` up, clears any whiteout of it, and leaves its upper path in
 * c->upper — the start of every change that writes `in`. */
static int prepare(const struct overlay *o, struct change *c) {
    int rc = clear_whiteout(o, c->in, &c->s);
    rc = rc != KERR_OK ? rc : copy_up(o, c->in, &c->s);
    return rc != KERR_OK ? rc : layer(o, true, c->in, c->upper);
}

static int write_op(void *self, const char *dir, const char *leaf, uint64_t off,
                    const void *bytes, uint32_t len) {
    struct change *c = kzalloc(sizeof *c);
    int rc = c == NULL ? KERR_NOMEM : joined(dir, leaf, c->in);
    rc = rc != KERR_OK ? rc : prepare(self, c);
    rc = rc != KERR_OK ? rc : layer(self, true, dir, c->s.a);
    rc = rc != KERR_OK ? rc : xfsmount_write_at(c->s.a, leaf, off, bytes, len);
    kfree(c);
    return rc;
}

/* Runs one upper-layer operation on `in` after preparing it. */
static int change_in_upper(void *self, const char *in, int (*fn)(const char *upper, uint64_t arg),
                           uint64_t arg) {
    struct change *c = kzalloc(sizeof *c);
    int rc = c == NULL ? KERR_NOMEM : overlay_join("/", in, c->in);
    rc = rc != KERR_OK ? rc : prepare(self, c);
    rc = rc != KERR_OK ? rc : fn(c->upper, arg);
    kfree(c);
    return rc;
}

static int mkdir_upper(const char *upper, uint64_t arg) {
    (void)arg;
    return xfsmount_mkdir_p(upper);
}

static int truncate_upper(const char *upper, uint64_t size) {
    return xfsmount_truncate_path(upper, size);
}

static int mkdir_op(void *self, const char *in) {
    return change_in_upper(self, in, mkdir_upper, 0);
}

static int truncate_op(void *self, const char *in, uint64_t size) {
    struct scratch *s = kzalloc(sizeof *s);
    bool upper = false;
    int rc = s == NULL ? KERR_NOMEM : visible(self, in, s, &upper);
    kfree(s);
    return rc != KERR_OK ? rc : change_in_upper(self, in, truncate_upper, size);
}

/* Removes `in`: the upper copy goes, and a lower copy is whited out. */
static int unlink_op(void *self, const char *in) {
    struct scratch *s = kzalloc(sizeof *s);
    uint32_t mode = 0;
    bool upper = false, lower;
    int rc = s == NULL ? KERR_NOMEM : visible(self, in, s, &upper);
    rc = rc != KERR_OK || !exists(s->a, &mode) || !is_dir(mode) ? rc : KERR_ISDIR;
    rc = rc != KERR_OK || !upper ? rc : xfsmount_unlink_path(s->a);
    lower = rc == KERR_OK && in_lower(self, in, s, NULL);
    rc = rc != KERR_OK || !lower ? rc : whiteout(self, in, s);
    kfree(s);
    return rc;
}

/* A rename moves the upper copy and whites out a lower one. A directory
 * the lower layer has cannot move: its lower contents would stay behind. */
static int rename_op(void *self, const char *from, const char *to) {
    struct change *c = kzalloc(sizeof *c);
    uint32_t mode = 0;
    bool upper = false, lower = false;
    int rc = c == NULL ? KERR_NOMEM : visible(self, from, &c->s, &upper);
    rc = rc != KERR_OK ? rc : overlay_join("/", from, c->in);
    lower = rc == KERR_OK && in_lower(self, from, &c->s, &mode);
    rc = rc != KERR_OK || !(lower && is_dir(mode)) ? rc : KERR_XDEV;
    rc = rc != KERR_OK ? rc : prepare(self, c);
    rc = rc != KERR_OK ? rc : overlay_join("/", to, c->in);
    rc = rc != KERR_OK ? rc : prepare(self, c);
    rc = rc != KERR_OK ? rc : layer(self, true, from, c->s.a);
    rc = rc != KERR_OK ? rc : xfsmount_rename_path(c->s.a, c->upper);
    rc = rc != KERR_OK || !lower ? rc : whiteout(self, from, &c->s);
    kfree(c);
    return rc;
}

static const struct vfs_ops overlay_ops = {
    .name = "overlay",
    .stat_full = stat_op,
    .owner = owner_op,
    .list = list_op,
    .read_file = read_op,
    .write_at = write_op,
    .mkdir_p = mkdir_op,
    .unlink = unlink_op,
    .truncate = truncate_op,
    .rename = rename_op,
    .times = times_op,
};

/* ── mounting ────────────────────────────────────────────────────────── */

/* A slot no mount in the table uses: an unmounted overlay leaves its slot
 * behind, since the table does not say when a mount goes. */
static struct overlay *free_slot(void) {
    for (uint32_t k = 0; k < OVERLAY_MOUNTS; k++) {
        bool used = false;
        for (uint32_t i = 0; i < vfs_count(); i++) {
            used = used || vfs_at(i)->self == &overlays[k];
        }
        if (!used) {
            return &overlays[k];
        }
    }
    return NULL;
}

/* Whether a layer may be one: a directory, not on another overlay, and
 * neither containing the target nor inside it. */
static int layer_allowed(const char *dir, const char *target) {
    const char *within = NULL;
    const struct vfs_mount *m = vfs_resolve(dir, &within);
    uint32_t mode = 0;
    if (overlay_path_under(dir, target) || overlay_path_under(target, dir)) {
        return KERR_INVAL;
    }
    if (m != NULL && m->ops == &overlay_ops) {
        return KERR_BUSY;
    }
    return exists(dir, &mode) && is_dir(mode) ? KERR_OK : KERR_INVAL;
}

int overlayfs_mount(const char *data, const char *target) {
    struct overlay parsed;
    struct overlay *slot;
    int rc = overlay_parse_options(data, parsed.lower, parsed.upper);
    rc = rc != KERR_OK ? rc : layer_allowed(parsed.lower, target);
    rc = rc != KERR_OK ? rc : layer_allowed(parsed.upper, target);
    rc = rc != KERR_OK || !overlay_path_under(parsed.lower, parsed.upper) ? rc : KERR_INVAL;
    rc = rc != KERR_OK || !overlay_path_under(parsed.upper, parsed.lower) ? rc : KERR_INVAL;
    slot = rc == KERR_OK ? free_slot() : NULL;
    rc = rc != KERR_OK ? rc : (slot == NULL ? KERR_NOMEM : KERR_OK);
    if (rc != KERR_OK) {
        return rc;
    }
    *slot = parsed;
    return vfs_mount(target, &overlay_ops, slot);
}
