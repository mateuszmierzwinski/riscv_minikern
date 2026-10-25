#include "kernel/procfs.h"

#include <stddef.h>

#include "core/errno.h"

/* How many `/proc` files there may be. Small on purpose: this is for state a
 * person or a script reads, and a long list is a sign something belongs in a
 * real filesystem instead. */
#define PROCFS_MAX_FILES 16u

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

bool procfs_has(const char *path) {
    for (uint32_t i = 0; i < count; i++) {
        if (same(files[i].path, path)) {
            return true;
        }
    }
    return false;
}

int procfs_generate(const char *path, char *out, uint32_t room, uint32_t *wrote) {
    for (uint32_t i = 0; i < count; i++) {
        if (same(files[i].path, path)) {
            *wrote = files[i].fn(out, room);
            return KERR_OK;
        }
    }
    return KERR_NOENT;
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
