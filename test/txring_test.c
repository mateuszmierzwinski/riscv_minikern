#include "core/txring.h"
#include "ktest.h"

static struct txring ring;

static void test_bytes_come_out_in_order(void) {
    char c = 0;
    ring = (struct txring){0};
    KTEST_CHECK(!txring_pop(&ring, &c));
    KTEST_CHECK(txring_push(&ring, 'a') && txring_push(&ring, 'b'));
    KTEST_CHECK(txring_queued(&ring) == 2u);
    KTEST_CHECK(txring_pop(&ring, &c) && c == 'a');
    KTEST_CHECK(txring_pop(&ring, &c) && c == 'b');
    KTEST_CHECK(!txring_pop(&ring, &c) && txring_queued(&ring) == 0u);
}

/* Full is refused without losing what is there; and the indices wrap. */
static void test_a_full_ring_refuses_and_wraps(void) {
    char c = 0;
    ring = (struct txring){.head = 0xfffffff0u, .tail = 0xfffffff0u};
    for (uint32_t i = 0; i < TXRING_BYTES; i++) {
        KTEST_CHECK(txring_push(&ring, (char)(i & 0x7fu)));
    }
    KTEST_CHECK(txring_full(&ring) && !txring_push(&ring, 'x'));
    for (uint32_t i = 0; i < TXRING_BYTES; i++) {
        KTEST_CHECK(txring_pop(&ring, &c) && c == (char)(i & 0x7fu));
    }
    KTEST_CHECK(txring_queued(&ring) == 0u && !txring_full(&ring));
}

int main(void) {
    KTEST_RUN(test_bytes_come_out_in_order);
    KTEST_RUN(test_a_full_ring_refuses_and_wraps);
    return ktest_failures();
}
