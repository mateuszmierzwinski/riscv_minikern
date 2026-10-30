#include "kernel/procfs.h"

#include <stddef.h>
#include <stdint.h>

#include "core/errno.h"

/* How many `/proc` files there may be. Small on purpose: this is for state a
 * person or a script reads, and a long list is a sign something belongs in a
 * real filesystem instead. */
#define PROCFS_MAX_FILES 48u

struct entry {
    const char *path;
    procfs_fn fn;
};

static struct entry files[PROCFS_MAX_FILES];
static uint32_t count;

static bool same(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

int procfs_add(const char *path, procfs_fn fn) {
    if (path == NULL || fn == NULL) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (same(files[i].path, path)) {
            /* **Replaced rather than added twice.** Two generators for one
             * path is a file whose content depends on which entry is found
             * first, and nothing above would report the ambiguity. */
            files[i].fn = fn;
            return KERR_OK;
        }
    }
    if (count == PROCFS_MAX_FILES) {
        return KERR_NOSPC;
    }
    files[count].path = path;
    files[count].fn = fn;
    count++;
    return KERR_OK;
}

/* Whose `/proc/self` a generator is describing: 0 for the caller. */
static uint32_t subject;
static uint32_t (*uid_of)(uint32_t pid);
static const char *(*name_of)(uint32_t pid);

void procfs_set_name_of(const char *(*source)(uint32_t pid)) {
    name_of = source;
}

const char *procfs_name_of(uint32_t pid) {
    return name_of != NULL ? name_of(pid) : NULL;
}

void procfs_set_uid_of(uint32_t (*source)(uint32_t pid)) {
    uid_of = source;
}

uint32_t procfs_subject(void) {
    return subject;
}

uint32_t procfs_uid_of(uint32_t pid) {
    return uid_of != NULL ? uid_of(pid) : UINT32_MAX;
}

/* Whether `path` starts with `prefix`; where it continues. */
static const char *after(const char *path, const char *prefix) {
    uint32_t i = 0;
    while (prefix[i] != '\0' && path[i] == prefix[i]) {
        i++;
    }
    return prefix[i] == '\0' ? path + i : NULL;
}

/* A decimal number ending at '/'; where the rest begins, or NULL. */
static const char *number(const char *p, uint32_t *out) {
    uint32_t v = 0, n = 0;
    while (p[n] >= '0' && p[n] <= '9' && v < 100000000u) {
        v = v * 10u + (uint32_t)(p[n++] - '0');
    }
    *out = v;
    return n > 0 && p[n] == '/' ? p + n + 1u : NULL;
}

static bool copy_joined(const char *head, const char *rest, char *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; head[i] != '\0'; i++) {
        out[n++] = head[i];
    }
    for (uint32_t i = 0; rest[i] != '\0'; i++) {
        if (n + 1u >= PROCFS_PATH_MAX) {
            return false;
        }
        out[n++] = rest[i];
    }
    out[n] = '\0';
    return true;
}

/* "<name>/<pid>/..." for a live pid running under exactly that name: the
 * rest after the pid. A name never holds a '/', so the first one ends it. */
static const char *named(const char *rest, uint32_t *pid) {
    const char *name = NULL;
    uint32_t n = 0;
    while (rest[n] != '\0' && rest[n] != '/') {
        n++;
    }
    if (n == 0 || rest[n] != '/') {
        return NULL;
    }
    const char *tail = number(rest + n + 1u, pid);
    name = tail != NULL ? procfs_name_of(*pid) : NULL;
    for (uint32_t i = 0; name != NULL && i < n; i++) {
        name = name[i] == rest[i] ? name : NULL;
    }
    return name != NULL && name[n] == '\0' ? tail : NULL;
}

bool procfs_translate(const char *path, char *out, uint32_t *pid) {
    const char *rest;
    uint32_t uid = 0;
    if (uid_of == NULL) {
        return false;
    }
    if ((rest = after(path, "/proc/ps/by-pid/")) != NULL &&
        (rest = number(rest, pid)) != NULL && uid_of(*pid) != UINT32_MAX) {
        return copy_joined("/proc/self/ps/", rest, out);
    }
    if ((rest = after(path, "/proc/ps/by-user/")) != NULL &&
        (rest = number(rest, &uid)) != NULL && (rest = number(rest, pid)) != NULL &&
        uid_of(*pid) == uid) {
        return copy_joined("/proc/self/ps/", rest, out);
    }
    if ((rest = after(path, "/proc/ps/by-name/")) != NULL && (rest = named(rest, pid)) != NULL) {
        return copy_joined("/proc/self/ps/", rest, out);
    }
    if ((rest = after(path, "/proc/")) != NULL && (rest = number(rest, pid)) != NULL &&
        uid_of(*pid) != UINT32_MAX) {
        return copy_joined("/proc/self/", rest, out);
    }
    return false;
}

static int find(const char *path) {
    for (uint32_t i = 0; i < count; i++) {
        if (same(files[i].path, path)) {
            return (int)i;
        }
    }
    return -1;
}

bool procfs_has(const char *path) {
    char self[PROCFS_PATH_MAX];
    uint32_t pid = 0;
    return find(path) >= 0 || (procfs_translate(path, self, &pid) && find(self) >= 0);
}

int procfs_generate(const char *path, char *out, uint32_t room, uint32_t *wrote) {
    char self[PROCFS_PATH_MAX];
    uint32_t pid = 0;
    int i = find(path);
    if (i < 0 && procfs_translate(path, self, &pid)) {
        i = find(self);
    }
    if (i < 0) {
        return KERR_NOENT;
    }
    subject = pid;
    *wrote = files[i].fn(out, room);
    subject = 0;
    return KERR_OK;
}

/* The leaf of each registered path, for listing `/proc` itself. The paths
 * are all `/proc/<name>`, so the leaf is what follows the last separator —
 * computed rather than stored, because storing it would be a second copy of
 * the same string to keep in step. */
const char *procfs_name_at(uint32_t index) {
    const char *path;
    uint32_t cut = 0;
    if (index >= count) {
        return NULL;
    }
    path = files[index].path;
    for (uint32_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            cut = i + 1u;
        }
    }
    return path + cut;
}

const char *procfs_path_at(uint32_t index) {
    return index < count ? files[index].path : NULL;
}
