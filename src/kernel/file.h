/* file — descriptors, and what they can do.
 *
 * Until now this kernel had five *pseudo*-descriptors: three for the console
 * and two the netpoller asked for, each recognised by its number in a
 * `switch`. That was honest while nothing else could be opened, and it stops
 * being honest the moment a socket exists — a socket is a descriptor whose
 * readiness changes, and a number in a `switch` cannot be waited on.
 *
 * So: a table of descriptors, each pointing at an object with operations.
 * The set is deliberately small — read, write, poll, close — because that is
 * what the things this kernel has can do. There is no `seek`, because
 * nothing here is seekable; adding one later is a line in a struct rather
 * than a change of shape.
 *
 * **`poll` is the interesting one.** It reports what an object could do
 * *right now*, and it is what makes a real `epoll` possible: a runtime asks
 * the kernel to tell it when a socket becomes readable, and the only
 * honest way to answer is to ask the socket. A kernel that reports "no
 * events" for ever is one on which a network program waits for ever.
 */
#ifndef MINIKERN_KERNEL_FILE_H
#define MINIKERN_KERNEL_FILE_H

#include <stdbool.h>
#include <stdint.h>

/* How many descriptors a process may hold. Go opens a handful per
 * connection; 64 is enough for the programs this kernel runs and small
 * enough that the table is scanned without thinking about it. */
#define FD_MAX 64u

/* epoll's readiness bits, which are Linux's values because they cross into
 * user space. */
#define EPOLLIN 0x001u
#define EPOLLPRI 0x002u
#define EPOLLOUT 0x004u
#define EPOLLERR 0x008u
#define EPOLLHUP 0x010u
#define EPOLLRDHUP 0x2000u

/* `open`'s status flags, of which only these matter here. */
#define O_RDONLY 0u
#define O_WRONLY 1u
#define O_RDWR 2u
#define O_NONBLOCK 0x800u
#define O_CLOEXEC 0x80000u

struct file;

struct file_ops {
    /* Negative returns are errno. A read of 0 means end of file; a
     * would-block is KERR_AGAIN, which is what a non-blocking caller
     * expects and what the netpoller is built around. */
    int32_t (*read)(struct file *f, void *buf, uint32_t n);
    int32_t (*write)(struct file *f, const void *buf, uint32_t n);
    /* What this object could do without blocking, as EPOLL* bits. NULL
     * means "always ready for everything", which is right for a console. */
    uint32_t (*poll)(struct file *f);
    /* Called when the last reference goes. NULL when there is nothing to
     * undo. */
    void (*release)(struct file *f);
};

struct file {
    const struct file_ops *ops;
    uint32_t refs;
    uint32_t flags;   /* O_NONBLOCK and friends */
    void *priv;       /* the object this descriptor is of */
    /* Bumped whenever readiness changes. An epoll waiter watches it so that
     * it can sleep rather than poll: a generation that has not moved means
     * nothing can have become ready. */
    uint32_t poll_gen;
};

/* Empties the table, for a process starting. */
void fd_reset(void);

/* Takes a file and gives it the lowest free descriptor. The file's reference
 * is transferred. Negative on failure (KERR_MFILE when the table is full). */
int fd_install(struct file *f);

/* The file a descriptor names, or NULL. Does not take a reference: the
 * caller is inside a syscall and holds the kernel lock, so the table cannot
 * change underneath it. */
struct file *fd_get(int32_t fd);

/* Closes a descriptor. KERR_BADF when it was not open. */
int fd_close(int32_t fd);

/* Duplicates a descriptor into the lowest free one, for `dup`. */
int fd_dup(int32_t fd);

/* Allocates a file with `ops` and `priv`. NULL when memory runs out. */
struct file *file_new(const struct file_ops *ops, void *priv, uint32_t flags);

struct file *file_get(struct file *f);
void file_put(struct file *f);

/* Says that a file's readiness may have changed, so anything waiting on it
 * looks again. Called by whatever changed it — a packet arriving, a
 * connection completing — and **not** by the waiter, which is the whole
 * point: a waiter that polls cannot sleep. */
void file_readiness_changed(struct file *f);

/* The generation counter across every file, which an epoll waiter can
 * watch: if it has not moved, nothing has become ready. */
uint32_t file_readiness_generation(void);

/* What a file can do now, honouring a NULL `poll` as "ready". */
uint32_t file_poll(struct file *f);

#endif
