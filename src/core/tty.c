#include "core/tty.h"

#include <stddef.h>

void tty_init(struct tty *t) {
    if (t == NULL) {
        return;
    }
    t->len = 0;
    t->ready = 0;
    t->echo = true;
    t->canonical = true;
    t->signals = true;
    t->cr_to_nl = true;
}

void tty_set_mode(struct tty *t, bool canonical, bool signals, bool cr_to_nl) {
    if (t == NULL) {
        return;
    }
    if (t->canonical && !canonical) {
        t->ready += t->len;
        t->len = 0;
    }
    t->canonical = canonical;
    t->signals = signals;
    t->cr_to_nl = cr_to_nl;
}

/* Whether one more byte fits, with room kept for a line's newline. */
static bool room_for_one(const struct tty *t) {
    return t->ready + t->len < TTY_LINE_MAX - 1u;
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
    if (t->ready + t->len < TTY_LINE_MAX) {
        t->line[t->ready + t->len++] = TTY_LF;
    }
    t->ready += t->len;
    t->len = 0;
    if (t->echo) {
        out[0] = TTY_LF; /* the output side's ONLCR adds the CR, as for any LF */
        *len = 1u;
    }
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
    t->ready += t->len;
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

/* ^C or ^Z under ISIG: echoed as ^X, the half-typed line going with ^C. */
static enum tty_action signal_key(struct tty *t, uint8_t byte, uint8_t *out, uint32_t *len) {
    if (t->echo) {
        echo_control(byte, out, len);
    }
    if (byte == TTY_INTR) {
        t->len = 0;
        return TTY_SIGNAL_INTR;
    }
    return TTY_SIGNAL_SUSP;
}

/* A byte that is data: onto the end of the line being edited (canonical)
 * or straight into what is ready (raw). A full buffer drops it, unechoed,
 * so the user can see input stopped being taken. */
static enum tty_action data_byte(struct tty *t, uint8_t byte, uint8_t *out, uint32_t *len) {
    if (!room_for_one(t)) {
        return TTY_NOTHING;
    }
    t->line[t->ready + t->len] = byte;
    if (t->canonical) {
        t->len++;
    } else {
        t->ready++;
    }
    if (t->echo) {
        out[0] = byte;
        *len = 1u;
    }
    return t->canonical ? TTY_NOTHING : TTY_LINE_READY;
}

/* The editing keys, which only canonical mode has. */
static enum tty_action editing_key(struct tty *t, uint8_t byte, uint8_t *out, uint32_t *len) {
    switch (byte) {
    case TTY_LF: return complete_line(t, out, len);
    case TTY_EOF: return end_of_file(t, len);
    case TTY_ERASE:
    case TTY_ERASE_BS: return erase_one(t, out, len);
    case TTY_KILL: return kill_line(t, out, len);
    default: return data_byte(t, byte, out, len);
    }
}

enum tty_action tty_input(struct tty *t, uint8_t byte, uint8_t *echo_out, uint32_t *echo_len) {
    if (t == NULL || echo_out == NULL || echo_len == NULL) {
        return TTY_NOTHING;
    }
    *echo_len = 0;
    if (t->signals && (byte == TTY_INTR || byte == TTY_SUSP)) {
        return signal_key(t, byte, echo_out, echo_len);
    }
    if (t->cr_to_nl && byte == TTY_CR) {
        byte = TTY_LF;
    }
    return t->canonical ? editing_key(t, byte, echo_out, echo_len)
                        : data_byte(t, byte, echo_out, echo_len);
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
    for (uint32_t i = 0; i < t->ready + t->len; i++) {
        t->line[i] = t->line[i + n]; /* the line being edited moves down too */
    }
    return n;
}
