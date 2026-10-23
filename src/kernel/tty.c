#include "kernel/tty.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "arch/riscv/timer.h"
#include "core/tty.h"
#include "kernel/sched.h"
#include "kernel/uspace.h"

/* The signals a terminal raises. `SIGINT` and `SIGTSTP` on the keys, and
 * `SIGTTIN` at a reader that is not in the foreground. */
#define SIGINT 2
#define SIGTSTP 20
#define SIGTTIN 21

/* How long a blocked reader waits before looking again. Short enough that a
 * lost wake costs a keystroke's delay rather than a hang, long enough to be
 * free: 50 ms is five housekeeping beats. */
#define TTY_WAIT_BACKSTOP_NS 50000000ull

static struct {
    struct tty line;
    struct console_input_port input;
    uint32_t session_pid;
    uint32_t foreground; /* the process group that owns the keyboard */
    uint16_t rows;
    uint16_t cols;
    bool up;
} term;

void ktty_init(const struct console_input_port *input, uint32_t session_pid) {
    tty_init(&term.line);
    term.input = input != NULL ? *input : (struct console_input_port){0};
    term.session_pid = session_pid;
    term.foreground = 0;
    /* The convention when nothing knows better. A program that asks and is
     * told 0x0 lays itself out for a window with no room in it. */
    term.rows = 24u;
    term.cols = 80u;
    term.up = input != NULL && input->read != NULL;
}

uint32_t ktty_foreground(void) {
    return term.foreground;
}

void ktty_set_foreground(uint32_t pgid) {
    term.foreground = pgid;
}

void ktty_window(uint16_t *rows, uint16_t *cols) {
    if (rows != NULL) {
        *rows = term.rows;
    }
    if (cols != NULL) {
        *cols = term.cols;
    }
}

void ktty_set_window(uint16_t rows, uint16_t cols) {
    term.rows = rows;
    term.cols = cols;
}

bool ktty_ready(void) {
    return tty_has_line(&term.line);
}

/* A signal from a key, to the group that owns the keyboard.
 *
 * To the **group**, which is the whole point of a controlling terminal: a
 * pipeline is several processes and `^C` has to reach all of them. Reaching
 * one leaves the others running with their input gone, which is worse than
 * reaching none because it looks like the key worked. */
static void raise_on_foreground(uint32_t signo) {
    if (term.foreground == 0) {
        return;
    }
    (void)uspace_signal_group(term.foreground, signo);
}

static void echo(const uint8_t *bytes, uint32_t n) {
    if (n != 0) {
        console_write((const char *)bytes, (size_t)n);
    }
}

void ktty_poll(void) {
    uint8_t byte = 0;
    if (!term.up) {
        return;
    }
    /* Everything that has arrived, not one byte: a paste is a burst, and
     * taking one byte a beat would deliver a pasted line over seconds. The
     * bound is the line buffer, which the discipline enforces itself. */
    for (uint32_t i = 0; i < TTY_LINE_MAX; i++) {
        uint8_t echo_out[4];
        uint32_t echo_len = 0;
        if (term.input.read(term.input.self, &byte, 1u) != 1) {
            return;
        }
        switch (tty_input(&term.line, byte, echo_out, &echo_len)) {
        case TTY_SIGNAL_INTR:
            echo(echo_out, echo_len);
            raise_on_foreground(SIGINT);
            break;
        case TTY_SIGNAL_SUSP:
            echo(echo_out, echo_len);
            raise_on_foreground(SIGTSTP);
            break;
        case TTY_LINE_READY:
            echo(echo_out, echo_len);
            /* Whoever is blocked in `ktty_read` is waiting on the terminal
             * itself as a channel. */
            (void)sched_wake_on(&term, SCHED_MAX_THREADS);
            break;
        case TTY_END_OF_FILE:
            (void)sched_wake_on(&term, SCHED_MAX_THREADS);
            break;
        case TTY_NOTHING:
        default:
            echo(echo_out, echo_len);
            break;
        }
    }
}

/* Whether the caller may read the keyboard.
 *
 * Only the foreground group. A background process that reads gets
 * `SIGTTIN` and `KERR_AGAIN` rather than a share of what somebody typed:
 * two readers would each get an arbitrary half of every line, which is a
 * data-loss bug that looks like a flaky keyboard.
 *
 * With no foreground group set — before any shell claims it — a read is
 * allowed, because refusing every read on a machine that has no shell
 * would make the terminal unusable rather than correct. */
static bool may_read_keyboard(void) {
    struct uproc *me = uspace_current();
    if (term.foreground == 0 || me == NULL) {
        return true;
    }
    if (uspace_pgid_of(me) == term.foreground) {
        return true;
    }
    sched_raise_on_process(uspace_pid_of(me), SIGTTIN);
    return false;
}

int32_t ktty_read(void *buf, uint32_t n) {
    if (buf == NULL || n == 0) {
        return 0;
    }
    if (!term.up) {
        return 0; /* no input device: end of file, which a runtime handles */
    }
    if (!may_read_keyboard()) {
        return KERR_AGAIN;
    }
    for (;;) {
        uint32_t got = tty_take_line(&term.line, buf, n);
        if (got != 0) {
            return (int32_t)got;
        }
        /* Nothing yet. The beat is what fills the line, and it wakes this
         * channel when a line completes — but **with a deadline anyway**,
         * for the same reason an idle hart has a backstop: a wake that is
         * only ever delivered by one path is a wake that path can lose.
         *
         * It is also what the kernel's own deadlock detector needs to see.
         * Without it the detector was right by its own rules and wrong
         * about this thread: "blocked with no deadline and no runnable
         * peers" meant unwakeable before anything could be woken by a
         * housekeeping hook, and a terminal read is exactly that. A thread
         * that can be woken only by a beat has to say so, and a deadline
         * is how it says it. */
        /* The wait's own return is **not** the interruption test: a timed
         * wait that simply expires reports that it expired, and treating
         * that as "interrupted" made a blocking read return after one
         * backstop with nothing to show — a read that does not block at
         * all, which is indistinguishable from end of file to its caller.
         * What interrupts a read is a signal, and that is asked
         * separately. */
        (void)sched_wait_on(&term, timer_ns() + TTY_WAIT_BACKSTOP_NS);
        if (sched_signal_pending()) {
            /* A signal arrived — `^C`, or `SIGTTIN` — and a read
             * interrupted by one returns to let the handler run. */
            return KERR_INTR;
        }
    }
}
