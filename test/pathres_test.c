/* pathres: `.`, `..` and links resolved a component at a time, never as
 * text — the sandbox escape a textual `..` allows is the case that matters. */
#include "core/pathres.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"

/* A fake tree's links: path -> target. */
static const char *const links[][2] = {
    {"/jail/out", "/etc"},         /* absolute */
    {"/jail/up", "../secret"},     /* relative to /jail */
    {"/a/self", "/a/self"},        /* a loop */
    {"/data/cur", "v2"},           /* relative, same directory */
};

static int fake_readlink(void *self, const char *path, char *buf, uint32_t max, uint32_t *got) {
    (void)self;
    for (size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        if (strcmp(path, links[i][0]) == 0) {
            uint32_t n = (uint32_t)strlen(links[i][1]);
            memcpy(buf, links[i][1], n < max ? n : max);
            *got = n;
            return KERR_OK;
        }
    }
    return KERR_INVAL;
}

static char scratch[PATHRES_SCRATCH];

static int resolve(const char *in, char *out) {
    return path_resolve(in, out, 128, fake_readlink, NULL, scratch);
}

static void test_dots_collapse(void) {
    char out[128];
    KTEST_CHECK(resolve("/usr/./bin/../lib//x", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/usr/lib/x");
    KTEST_CHECK(resolve("/../../etc", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/etc"); /* .. at the root stays at the root */
    KTEST_CHECK(resolve("/", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/");
}

static void test_dotdot_through_a_link_follows_the_link(void) {
    char out[128];
    /* As text this is "/jail"; through the link it is the target's parent. */
    KTEST_CHECK(resolve("/jail/out/../passwd", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/passwd");
    KTEST_CHECK(resolve("/jail/up/../x", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/x");
}

static void test_relative_link_resolves_from_its_directory(void) {
    char out[128];
    KTEST_CHECK(resolve("/data/cur/file", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/data/v2/file");
}

static void test_last_component_is_not_followed(void) {
    char out[128];
    KTEST_CHECK(resolve("/jail/out", out) == KERR_OK);
    KTEST_CHECK_STR(out, "/jail/out");
}

static void test_a_loop_is_eloop(void) {
    char out[128];
    KTEST_CHECK(resolve("/a/self/x", out) == KERR_LOOP);
}

static void test_bounds(void) {
    char out[128], tiny[4], longp[200];
    KTEST_CHECK(resolve("relative", out) == KERR_INVAL);
    KTEST_CHECK(path_resolve("/abcdef", tiny, sizeof tiny, fake_readlink, NULL, scratch) ==
                KERR_NAMETOOLONG);
    memset(longp, 'a', sizeof longp);
    longp[0] = '/';
    longp[sizeof longp - 1] = '\0';
    KTEST_CHECK(resolve(longp, out) == KERR_NAMETOOLONG);
}

int main(void) {
    KTEST_RUN(test_dots_collapse);
    KTEST_RUN(test_dotdot_through_a_link_follows_the_link);
    KTEST_RUN(test_relative_link_resolves_from_its_directory);
    KTEST_RUN(test_last_component_is_not_followed);
    KTEST_RUN(test_a_loop_is_eloop);
    KTEST_RUN(test_bounds);
    return ktest_failures();
}
