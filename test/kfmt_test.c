#include "lib/kfmt.h"

#include <stdint.h>

#include "ktest.h"

static char out[128];

/* The deliberately malformed formats below must reach kfmt unchecked: called
 * through a pointer, the compiler's printf checking does not see them. */
static size_t (*const unchecked_snprintf)(char *, size_t, const char *, ...) = kfmt_snprintf;

static void test_integers_match_c_printf(void) {
    char want[128];
    kfmt_snprintf(out, sizeof out, "%d|%i|%u|%x|%X|%5d|%-5d|%05d|%08x", -42, 7, 4000000000u,
                  0xbeefu, 0xbeefu, 42, 42, -42, 0x1234u);
    snprintf(want, sizeof want, "%d|%i|%u|%x|%X|%5d|%-5d|%05d|%08x", -42, 7, 4000000000u,
             0xbeefu, 0xbeefu, 42, 42, -42, 0x1234u);
    KTEST_CHECK_STR(out, want);
}

static void test_length_modifiers(void) {
    kfmt_snprintf(out, sizeof out, "%lu %llx %lld", 123456ul, 0x1122334455667788ull, -9000000000ll);
    KTEST_CHECK_STR(out, "123456 1122334455667788 -9000000000");
}

static void test_extremes(void) {
    char want[64];
    kfmt_snprintf(out, sizeof out, "%d %u %x", INT32_MIN, 0u, 0u);
    snprintf(want, sizeof want, "%d %u %x", INT32_MIN, 0u, 0u);
    KTEST_CHECK_STR(out, want);
}

static void test_strings_and_chars(void) {
    kfmt_snprintf(out, sizeof out, "[%s][%6s][%-6s][%c][%%]", "abc", "abc", "abc", 'z');
    KTEST_CHECK_STR(out, "[abc][   abc][abc   ][z][%]");
}

static void test_null_string_does_not_fault(void) {
    unchecked_snprintf(out, sizeof out, "%s", (const char *)NULL);
    KTEST_CHECK_STR(out, "(null)");
}

static void test_pointer_is_full_width(void) {
    char want[64];
    void *p = (void *)(uintptr_t)0x40008000u;
    kfmt_snprintf(out, sizeof out, "%p", p);
    snprintf(want, sizeof want, "0x%0*llx", (int)(2 * sizeof(void *)), (unsigned long long)(uintptr_t)p);
    KTEST_CHECK_STR(out, want);
}

static void test_unknown_conversion_is_echoed(void) {
    unchecked_snprintf(out, sizeof out, "a%qb");
    KTEST_CHECK_STR(out, "a%qb");
}

static void test_trailing_percent_stops(void) {
    unchecked_snprintf(out, sizeof out, "end%");
    KTEST_CHECK_STR(out, "end");
}

static void test_truncation_reports_full_length(void) {
    char small[5];
    size_t n = kfmt_snprintf(small, sizeof small, "%s", "abcdefgh");
    KTEST_CHECK(n == 8);
    KTEST_CHECK_STR(small, "abcd");
}

static void test_zero_capacity_writes_nothing(void) {
    char guard = 'G';
    size_t n = kfmt_snprintf(&guard, 0, "xyz");
    KTEST_CHECK(n == 3);
    KTEST_CHECK(guard == 'G');
}

static void test_string_precision(void) {
    char got[64], want[64];
    kfmt_snprintf(got, sizeof got, "[%.3s][%.*s][%5.2s][%.*s]", "abcdef", 2, "xyz", "hello", -1, "all");
    snprintf(want, sizeof want, "[%.3s][%.*s][%5.2s][%.*s]", "abcdef", 2, "xyz", "hello", -1, "all");
    KTEST_CHECK_STR(got, want);
    kfmt_snprintf(got, sizeof got, "%.*s|", 4, (const char[]){'a', 'b', 'c', 'd'});  /* not NUL-terminated */
    KTEST_CHECK_STR(got, "abcd|");
}

int main(void) {
    KTEST_RUN(test_string_precision);
    KTEST_RUN(test_integers_match_c_printf);
    KTEST_RUN(test_length_modifiers);
    KTEST_RUN(test_extremes);
    KTEST_RUN(test_strings_and_chars);
    KTEST_RUN(test_null_string_does_not_fault);
    KTEST_RUN(test_pointer_is_full_width);
    KTEST_RUN(test_unknown_conversion_is_echoed);
    KTEST_RUN(test_trailing_percent_stops);
    KTEST_RUN(test_truncation_reports_full_length);
    KTEST_RUN(test_zero_capacity_writes_nothing);
    return ktest_failures();
}
