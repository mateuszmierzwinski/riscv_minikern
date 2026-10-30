#include "kernel/perm.h"

#include <stddef.h>

#include "core/access.h"
#include "core/caps.h"
#include "core/errno.h"
#include "kernel/uspace.h"
#include "kernel/xfsmount.h"

#define PERM_PATH_MAX 256u

/* The directory holding `path`: "/a/b/c" -> "/a/b", "/a" -> "/". */
static void parent_of(const char *path, char *out) {
    uint32_t n = 0, cut = 1;
    while (path[n] != '\0' && n + 1u < PERM_PATH_MAX) {
        out[n] = path[n];
        cut = path[n] == '/' && n > 0 ? n : cut;
        n++;
    }
    out[cut] = '\0';
    if (cut == 1) {
        out[0] = '/';
    }
}

static int may(const char *path, uint32_t want) {
    uint32_t uid = 0, gid = 0, mode = 0;
    int rc = xfsmount_stat(path, &uid, &gid, &mode);
    if (rc != KERR_OK) {
        return rc;
    }
    return uspace_may_access(uid, gid, mode, want) ? KERR_OK : KERR_ACCES;
}

int perm_may_change_entry(const char *path) {
    char dir[PERM_PATH_MAX];
    parent_of(path, dir);
    return may(dir, ACCESS_WRITE | ACCESS_EXEC);
}

int perm_may_write(const char *path, bool *exists) {
    int rc = may(path, ACCESS_WRITE);
    *exists = rc != KERR_NOENT;
    return rc == KERR_NOENT ? perm_may_change_entry(path) : rc;
}

/* Whether the caller owns `path`, or holds CAP_FOWNER. */
static int owner_or_fowner(const char *path) {
    uint32_t uid = 0, gid = 0, mode = 0;
    int rc = xfsmount_stat(path, &uid, &gid, &mode);
    if (rc != KERR_OK) {
        return rc;
    }
    return uid == uspace_euid() || uspace_capable(CAP_FOWNER) ? KERR_OK : KERR_PERM;
}

int perm_may_chmod(const char *path) {
    return owner_or_fowner(path);
}

int perm_may_chown(const char *path, uint32_t uid, uint32_t gid) {
    uint32_t cur_uid = 0, cur_gid = 0, mode = 0;
    int rc = xfsmount_stat(path, &cur_uid, &cur_gid, &mode);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((uid == UINT32_MAX || uid == cur_uid) && (gid == UINT32_MAX || gid == cur_gid)) {
        return KERR_OK; /* no change: allowed, as Linux allows it */
    }
    return uspace_capable(CAP_CHOWN) ? KERR_OK : KERR_PERM;
}

int perm_may_set_times(const char *path, bool explicit_times) {
    int rc = owner_or_fowner(path);
    if (rc == KERR_PERM && !explicit_times) {
        rc = may(path, ACCESS_WRITE);
    }
    return rc;
}

int perm_may_exec(const char *path) {
    return may(path, ACCESS_EXEC);
}
