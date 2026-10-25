/* The kernel log's format and its two buffers.
 *
 * **The format is the contract**, because anything that reads a log — a
 * person scanning for the first error, a script bisecting a boot — depends
 * on the prefix being exactly one shape. So the tests assert the characters
 * rather than "it contains a time".
 */
#include "core/console.h"
#include "core/klog.h"
#include "ktest.h"
#include <string.h>

/* A console that keeps what it was given, so a test can read it back. */
static char seen[8192];
static size_t seen_len;

static void capture(void *self, const char *buf, size_t len) {
    (void)self;
    for (size_t i = 0; i < len && seen_len < sizeof seen - 1u; i++) {
        seen[seen_len++] = buf[i];
    }
    seen[seen_len] = '\0';
}

static void attach_capture(void) {
    struct console_port port = {.write = capture, .self = NULL};
    seen_len = 0;
    seen[0] = '\0';
    console_attach(&port);
}

/* A clock the test drives, because the interesting times take an hour to
 * reach by waiting. */
static uint64_t fake_ns;

static uint64_t fake_clock(void) {
    return fake_ns;
}

static void test_the_prefix_is_exactly_one_shape(void) {
    attach_capture();
    klog_time_source(fake_clock);
    fake_ns = 0;
    klog(KLOG_INFO, "hello\n");
    KTEST_CHECK_STR(seen, "[00:00:00.000] I hello\n");
}

static void test_every_level_has_its_letter(void) {
    KTEST_CHECK(klog_letter(KLOG_NOTICE) == 'N');
    KTEST_CHECK(klog_letter(KLOG_INFO) == 'I');
    KTEST_CHECK(klog_letter(KLOG_WARN) == 'W');
    KTEST_CHECK(klog_letter(KLOG_ERROR) == 'E');
    KTEST_CHECK(klog_letter(KLOG_DEBUG) == 'D');
    attach_capture();
    klog_time_source(fake_clock);
    fake_ns = 0;
    klog(KLOG_ERROR, "bad\n");
    KTEST_CHECK_STR(seen, "[00:00:00.000] E bad\n");
}

static void test_kprintf_is_an_info_line(void) {
    /* The whole claim of a *unified* log: an untouched `kprintf` call site
     * comes out timestamped and levelled. */
    attach_capture();
    klog_time_source(fake_clock);
    fake_ns = 1500000000ull; /* 1.5 s */
    kprintf("from kprintf\n");
    KTEST_CHECK_STR(seen, "[00:00:01.500] I from kprintf\n");
}

static void test_the_time_is_elapsed_and_does_not_wrap_at_a_day(void) {
    char out[16];
    klog_format_time(0, out, sizeof out);
    KTEST_CHECK_STR(out, "[00:00:00.000]");
    klog_format_time(61999000000ull, out, sizeof out); /* 1m 1.999s */
    KTEST_CHECK_STR(out, "[00:01:01.999]");
    klog_format_time(3600000000000ull, out, sizeof out); /* one hour */
    KTEST_CHECK_STR(out, "[01:00:00.000]");
    /* **Two days reads as 48 hours, not as zero.** This is an elapsed time,
     * and a reader must not have to wonder how many days to add. */
    klog_format_time(172800000000000ull, out, sizeof out);
    KTEST_CHECK_STR(out, "[48:00:00.000]");
}

static void test_one_timestamp_per_line_not_per_call(void) {
    /* Several places in this kernel build a line out of several calls. A
     * prefix emitted per *call* would land in the middle of the line. */
    attach_capture();
    klog_time_source(fake_clock);
    fake_ns = 0;
    kprintf("one ");
    kprintf("two ");
    kprintf("three\n");
    KTEST_CHECK_STR(seen, "[00:00:00.000] I one two three\n");
}

static void test_the_tail_keeps_the_last_lines(void) {
    char out[4096];
    uint32_t n;
    attach_capture();
    klog_time_source(fake_clock);
    fake_ns = 0;
    for (uint32_t i = 0; i < KLOG_TAIL_LINES + 5u; i++) {
        klog(KLOG_INFO, "line %u\n", i);
    }
    n = klog_tail(out, sizeof out - 1u);
    out[n] = '\0';
    /* The oldest five are gone and the newest is present — the tail keeps
     * the *end* of the log, which is what somebody looking at a running
     * machine wants. */
    KTEST_CHECK(strstr(out, "line 0\n") == NULL);
    KTEST_CHECK(strstr(out, "line 4\n") == NULL);
    KTEST_CHECK(strstr(out, "line 5\n") != NULL);
    KTEST_CHECK(strstr(out, "line 34\n") != NULL);
    /* Exactly thirty lines, so a reader gets a screenful rather than
     * whatever happened to fit. */
    {
        uint32_t lines = 0;
        for (uint32_t i = 0; i < n; i++) {
            lines += out[i] == '\n' ? 1u : 0u;
        }
        KTEST_CHECK(lines == KLOG_TAIL_LINES);
    }
}

static void test_the_kept_log_holds_the_boot(void) {
    char out[KLOG_BYTES];
    uint32_t n;
    attach_capture();
    klog_time_source(fake_clock);
    fake_ns = 0;
    klog(KLOG_NOTICE, "first\n");
    klog(KLOG_INFO, "second\n");
    n = klog_all(out, sizeof out);
    KTEST_CHECK(n > 0);
    /* Oldest first, because a log read backwards is a different document. */
    {
        const char *a = NULL, *b = NULL;
        out[n < sizeof out ? n : sizeof out - 1u] = '\0';
        a = strstr(out, "N first");
        b = strstr(out, "I second");
        KTEST_CHECK(a != NULL && b != NULL && a < b);
    }
}

static void test_an_unset_clock_reads_as_zero_rather_than_guessing(void) {
    /* The earliest lines of a boot happen before the timer runs. Saying
     * `[00:00:00.000]` is honest; inventing a time would make the order of
     * two early lines look like a duration. */
    attach_capture();
    klog_time_source(NULL);
    klog(KLOG_WARN, "early\n");
    KTEST_CHECK_STR(seen, "[00:00:00.000] W early\n");
}

int main(void) {
    KTEST_RUN(test_the_prefix_is_exactly_one_shape);
    KTEST_RUN(test_every_level_has_its_letter);
    KTEST_RUN(test_kprintf_is_an_info_line);
    KTEST_RUN(test_the_time_is_elapsed_and_does_not_wrap_at_a_day);
    KTEST_RUN(test_one_timestamp_per_line_not_per_call);
    KTEST_RUN(test_the_tail_keeps_the_last_lines);
    KTEST_RUN(test_the_kept_log_holds_the_boot);
    KTEST_RUN(test_an_unset_clock_reads_as_zero_rather_than_guessing);
    return ktest_failures();
}
