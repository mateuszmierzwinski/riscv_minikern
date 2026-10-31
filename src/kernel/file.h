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

#include "core/types.h"

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
#define EPOLLET (1u << 31)

/* `open`'s status flags, of which only these matter here. */
#define O_RDONLY 0u
/* The access mode is the low two bits, and it is a *field* rather than a
 * set of flags: `O_RDONLY` is zero, so testing for it with a bitwise and
 * always succeeds. The mask is named so that no caller has to remember
 * that. */
#define O_ACCMODE 3u
#define O_WRONLY 1u
#define O_RDWR 2u
/* The generic ABI's values, which riscv64 uses. */
#define O_CREAT 0100u
#define O_EXCL 0200u
#define O_TRUNC 01000u
#define O_APPEND 02000u
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
    /* A device request. NULL means this object is not a device, and the
     * syscall answers `ENOTTY` — which is what a program checks to find out
     * whether its output is a terminal, so answering 0 instead would make
     * every pipe claim to be one. `arg` is a user pointer and belongs to the
     * implementation to validate. */
    int32_t (*ioctl)(struct file *f, uint32_t request, virt_t arg);
    /* `pread64` and `pwrite64`: at `off`, leaving the descriptor's own
     * offset where it was. `buf` is a user pointer, as for `read`. NULL
     * means the object has no positions (a pipe, a socket): `ESPIPE`. */
    int32_t (*pread)(struct file *f, void *buf, uint32_t n, uint64_t off);
    int32_t (*pwrite)(struct file *f, const void *buf, uint32_t n, uint64_t off);
    /* The frame behind the page at `offset` of a device that is mapped by
     * sharing its own memory (`MAP_SHARED` of a framebuffer). KERR_INVAL past
     * the device's end. The frame stays the device's: a mapping takes a
     * share of it (`mem_share_page`) and gives the share back, never the
     * frame. NULL means the object cannot be mapped shared, and `mmap`
     * refuses `MAP_SHARED` of it. */
    int32_t (*map_page)(struct file *f, uint64_t offset, phys_t *phys);
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

struct uproc;

/* Closes every descriptor of the running process. */
void fd_reset(void);

/* Closes the ones marked close-on-exec, which is what `execve` does: the
 * table survives an exec, except for what a caller asked to be dropped. */
void fd_close_on_exec(void);

/* Gives `to` a copy of `from`'s table, sharing the files. Inheriting a
 * descriptor means sharing the file behind it — both processes then write to
 * the same console and read the same pipe. */
int fd_copy_table(struct uproc *to, const struct uproc *from);

/* Closes every descriptor `p` holds: what exiting does to its table. */
void fd_release_table(struct uproc *p);

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

/* Duplicates onto a **chosen** descriptor, closing whatever was there. This
 * is what puts a pipe on descriptor 1 before an `exec`, and it is the reason
 * `dup2`/`dup3` exist at all. */
int fd_dup_to(int32_t from, int32_t to);

/* Descriptor fd's close-on-exec flag, which is the descriptor's and not
 * its file's: a dup'd copy starts without it. */
bool fd_cloexec(int32_t fd);
void fd_set_cloexec(int32_t fd, bool on);

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
