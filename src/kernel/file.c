#include "kernel/file.h"

#include "kernel/uspace.h"

#include <stddef.h>

#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/uspace.h"

/* The running process's table, which **lives in the process**: two
 * processes with one descriptor table would see each other's files, and the
 * first thing a `fork` does is give the child a copy of its parent's.
 *
 * A NULL process — the kernel between programs — has no descriptors, and
 * every operation on one fails rather than touching another process's. */
static struct file **table_of_current(void) {
    struct uproc *p = uspace_current();
    return p == NULL ? NULL : p->fds;
}

/* Sets or clears descriptor fd's close-on-exec bit. */
static void mark_cloexec(int32_t fd, bool on) {
    struct uproc *p = uspace_current();
    uint64_t bit;
    if (p == NULL || fd < 0 || (uint32_t)fd >= FD_MAX) {
        return;
    }
    bit = 1ull << (uint32_t)fd;
    p->fd_cloexec = on ? (p->fd_cloexec | bit) : (p->fd_cloexec & ~bit);
}

bool fd_cloexec(int32_t fd) {
    struct uproc *p = uspace_current();
    return p != NULL && fd >= 0 && (uint32_t)fd < FD_MAX && (p->fd_cloexec >> (uint32_t)fd & 1u) != 0;
}

void fd_set_cloexec(int32_t fd, bool on) {
    mark_cloexec(fd, on);
}

/* Bumped by every readiness change anywhere. An epoll waiter reads it before
 * it sleeps and again when it wakes: unchanged means nothing could have
 * become ready, so there is nothing to re-scan. One counter rather than one
 * per file because a waiter watches a *set*. */
static uint32_t readiness_generation;

void fd_reset(void) {
    struct file **table = table_of_current();
    if (table == NULL) {
        return;
    }
    for (uint32_t i = 0; i < FD_MAX; i++) {
        struct file *old = table[i];
        table[i] = NULL;
        file_put(old);
    }
}

void fd_close_on_exec(void) {
    struct file **table = table_of_current();
    if (table == NULL) {
        return;
    }
    /* `execve` keeps the descriptor table, **except** what was marked
     * close-on-exec: that is the whole point of the flag, and it is how a
     * shell stops a child inheriting the pipe ends it should not have. */
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (table[i] != NULL && fd_cloexec((int32_t)i)) {
            file_put(table[i]);
            table[i] = NULL;
        }
    }
    uspace_current()->fd_cloexec = 0;
}

void fd_release_table(struct uproc *p) {
    for (uint32_t i = 0; p != NULL && i < FD_MAX; i++) {
        if (p->fds[i] != NULL) {
            struct file *f = p->fds[i];
            p->fds[i] = NULL;
            file_put(f);
        }
    }
    if (p != NULL) {
        p->fd_cloexec = 0;
    }
}

int fd_copy_table(struct uproc *to, const struct uproc *from) {
    for (uint32_t i = 0; i < FD_MAX; i++) {
        /* A copy shares the **files**, not the descriptors: both processes
         * then write to the same console and read the same pipe, which is
         * what inheriting a descriptor means. */
        to->fds[i] = from->fds[i] != NULL ? file_get(from->fds[i]) : NULL;
    }
    to->fd_cloexec = from->fd_cloexec;
    return KERR_OK;
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
    struct file **table = table_of_current();
    if (f == NULL) {
        return KERR_NOMEM;
    }
    if (table == NULL) {
        file_put(f);
        return KERR_BADF;
    }
    /* The **lowest** free descriptor, which is what the ABI promises and
     * what a program relies on when it dups onto 0, 1 or 2.
     *
     * Bounded by `RLIMIT_NOFILE` as well as by the table, which is what
     * makes that limit a limit: it read as a number and held nobody to it
     * until this line. A descriptor at or above the soft limit is `EMFILE`,
     * the same answer a full table gives, because to the caller they are
     * the same situation. */
    uint64_t limit = uspace_rlimit_nofile();
    for (uint32_t i = 0; i < FD_MAX && i < limit; i++) {
        if (table[i] == NULL) {
            table[i] = f;
            /* A new object brings the flag it was opened with; the flag
             * then lives on the descriptor, not on the shared file. */
            mark_cloexec((int32_t)i, (f->flags & O_CLOEXEC) != 0);
            f->flags &= ~O_CLOEXEC;
            return (int)i;
        }
    }
    file_put(f);
    return KERR_MFILE;
}

struct file *fd_get(int32_t fd) {
    struct file **table = table_of_current();
    return table != NULL && fd >= 0 && (uint32_t)fd < FD_MAX ? table[fd] : NULL;
}

int fd_close(int32_t fd) {
    struct file **table = table_of_current();
    struct file *f = fd_get(fd);
    if (f == NULL || table == NULL) {
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
    return fd_install(file_get(f)); /* the copy is not close-on-exec */
}

int fd_dup_to(int32_t from, int32_t to) {
    struct file **table = table_of_current();
    struct file *f = fd_get(from);
    if (f == NULL || table == NULL) {
        return KERR_BADF;
    }
    if (to < 0 || (uint32_t)to >= FD_MAX) {
        return KERR_BADF;
    }
    if (from == to) {
        return to; /* `dup3` refuses this; `dup2` returns it unchanged */
    }
    /* Whatever was there is closed first, silently — that is what makes
     * `dup2` the tool for putting a pipe on descriptor 1. */
    {
        struct file *old = table[to];
        table[to] = file_get(f); /* replaced first: a release that sleeps must not find it */
        file_put(old);
    }
    mark_cloexec(to, false); /* `dup3` sets it afterwards when asked */
    return to;
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
