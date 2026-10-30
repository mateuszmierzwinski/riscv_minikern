#include "core/pb.h"

#include "core/errno.h"

void pb_writer_init(struct pb_writer *w, uint8_t *buf, uint32_t cap) {
    *w = (struct pb_writer){.buf = buf, .cap = cap};
}

static void put_raw_varint(struct pb_writer *w, uint64_t v) {
    do {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        if (v != 0) {
            b |= 0x80u;
        }
        if (w->len >= w->cap) {
            w->overflow = true;
            return;
        }
        w->buf[w->len++] = b;
    } while (v != 0);
}

void pb_put_varint(struct pb_writer *w, uint32_t field, uint64_t value) {
    put_raw_varint(w, ((uint64_t)field << 3) | PB_VARINT);
    put_raw_varint(w, value);
}

void pb_put_bytes(struct pb_writer *w, uint32_t field, const void *data, uint32_t n) {
    const uint8_t *src = data;
    put_raw_varint(w, ((uint64_t)field << 3) | PB_BYTES);
    put_raw_varint(w, n);
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        w->buf[w->len + i] = src[i];
    }
    w->len += n;
}

void pb_put_string(struct pb_writer *w, uint32_t field, const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    pb_put_bytes(w, field, s, n);
}

void pb_reader_init(struct pb_reader *r, const uint8_t *buf, uint32_t len) {
    *r = (struct pb_reader){.buf = buf, .len = len};
}

/* At most ten bytes, the most a 64-bit value takes. */
static int get_raw_varint(struct pb_reader *r, uint64_t *out) {
    uint64_t v = 0;
    for (uint32_t shift = 0; shift < 70u; shift += 7u) {
        uint8_t b;
        if (r->at >= r->len) {
            return KERR_PROTO;
        }
        b = r->buf[r->at++];
        v |= (uint64_t)(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            *out = v;
            return KERR_OK;
        }
    }
    return KERR_PROTO;
}

/* A fixed-width field's value, `bytes` long, little-endian. */
static int get_fixed(struct pb_reader *r, uint32_t bytes, uint64_t *out) {
    uint64_t v = 0;
    if (bytes > r->len - r->at) {
        return KERR_PROTO;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        v |= (uint64_t)r->buf[r->at + i] << (8u * i);
    }
    r->at += bytes;
    *out = v;
    return KERR_OK;
}

static int get_payload(struct pb_reader *r, struct pb_field *f) {
    switch (f->wire) {
        case PB_VARINT: return get_raw_varint(r, &f->value);
        case PB_FIXED64: return get_fixed(r, 8u, &f->value);
        case PB_FIXED32: return get_fixed(r, 4u, &f->value);
        case PB_BYTES: {
            uint64_t n = 0;
            int rc = get_raw_varint(r, &n);
            if (rc != KERR_OK || n > r->len - r->at) {
                return KERR_PROTO;
            }
            f->data = r->buf + r->at;
            f->data_len = (uint32_t)n;
            r->at += (uint32_t)n;
            return KERR_OK;
        }
        default: return KERR_PROTO;
    }
}

int pb_next(struct pb_reader *r, struct pb_field *f) {
    uint64_t key = 0;
    int rc;
    if (r->at >= r->len) {
        return KERR_NOENT;
    }
    rc = get_raw_varint(r, &key);
    if (rc != KERR_OK || (key >> 3) == 0 || (key >> 3) > 0x1fffffffu) {
        return KERR_PROTO;
    }
    *f = (struct pb_field){.number = (uint32_t)(key >> 3), .wire = (uint32_t)(key & 7u)};
    return get_payload(r, f);
}
