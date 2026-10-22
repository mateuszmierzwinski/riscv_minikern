#include "kernel/file.h"

#include <stddef.h>

#include "core/errno.h"
#include "kernel/mem.h"

/* The process's table. One process, so one table — the same arrangement as
 * the signal dispositions, and it moves into `struct uproc` at the same
 * time as those do. */
static struct file *table[FD_MAX];

/* Bumped by every readiness change anywhere. An epoll waiter reads it before
 * it sleeps and again when it wakes: unchanged means nothing could have
 * become ready, so there is nothing to re-scan. One counter rather than one
 * per file because a waiter watches a *set*. */
static uint32_t readiness_generation;

void fd_reset(void) {
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (table[i] != NULL) {
            file_put(table[i]);
            table[i] = NULL;
        }
    }
    readiness_generation = 0;
}

struct file *file_new(const struct file_ops *ops, void *priv, uint32_t flags) {
    struct file *f = kzalloc(sizeof *f);
    if (f == NULL) {
        return NULL;
    }
    f->ops = ops;
    f->priv = priv;
    f->flags = flags;
    f->refs = 1;
    return f;
}

struct file *file_get(struct file *f) {
    if (f != NULL) {
        f->refs++;
    }
    return f;
}

void file_put(struct file *f) {
    if (f == NULL || --f->refs > 0) {
        return;
    }
    if (f->ops->release != NULL) {
        f->ops->release(f);
    }
    kfree(f);
}

int fd_install(struct file *f) {
    if (f == NULL) {
        return KERR_NOMEM;
    }
    /* The **lowest** free descriptor, which is what the ABI promises and
     * what a program relies on when it dups onto 0, 1 or 2. */
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (table[i] == NULL) {
            table[i] = f;
            return (int)i;
        }
    }
    file_put(f);
    return KERR_MFILE;
}

struct file *fd_get(int32_t fd) {
    return fd >= 0 && (uint32_t)fd < FD_MAX ? table[fd] : NULL;
}

int fd_close(int32_t fd) {
    struct file *f = fd_get(fd);
    if (f == NULL) {
        return KERR_BADF;
    }
    table[fd] = NULL;
    file_put(f);
    return KERR_OK;
}

int fd_dup(int32_t fd) {
    struct file *f = fd_get(fd);
    if (f == NULL) {
        return KERR_BADF;
    }
    return fd_install(file_get(f));
}

void file_readiness_changed(struct file *f) {
    if (f != NULL) {
        f->poll_gen++;
    }
    readiness_generation++;
}

uint32_t file_readiness_generation(void) {
    return readiness_generation;
}

uint32_t file_poll(struct file *f) {
    if (f == NULL) {
        return EPOLLERR;
    }
    /* No `poll` means always ready, which is right for a console: a write
     * never blocks and a read is immediately at end of file. */
    return f->ops->poll == NULL ? (EPOLLIN | EPOLLOUT) : f->ops->poll(f);
}
