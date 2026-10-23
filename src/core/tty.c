#include "core/tty.h"

#include <stddef.h>

void tty_init(struct tty *t) {
    if (t == NULL) {
        return;
    }
    t->len = 0;
    t->ready = 0;
    t->echo = true;
}

bool tty_has_line(const struct tty *t) {
    return t != NULL && t->ready != 0;
}

/* Erasing one character is three bytes on the wire: move the cursor back,
 * write a space over the glyph, move back again. A lone `\b` moves the
 * cursor and leaves the character on the screen, which is the commonest
 * terminal bug there is and looks like the kernel ignoring the key. */
static void echo_erase(uint8_t *out, uint32_t *len) {
    out[0] = TTY_ERASE_BS;
    out[1] = ' ';
    out[2] = TTY_ERASE_BS;
    *len = 3u;
}

/* A control character is echoed as `^X`, never as itself: writing a raw
 * `\003` to a terminal does whatever that terminal does with it, which is
 * not "show the user what they typed". */
static void echo_control(uint8_t byte, uint8_t *out, uint32_t *len) {
    out[0] = '^';
    out[1] = (uint8_t)(byte + 0x40u);
    *len = 2u;
}

/* The line is complete: it keeps its newline, because a program reading a
 * terminal expects one and a line delivered without it reads as a line that
 * is still being typed. */
static enum tty_action complete_line(struct tty *t, uint8_t *out, uint32_t *len) {
    if (t->len < TTY_LINE_MAX) {
        t->line[t->len++] = TTY_LF;
    }
    t->ready = t->len;
    t->len = 0;
    out[0] = TTY_CR;
    out[1] = TTY_LF;
    *len = 2u;
    return TTY_LINE_READY;
}

/* `^D`: end of file only on an empty line. On a line with something typed
 * it delivers what is there *without* a newline — which is what makes
 * `printf 'x' | ...` work from a terminal — and must not throw the line
 * away, which is the version of this that loses a user's work. */
static enum tty_action end_of_file(struct tty *t, uint32_t *len) {
    *len = 0;
    if (t->len == 0) {
        return TTY_END_OF_FILE;
    }
    t->ready = t->len;
    t->len = 0;
    return TTY_LINE_READY;
}

static enum tty_action erase_one(struct tty *t, uint8_t *out, uint32_t *len) {
    *len = 0;
    if (t->len == 0) {
        /* Nothing typed: **echo nothing.** Erasing here would walk the
         * cursor back over the prompt, which is not this line's to erase. */
        return TTY_NOTHING;
    }
    t->len--;
    if (t->echo) {
        echo_erase(out, len);
    }
    return TTY_NOTHING;
}

static enum tty_action kill_line(struct tty *t, uint8_t *out, uint32_t *len) {
    *len = 0;
    /* One erase per character actually typed, so the cursor ends where the
     * line began and no further. */
    while (t->len != 0 && t->echo && *len + 3u <= 4u) {
        t->len--;
        echo_erase(out, len);
    }
    t->len = 0;
    return TTY_NOTHING;
}

enum tty_action tty_input(struct tty *t, uint8_t byte, uint8_t *echo_out, uint32_t *echo_len) {
    if (t == NULL || echo_out == NULL || echo_len == NULL) {
        return TTY_NOTHING;
    }
    *echo_len = 0;
    switch (byte) {
    case TTY_CR:
    case TTY_LF:
        return complete_line(t, echo_out, echo_len);
    case TTY_INTR:
        if (t->echo) {
            echo_control(byte, echo_out, echo_len);
        }
        t->len = 0; /* the half-typed line goes with the interrupt */
        return TTY_SIGNAL_INTR;
    case TTY_SUSP:
        if (t->echo) {
            echo_control(byte, echo_out, echo_len);
        }
        return TTY_SIGNAL_SUSP;
    case TTY_EOF:
        return end_of_file(t, echo_len);
    case TTY_ERASE:
    case TTY_ERASE_BS:
        return erase_one(t, echo_out, echo_len);
    case TTY_KILL:
        return kill_line(t, echo_out, echo_len);
    default:
        break;
    }
    if (t->len >= TTY_LINE_MAX - 1u) {
        /* Full: the byte is dropped rather than overrunning, and nothing is
         * echoed so the user can see the line stopped accepting input. */
        return TTY_NOTHING;
    }
    t->line[t->len++] = byte;
    if (t->echo) {
        echo_out[0] = byte;
        *echo_len = 1u;
    }
    return TTY_NOTHING;
}

uint32_t tty_take_line(struct tty *t, void *to, uint32_t cap) {
    uint32_t n;
    if (t == NULL || to == NULL || t->ready == 0 || cap == 0) {
        return 0;
    }
    n = t->ready < cap ? t->ready : cap;
    for (uint32_t i = 0; i < n; i++) {
        ((uint8_t *)to)[i] = t->line[i];
    }
    /* **What is left stays ready.** A reader with a small buffer is
     * ordinary; losing the rest of the line to it is not. */
    t->ready -= n;
    for (uint32_t i = 0; i < t->ready; i++) {
        t->line[i] = t->line[i + n];
    }
    return n;
}
