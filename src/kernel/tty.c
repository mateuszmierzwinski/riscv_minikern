#include "kernel/tty.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "arch/riscv/timer.h"
#include "core/tty.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/uspace.h"

/* `SIGINT` is the only one this file names that is not already in
 * `signal.h`: the stop signals are there, because what they *mean* is the
 * same wherever they come from. */
#define SIGINT 2

/* How long a blocked reader waits before draining the wire itself. With
 * dynamic ticks an idle machine runs no housekeeping beat, so this poll is
 * what a keystroke waits for: 20 ms is below what a typist notices. */
#define TTY_WAIT_BACKSTOP_NS 20000000ull

#define T_ISIG 0000001u
#define T_ICANON 0000002u
#define T_ECHO 0000010u
#define T_ECHOE 0000020u
#define T_ECHOK 0000040u
#define T_IEXTEN 0100000u
#define T_ICRNL 0000400u
#define T_OPOST 0000001u
#define T_ONLCR 0000004u
#define T_B115200 0010002u
#define T_CS8 0000060u
#define T_CREAD 0000200u
#define T_IFLAGS_HONOURED T_ICRNL
#define T_OFLAGS_HONOURED (T_OPOST | T_ONLCR)
#define T_LFLAGS_HONOURED (T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_IEXTEN)
#define T_CFLAGS (T_B115200 | T_CS8 | T_CREAD)

/* ^C ^\ DEL ^U ^D, VTIME 0, VMIN 1, ^Q ^S ^Z: what a terminal starts with. */
static const uint8_t default_cc[KTTY_NCC] = {3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a};

static struct {
    struct tty line;
    struct ktty_mode mode;
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
    term.mode = (struct ktty_mode){.iflag = T_ICRNL, .oflag = T_OPOST | T_ONLCR, .cflag = T_CFLAGS,
                                   .lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK |
                                            T_IEXTEN};
    for (uint32_t i = 0; i < KTTY_NCC; i++) {
        term.mode.cc[i] = default_cc[i];
    }
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
    ktty_poll();
    return tty_has_line(&term.line);
}

uint32_t ktty_pending(void) {
    ktty_poll();
    return term.line.ready;
}

bool ktty_echo(void) {
    return term.line.echo;
}

void ktty_set_echo(bool on) {
    term.line.echo = on;
    term.mode.lflag = on ? term.mode.lflag | T_ECHO : term.mode.lflag & ~T_ECHO;
}

void ktty_get_mode(struct ktty_mode *out) {
    *out = term.mode;
    out->lflag = term.line.echo ? out->lflag | T_ECHO : out->lflag & ~T_ECHO;
}

/* Whether a mode asks only for what this terminal does. */
static bool mode_honoured(const struct ktty_mode *m) {
    bool raw_output = (m->oflag & (T_OPOST | T_ONLCR)) != (T_OPOST | T_ONLCR);
    if ((m->iflag & ~T_IFLAGS_HONOURED) != 0 || (m->oflag & ~T_OFLAGS_HONOURED) != 0 ||
        (m->lflag & ~T_LFLAGS_HONOURED) != 0 || m->cflag != T_CFLAGS ||
        (raw_output && !console_untranslated())) {
        return false;
    }
    for (uint32_t i = 0; i < KTTY_NCC; i++) {
        if (i != KTTY_VMIN && i != KTTY_VTIME && m->cc[i] != default_cc[i]) {
            return false;
        }
    }
    return true;
}

int ktty_set_mode(const struct ktty_mode *mode) {
    if (!mode_honoured(mode)) {
        return KERR_INVAL;
    }
    term.mode = *mode;
    term.line.echo = (mode->lflag & T_ECHO) != 0;
    tty_set_mode(&term.line, (mode->lflag & T_ICANON) != 0, (mode->lflag & T_ISIG) != 0,
                 (mode->iflag & T_ICRNL) != 0);
    return KERR_OK;
}

void ktty_write(const void *buf, uint32_t n) {
    bool onlcr = (term.mode.oflag & (T_OPOST | T_ONLCR)) == (T_OPOST | T_ONLCR);
    if (onlcr) {
        console_write(buf, (size_t)n);
    } else {
        console_write_untranslated(buf, (size_t)n);
    }
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

/* Echo goes out the way a program's output does, so ONLCR applies to it. */
static void echo(const uint8_t *bytes, uint32_t n) {
    if (n != 0) {
        ktty_write(bytes, n);
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
    /* **Through the signal's own meaning**, not straight at the thread: a
     * background reader is *stopped* by `SIGTTIN` unless it has claimed it,
     * and raising it as an ordinary signal killed the reader instead. */
    uspace_deliver_signal(me, SIGTTIN);
    return false;
}

/* A canonical read: blocks until a line is ready. */
static int32_t read_line(void *buf, uint32_t n) {
    for (;;) {
        ktty_poll(); /* the reader drains the wire: an idle machine may run no beat */
        uint32_t got = tty_take_line(&term.line, buf, n);
        if (got != 0) {
            return (int32_t)got;
        }
        /* With a deadline anyway: a wake only one path delivers is one that
         * path can lose, and the deadlock detector must see a reader the
         * beat will wake. An expired wait is not an interruption — a
         * signal is, and is asked separately. */
        (void)sched_wait_on(&term, timer_ns() + TTY_WAIT_BACKSTOP_NS);
        if (sched_signal_pending()) {
            return KERR_INTR;
        }
    }
}

/* Whether a raw read is satisfied: VMIN bytes (or as many as asked for);
 * with VMIN 0, at once; with VTIME, once its timer has run out — counted
 * from the read with VMIN 0, from the last byte to arrive otherwise. */
static bool raw_satisfied(uint32_t want, uint64_t vtime_ns, uint64_t timer_from) {
    ktty_poll();
    uint32_t ready = term.line.ready;
    if (want != 0 ? ready >= want : (vtime_ns == 0 || ready != 0)) {
        return true;
    }
    return vtime_ns != 0 && (want == 0 || ready != 0) && timer_ns() >= timer_from + vtime_ns;
}

/* A read with ICANON off, as `VMIN` and `VTIME` say. */
static int32_t read_raw(void *buf, uint32_t n) {
    uint32_t vmin = term.mode.cc[KTTY_VMIN];
    uint32_t want = vmin < n ? vmin : n;
    uint64_t vtime_ns = (uint64_t)term.mode.cc[KTTY_VTIME] * 100000000ull;
    uint64_t timer_from = timer_ns();
    uint32_t seen = term.line.ready;
    while (!raw_satisfied(want, vtime_ns, timer_from)) {
        uint64_t until = vtime_ns != 0 ? timer_from + vtime_ns : timer_ns() + TTY_WAIT_BACKSTOP_NS;
        (void)sched_wait_on(&term, until);
        if (sched_signal_pending()) {
            return KERR_INTR;
        }
        if (want != 0 && term.line.ready != seen) {
            seen = term.line.ready;
            timer_from = timer_ns(); /* VMIN > 0: the timer is between bytes */
        }
    }
    return (int32_t)tty_take_line(&term.line, buf, n);
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
    return term.line.canonical ? read_line(buf, n) : read_raw(buf, n);
}
