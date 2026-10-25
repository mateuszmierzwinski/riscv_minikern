#include "core/klog.h"

#include "core/console.h"
#include "lib/kfmt.h"

/* The kept log: a byte ring, because entries are lines of wildly different
 * lengths and a ring of fixed slots would either waste most of itself or
 * truncate the long ones. */
static char kept[KLOG_BYTES];
static uint32_t kept_at;   /* where the next byte goes */
static uint32_t kept_len;  /* how much is valid, up to KLOG_BYTES */
static uint32_t dropped;
static uint32_t entries;

/* The live tail: whole lines, because what reads it wants lines and
 * reconstructing them from a byte ring would mean finding a line boundary
 * that may have been overwritten. */
static char tail[KLOG_TAIL_LINES][KLOG_LINE_MAX];
static uint16_t tail_len[KLOG_TAIL_LINES];
static uint32_t tail_at;    /* the slot the next line goes in */
static uint32_t tail_count; /* how many slots are valid */

/* The line being assembled, so that a prefix is emitted once per line and
 * the tail stores lines rather than fragments. */
static char building[KLOG_LINE_MAX];
static uint32_t building_len;
static bool at_line_start = true;

static uint64_t (*clock_ns)(void);

void klog_time_source(uint64_t (*ns_since_boot)(void)) {
    clock_ns = ns_since_boot;
}

char klog_letter(enum klog_level level) {
    switch (level) {
    case KLOG_NOTICE: return 'N';
    case KLOG_WARN: return 'W';
    case KLOG_ERROR: return 'E';
    case KLOG_DEBUG: return 'D';
    default: return 'I';
    }
}

/* Two digits, zero-padded, without a formatter: this runs inside the
 * formatter's own sink and calling it again would be re-entrant. */
static void two(char *at, uint32_t v) {
    at[0] = (char)('0' + (v / 10u) % 10u);
    at[1] = (char)('0' + v % 10u);
}

void klog_format_time(uint64_t ns, char *out, uint32_t room) {
    uint64_t ms = ns / 1000000u;
    uint64_t s = ms / 1000u;
    uint32_t msec = (uint32_t)(ms % 1000u);
    /* **Hours are not wrapped at 24.** A board that has been up for two days
     * should say 48, not 00 — this is an elapsed time, and the one thing a
     * reader must not have to wonder is how many days to add. */
    uint64_t hours = s / 3600u;
    if (room < 15u) {
        if (room > 0u) {
            out[0] = '\0';
        }
        return;
    }
    out[0] = '[';
    if (hours > 99u) {
        /* Past four days the field grows rather than lying; a fixed width
         * is worth less than a true number. */
        out[1] = '9';
        out[2] = '9';
    } else {
        two(&out[1], (uint32_t)hours);
    }
    out[3] = ':';
    two(&out[4], (uint32_t)((s / 60u) % 60u));
    out[6] = ':';
    two(&out[7], (uint32_t)(s % 60u));
    out[9] = '.';
    out[10] = (char)('0' + (msec / 100u) % 10u);
    out[11] = (char)('0' + (msec / 10u) % 10u);
    out[12] = (char)('0' + msec % 10u);
    out[13] = ']';
    out[14] = '\0';
}

/* One byte into the kept ring. */
static void keep(char ch) {
    if (kept_len == KLOG_BYTES) {
        dropped++;
    } else {
        kept_len++;
    }
    kept[kept_at] = ch;
    kept_at = (kept_at + 1u) % KLOG_BYTES;
}

/* The finished line into the tail. */
static void close_line(void) {
    uint32_t n = building_len < KLOG_LINE_MAX ? building_len : KLOG_LINE_MAX;
    for (uint32_t i = 0; i < n; i++) {
        tail[tail_at][i] = building[i];
    }
    tail_len[tail_at] = (uint16_t)n;
    tail_at = (tail_at + 1u) % KLOG_TAIL_LINES;
    tail_count += tail_count < KLOG_TAIL_LINES ? 1u : 0u;
    building_len = 0;
}

/* Characters gathered before one console write.
 *
 * **Per character was a sixty-fourfold regression.** `kvprintf` used to
 * batch sixty-four characters into one port write; routing it through the
 * log made it `console_write(&ch, 1)` per character, and each of those is a
 * polled UART register write with a readiness loop. Nothing failed
 * outright — the log looked perfect — but the machine got slow enough that
 * a timing-sensitive test stopped completing, and the boot then hung
 * *after* the lines that would have explained it. A change that only costs
 * time is still a change that breaks things. */
#define KLOG_CHUNK 64u

/* The formatter's sink: the level, so a line begun in one call and finished
 * in another is prefixed once, **and the output chunk, which is per call and
 * so on the stack**.
 *
 * A static chunk is what `console.c` deliberately did not have. Two harts
 * share a static one, and `chunk[len++]` from both can step past the end of
 * a 64-byte array and over whatever follows it in `.bss` — a corruption
 * caused by logging, which is the worst possible thing for logging to cause,
 * because the log is what anybody would use to find it. */
struct sink_ctx {
    enum klog_level level;
    char chunk[KLOG_CHUNK];
    uint32_t len;
};

static void out_flush(struct sink_ctx *c) {
    if (c->len > 0u) {
        console_write(c->chunk, c->len);
        c->len = 0;
    }
}

static void out_put(struct sink_ctx *c, char ch) {
    if (c->len >= KLOG_CHUNK) {
        out_flush(c); /* bounded, whatever happened to `len` */
    }
    c->chunk[c->len++] = ch;
    if (c->len == KLOG_CHUNK) {
        out_flush(c);
    }
}

/* Everything a character goes to: the console, the kept ring, and the line
 * under construction. */
static void emit(struct sink_ctx *c, char ch) {
    out_put(c, ch);
    keep(ch);
    if (ch == '\n') {
        close_line();
        at_line_start = true;
        /* **Flushed at the end of a line**, not at the end of a call: a
         * line assembled by several calls would otherwise sit in the chunk
         * until something else happened to fill it, and a kernel whose last
         * words before a hang are still in a buffer has lost exactly the
         * output that mattered. */
        out_flush(c);
        return;
    }
    if (building_len < KLOG_LINE_MAX) {
        building[building_len++] = ch;
    }
    at_line_start = false;
}

static void prefix(struct sink_ctx *c, enum klog_level level) {
    char when[16];
    uint64_t ns = clock_ns == NULL ? 0u : clock_ns();
    klog_format_time(ns, when, sizeof when);
    for (uint32_t i = 0; when[i] != '\0'; i++) {
        emit(c, when[i]);
    }
    emit(c, ' ');
    emit(c, klog_letter(level));
    emit(c, ' ');
}

static void klog_sink(void *ctx, char ch) {
    struct sink_ctx *s = ctx;
    if (at_line_start && ch != '\n') {
        /* **The prefix goes in at the first character of a line, not at the
         * start of a call.** A message assembled by several `kprintf`s —
         * which this kernel does in several places — would otherwise carry
         * a timestamp in the middle of itself. */
        prefix(s, s->level);
    }
    emit(s, ch);
}

void klog_vwrite(enum klog_level level, const char *fmt, va_list ap) {
    struct sink_ctx ctx = {.level = level, .len = 0};
    entries++;
    kfmt_vformat(klog_sink, &ctx, fmt, ap);
    out_flush(&ctx);
}

void klog(enum klog_level level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vwrite(level, fmt, ap);
    va_end(ap);
}

uint32_t klog_all(char *out, uint32_t room) {
    uint32_t start = kept_len == KLOG_BYTES ? kept_at : 0u;
    uint32_t n = kept_len < room ? kept_len : room;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = kept[(start + i) % KLOG_BYTES];
    }
    return n;
}

uint32_t klog_tail(char *out, uint32_t room) {
    uint32_t first = tail_count == KLOG_TAIL_LINES ? tail_at : 0u;
    uint32_t done = 0;
    for (uint32_t i = 0; i < tail_count; i++) {
        uint32_t slot = (first + i) % KLOG_TAIL_LINES;
        for (uint32_t k = 0; k < tail_len[slot] && done < room; k++) {
            out[done++] = tail[slot][k];
        }
        if (done < room) {
            out[done++] = '\n';
        }
    }
    return done;
}

uint32_t klog_dropped(void) {
    return dropped;
}

uint32_t klog_entries(void) {
    return entries;
}
