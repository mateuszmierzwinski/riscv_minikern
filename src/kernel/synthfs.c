#include "kernel/synthfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/errno.h"
#include "core/vfs.h"
#include "kernel/mem.h"
#include "kernel/procfs.h"

#define DEVICES_MAX 16u
#define PATH_MAX_LEN PROCFS_PATH_MAX

#define MODE_FILE 0100444u
#define MODE_DIR 0040555u
#define MODE_CHAR 0020666u

static const char *devices[DEVICES_MAX];
static uint32_t device_count;
static uint32_t (*pid_at)(uint32_t index);

int synthfs_add_device(const char *path) {
    if (device_count == DEVICES_MAX) {
        return KERR_NOSPC;
    }
    devices[device_count++] = path;
    return KERR_OK;
}

void synthfs_set_pids(uint32_t (*source)(uint32_t index)) {
    pid_at = source;
}

static bool same(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/* The `i`th known path: the generated files, then the devices. */
static const char *known_at(uint32_t i, bool *device) {
    const char *p = procfs_path_at(i);
    uint32_t files = 0, d;
    *device = false;
    if (p != NULL) {
        return p;
    }
    while (procfs_path_at(files) != NULL) {
        files++;
    }
    if (i < files) {
        return NULL; /* the table changed under the walk */
    }
    d = i - files;
    *device = true;
    return d < device_count && d < DEVICES_MAX ? devices[d] : NULL;
}

/* How far `p` extends `dir` below it: the index of the first character of
 * the next component, or 0 when p is not below dir. */
static uint32_t below(const char *dir, const char *p) {
    uint32_t i = 0;
    while (dir[i] != '\0' && dir[i] == p[i]) {
        i++;
    }
    if (dir[i] != '\0') {
        return 0;
    }
    return p[i] == '/' && p[i + 1u] != '\0' ? i + 1u : 0u;
}

/* The mount point and the path inside it, joined: "/proc" + "/self/maps".
 * Trailing slashes go, so "dir/" names "dir" as it does on Linux. */
static int full_path(const char *at, const char *in, char *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; at[i] != '\0'; i++) {
        out[n++] = at[i];
    }
    if (!(in[0] == '/' && in[1] == '\0')) {
        for (uint32_t i = 0; in[i] != '\0'; i++) {
            if (n + 1u >= PATH_MAX_LEN) {
                return KERR_NAMETOOLONG;
            }
            out[n++] = in[i];
        }
    }
    while (n > 1u && out[n - 1u] == '/') {
        n--;
    }
    out[n] = '\0';
    return KERR_OK;
}

/* A per-process directory ("/proc/7", "/proc/ps/by-pid/7") as the
 * /proc/self directory it mirrors, in out; false for anything else. */
static bool mirrored_dir(const char *path, char *out) {
    char probe[PATH_MAX_LEN];
    uint32_t n = 0, pid = 0;
    while (path[n] != '\0' && n + 3u < PATH_MAX_LEN) {
        probe[n] = path[n];
        n++;
    }
    probe[n] = '/';
    probe[n + 1u] = 'x';
    probe[n + 2u] = '\0';
    if (!procfs_translate(probe, out, &pid)) {
        return false;
    }
    for (n = 0; n < PATH_MAX_LEN && out[n] != '\0'; n++) {
    }
    if (n < 2u || n >= PATH_MAX_LEN) {
        return false;
    }
    out[n - 2u] = '\0'; /* drop the "/x" probe */
    return true;
}

/* "/proc/ps/by-user/<uid>" with a live process of that uid: the uid. */
static bool user_dir(const char *path, uint32_t *uid) {
    const char *prefix = "/proc/ps/by-user/";
    uint32_t i = 0, v = 0, n = 0, pid;
    while (prefix[i] != '\0' && path[i] == prefix[i]) {
        i++;
    }
    if (prefix[i] != '\0') {
        return false;
    }
    while (path[i + n] >= '0' && path[i + n] <= '9' && v < 100000000u) {
        v = v * 10u + (uint32_t)(path[i + n++] - '0');
    }
    if (n == 0 || path[i + n] != '\0') {
        return false;
    }
    for (uint32_t k = 0; pid_at != NULL && (pid = pid_at(k)) != UINT32_MAX; k++) {
        if (pid != 0 && procfs_uid_of(pid) == v) {
            *uid = v;
            return true;
        }
    }
    return false;
}

static bool same_name(const char *a, const char *b) {
    return a != NULL && b != NULL && same(a, b);
}

/* "/proc/ps/by-name/<name>" with a live process of that name: the name. */
static const char *name_dir(const char *path) {
    const char *prefix = "/proc/ps/by-name/";
    uint32_t i = 0, pid;
    while (prefix[i] != '\0' && path[i] == prefix[i]) {
        i++;
    }
    if (prefix[i] != '\0' || path[i] == '\0') {
        return NULL;
    }
    for (uint32_t k = 0; pid_at != NULL && (pid = pid_at(k)) != UINT32_MAX; k++) {
        if (pid != 0 && same_name(procfs_name_of(pid), path + i)) {
            return path + i;
        }
    }
    return NULL;
}

static bool is_fixed_dir(const char *path) {
    return same(path, "/proc/ps") || same(path, "/proc/ps/by-pid") ||
           same(path, "/proc/ps/by-user") || same(path, "/proc/ps/by-name");
}

/* Whether any known path lies below dir. */
static bool has_children(const char *dir) {
    bool device = false;
    const char *p;
    for (uint32_t i = 0; (p = known_at(i, &device)) != NULL; i++) {
        if (below(dir, p) != 0) {
            return true;
        }
    }
    return false;
}

/* What `path` is: a file (generated or a device) or a directory. */
static int classify(const char *path, void *self, uint32_t *mode) {
    char mirror[PATH_MAX_LEN];
    uint32_t uid = 0;
    bool device = false;
    const char *p;
    for (uint32_t i = 0; (p = known_at(i, &device)) != NULL; i++) {
        if (same(p, path)) {
            *mode = device ? MODE_CHAR : MODE_FILE;
            return KERR_OK;
        }
    }
    if (procfs_has(path) && !mirrored_dir(path, mirror)) {
        *mode = MODE_FILE; /* a per-process file, served by its /proc/self one */
        return KERR_OK;
    }
    if (same(path, self) || is_fixed_dir(path) || mirrored_dir(path, mirror) ||
        user_dir(path, &uid) || name_dir(path) != NULL || has_children(path)) {
        *mode = MODE_DIR;
        return KERR_OK;
    }
    return KERR_NOENT;
}

static int stat_op(void *self, const char *in, uint32_t *mode, uint64_t *size, uint32_t *nlink) {
    char path[PATH_MAX_LEN];
    int rc = full_path(self, in, path);
    rc = rc != KERR_OK ? rc : classify(path, self, mode);
    if (rc == KERR_OK) {
        *size = 0; /* generated at open: like Linux, the size is not known */
        *nlink = *mode == MODE_DIR ? 2u : 1u;
    }
    return rc;
}

static int owner_op(void *self, const char *in, uint32_t *uid, uint32_t *gid) {
    uint32_t mode = 0, nlink = 0;
    uint64_t size = 0;
    *uid = 0;
    *gid = 0;
    return stat_op(self, in, &mode, &size, &nlink);
}

/* Hands fn one entry named by a decimal number. */
static int emit_number(xfs_dirent_fn fn, void *ctx, uint32_t v, uint64_t ino, uint8_t type) {
    char name[12];
    char rev[12];
    uint32_t n = 0;
    do {
        rev[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    for (uint32_t j = 0; j < n; j++) {
        name[j] = rev[n - 1u - j];
    }
    struct xfs_dirent d = {.ino = ino, .name = name, .name_len = n, .type = type};
    return fn(ctx, &d);
}

/* One entry per live process, or per live process of `uid` when `only`. */
/* Which live processes a listing shows: all of them, one uid's, or one
 * name's. */
struct pid_filter {
    bool by_uid;
    uint32_t uid;
    const char *name; /* NULL: not by name */
};

static bool kept(const struct pid_filter *f, uint32_t pid) {
    return pid != 0 && (!f->by_uid || procfs_uid_of(pid) == f->uid) &&
           (f->name == NULL || same_name(procfs_name_of(pid), f->name));
}

static int list_pids(xfs_dirent_fn fn, void *ctx, const struct pid_filter *f) {
    uint32_t pid;
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && pid_at != NULL && (pid = pid_at(i)) != UINT32_MAX; i++) {
        if (kept(f, pid)) {
            rc = emit_number(fn, ctx, pid, 10000u + pid, XFS_DIR3_FT_DIR);
        }
    }
    return rc;
}

/* One entry per name some live process runs under, each once. */
static int list_names(xfs_dirent_fn fn, void *ctx) {
    uint32_t pid;
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && pid_at != NULL && (pid = pid_at(i)) != UINT32_MAX; i++) {
        const char *name = pid != 0 ? procfs_name_of(pid) : NULL;
        bool seen = name == NULL || name[0] == '\0';
        for (uint32_t j = 0; !seen && j < i; j++) {
            uint32_t earlier = pid_at(j);
            seen = earlier != 0 && same_name(procfs_name_of(earlier), name);
        }
        uint32_t n = 0;
        while (!seen && name[n] != '\0') {
            n++;
        }
        struct xfs_dirent d = {.ino = 40000u + pid, .name = name, .name_len = n,
                               .type = XFS_DIR3_FT_DIR};
        rc = seen ? KERR_OK : fn(ctx, &d);
    }
    return rc;
}

/* One entry per uid some live process has, each once. */
static int list_uids(xfs_dirent_fn fn, void *ctx) {
    uint32_t pid, earlier;
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && pid_at != NULL && (pid = pid_at(i)) != UINT32_MAX; i++) {
        bool seen = false;
        uint32_t uid = procfs_uid_of(pid);
        for (uint32_t j = 0; pid != 0 && j < i; j++) {
            earlier = pid_at(j);
            seen = seen || (earlier != 0 && procfs_uid_of(earlier) == uid);
        }
        if (pid != 0 && !seen) {
            rc = emit_number(fn, ctx, uid, 20000u + uid, XFS_DIR3_FT_DIR);
        }
    }
    return rc;
}

/* Whether the component starting at p[at] was already listed by an earlier
 * known path, which is how two files in one subdirectory list it once. */
static bool listed_before(const char *dir, uint32_t upto, const char *p, uint32_t at,
                          uint32_t len) {
    bool device = false;
    for (uint32_t i = 0; i < upto; i++) {
        const char *q = known_at(i, &device);
        uint32_t qa = below(dir, q), j = 0;
        while (qa != 0 && j < len && q[qa + j] == p[at + j]) {
            j++;
        }
        if (qa != 0 && j == len && (q[qa + j] == '/' || q[qa + j] == '\0')) {
            return true;
        }
    }
    return false;
}

/* The known paths' next components below dir, each once. "/proc/self" is
 * not listed in /proc/ps's terms: it is a link on Linux, here a directory. */
static int list_known(const char *dir, xfs_dirent_fn fn, void *ctx) {
    bool device = false;
    const char *p;
    int rc = KERR_OK;
    for (uint32_t i = 0; rc == KERR_OK && (p = known_at(i, &device)) != NULL; i++) {
        uint32_t at = below(dir, p), len = 0;
        if (at == 0) {
            continue;
        }
        while (p[at + len] != '\0' && p[at + len] != '/') {
            len++;
        }
        if (!listed_before(dir, i, p, at, len)) {
            bool leaf = p[at + len] == '\0';
            struct xfs_dirent d = {.ino = 100u + i, .name = p + at, .name_len = len,
                                   .type = !leaf ? XFS_DIR3_FT_DIR
                                                 : (device ? XFS_DIR3_FT_CHRDEV : XFS_DIR3_FT_REG)};
            rc = fn(ctx, &d);
        }
    }
    return rc;
}

static int list_ps_root(xfs_dirent_fn fn, void *ctx) {
    struct xfs_dirent pid = {.ino = 30001u, .name = "by-pid", .name_len = 6,
                             .type = XFS_DIR3_FT_DIR};
    struct xfs_dirent user = {.ino = 30002u, .name = "by-user", .name_len = 7,
                              .type = XFS_DIR3_FT_DIR};
    struct xfs_dirent name = {.ino = 30003u, .name = "by-name", .name_len = 7,
                              .type = XFS_DIR3_FT_DIR};
    int rc = fn(ctx, &pid);
    rc = rc != KERR_OK ? rc : fn(ctx, &user);
    return rc != KERR_OK ? rc : fn(ctx, &name);
}

/* The directories that are not made of known paths: /proc/ps and its
 * views. True when `dir` was one (and *rc is its listing's result). */
static bool list_special(const char *dir, xfs_dirent_fn fn, void *ctx, int *rc) {
    uint32_t uid = 0;
    if (same(dir, "/proc/ps")) {
        *rc = list_ps_root(fn, ctx);
    } else if (same(dir, "/proc/ps/by-pid")) {
        *rc = list_pids(fn, ctx, &(struct pid_filter){.name = NULL});
    } else if (same(dir, "/proc/ps/by-user")) {
        *rc = list_uids(fn, ctx);
    } else if (user_dir(dir, &uid)) {
        *rc = list_pids(fn, ctx, &(struct pid_filter){.by_uid = true, .uid = uid});
    } else if (same(dir, "/proc/ps/by-name")) {
        *rc = list_names(fn, ctx);
    } else if (name_dir(dir) != NULL) {
        *rc = list_pids(fn, ctx, &(struct pid_filter){.name = name_dir(dir)});
    } else {
        return false;
    }
    return true;
}

/* /proc itself: its known files, a directory per process, and ps/. */
static int list_proc_root(xfs_dirent_fn fn, void *ctx) {
    struct xfs_dirent ps = {.ino = 30000u, .name = "ps", .name_len = 2, .type = XFS_DIR3_FT_DIR};
    int rc = list_known("/proc", fn, ctx);
    rc = rc != KERR_OK ? rc : list_pids(fn, ctx, &(struct pid_filter){.name = NULL});
    return rc != KERR_OK ? rc : fn(ctx, &ps);
}

static int list_op(void *self, const char *in, xfs_dirent_fn fn, void *ctx) {
    char dir[PATH_MAX_LEN], mirror[PATH_MAX_LEN];
    int rc = full_path(self, in, dir);
    if (rc != KERR_OK || list_special(dir, fn, ctx, &rc)) {
        return rc;
    }
    if (mirrored_dir(dir, mirror)) {
        return list_known(mirror, fn, ctx);
    }
    return same(dir, "/proc") ? list_proc_root(fn, ctx) : list_known(dir, fn, ctx);
}

static int read_op(void *self, const char *in, uint32_t limit, uint8_t **out, uint32_t *size) {
    char path[PATH_MAX_LEN];
    uint32_t wrote = 0;
    uint8_t *buf;
    int rc = full_path(self, in, path);
    if (rc != KERR_OK || !procfs_has(path)) {
        return rc != KERR_OK ? rc : KERR_NOENT;
    }
    buf = kzalloc(PROCFS_MAX_BYTES);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    rc = procfs_generate(path, (char *)buf, PROCFS_MAX_BYTES < limit ? PROCFS_MAX_BYTES : limit,
                         &wrote);
    if (rc != KERR_OK) {
        kfree(buf);
        return rc;
    }
    *out = buf;
    *size = wrote;
    return KERR_OK;
}

static int refuse_write(void *self, const char *dir, const char *leaf, uint64_t off,
                        const void *bytes, uint32_t len) {
    (void)self, (void)dir, (void)leaf, (void)off, (void)bytes, (void)len;
    return KERR_PERM;
}

static int refuse_path(void *self, const char *in) {
    (void)self, (void)in;
    return KERR_PERM;
}

static int refuse_truncate(void *self, const char *in, uint64_t size) {
    (void)self, (void)in, (void)size;
    return KERR_PERM;
}

static int refuse_rename(void *self, const char *from, const char *to) {
    (void)self, (void)from, (void)to;
    return KERR_PERM;
}

static const struct vfs_ops synth_ops = {
    .name = "synthfs",
    .stat_full = stat_op,
    .owner = owner_op,
    .list = list_op,
    .read_file = read_op,
    .write_at = refuse_write,
    .mkdir_p = refuse_path,
    .unlink = refuse_path,
    .truncate = refuse_truncate,
    .rename = refuse_rename,
};

int synthfs_mount(void) {
    static char proc[] = "/proc", sys[] = "/sys", dev[] = "/dev";
    int rc = vfs_mount(proc, &synth_ops, proc);
    rc = rc != KERR_OK ? rc : vfs_mount(sys, &synth_ops, sys);
    return rc != KERR_OK ? rc : vfs_mount(dev, &synth_ops, dev);
}
