#include "core/cmdline.h"

#include <stdint.h>
#include <string.h>

#include "ktest.h"

static const char *const line = "console=uart0  minikern.halt=poweroff quiet ip=dhcp";

static void test_finds_value(void) {
    size_t len = 0;
    const char *v = cmdline_value(line, "console", &len);
    KTEST_CHECK(v != NULL && len == 5 && strncmp(v, "uart0", len) == 0);
}

static void test_bare_word_has_empty_value(void) {
    size_t len = 99;
    KTEST_CHECK(cmdline_value(line, "quiet", &len) != NULL);
    KTEST_CHECK(len == 0);
}

static void test_last_word(void) {
    KTEST_CHECK(cmdline_equals(line, "ip", "dhcp"));
}

static void test_equals_is_exact(void) {
    KTEST_CHECK(cmdline_equals(line, "minikern.halt", "poweroff"));
    KTEST_CHECK(!cmdline_equals(line, "minikern.halt", "power"));
    KTEST_CHECK(!cmdline_equals(line, "minikern.halt", "poweroffx"));
}

static void test_prefix_of_a_key_is_not_the_key(void) {
    size_t len = 0;
    KTEST_CHECK(cmdline_value(line, "cons", &len) == NULL);
    KTEST_CHECK(cmdline_value(line, "minikern", &len) == NULL);
}

static void test_absent_and_degenerate(void) {
    size_t len = 0;
    KTEST_CHECK(cmdline_value(line, "missing", &len) == NULL);
    KTEST_CHECK(cmdline_value("", "console", &len) == NULL);
    KTEST_CHECK(cmdline_value(NULL, "console", &len) == NULL);
    KTEST_CHECK(cmdline_value(line, "", &len) == NULL);
    KTEST_CHECK(!cmdline_equals("   ", "a", ""));
}

/* Walking the words, including the odd spacing a boot loader may leave. */
static void test_next_walks_every_word(void) {
    static const char *const want[] = {"a=1", "bare", "b=2"};
    size_t at = 0, len = 0;
    uint32_t n = 0;
    for (const char *w = cmdline_next("  a=1   bare b=2  ", &at, &len); w != NULL;
         w = cmdline_next("  a=1   bare b=2  ", &at, &len)) {
        KTEST_CHECK(n < 3 && len == strlen(want[n]) && memcmp(w, want[n], len) == 0);
        n++;
    }
    KTEST_CHECK(n == 3);
    at = 0;
    KTEST_CHECK(cmdline_next("", &at, &len) == NULL);
    at = 0;
    KTEST_CHECK(cmdline_next("   ", &at, &len) == NULL);
    at = 0;
    KTEST_CHECK(cmdline_next(NULL, &at, &len) == NULL);
}

/* Hexadecimal, which a bitmask option needs and which this parser silently
 * refused — returning the fallback, so `minikern.isolate=0x8` read as "the
 * operator did not ask" and a hart that should have been reserved went on
 * taking work. */
static void test_numbers_in_hexadecimal(void) {
    KTEST_CHECK(cmdline_number("a=0x8", "a", 99u) == 8u);
    KTEST_CHECK(cmdline_number("a=0xff", "a", 99u) == 255u);
    KTEST_CHECK(cmdline_number("a=0XFF", "a", 99u) == 255u);
    KTEST_CHECK(cmdline_number("a=0xdeadbeef", "a", 99u) == 0xdeadbeefu);
    /* Decimal still is decimal: `0x` is the only prefix, so a leading zero
     * does not make a number octal the way C would. */
    KTEST_CHECK(cmdline_number("a=10", "a", 99u) == 10u);
    KTEST_CHECK(cmdline_number("a=010", "a", 99u) == 10u);
    /* And malformed is still the fallback, which every caller relies on. */
    KTEST_CHECK(cmdline_number("a=0x", "a", 99u) == 99u);
    KTEST_CHECK(cmdline_number("a=0xg", "a", 99u) == 99u);
    KTEST_CHECK(cmdline_number("a=8x", "a", 99u) == 99u);
    KTEST_CHECK(cmdline_number("a=", "a", 99u) == 99u);
    /* The full width, since this is what a 64-hart mask would need. */
    KTEST_CHECK(cmdline_number("a=0xffffffffffffffff", "a", 0u) == 0xffffffffffffffffull);
}

int main(void) {
    KTEST_RUN(test_next_walks_every_word);
    KTEST_RUN(test_finds_value);
    KTEST_RUN(test_bare_word_has_empty_value);
    KTEST_RUN(test_last_word);
    KTEST_RUN(test_equals_is_exact);
    KTEST_RUN(test_prefix_of_a_key_is_not_the_key);
    KTEST_RUN(test_absent_and_degenerate);
    KTEST_RUN(test_numbers_in_hexadecimal);
    return ktest_failures();
}
