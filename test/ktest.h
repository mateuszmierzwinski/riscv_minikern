/* ktest — the smallest test harness that does the job: no framework, the
 * kernel code under test is plain C. A test file defines test functions,
 * lists them in main with KTEST_RUN, and returns ktest_failures(). */
#ifndef MINIKERN_TEST_KTEST_H
#define MINIKERN_TEST_KTEST_H

#include <stdio.h>
#include <string.h>

static int ktest_failed_count;

#define KTEST_CHECK(cond)                                                   \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            ktest_failed_count++;                                           \
        }                                                                   \
    } while (0)

#define KTEST_CHECK_STR(got, want)                                          \
    do {                                                                    \
        if (strcmp((got), (want)) != 0) {                                   \
            fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
            ktest_failed_count++;                                           \
        }                                                                   \
    } while (0)

#define KTEST_RUN(fn)                     \
    do {                                  \
        fprintf(stderr, "== %s\n", #fn);  \
        fn();                             \
    } while (0)

static inline int ktest_failures(void) {
    fprintf(stderr, ktest_failed_count == 0 ? "PASS\n" : "FAIL: %d check(s)\n", ktest_failed_count);
    return ktest_failed_count == 0 ? 0 : 1;
}

#endif
