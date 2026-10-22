#include "core/ustack.h"

#include "core/errno.h"
#include "ktest.h"

#define TOP 0x7fff0000u
#define LEN 4096u

static uint8_t buf[LEN];

static uint32_t word_at(uint32_t uaddr) {
    uint32_t off = uaddr - (TOP - LEN);
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) | ((uint32_t)buf[off + 2] << 16) |
           ((uint32_t)buf[off + 3] << 24);
}

static const char *str_at(uint32_t uaddr) {
    return (const char *)&buf[uaddr - (TOP - LEN)];
}

static const char *const argv[] = {"/init", "-v"};
static const char *const envp[] = {"HOME=/", "TERM=linux"};
static const struct auxv_pair aux[] = {{AT_PAGESZ, 4096}, {AT_HWCAP, 0x1234}};

static struct ustack_args args(void) {
    struct ustack_args a = {.argv = argv, .argc = 2, .envp = envp, .envc = 2, .auxv = aux, .nauxv = 2,
                            .platform = "v7l"};
    for (int i = 0; i < 16; i++) {
        a.random[i] = (uint8_t)(i * 7);
    }
    return a;
}

/* Walks the built stack the way a C runtime does. */
static void test_layout_matches_linux(void) {
    struct ustack_args a = args();
    uint32_t sp, used, p;
    KTEST_CHECK(ustack_build(buf, LEN, TOP, &a, &sp, &used) == KERR_OK);
    KTEST_CHECK(sp % 16 == 0 && sp == TOP - used);
    KTEST_CHECK(word_at(sp) == 2);
    KTEST_CHECK_STR(str_at(word_at(sp + 4)), "/init");
    KTEST_CHECK_STR(str_at(word_at(sp + 8)), "-v");
    KTEST_CHECK(word_at(sp + 12) == 0);
    KTEST_CHECK_STR(str_at(word_at(sp + 16)), "HOME=/");
    KTEST_CHECK_STR(str_at(word_at(sp + 20)), "TERM=linux");
    KTEST_CHECK(word_at(sp + 24) == 0);
    p = sp + 28;
    KTEST_CHECK(word_at(p) == AT_PAGESZ && word_at(p + 4) == 4096);
    KTEST_CHECK(word_at(p + 8) == AT_HWCAP && word_at(p + 12) == 0x1234);
    KTEST_CHECK(word_at(p + 16) == AT_RANDOM && buf[word_at(p + 20) - (TOP - LEN) + 15] == 105);
    KTEST_CHECK(word_at(p + 24) == AT_PLATFORM);
    KTEST_CHECK_STR(str_at(word_at(p + 28)), "v7l");
    KTEST_CHECK(word_at(p + 32) == AT_EXECFN);
    KTEST_CHECK_STR(str_at(word_at(p + 36)), "/init");
    KTEST_CHECK(word_at(p + 40) == AT_NULL && word_at(p + 44) == 0);
    KTEST_CHECK(word_at(TOP - 4) == 0);
}

static void test_too_small_is_range(void) {
    struct ustack_args a = args();
    uint32_t sp, used;
    KTEST_CHECK(ustack_build(buf, 64, TOP, &a, &sp, &used) == KERR_RANGE);
    KTEST_CHECK(ustack_build(buf, 112, TOP, &a, &sp, &used) == KERR_RANGE);  /* strings fit, vector not */
    KTEST_CHECK(ustack_build(buf, LEN, TOP + 4, &a, &sp, &used) == KERR_RANGE);
}

static void test_limits(void) {
    struct ustack_args a = args();
    struct auxv_pair many[USTACK_MAX_AUXV];
    uint32_t sp, used;
    memset(many, 0, sizeof many);
    a.auxv = many;
    a.nauxv = USTACK_MAX_AUXV - 2;
    KTEST_CHECK(ustack_build(buf, LEN, TOP, &a, &sp, &used) == KERR_RANGE);
    a = args();
    a.argc = USTACK_MAX_ARGS + 1;
    KTEST_CHECK(ustack_build(buf, LEN, TOP, &a, &sp, &used) == KERR_RANGE);
}

static void test_no_args(void) {
    struct ustack_args a = args();
    uint32_t sp, used;
    a.argc = 0;
    a.envc = 0;
    KTEST_CHECK(ustack_build(buf, LEN, TOP, &a, &sp, &used) == KERR_OK);
    KTEST_CHECK(word_at(sp) == 0 && word_at(sp + 4) == 0 && word_at(sp + 8) == 0);
}

int main(void) {
    KTEST_RUN(test_layout_matches_linux);
    KTEST_RUN(test_too_small_is_range);
    KTEST_RUN(test_limits);
    KTEST_RUN(test_no_args);
    return ktest_failures();
}
