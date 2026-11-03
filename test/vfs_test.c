/* The vfs mount table: longest prefix wins, at a path boundary, and the
 * claim hook hands a path to a longer mount in another table. */
#include "core/vfs.h"

#include "ktest.h"

static const struct vfs_ops ops = {.name = "fake"};
static int a, b;

static uint32_t claim_volumes(const char *path) {
    return strncmp(path, "/volumes/data", 13) == 0 ? 13u : 0u;
}

static void test_longest_prefix(void) {
    const char *in = NULL;
    vfs_reset();
    vfs_set_claim(NULL);
    KTEST_CHECK(vfs_mount("/", &ops, &a) == 0);
    KTEST_CHECK(vfs_mount("/boot", &ops, &b) == 0);
    KTEST_CHECK(vfs_resolve("/boot/x", &in)->self == &b);
    KTEST_CHECK_STR(in, "/x");
    KTEST_CHECK(vfs_resolve("/bootstrap", &in)->self == &a);
    KTEST_CHECK_STR(in, "/bootstrap");
    KTEST_CHECK(vfs_resolve("/boot", &in)->self == &b);
    KTEST_CHECK_STR(in, "/");
}

static void test_claim_takes_a_longer_path(void) {
    const char *in = NULL;
    vfs_reset();
    KTEST_CHECK(vfs_mount("/", &ops, &a) == 0);
    vfs_set_claim(claim_volumes);
    KTEST_CHECK(vfs_resolve("/volumes/data/f", &in) == NULL);
    KTEST_CHECK(vfs_resolve("/volumes/other", &in)->self == &a);
    KTEST_CHECK(vfs_resolve("/var/x", &in)->self == &a);
    vfs_set_claim(NULL);
    KTEST_CHECK(vfs_resolve("/volumes/data/f", &in)->self == &a);
}

int main(void) {
    KTEST_RUN(test_longest_prefix);
    KTEST_RUN(test_claim_takes_a_longer_path);
    return ktest_failures();
}
