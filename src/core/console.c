#include "core/console.h"

#include <stdarg.h>

#include "core/klog.h"
#include "lib/kfmt.h"

/* 64 bytes — one burst per UART poll loop; flushed at the end of each call. */
#define CONSOLE_CHUNK 64

/* Characters gathered from the formatter before one port write. */
struct console_chunk {
    char buf[CONSOLE_CHUNK];
    size_t len;
};

static struct console_port attached;

static void flush(struct console_chunk *c) {
    if (c->len > 0 && attached.write != NULL) {
        attached.write(attached.self, c->buf, c->len);
    }
    c->len = 0;
}

static void chunk_sink(void *ctx, char ch) {
    struct console_chunk *c = ctx;
    c->buf[c->len++] = ch;
    if (c->len == CONSOLE_CHUNK) {
        flush(c);
    }
}

void console_attach(const struct console_port *port) {
    attached = *port;
}

void kvprintf(const char *fmt, va_list ap) {
    /* **Through the log, at info.**
     *
     * This is what makes the log unified rather than a second channel: every
     * one of the kernel's several hundred `kprintf` call sites gets a
     * timestamp and a level without being touched, and the log keeps what
     * they said. Converting them by hand would have produced a log where
     * some lines carry a time and some do not — which cannot be read in
     * order, and so is worth less than either alternative.
     *
     * A caller that wants another level says so with `klog`. */
    klog_vwrite(KLOG_INFO, fmt, ap);
}

/* The raw path, for the log itself and for anything that must reach the
 * console without being logged — which is only `klog`, writing the bytes it
 * has already accounted for. */
void console_write_raw(const char *buf, size_t len) {
    struct console_chunk c = {.len = 0};
    for (size_t i = 0; i < len; i++) {
        chunk_sink(&c, buf[i]);
    }
    flush(&c);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void console_write(const char *buf, size_t len) {
    if (attached.write != NULL) {
        attached.write(attached.self, buf, len);
    }
}

bool console_attached(void) {
    return attached.write != NULL;
}
