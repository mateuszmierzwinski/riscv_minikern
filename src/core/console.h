/* console — the kernel log. One attached console_port receives everything
 * kprintf produces. Later the /dev console devices (stdout, stderr) are
 * built on the same port. */
#ifndef MINIKERN_CORE_CONSOLE_H
#define MINIKERN_CORE_CONSOLE_H

#include <stdarg.h>
#include <stddef.h>

#include <stdbool.h>

#include "core/ports.h"

/* Makes port the kernel console. Output before this call is dropped. */
void console_attach(const struct console_port *port);

/* Formats (kfmt rules) and writes to the console. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* kprintf with a va_list. */
void kvprintf(const char *fmt, va_list ap);

/* Writes len raw bytes to the console (the /dev console devices use this).
 *
 * **Not logged**: this is the path `klog` itself writes through, so logging
 * here would be infinite. Anything that wants its output in the log calls
 * `kprintf` or `klog`. */
void console_write(const char *buf, size_t len);

/* Writes len bytes with no CR added before an LF — a terminal with `OPOST`
 * off. Only when `console_untranslated()`: otherwise it is `console_write`. */
void console_write_untranslated(const char *buf, size_t len);
bool console_untranslated(void);

/* Whether a port has been attached yet. Output before that is dropped by
 * the console — but it is still *kept* by the log, which is the whole
 * reason the log exists: the earliest lines of a boot are the ones most
 * likely to matter and least likely to be watched. */
bool console_attached(void);

#endif
