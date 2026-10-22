#include "lib/kfmt.h"

#include <stdint.h>

/* One parsed conversion: flags, width, length modifier and the conversion char. */
struct kfmt_spec {
    char pad;          /* ' ' or '0' */
    int left;          /* '-' flag: pad on the right */
    unsigned width;
    int precision;     /* %s only: at most this many chars; -1 = unlimited */
    int length;        /* 0 = int, 1 = long, 2 = long long */
    char conv;
};

/* The sink plus a running count of what has been emitted. */
struct kfmt_out {
    kfmt_sink sink;
    void *ctx;
    size_t count;
};

static void put(struct kfmt_out *o, char c) {
    o->sink(o->ctx, c);
    o->count++;
}

static void put_repeat(struct kfmt_out *o, char c, unsigned n) {
    while (n-- > 0) {
        put(o, c);
    }
}

/* Emits text of length len padded to the spec's width. */
static void put_padded(struct kfmt_out *o, const char *text, unsigned len,
                       const struct kfmt_spec *s) {
    unsigned fill = s->width > len ? s->width - len : 0;
    if (!s->left) {
        put_repeat(o, s->pad, fill);
    }
    for (unsigned i = 0; i < len; i++) {
        put(o, text[i]);
    }
    if (s->left) {
        put_repeat(o, ' ', fill);
    }
}

/* Parses ".N" or ".*" (taken from the arguments); a negative * is "none". */
static const char *parse_precision(const char *p, struct kfmt_spec *s, va_list *ap) {
    if (*p != '.') {
        return p;
    }
    p++;
    if (*p == '*') {
        s->precision = va_arg(*ap, int);
        s->precision = s->precision < 0 ? -1 : s->precision;
        return p + 1;
    }
    for (s->precision = 0; *p >= '0' && *p <= '9'; p++) {
        s->precision = s->precision * 10 + (*p - '0');
    }
    return p;
}

/* Parses flags, width, precision and length after '%'; returns the
 * conversion position. */
static const char *parse_spec(const char *p, struct kfmt_spec *s, va_list *ap) {
    *s = (struct kfmt_spec){.pad = ' ', .precision = -1};
    for (; *p == '0' || *p == '-'; p++) {
        if (*p == '0') {
            s->pad = '0';
        } else {
            s->left = 1;
        }
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        s->width = s->width * 10u + (unsigned)(*p - '0');
    }
    p = parse_precision(p, s, ap);
    for (; *p == 'l' && s->length < 2; p++) {
        s->length++;
    }
    s->conv = *p;
    return p;
}

/* Renders v in base into the tail of buf; returns the first digit's index. */
static unsigned render_digits(char buf[24], unsigned long long v, unsigned base, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned i = 24;
    do {
        buf[--i] = digits[v % base];
        v /= base;
    } while (v != 0);
    return i;
}

/* Emits an integer; a minus sign goes before zero padding, as in C printf. */
static void put_number(struct kfmt_out *o, unsigned long long v, int negative,
                       unsigned base, const struct kfmt_spec *s) {
    char buf[24];
    unsigned first = render_digits(buf, v, base, s->conv == 'X');
    struct kfmt_spec rest = *s;
    if (negative) {
        if (s->pad == '0') {
            put(o, '-');
            rest.width = s->width > 0 ? s->width - 1 : 0;
        } else {
            buf[--first] = '-';
        }
    }
    put_padded(o, buf + first, 24u - first, &rest);
}

static long long signed_arg(va_list *ap, int length) {
    if (length == 2) {
        return va_arg(*ap, long long);
    }
    return length == 1 ? va_arg(*ap, long) : va_arg(*ap, int);
}

static unsigned long long unsigned_arg(va_list *ap, int length) {
    if (length == 2) {
        return va_arg(*ap, unsigned long long);
    }
    return length == 1 ? va_arg(*ap, unsigned long) : va_arg(*ap, unsigned int);
}

static void put_signed(struct kfmt_out *o, va_list *ap, const struct kfmt_spec *s) {
    long long v = signed_arg(ap, s->length);
    unsigned long long magnitude = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
    put_number(o, magnitude, v < 0, 10, s);
}

static void put_string(struct kfmt_out *o, const char *str, const struct kfmt_spec *s) {
    unsigned len = 0;
    if (str == NULL) {
        str = "(null)";
    }
    while ((s->precision < 0 || len < (unsigned)s->precision) && str[len] != '\0') {
        len++;
    }
    put_padded(o, str, len, s);
}

/* %p: always 0x plus the full pointer width in hex, whatever the flags say. */
static void put_pointer(struct kfmt_out *o, const void *p) {
    struct kfmt_spec s = {.pad = '0', .width = (unsigned)(2 * sizeof(void *)), .conv = 'x'};
    put(o, '0');
    put(o, 'x');
    put_number(o, (unsigned long long)(uintptr_t)p, 0, 16, &s);
}

/* Performs one conversion; an unknown one is echoed so the mistake is visible. */
static void put_conversion(struct kfmt_out *o, va_list *ap, const struct kfmt_spec *s) {
    char c;
    switch (s->conv) {
    case 'd':
    case 'i': put_signed(o, ap, s); break;
    case 'u': put_number(o, unsigned_arg(ap, s->length), 0, 10, s); break;
    case 'x':
    case 'X': put_number(o, unsigned_arg(ap, s->length), 0, 16, s); break;
    case 's': put_string(o, va_arg(*ap, const char *), s); break;
    case 'p': put_pointer(o, va_arg(*ap, const void *)); break;
    case 'c': c = (char)va_arg(*ap, int); put_padded(o, &c, 1, s); break;
    case '%': put(o, '%'); break;
    default: put(o, '%'); put(o, s->conv); break;
    }
}

size_t kfmt_vformat(kfmt_sink sink, void *ctx, const char *fmt, va_list ap) {
    struct kfmt_out o = {.sink = sink, .ctx = ctx};
    struct kfmt_spec s;
    va_list args;
    va_copy(args, ap);
    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        p = parse_spec(p + 1, &s, &args);
        if (s.conv == '\0') {
            break;
        }
        put_conversion(&o, &args, &s);
    }
    va_end(args);
    return o.count;
}

/* A bounded buffer: keeps room for the terminating NUL, counts the rest. */
struct kfmt_buffer {
    char *buf;
    size_t cap;
    size_t len;
};

static void buffer_sink(void *ctx, char c) {
    struct kfmt_buffer *b = ctx;
    if (b->len + 1 < b->cap) {
        b->buf[b->len] = c;
    }
    b->len++;
}

size_t kfmt_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    struct kfmt_buffer b = {.buf = buf, .cap = cap};
    va_list ap;
    va_start(ap, fmt);
    kfmt_vformat(buffer_sink, &b, fmt, ap);
    va_end(ap);
    if (cap > 0) {
        buf[b.len < cap ? b.len : cap - 1] = '\0';
    }
    return b.len;
}
