/* perm — who may change what, for the calls that change the filesystem.
 *
 * Linux's rules, the ones a program depends on: writing a file needs write
 * permission on it; creating, removing or renaming a name needs write and
 * search permission on the directory holding it; `chmod` and `utimensat`
 * with explicit times need ownership (or `CAP_FOWNER`); `chown` needs
 * `CAP_CHOWN`; running a program needs execute permission on it. Each
 * check goes through `uspace_may_access`, so `CAP_DAC_OVERRIDE` (root by
 * default) bypasses the mode bits exactly as it does for reading.
 *
 * Every one of these was missing: only reading was checked, so any user
 * could truncate /etc/passwd or chown /etc/shadow to themselves.
 * Each returns KERR_OK, KERR_ACCES / KERR_PERM, or the lookup's error.
 */
#ifndef MINIKERN_KERNEL_PERM_H
#define MINIKERN_KERNEL_PERM_H

#include <stdbool.h>
#include <stdint.h>

/* Writing `path`: write permission on it when it exists, on its directory
 * (with search) when it does not. *exists says which. */
int perm_may_write(const char *path, bool *exists);

/* Adding or removing the name `path` in its directory. */
int perm_may_change_entry(const char *path);

/* chmod: the owner, or CAP_FOWNER. */
int perm_may_chmod(const char *path);

/* chown: CAP_CHOWN, unless nothing changes. */
int perm_may_chown(const char *path, uint32_t uid, uint32_t gid);

/* utimensat: explicit times need ownership; "now" needs ownership or
 * write permission. */
int perm_may_set_times(const char *path, bool explicit_times);

/* execve: execute permission on the program. */
int perm_may_exec(const char *path);

#endif
