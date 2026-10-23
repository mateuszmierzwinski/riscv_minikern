/* cmdline — reading the kernel command line ("key=value key2 ...").
 * Pure: no allocation, the results point into the caller's string. */
#ifndef MINIKERN_CORE_CMDLINE_H
#define MINIKERN_CORE_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* True when the key is there at all, whatever its value.
 *
 * It exists because the alternative was `cmdline_value(cmdline, key, NULL)`,
 * and that length out-parameter is written unconditionally: passing NULL
 * stored to address zero. A function that cannot be called wrongly beats a
 * comment telling callers not to. */
bool cmdline_has(const char *cmdline, const char *key);

/* A numeric option's value, or `fallback` when it is absent or not a
 * number. For the options whose interesting part is how many. */
uint64_t cmdline_number(const char *cmdline, const char *key, uint64_t fallback);

/* The same parser over a piece of text that is not a whole option — a field
 * inside a value, like the swappiness in `minikern.swap=/swapfile:60`.
 * `cmdline_number` is this with the key looked up first, which is why they
 * agree about hexadecimal and about what a malformed number does. */
uint64_t cmdline_number_in(const char *v, size_t len, uint64_t fallback);

#endif
