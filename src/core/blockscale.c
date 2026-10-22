#include "core/blockscale.h"

#include <stddef.h>

#include "core/errno.h"

static int scaled_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    const struct blockscale *s = self;
    return s->under->read(s->under->self, lba * s->factor, count * s->factor, buf);
}

static int scaled_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    const struct blockscale *s = self;
    return s->under->write(s->under->self, lba * s->factor, count * s->factor, buf);
}

static int scaled_flush(void *self) {
    const struct blockscale *s = self;
    return s->under->flush(s->under->self);
}

int blockscale_wrap(const struct blockdev *under, uint32_t logical_block, struct blockscale *state,
                    struct blockdev *out) {
    uint32_t factor;
    if (under == NULL || under->block_size == 0 || logical_block < under->block_size ||
        (logical_block & (logical_block - 1u)) != 0 ||
        (logical_block % under->block_size) != 0) {
        return KERR_INVAL;
    }
    factor = logical_block / under->block_size;
    *state = (struct blockscale){.under = under, .factor = factor};
    *out = (struct blockdev){
        .read = scaled_read,
        .write = scaled_write,
        .flush = scaled_flush,
        .self = state,
        .block_size = logical_block,
        /* Whole blocks only. A device whose sector count is not a multiple
         * of the ratio has a tail this cannot address, and pretending
         * otherwise would put the last partial block past the end. */
        .blocks = under->blocks / factor,
        .read_only = under->read_only,
    };
    return KERR_OK;
}
