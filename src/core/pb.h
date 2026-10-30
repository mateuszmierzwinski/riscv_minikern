/* pb — the protobuf wire format, the part netfs needs: varints and
 * length-delimited fields, written into a bounded buffer and read back with
 * every length checked against what is left. Unknown fields are skipped, so
 * a newer server's additions do not break an older kernel.
 */
#ifndef MINIKERN_CORE_PB_H
#define MINIKERN_CORE_PB_H

#include <stdbool.h>
#include <stdint.h>

#define PB_VARINT 0u
#define PB_FIXED64 1u
#define PB_BYTES 2u
#define PB_FIXED32 5u

/* A message being written. `overflow` is sticky: a field that does not fit
 * is dropped and the whole message is then refused by its sender. */
struct pb_writer {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    bool overflow;
};

void pb_writer_init(struct pb_writer *w, uint8_t *buf, uint32_t cap);
void pb_put_varint(struct pb_writer *w, uint32_t field, uint64_t value);
void pb_put_bytes(struct pb_writer *w, uint32_t field, const void *data, uint32_t n);
void pb_put_string(struct pb_writer *w, uint32_t field, const char *s);

/* A message being read. */
struct pb_reader {
    const uint8_t *buf;
    uint32_t len;
    uint32_t at;
};

/* One field: its number and wire type, and either its value (varint,
 * fixed) or where its bytes are (length-delimited). */
struct pb_field {
    uint32_t number;
    uint32_t wire;
    uint64_t value;
    const uint8_t *data;
    uint32_t data_len;
};

void pb_reader_init(struct pb_reader *r, const uint8_t *buf, uint32_t len);

/* The next field. KERR_OK, KERR_NOENT at the end of the message, or
 * KERR_PROTO for anything malformed (a truncated varint, a length past the
 * end, a wire type proto3 does not have). */
int pb_next(struct pb_reader *r, struct pb_field *f);

#endif
