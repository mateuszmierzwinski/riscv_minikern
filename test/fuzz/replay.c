/* Replays a fuzz corpus through one harness, as an ordinary test.
 *
 * Every argument is a directory; every file in it is one input, read whole
 * and handed to `LLVMFuzzerTestOneInput` in a buffer of exactly its size,
 * so that a read past the end lands on the allocator's redzone rather than
 * on the next input. A harness that crashes fails the test, which is the
 * point: a committed crasher is a regression gate. A directory that does
 * not exist is an empty corpus, which is a normal state. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"

/* Reads `path` whole into a buffer of exactly its length; NULL on failure. */
static uint8_t *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc(n > 0 ? (size_t)n : 1u);
            *len = (size_t)n;
            if (buf != NULL && fread(buf, 1, *len, f) != *len) {
                free(buf);
                buf = NULL;
            }
        }
    }
    fclose(f);
    return buf;
}

/* Runs every regular file in `dir`; returns how many, or -1 on a read error. */
static long replay_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    long n = 0;
    char path[4096];
    for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        size_t len = 0;
        if (e->d_name[0] == '.' || snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) {
            continue;
        }
        uint8_t *buf = read_whole(path, &len);
        if (buf == NULL) {
            fprintf(stderr, "replay: cannot read %s\n", path);
            n = -1;
            break;
        }
        (void)LLVMFuzzerTestOneInput(buf, len);
        free(buf);
        n++;
    }
    closedir(d);
    return n;
}

int main(int argc, char **argv) {
    long total = 0;
    for (int i = 1; i < argc; i++) {
        long n = replay_dir(argv[i]);
        if (n < 0) {
            return 1;
        }
        printf("replay: %ld input(s) from %s\n", n, argv[i]);
        total += n;
    }
    printf("replay: %ld input(s), none crashed\n", total);
    return 0;
}
