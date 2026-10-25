/* klog — one kernel log, timestamped, kept, and readable afterwards.
 *
 * **Why this is not just a prefix on `kprintf`.** A kernel's log is the only
 * account of what happened before anything else existed: before the
 * filesystem, before a process, often before the real console. Three things
 * follow, and each is a reason this is a module rather than a format string.
 *
 * It has to be **kept in memory until there is somewhere to put it**,
 * because the interesting part of a boot is over before a disk is mounted.
 * It has to be **readable by a program**, or the only way to see it is to
 * be watching the serial port at the time. And it has to carry a **time and
 * a level**, because "what order did these happen in" and "which of these
 * is a problem" are the first two questions anybody asks of a log and
 * neither can be reconstructed afterwards.
 *
 * The format is one line:
 *
 *     [HH:MM:SS.mmm] L message
 *
 * The time is since the kernel started, not a wall clock — there is no
 * battery-backed clock on this board and a made-up date would be worse than
 * an elapsed one. `L` is a single letter so that a line stays scannable:
 * **N**otice, **I**nfo, **W**arning, **E**rror, **D**ebug.
 *
 * **Every existing `kprintf` becomes an info line**, which is the point of
 * calling it unified: a log where half the output is timestamped and half is
 * not cannot be read in order, and converting hundreds of call sites by
 * hand would be a worse way to get there than routing the one function they
 * all go through.
 */
#ifndef MINIKERN_CORE_KLOG_H
#define MINIKERN_CORE_KLOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum klog_level {
    KLOG_NOTICE = 0,
    KLOG_INFO = 1,
    KLOG_WARN = 2,
    KLOG_ERROR = 3,
    KLOG_DEBUG = 4,
};

/* How much of the boot is kept in memory.
 *
 * 64 KiB is about eight hundred lines, which covers every boot this kernel
 * has produced with room to spare — and when it does not, the **oldest**
 * lines are dropped rather than the newest, because a log that stops
 * recording at the moment things go wrong is the one shape of log that is
 * no use at all. */
#define KLOG_BYTES 65536u

/* How many lines the live tail keeps, for `/dev/virtual/kernel_logger`.
 * Thirty is what fits on a screen, which is what the number is for. */
#define KLOG_TAIL_LINES 30u

/* How many boots are kept on disk, as `kernel.0.log` .. `kernel.9.log`.
 * Ten is enough to compare a failure against the last time it worked, which
 * is what these are actually used for. */
#define KLOG_KEEP_BOOTS 10u

/* The longest line the tail will hold whole. A longer one is kept truncated
 * rather than dropped: the beginning of a line says what it is about. */
#define KLOG_LINE_MAX 200u

/* Where the elapsed time comes from.
 *
 * Given rather than called directly, because this module is in `core/` and
 * the counter is the architecture's — and because **before the timer is
 * running there is no time to report**. Until this is set every line reads
 * `[00:00:00.000]`, which is honest: those lines happened before the
 * kernel could tell how long it had been alive. */
void klog_time_source(uint64_t (*ns_since_boot)(void));

/* Writes one formatted entry. The prefix is emitted at the start of each
 * line, so a message built by several calls gets one timestamp rather than
 * one per fragment. */
void klog(enum klog_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void klog_vwrite(enum klog_level level, const char *fmt, va_list ap);

/* The whole kept log, oldest first, copied into `out`. Returns how many
 * bytes were written. Used to put the boot on disk once there is a disk. */
uint32_t klog_all(char *out, uint32_t room);

/* The last `KLOG_TAIL_LINES` lines, oldest first, newline-separated. This is
 * what `/dev/virtual/kernel_logger` serves. */
uint32_t klog_tail(char *out, uint32_t room);

/* How many bytes have been dropped from the front of the kept log, and how
 * many entries have been written at all.
 *
 * Reported rather than hidden: a reader who cannot tell a complete log from
 * a truncated one will read the first line as the first thing that
 * happened. */
uint32_t klog_dropped(void);
uint32_t klog_entries(void);

/* Formats an elapsed time the way a line's prefix carries it. Exposed for
 * the tests, because the rollover cases — an hour, a day — are the ones
 * worth asserting and they take too long to reach by waiting. */
void klog_format_time(uint64_t ns, char *out, uint32_t room);

/* The letter a level is written as. */
char klog_letter(enum klog_level level);

#endif /* MINIKERN_CORE_KLOG_H */
