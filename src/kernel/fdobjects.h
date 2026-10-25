/* The descriptor kinds this kernel can open, besides a socket.
 *
 * Three of them, and each exists for a reason a program forced:
 *
 *   - the **console**, as descriptors 0, 1 and 2, because a runtime checks
 *     all three before it does anything else and aborts if any is missing;
 *   - an **eventfd**, because a runtime interrupts its own poller with one;
 *   - an **epoll** instance, because a runtime's timers go through the
 *     netpoller and its network code waits on readiness.
 *
 * They were a `switch` on the descriptor number until there were sockets to
 * wait on. Now they are objects like anything else.
 */
#ifndef MINIKERN_KERNEL_FDOBJECTS_H
#define MINIKERN_KERNEL_FDOBJECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/file.h"

/* Installs the console on 0, 1 and 2. Called once per process, before it
 * runs: a program may assume they exist. */
int fdobjects_open_console(void);

/* An eventfd with an initial count. */
int fdobjects_open_eventfd(uint64_t initial, uint32_t flags);

/* An epoll instance. */
int fdobjects_open_epoll(uint32_t flags);

/* Opens one of the few paths a process can name.
 *
 * There is no filesystem in a process's view of the world — the kernel reads
 * its programs off the disk, but a process has no path namespace — so this
 * is a short list of **synthesised** files rather than a lookup. The list is
 * short because it is demand-driven: each entry is here because a program
 * asked for it and could not proceed without it.
 *
 * `/dev/null` is the one that matters: a runtime opens it for any command
 * whose input or output it was not given, so `os/exec` cannot start a
 * program without it.
 *
 * KERR_NOENT for anything else, which is the right answer and one a runtime
 * handles — it is what a probe for an optional file expects. */
/* Fills `buf` with `struct linux_dirent64` records from an open directory,
 * returning the bytes written or -errno. A short answer with room left means
 * the directory is finished; a full buffer means there is more, and the next
 * call resumes where this one stopped. */
int32_t fdobjects_getdents(struct file *f, void *buf, uint32_t room);

int fdobjects_open_path(const char *path, uint32_t flags);

/* The bytes behind a descriptor that is a regular file, for a mapping to be
 * backed from. KERR_BADF for a descriptor that is not open, KERR_NODEV for
 * one that is not a file — which is what `mmap` answers for a console or a
 * socket. */
int fdobjects_file_bytes(int32_t fd, const uint8_t **bytes, uint32_t *size);

/* The file object itself, or NULL when that descriptor is not a regular
 * file. A mapping holds a reference to it, so that closing the descriptor
 * does not take the bytes out from under the mapping — which is the whole
 * of why `mmap` does not need the file to stay open. */
struct file *fdobjects_file_of(int32_t fd);

/* The bytes behind a file *object*, which is what a mapping holds — it
 * keeps the file rather than a descriptor number, so that a program closing
 * the descriptor does not disturb the mapping. */
int fdobjects_file_of_object(void *object, const uint8_t **bytes, uint32_t *size);

/* A pipe: two descriptors, the read end first, which is the order `pipe2`
 * writes them in. This is how a program started by another reports that it
 * could not start — the only way, in fact, because by then it has no
 * descriptors of its own to speak through. */
int fdobjects_open_pipe(int32_t out[2], uint32_t flags);

/* The three `whence` values, which are the ABI's and not ours to choose. */
#define SEEK_SET 0u
#define SEEK_CUR 1u
#define SEEK_END 2u

/* Moves a descriptor's position and reports where it landed.
 *
 * `KERR_SPIPE` for anything without a position — a console, a pipe, a
 * socket — which is what a program tests to find out whether it may seek at
 * all, so answering it correctly matters as much as seeking does. A result
 * that would be negative is refused rather than clamped: clamping would let
 * a program that computed an offset wrongly read plausible data from the
 * wrong place. */
int fdobjects_seek(int32_t fd, int64_t offset, uint32_t whence, uint64_t *result);

/* The same two operations a program gets from `lseek` and `read`, for the
 * board self-test, which runs inside the kernel and has no syscall layer to
 * go through. The seek returns its result signed so a caller can compare it
 * against `KERR_SPIPE` directly. */
/* The path a descriptor was opened from, or NULL when it does not remember
 * one — which is every descriptor opened for reading, because those hold the
 * file's bytes rather than its name. `ftruncate` depends on this and names
 * the limit. */
const char *fdobjects_path_of(int32_t fd);

int64_t fdobjects_seek_for_test(int32_t fd, int64_t offset, uint32_t whence);
int32_t fdobjects_read_for_test(int32_t fd, void *buf, uint32_t n);

/* One registration in an epoll instance, as user space passes it.
 *
 * **Not packed.** `struct epoll_event` carries `__attribute__((packed))` in
 * the kernel's headers only under `#ifdef __x86_64__`; everywhere else it is
 * an ordinary struct, so a 4-byte event mask is followed by **4 bytes of
 * padding** and then the 8-byte token. 16 bytes, not 12.
 *
 * This was written packed first, with a comment asserting that the ABI packs
 * it — which is true of exactly one architecture and not this one. The token
 * is the pointer a runtime identifies the descriptor by, so writing it four
 * bytes early meant the runtime dereferenced a value made of half a pointer
 * and half a flag word: a segmentation fault inside `runtime.netpoll`, which
 * is a confident-looking crash a long way from the mistake.
 *
 * Verified against what the runtime declares (`EpollEvent` for this
 * architecture: `Events uint32`, four bytes of padding, `Data [8]byte`)
 * rather than against the architecture this kernel was ported from. */
struct user_epoll_event {
    uint32_t events;
    uint32_t padding;
    uint64_t data;
};

_Static_assert(sizeof(struct user_epoll_event) == 16u, "epoll_event is 16 bytes here");
_Static_assert(__builtin_offsetof(struct user_epoll_event, data) == 8u,
               "the token is at offset 8: packed only on x86-64");

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* `epoll_ctl`. KERR_BADF for a descriptor that is not open, KERR_EXIST for
 * adding one twice, KERR_NOENT for modifying one that is not there. */
int fdobjects_epoll_ctl(int32_t epfd, int32_t op, int32_t fd,
                        const struct user_epoll_event *event);

/* Collects whatever is ready, up to `max`. Reports how many — 0 means
 * nothing is ready now, which is not an error. */
int fdobjects_epoll_collect(int32_t epfd, struct user_epoll_event *out, uint32_t max);

/* Whether anything registered in this instance is ready, without collecting
 * — for a waiter deciding whether to sleep. */
bool fdobjects_epoll_has_events(int32_t epfd);

#endif
