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
int fdobjects_open_path(const char *path, uint32_t flags);

/* A pipe: two descriptors, the read end first, which is the order `pipe2`
 * writes them in. This is how a program started by another reports that it
 * could not start — the only way, in fact, because by then it has no
 * descriptors of its own to speak through. */
int fdobjects_open_pipe(int32_t out[2], uint32_t flags);

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
