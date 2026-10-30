/* overlay: the option string a mount names its layers with, and the names
 * a path maps to on each layer — the parts of the overlay a mistake in
 * would send a lookup to the wrong file, or a write outside the upper layer. */
#include "core/overlay.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"

static void test_options_name_both_layers(void) {
    char lower[OVERLAY_PATH_MAX], upper[OVERLAY_PATH_MAX];
    KTEST_CHECK(overlay_parse_options("lowerdir=/net/ro,upperdir=/var/up,workdir=/var/w", lower,
                                      upper) == KERR_OK);
    KTEST_CHECK_STR(lower, "/net/ro");
    KTEST_CHECK_STR(upper, "/var/up");
}

static void test_bad_options_are_refused(void) {
    char lower[OVERLAY_PATH_MAX], upper[OVERLAY_PATH_MAX];
    const char *bad[] = {"",
                         "lowerdir=/a",
                         "upperdir=/b",
                         "lowerdir=a,upperdir=/b",
                         "lowerdir=/a,upperdir=/b,lowerdir=/c",
                         "lowerdir=/a,upperdir=/b,nosuch=1",
                         "lowerdir=,upperdir=/b",
                         "lowerdir=/a,upperdir=/b,workdir=/w,workdir=/w"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        KTEST_CHECK(overlay_parse_options(bad[i], lower, upper) == KERR_INVAL);
    }
}

static void test_a_too_long_layer_is_refused(void) {
    char lower[OVERLAY_PATH_MAX], upper[OVERLAY_PATH_MAX], data[600];
    strcpy(data, "lowerdir=/");
    memset(data + 10, 'a', 300);
    strcpy(data + 310, ",upperdir=/b");
    KTEST_CHECK(overlay_parse_options(data, lower, upper) == KERR_INVAL);
}

static void test_join(void) {
    char out[OVERLAY_PATH_MAX], longp[OVERLAY_PATH_MAX + 8];
    KTEST_CHECK(overlay_join("/var/up", "/etc/passwd", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/var/up/etc/passwd");
    KTEST_CHECK(overlay_join("/var/up/", "/", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/var/up");
    KTEST_CHECK(overlay_join("/", "/x", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/x");
    KTEST_CHECK(overlay_join("/", "/", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/");
    memset(longp, 'a', sizeof longp);
    longp[0] = '/';
    longp[sizeof longp - 1] = '\0';
    KTEST_CHECK(overlay_join("/var/up", longp, out) == KERR_NAMETOOLONG);
}

static void test_whiteouts(void) {
    char out[OVERLAY_PATH_MAX];
    KTEST_CHECK(overlay_whiteout_of("/etc/passwd", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/etc/.wh.passwd");
    KTEST_CHECK(overlay_whiteout_of("/top", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/.wh.top");
    KTEST_CHECK(overlay_whiteout_of("/", out) == KERR_INVAL);
    KTEST_CHECK(overlay_whiteout_of("/dir/", out) == KERR_INVAL);
    KTEST_CHECK(overlay_is_whiteout(".wh.passwd", 10));
    KTEST_CHECK(!overlay_is_whiteout(".wh.", 4));
    KTEST_CHECK(!overlay_is_whiteout("passwd", 6));
}

static void test_under(void) {
    KTEST_CHECK(overlay_path_under("/a/b", "/a"));
    KTEST_CHECK(overlay_path_under("/a", "/a/"));
    KTEST_CHECK(!overlay_path_under("/ab", "/a"));
    KTEST_CHECK(overlay_path_under("/anything", "/"));
    KTEST_CHECK(!overlay_path_under("/a", "/a/b"));
}

int main(void) {
    KTEST_RUN(test_options_name_both_layers);
    KTEST_RUN(test_bad_options_are_refused);
    KTEST_RUN(test_a_too_long_layer_is_refused);
    KTEST_RUN(test_join);
    KTEST_RUN(test_whiteouts);
    KTEST_RUN(test_under);
    return ktest_failures();
}
