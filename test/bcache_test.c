#include "core/bcache.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"

#define BS 512u
#define DEV_BLOCKS 32u
#define SLOTS 4u

/* A RAM-backed block device that counts what the cache asked of it. */
struct ram {
    uint8_t data[DEV_BLOCKS * BS];
    uint32_t reads, writes, flushes;
    int fail_read, fail_write;
    struct blockdev dev;
};

static int ram_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    struct ram *r = self;
    r->reads++;
    if (r->fail_read != KERR_OK) {
        return r->fail_read;
    }
    if (!blockdev_in_range(&r->dev, lba, count)) {
        return KERR_INVAL;
    }
    memcpy(buf, r->data + lba * BS, count * BS);
    return KERR_OK;
}

static int ram_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    struct ram *r = self;
    r->writes++;
    if (r->fail_write != KERR_OK) {
        return r->fail_write;
    }
    if (!blockdev_in_range(&r->dev, lba, count) || r->dev.read_only) {
        return KERR_INVAL;
    }
    memcpy(r->data + lba * BS, buf, count * BS);
    return KERR_OK;
}

static int ram_flush(void *self) {
    ((struct ram *)self)->flushes++;
    return KERR_OK;
}

static struct ram ram;
static struct bcache cache;
static uint8_t cache_mem[SLOTS * BS];

static void setup(bool read_only) {
    memset(&ram, 0, sizeof ram);
    ram.dev = (struct blockdev){.read = ram_read,
                                .write = ram_write,
                                .flush = ram_flush,
                                .self = &ram,
                                .block_size = BS,
                                .blocks = DEV_BLOCKS,
                                .read_only = read_only};
    for (uint32_t i = 0; i < DEV_BLOCKS; i++) {
        memset(ram.data + i * BS, (int)i, BS);  /* block n is full of n */
    }
    KTEST_CHECK(bcache_init(&cache, &ram.dev, cache_mem, SLOTS) == KERR_OK);
}

static void test_init_checks_its_arguments(void) {
    struct blockdev bad;
    setup(false);
    bad = ram.dev;
    bad.block_size = 500u;  /* not a power of two, and under 512 */
    KTEST_CHECK(bcache_init(&cache, &bad, cache_mem, SLOTS) == KERR_INVAL);
    bad.block_size = 1024u;  /* fine, but the slots are not */
    KTEST_CHECK(bcache_init(&cache, &bad, cache_mem, 0) == KERR_INVAL);
    KTEST_CHECK(bcache_init(&cache, &bad, cache_mem, BCACHE_MAX_SLOTS + 1u) == KERR_INVAL);
    KTEST_CHECK(bcache_init(&cache, NULL, cache_mem, SLOTS) == KERR_INVAL);
    KTEST_CHECK(bcache_init(&cache, &ram.dev, NULL, SLOTS) == KERR_INVAL);
}

/* The point of the cache: the second read of a block costs no round trip. */
static void test_a_second_read_is_free(void) {
    struct bbuf *a, *b;
    setup(false);
    KTEST_CHECK(bcache_get(&cache, 3, &a) == KERR_OK);
    KTEST_CHECK(a->data[0] == 3 && a->lba == 3);
    bcache_put(&cache, a);
    KTEST_CHECK(bcache_get(&cache, 3, &b) == KERR_OK);
    bcache_put(&cache, b);
    KTEST_CHECK(ram.reads == 1 && cache.hits == 1 && cache.misses == 1);
}

static void test_out_of_range_is_refused(void) {
    struct bbuf *b;
    setup(false);
    KTEST_CHECK(bcache_get(&cache, DEV_BLOCKS, &b) == KERR_INVAL);
    KTEST_CHECK(bcache_get(&cache, DEV_BLOCKS + 100u, &b) == KERR_INVAL);
    KTEST_CHECK(ram.reads == 0);
}

/* A pinned buffer cannot be evicted; when every slot is pinned the cache
 * says so rather than quietly stealing one out from under a caller. */
static void test_pins_are_honoured(void) {
    struct bbuf *held[SLOTS];
    struct bbuf *extra;
    setup(false);
    for (uint32_t i = 0; i < SLOTS; i++) {
        KTEST_CHECK(bcache_get(&cache, i, &held[i]) == KERR_OK);
    }
    KTEST_CHECK(bcache_get(&cache, SLOTS, &extra) == KERR_NOMEM);
    bcache_put(&cache, held[0]);
    KTEST_CHECK(bcache_get(&cache, SLOTS, &extra) == KERR_OK);
    KTEST_CHECK(extra->data[0] == SLOTS);
    /* The ones still pinned kept their contents. */
    for (uint32_t i = 1; i < SLOTS; i++) {
        KTEST_CHECK(held[i]->data[0] == i && held[i]->lba == i);
    }
}

static void test_eviction_writes_dirty_blocks_back(void) {
    struct bbuf *b;
    setup(false);
    KTEST_CHECK(bcache_get(&cache, 0, &b) == KERR_OK);
    b->data[7] = 0xaa;
    bcache_dirty(b);
    bcache_put(&cache, b);
    /* Fill the cache past its size, so block 0's slot is reused. */
    for (uint32_t i = 1; i <= SLOTS; i++) {
        KTEST_CHECK(bcache_get(&cache, i, &b) == KERR_OK);
        bcache_put(&cache, b);
    }
    KTEST_CHECK(cache.writebacks == 1 && ram.writes == 1);
    KTEST_CHECK(ram.data[7] == 0xaa);
}

static void test_the_least_recently_used_goes_first(void) {
    struct bbuf *b;
    setup(false);
    for (uint32_t i = 0; i < SLOTS; i++) {
        KTEST_CHECK(bcache_get(&cache, i, &b) == KERR_OK);
        bcache_put(&cache, b);
    }
    KTEST_CHECK(bcache_get(&cache, 0, &b) == KERR_OK);  /* 0 is now the newest */
    bcache_put(&cache, b);
    KTEST_CHECK(bcache_get(&cache, SLOTS, &b) == KERR_OK);  /* evicts 1, not 0 */
    bcache_put(&cache, b);
    ram.reads = 0;
    KTEST_CHECK(bcache_get(&cache, 0, &b) == KERR_OK);
    bcache_put(&cache, b);
    KTEST_CHECK(ram.reads == 0);  /* still cached */
    KTEST_CHECK(bcache_get(&cache, 1, &b) == KERR_OK);
    bcache_put(&cache, b);
    KTEST_CHECK(ram.reads == 1);  /* was evicted */
}

static void test_flush_writes_everything_once(void) {
    struct bbuf *b;
    setup(false);
    for (uint32_t i = 0; i < 3; i++) {
        KTEST_CHECK(bcache_get(&cache, i, &b) == KERR_OK);
        b->data[0] = 0x5a;
        bcache_dirty(b);
        bcache_put(&cache, b);
    }
    KTEST_CHECK(bcache_flush(&cache) == KERR_OK);
    KTEST_CHECK(ram.writes == 3 && ram.flushes == 1);
    KTEST_CHECK(bcache_flush(&cache) == KERR_OK);
    KTEST_CHECK(ram.writes == 3);  /* nothing is dirty any more */
    KTEST_CHECK(ram.data[0] == 0x5a && ram.data[BS] == 0x5a && ram.data[2 * BS] == 0x5a);
}

static void test_a_failed_read_leaves_nothing_behind(void) {
    struct bbuf *b;
    setup(false);
    ram.fail_read = KERR_IO;
    KTEST_CHECK(bcache_get(&cache, 5, &b) == KERR_IO);
    ram.fail_read = KERR_OK;
    KTEST_CHECK(bcache_get(&cache, 5, &b) == KERR_OK);
    KTEST_CHECK(b->data[0] == 5);  /* not the failed read's leftovers */
    bcache_put(&cache, b);
}

/* A write-back that fails must be reported, not swallowed by the flush. */
static void test_a_failed_write_is_reported(void) {
    struct bbuf *b;
    setup(false);
    KTEST_CHECK(bcache_get(&cache, 1, &b) == KERR_OK);
    bcache_dirty(b);
    bcache_put(&cache, b);
    ram.fail_write = KERR_IO;
    KTEST_CHECK(bcache_flush(&cache) == KERR_IO);
}

static void test_byte_access_spans_blocks(void) {
    uint8_t buf[BS * 2 + 8];
    setup(false);
    /* Straddling three blocks: the last 4 bytes of block 0, all of block 1,
     * and the head of block 2. */
    KTEST_CHECK(bcache_pread(&cache, BS - 4u, buf, sizeof buf) == KERR_OK);
    KTEST_CHECK(buf[0] == 0 && buf[3] == 0);
    KTEST_CHECK(buf[4] == 1 && buf[4 + BS - 1u] == 1);
    KTEST_CHECK(buf[4 + BS] == 2);
    memset(buf, 0xc3, sizeof buf);
    KTEST_CHECK(bcache_pwrite(&cache, BS - 4u, buf, sizeof buf) == KERR_OK);
    KTEST_CHECK(bcache_flush(&cache) == KERR_OK);
    KTEST_CHECK(ram.data[BS - 5u] == 0);        /* just before: untouched */
    KTEST_CHECK(ram.data[BS - 4u] == 0xc3);
    KTEST_CHECK(ram.data[BS - 4u + sizeof buf - 1u] == 0xc3);
    KTEST_CHECK(ram.data[BS - 4u + sizeof buf] == 3);  /* just after: untouched */
}

static void test_byte_access_checks_the_end(void) {
    uint8_t buf[16];
    uint64_t bytes = (uint64_t)DEV_BLOCKS * BS;
    setup(false);
    KTEST_CHECK(bcache_pread(&cache, bytes - 8u, buf, 8u) == KERR_OK);
    KTEST_CHECK(bcache_pread(&cache, bytes - 8u, buf, 9u) == KERR_INVAL);
    KTEST_CHECK(bcache_pread(&cache, bytes, buf, 1u) == KERR_INVAL);
    KTEST_CHECK(bcache_pwrite(&cache, bytes - 1u, buf, 2u) == KERR_INVAL);
    KTEST_CHECK(bcache_pread(&cache, 0, buf, 0) == KERR_OK);  /* nothing is nothing */
}

static void test_a_read_only_device_refuses_writes(void) {
    uint8_t buf[8] = {0};
    setup(true);
    KTEST_CHECK(bcache_pwrite(&cache, 0, buf, sizeof buf) == KERR_ROFS);
    KTEST_CHECK(ram.writes == 0);
}

static void test_invalidate_keeps_what_is_not_safe_to_drop(void) {
    struct bbuf *clean, *dirty, *pinned;
    setup(false);
    KTEST_CHECK(bcache_get(&cache, 0, &clean) == KERR_OK);
    bcache_put(&cache, clean);
    KTEST_CHECK(bcache_get(&cache, 1, &dirty) == KERR_OK);
    bcache_dirty(dirty);
    bcache_put(&cache, dirty);
    KTEST_CHECK(bcache_get(&cache, 2, &pinned) == KERR_OK);
    bcache_invalidate(&cache);
    KTEST_CHECK(!clean->valid);
    KTEST_CHECK(dirty->valid && dirty->dirty);
    KTEST_CHECK(pinned->valid);
    bcache_put(&cache, pinned);
}

int main(void) {
    KTEST_RUN(test_init_checks_its_arguments);
    KTEST_RUN(test_a_second_read_is_free);
    KTEST_RUN(test_out_of_range_is_refused);
    KTEST_RUN(test_pins_are_honoured);
    KTEST_RUN(test_eviction_writes_dirty_blocks_back);
    KTEST_RUN(test_the_least_recently_used_goes_first);
    KTEST_RUN(test_flush_writes_everything_once);
    KTEST_RUN(test_a_failed_read_leaves_nothing_behind);
    KTEST_RUN(test_a_failed_write_is_reported);
    KTEST_RUN(test_byte_access_spans_blocks);
    KTEST_RUN(test_byte_access_checks_the_end);
    KTEST_RUN(test_a_read_only_device_refuses_writes);
    KTEST_RUN(test_invalidate_keeps_what_is_not_safe_to_drop);
    return ktest_failures();
}
