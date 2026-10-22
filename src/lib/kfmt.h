/* kfmt — the kernel's printf: a formatter that writes through a sink.
 *
 * Freestanding (no libc) and portable, so it is unit-tested on the host and
 * linked into the kernel unchanged. Supports %c %s %d %i %u %x %X %p %%, the
 * '0' and '-' flags, a decimal width, and the l / ll length modifiers.
 * Unknown conversions are printed literally, so a bad format never faults. */
#ifndef MINIKERN_LIB_KFMT_H
#define MINIKERN_LIB_KFMT_H

#include <stdarg.h>
#include <stddef.h>

/* Receives every character the formatter produces, in order. */
typedef void (*kfmt_sink)(void *ctx, char c);

/* Formats fmt with ap into sink; returns the number of characters produced. */
size_t kfmt_vformat(kfmt_sink sink, void *ctx, const char *fmt, va_list ap);

/* Formats into buf (always NUL-terminated when cap > 0); returns the length
 * the full output would have had, like C99 snprintf. */
size_t kfmt_snprintf(char *buf, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
