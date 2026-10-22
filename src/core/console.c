#include "core/console.h"

#include <stdarg.h>

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
    struct console_chunk c = {.len = 0};
    kfmt_vformat(chunk_sink, &c, fmt, ap);
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
