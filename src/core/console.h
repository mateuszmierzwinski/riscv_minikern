/* console — the kernel log. One attached console_port receives everything
 * kprintf produces. Later the /dev console devices (stdout, stderr) are
 * built on the same port. */
#ifndef MINIKERN_CORE_CONSOLE_H
#define MINIKERN_CORE_CONSOLE_H

#include <stdarg.h>
#include <stddef.h>

#include "core/ports.h"

/* Makes port the kernel console. Output before this call is dropped. */
void console_attach(const struct console_port *port);

/* Formats (kfmt rules) and writes to the console. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* kprintf with a va_list. */
void kvprintf(const char *fmt, va_list ap);

/* Writes len raw bytes to the console (the /dev console devices use this). */
void console_write(const char *buf, size_t len);

#endif
