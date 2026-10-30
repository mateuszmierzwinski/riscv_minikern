/* boardspec: the command-line description of a board. */
#include "core/boardspec.h"

#include "ktest.h"

static void test_nothing_given_changes_nothing(void) {
    struct board_spec s;
    board_spec_parse("console=sbi minikern.halt=poweroff", &s);
    KTEST_CHECK(!s.has_mem && !s.has_uart && s.timebase == 0);
}

static void test_memory_with_a_suffix(void) {
    struct board_spec s;
    board_spec_parse("minikern.mem=0x40000000:8G", &s);
    KTEST_CHECK(s.has_mem && s.mem_base == 0x40000000u && s.mem_size == (8ull << 30));
    board_spec_parse("minikern.mem=0x80000000:268435456", &s);
    KTEST_CHECK(s.has_mem && s.mem_size == 268435456u);
}

static void test_earlycon_in_linux_syntax(void) {
    struct board_spec s;
    board_spec_parse("earlycon=uart8250,mmio32,0x10000000,115200n8", &s);
    KTEST_CHECK(s.has_uart && s.uart_base == 0x10000000u && s.uart_shift == 2 && s.uart_width == 4);
    board_spec_parse("earlycon=uart8250,mmio,0x10000000", &s);
    KTEST_CHECK(s.has_uart && s.uart_shift == 0 && s.uart_width == 1);
}

static void test_timebase(void) {
    struct board_spec s;
    board_spec_parse("minikern.timebase=4000000", &s);
    KTEST_CHECK(s.timebase == 4000000u);
}

static void test_malformed_is_ignored_whole(void) {
    struct board_spec s;
    board_spec_parse("minikern.mem=0x40000000 earlycon=pl011,mmio32,0x9000000 minikern.timebase=4M", &s);
    KTEST_CHECK(!s.has_mem); /* no size */
    KTEST_CHECK(!s.has_uart); /* not a 16550 */
    KTEST_CHECK(s.timebase == 0); /* a suffix is not allowed here */
    board_spec_parse("minikern.mem=0x40000000:0 earlycon=uart8250,mmio32,zz", &s);
    KTEST_CHECK(!s.has_mem && !s.has_uart);
}

int main(void) {
    KTEST_RUN(test_nothing_given_changes_nothing);
    KTEST_RUN(test_memory_with_a_suffix);
    KTEST_RUN(test_earlycon_in_linux_syntax);
    KTEST_RUN(test_timebase);
    KTEST_RUN(test_malformed_is_ignored_whole);
    return ktest_failures();
}
