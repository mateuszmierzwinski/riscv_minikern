#include "kernel/fdobjects.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/access.h"
#include "kernel/mem.h"
#include "kernel/sched.h"
#include "kernel/tty.h"
#include "kernel/uspace.h"
#include "kernel/xfsmount.h"

/* ── the console ────────────────────────────────────────────────────────── */

/* Reading the console is reading the **terminal** now: a line at a time,
 * blocking until one is typed, and refused with `SIGTTIN` for a process
 * that is not in the foreground group.
 *
 * It used to return 0 — end of file — because nothing was wired to the
 * input side, and the comment said so. That answer was true and is now
 * wrong, which is the usual fate of a stub that reports success. */
static int32_t console_file_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    return ktty_read(buf, n);
}

/* Ready when a line is waiting, so `poll` on the console means something.
 * `NULL` meant "always ready both ways", which for the read side was a lie
 * that a runtime acts on by reading and getting nothing. */
static uint32_t console_file_poll(struct file *f) {
    (void)f;
    /* Writable always; readable when a line is waiting. `NULL` here meant
     * "always ready for everything", which for the read side was a lie a
     * runtime acts on by reading and getting nothing. */
    return EPOLLOUT | (ktty_ready() ? EPOLLIN : 0u);
}

/* `ioctl`, for the two things a program asks a terminal.
 *
 * A program that cannot get a window size lays itself out for 0x0, and one
 * that cannot find the foreground group cannot do job control at all. The
 * rest of `ioctl` is refused rather than pretended: returning 0 for an
 * unknown request tells a caller its request was honoured. */
static int32_t console_file_ioctl(struct file *f, uint32_t request, virt_t arg) {
    struct user_winsize ws;
    uint32_t pgid;
    (void)f;
    switch (request) {
    case TIOCGWINSZ:
        ktty_window(&ws.rows, &ws.cols);
        ws.xpixel = 0;
        ws.ypixel = 0;
        return uspace_copy_out(arg, &ws, sizeof ws) == KERR_OK ? 0 : KERR_FAULT;
    case TIOCSWINSZ:
        if (uspace_copy_in(&ws, arg, sizeof ws) != KERR_OK) {
            return KERR_FAULT;
        }
        ktty_set_window(ws.rows, ws.cols);
        return 0;
    case TIOCGPGRP:
        pgid = ktty_foreground();
        return uspace_copy_out(arg, &pgid, sizeof pgid) == KERR_OK ? 0 : KERR_FAULT;
    case TIOCSPGRP:
        if (uspace_copy_in(&pgid, arg, sizeof pgid) != KERR_OK) {
            return KERR_FAULT;
        }
        ktty_set_foreground(pgid);
        return 0;
    default:
        return KERR_NOTTY;
    }
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
    .poll = console_file_poll,
    .ioctl = console_file_ioctl,
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

/* ── the few paths that exist ───────────────────────────────────────────── */

/* `/dev/null`: reads end immediately, writes vanish. */
static int32_t null_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    return 0;
}

static int32_t null_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    return (int32_t)n;
}

/* `/dev/zero`: reads give zeros, for ever. */
static int32_t zero_read(struct file *f, void *buf, uint32_t n) {
    uint8_t *to = buf;
    (void)f;
    for (uint32_t i = 0; i < n; i++) {
        to[i] = 0;
    }
    return (int32_t)n;
}

static const struct file_ops null_ops = {.read = null_read, .write = null_write};
static const struct file_ops zero_ops = {.read = zero_read, .write = null_write};

/* ── a regular file ─────────────────────────────────────────────────────── */

/* A file off the disk, read **whole** into the kernel's heap when it is
 * opened.
 *
 * Not how a real kernel does it, and the reason is the filesystem
 * underneath: `xfsmount` reads a file by name, not a block of one by
 * offset, so there is nothing to be lazy with yet. What this buys is the
 * two things that were missing — a descriptor a program can `read`, and a
 * mapping that can be backed from a file — and what it costs is the file's
 * size in kernel memory for as long as the descriptor is open. The limit is
 * stated rather than discovered: a file larger than it will not open.
 *
 * It is read-only. Writing one needs the filesystem's write path, which is
 * `ROADMAP.md` §3's business. */
#define DISKFILE_MAX_BYTES (16u * 1024u * 1024u)

struct diskfile {
    uint8_t *bytes;
    uint32_t size;
    uint32_t offset; /* how much of it this descriptor has handed over */
};

static struct diskfile diskfiles[FD_MAX];

static struct diskfile *diskfile_alloc(void) {
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (diskfiles[i].bytes == NULL) {
            return &diskfiles[i];
        }
    }
    return NULL;
}

static int32_t diskfile_read(struct file *f, void *buf, uint32_t n) {
    struct diskfile *d = f->priv;
    uint32_t left = d->size - d->offset;
    uint32_t take = n < left ? n : left;
    for (uint32_t i = 0; i < take; i++) {
        ((uint8_t *)buf)[i] = d->bytes[d->offset + i];
    }
    d->offset += take;
    return (int32_t)take;
}

static void diskfile_release_buffer(struct diskfile *d) {
    kfree_image(d->bytes);
    *d = (struct diskfile){0};
}

static void diskfile_release(struct file *f) {
    diskfile_release_buffer(f->priv);
}

static const struct file_ops diskfile_ops = {.read = diskfile_read, .release = diskfile_release};

/* The bytes behind a descriptor, for a mapping to be backed from — and
 * `KERR_NODEV` for a descriptor that is not a file, which is `mmap`'s
 * answer for a console or a socket. */
int fdobjects_file_bytes(int32_t fd, const uint8_t **bytes, uint32_t *size) {
    struct file *f = fd_get(fd);
    const struct diskfile *d;
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops != &diskfile_ops) {
        return KERR_NODEV;
    }
    d = f->priv;
    *bytes = d->bytes;
    *size = d->size;
    return KERR_OK;
}

struct file *fdobjects_file_of(int32_t fd) {
    struct file *f = fd_get(fd);
    return f != NULL && f->ops == &diskfile_ops ? f : NULL;
}

/* The same question asked of the object rather than a descriptor, which is
 * what a *mapping* has: it holds the file itself, precisely so that it does
 * not depend on a descriptor number that the program may have closed. */
int fdobjects_file_of_object(void *object, const uint8_t **bytes, uint32_t *size) {
    const struct file *f = object;
    const struct diskfile *d;
    if (f == NULL || f->ops != &diskfile_ops) {
        return KERR_NODEV;
    }
    d = f->priv;
    *bytes = d->bytes;
    *size = d->size;
    return KERR_OK;
}

/* Opens a regular file, if the caller may read it.
 *
 * **The permission rule's first caller.** `access_permitted` and
 * `uspace_may_access` were written with §1's identity model and nothing
 * asked them anything: there was no file to be denied. Opening one by name
 * is exactly where the owner/group/other bits mean something, and the mode
 * comes from the filesystem's own inode rather than from a guess. */
static int open_regular(const char *path, uint32_t flags) {
    uint32_t uid = 0, gid = 0, mode = 0;
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    struct diskfile *d;
    struct file *f;
    int rc = xfsmount_stat(path, &uid, &gid, &mode);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!uspace_may_access(uid, gid, mode, ACCESS_READ)) {
        return KERR_ACCES;
    }
    if ((flags & 3u) != O_RDONLY) {
        return KERR_ROFS; /* nothing here can write a file yet */
    }
    d = diskfile_alloc();
    rc = d == NULL ? KERR_NOMEM : xfsmount_read_file(path, DISKFILE_MAX_BYTES, &bytes, &size);
    if (rc != KERR_OK) {
        return rc;
    }
    d->bytes = bytes;
    d->size = size;
    d->offset = 0;
    f = file_new(&diskfile_ops, d, flags);
    if (f == NULL) {
        diskfile_release_buffer(d);
        return KERR_NOMEM;
    }
    return fd_install(f);
}

static bool same_path(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

int fdobjects_open_path(const char *path, uint32_t flags) {
    const struct file_ops *ops = NULL;
    struct file *f;
    if (same_path(path, "/dev/null")) {
        ops = &null_ops;
    } else if (same_path(path, "/dev/zero")) {
        ops = &zero_ops;
    } else if (same_path(path, "/dev/console") || same_path(path, "/dev/tty")) {
        ops = &console_ops;
    }
    if (ops == NULL) {
        /* Not one of the kernel's own objects, so it is a name on the
         * filesystem — or nothing at all, which the filesystem says. */
        return open_regular(path, flags);
    }
    f = file_new(ops, NULL, flags);
    return f == NULL ? KERR_NOMEM : fd_install(f);
}

/* ── a pipe ─────────────────────────────────────────────────────────────── */

/* A ring, shared by the two ends, and a count of how many ends are still
 * open — which is what makes a read return end-of-file rather than blocking
 * for ever once the writer has gone, and what makes a write fail rather than
 * filling a buffer nobody will drain.
 *
 * 4 KiB because that is the capacity a program may assume; it is also what
 * makes a report from a failed `exec` fit in one write. */
#define PIPE_BYTES 4096u

struct pipe {
    uint8_t data[PIPE_BYTES];
    uint32_t head;
    uint32_t count;
    bool reader_open;
    bool writer_open;
};

static uint32_t pipe_used(const struct pipe *pp) {
    return pp->count;
}

static int32_t pipe_read(struct file *f, void *buf, uint32_t n) {
    struct pipe *pp = f->priv;
    uint8_t *to = buf;
    uint32_t took = 0;
    if (pipe_used(pp) == 0) {
        /* **End of file only when the writer has gone.** An empty pipe with
         * a live writer is "not yet", which is `EAGAIN` — reporting 0 there
         * would tell the reader the conversation was over. */
        return pp->writer_open ? KERR_AGAIN : 0;
    }
    while (took < n && pp->count > 0) {
        to[took++] = pp->data[pp->head];
        pp->head = (pp->head + 1u) % PIPE_BYTES;
        pp->count--;
    }
    file_readiness_changed(f);
    (void)sched_futex_wake(SCHED_POLLER_KEY, 64u);
    return (int32_t)took;
}

static int32_t pipe_write(struct file *f, const void *buf, uint32_t n) {
    struct pipe *pp = f->priv;
    const uint8_t *from = buf;
    uint32_t put = 0;
    if (!pp->reader_open) {
        /* Writing to a pipe nobody will read. Linux also raises SIGPIPE;
         * the error alone is what a runtime checks. */
        return KERR_PIPE;
    }
    while (put < n && pp->count < PIPE_BYTES) {
        pp->data[(pp->head + pp->count) % PIPE_BYTES] = from[put++];
        pp->count++;
    }
    if (put == 0) {
        return KERR_AGAIN; /* full: a short write of zero is a would-block */
    }
    file_readiness_changed(f);
    (void)sched_futex_wake(SCHED_POLLER_KEY, 64u);
    return (int32_t)put;
}

static uint32_t pipe_read_poll(struct file *f) {
    const struct pipe *pp = f->priv;
    uint32_t ready = pipe_used(pp) != 0 ? EPOLLIN : 0u;
    /* A closed writer makes the end readable *and* hung up: a reader must
     * wake to see the end of the stream, not wait for data that cannot
     * come. */
    return ready | (pp->writer_open ? 0u : (EPOLLIN | EPOLLHUP));
}

static uint32_t pipe_write_poll(struct file *f) {
    const struct pipe *pp = f->priv;
    uint32_t ready = pipe_used(pp) < PIPE_BYTES ? EPOLLOUT : 0u;
    return ready | (pp->reader_open ? 0u : EPOLLERR);
}

/* The ring is freed when **both** ends have gone, which is why each end's
 * release only clears its own flag. */
static void pipe_release_read(struct file *f) {
    struct pipe *pp = f->priv;
    pp->reader_open = false;
    if (!pp->writer_open) {
        kfree(pp);
    }
}

static void pipe_release_write(struct file *f) {
    struct pipe *pp = f->priv;
    pp->writer_open = false;
    if (!pp->reader_open) {
        kfree(pp);
    }
}

static const struct file_ops pipe_read_ops = {
    .read = pipe_read,
    .poll = pipe_read_poll,
    .release = pipe_release_read,
};

static const struct file_ops pipe_write_ops = {
    .write = pipe_write,
    .poll = pipe_write_poll,
    .release = pipe_release_write,
};

int fdobjects_open_pipe(int32_t out[2], uint32_t flags) {
    struct pipe *pp = kzalloc(sizeof *pp);
    struct file *r;
    struct file *w;
    if (pp == NULL) {
        return KERR_NOMEM;
    }
    pp->reader_open = true;
    pp->writer_open = true;
    r = file_new(&pipe_read_ops, pp, flags);
    w = file_new(&pipe_write_ops, pp, flags);
    if (r == NULL || w == NULL) {
        file_put(r);
        file_put(w);
        kfree(pp);
        return KERR_NOMEM;
    }
    out[0] = (int32_t)fd_install(r);
    out[1] = (int32_t)fd_install(w);
    if (out[0] < 0 || out[1] < 0) {
        (void)fd_close(out[0]);
        (void)fd_close(out[1]);
        return KERR_MFILE;
    }
    return KERR_OK;
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
