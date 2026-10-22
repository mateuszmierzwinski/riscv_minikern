/* cmdline — reading the kernel command line ("key=value key2 ...").
 * Pure: no allocation, the results point into the caller's string. */
#ifndef MINIKERN_CORE_CMDLINE_H
#define MINIKERN_CORE_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

/* Finds key=value among space-separated words. On success returns the value
 * (not NUL-terminated at the word end) and stores its length in *len. A bare
 * "key" yields an empty value. Returns NULL when key is absent. */
const char *cmdline_value(const char *cmdline, const char *key, size_t *len);

/* True when key is present with exactly this value. */
/* Walks the words of a command line. *at is the offset to resume from (0
 * to start); it is advanced past the word returned. Returns the word (not
 * NUL-terminated, *len bytes) or NULL at the end. */
const char *cmdline_next(const char *cmdline, size_t *at, size_t *len);

bool cmdline_equals(const char *cmdline, const char *key, const char *value);

#endif
