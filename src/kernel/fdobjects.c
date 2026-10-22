#include "kernel/fdobjects.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/sched.h"

/* ── the console ────────────────────────────────────────────────────────── */

/* Reading from the console is end of file. There is no input device wired to
 * a process yet, and 0 is a true answer a runtime handles; an error is not. */
static int32_t console_file_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    return 0;
}

/* Named for the object rather than the operation, because `console_write`
 * is already the console port's own function and this is the file layer's
 * adapter onto it. */
static int32_t console_file_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    console_write(buf, (size_t)n);
    return (int32_t)n;
}

static const struct file_ops console_ops = {
    .read = console_file_read,
    .write = console_file_write,
    .poll = NULL, /* always ready both ways */
};

int fdobjects_open_console(void) {
    for (uint32_t i = 0; i < 3u; i++) {
        struct file *f = file_new(&console_ops, NULL, 0);
        int fd = fd_install(f);
        if (fd < 0) {
            return fd;
        }
    }
    return KERR_OK;
}

/* ── the eventfd ────────────────────────────────────────────────────────── */

/* A counter, and the semantics are the counter: a write adds, a read takes
 * it all and leaves zero, and a read of zero is a would-block rather than
 * end of file — a reader that got 0 bytes would take it for end of file and
 * stop polling. */
struct eventfd {
    uint64_t count;
};

static int32_t eventfd_read(struct file *f, void *buf, uint32_t n) {
    struct eventfd *e = f->priv;
    if (n < 8u) {
        return KERR_INVAL;
    }
    if (e->count == 0) {
        return KERR_AGAIN;
    }
    *(uint64_t *)buf = e->count;
    e->count = 0;
    file_readiness_changed(f);
    return 8;
}

static int32_t eventfd_write(struct file *f, const void *buf, uint32_t n) {
    struct eventfd *e = f->priv;
    if (n != 8u) {
        return KERR_INVAL;
    }
    e->count += *(const uint64_t *)buf;
    /* **The point of the whole object**: waking whoever is waiting. A
     * runtime writes here to interrupt its own poller, and a kernel that
     * only stores the number leaves the poller asleep until its original
     * timeout — which turned a 50 ms sleep into 950 ms. */
    file_readiness_changed(f);
    (void)sched_futex_wake(SCHED_POLLER_KEY, 64u);
    return 8;
}

static uint32_t eventfd_poll(struct file *f) {
    const struct eventfd *e = f->priv;
    return EPOLLOUT | (e->count != 0 ? EPOLLIN : 0u);
}

static void eventfd_release(struct file *f) {
    kfree(f->priv);
}

static const struct file_ops eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
    .release = eventfd_release,
};

int fdobjects_open_eventfd(uint64_t initial, uint32_t flags) {
    struct eventfd *e = kzalloc(sizeof *e);
    struct file *f;
    if (e == NULL) {
        return KERR_NOMEM;
    }
    e->count = initial;
    f = file_new(&eventfd_ops, e, flags);
    if (f == NULL) {
        kfree(e);
        return KERR_NOMEM;
    }
    return fd_install(f);
}

/* ── the epoll instance ─────────────────────────────────────────────────── */

#define EPOLL_WATCHED 32u

struct epoll_watch {
    int32_t fd;
    bool used;
    uint32_t events; /* what the caller is interested in */
    uint64_t data;   /* the caller's token, returned unchanged */
};

struct epoll {
    struct epoll_watch watch[EPOLL_WATCHED];
};

static void epoll_release(struct file *f) {
    kfree(f->priv);
}

/* An epoll instance is itself pollable — a program may put one inside
 * another — and it is readable when anything it watches is ready. */
static uint32_t epoll_poll(struct file *f) {
    const struct epoll *ep = f->priv;
    for (uint32_t i = 0; i < EPOLL_WATCHED; i++) {
        struct file *watched;
        if (!ep->watch[i].used) {
            continue;
        }
        watched = fd_get(ep->watch[i].fd);
        if (watched != NULL && (file_poll(watched) & ep->watch[i].events) != 0) {
            return EPOLLIN;
        }
    }
    return 0;
}

static const struct file_ops epoll_ops = {
    .poll = epoll_poll,
    .release = epoll_release,
};

int fdobjects_open_epoll(uint32_t flags) {
    struct epoll *ep = kzalloc(sizeof *ep);
    struct file *f;
    if (ep == NULL) {
        return KERR_NOMEM;
    }
    f = file_new(&epoll_ops, ep, flags);
    if (f == NULL) {
        kfree(ep);
        return KERR_NOMEM;
    }
    return fd_install(f);
}

/* The instance a descriptor names, or NULL when it is not one. */
static struct epoll *epoll_of(int32_t epfd) {
    struct file *f = fd_get(epfd);
    return f != NULL && f->ops == &epoll_ops ? f->priv : NULL;
}

static struct epoll_watch *find_watch(struct epoll *ep, int32_t fd) {
    for (uint32_t i = 0; i < EPOLL_WATCHED; i++) {
        if (ep->watch[i].used && ep->watch[i].fd == fd) {
            return &ep->watch[i];
        }
    }
    return NULL;
}

int fdobjects_epoll_ctl(int32_t epfd, int32_t op, int32_t fd,
                        const struct user_epoll_event *event) {
    struct epoll *ep = epoll_of(epfd);
    struct epoll_watch *w;
    if (ep == NULL) {
        return KERR_BADF;
    }
    if (fd_get(fd) == NULL) {
        return KERR_BADF;
    }
    w = find_watch(ep, fd);
    switch (op) {
    case EPOLL_CTL_DEL:
        if (w == NULL) {
            return KERR_NOENT;
        }
        w->used = false;
        return KERR_OK;
    case EPOLL_CTL_MOD:
        if (w == NULL) {
            return KERR_NOENT;
        }
        break;
    case EPOLL_CTL_ADD:
        if (w != NULL) {
            return KERR_EXIST;
        }
        for (uint32_t i = 0; i < EPOLL_WATCHED && w == NULL; i++) {
            w = ep->watch[i].used ? NULL : &ep->watch[i];
        }
        if (w == NULL) {
            return KERR_NOSPC;
        }
        break;
    default:
        return KERR_INVAL;
    }
    if (event == NULL) {
        return KERR_FAULT;
    }
    /* **`EPOLLERR` and `EPOLLHUP` are always reported**, whether or not they
     * were asked for — that is the interface's rule, and a runtime relies on
     * it to notice a connection that failed rather than one that became
     * readable. */
    *w = (struct epoll_watch){.fd = fd,
                              .used = true,
                              .events = event->events | EPOLLERR | EPOLLHUP,
                              .data = event->data};
    return KERR_OK;
}

int fdobjects_epoll_collect(int32_t epfd, struct user_epoll_event *out, uint32_t max) {
    struct epoll *ep = epoll_of(epfd);
    uint32_t found = 0;
    if (ep == NULL) {
        return KERR_BADF;
    }
    for (uint32_t i = 0; i < EPOLL_WATCHED && found < max; i++) {
        struct file *watched;
        uint32_t ready;
        if (!ep->watch[i].used) {
            continue;
        }
        watched = fd_get(ep->watch[i].fd);
        if (watched == NULL) {
            /* The descriptor was closed without being removed, which is a
             * program's mistake rather than a kernel's: drop the watch
             * rather than report on a file that is gone. */
            ep->watch[i].used = false;
            continue;
        }
        ready = file_poll(watched) & ep->watch[i].events;
        if (ready != 0) {
            out[found].events = ready;
            out[found].padding = 0;
            out[found].data = ep->watch[i].data;
            found++;
        }
    }
    return (int)found;
}

bool fdobjects_epoll_has_events(int32_t epfd) {
    struct file *f = fd_get(epfd);
    return f != NULL && f->ops == &epoll_ops && epoll_poll(f) != 0;
}
