/* Names: the 8.3 name every entry has, the long name beside it, and the
 * conversions between the UTF-8 the rest of the kernel speaks and the
 * UTF-16 the long name is stored in.
 *
 * A name that fits 8.3 exactly — upper-case, a base of eight and an
 * extension of three, from the characters an 8.3 name may hold — is stored
 * as that alone. Anything else gets a long name and a generated short one,
 * `BASE~N.EXT`, unique in its directory: the rule Windows and Linux's vfat
 * both follow, and the one `fsck.fat` expects. */
#include "core/errno.h"
#include "core/fat/fat_internal.h"

static bool upper_ascii(char c) {
    return c >= 'A' && c <= 'Z';
}

static char to_upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

/* A character an 8.3 name may hold as it is. */
static bool short_char(char c) {
    static const char extra[] = "!#$%&'()-@^_`{}~";
    if (upper_ascii(c) || (c >= '0' && c <= '9')) {
        return true;
    }
    for (uint32_t i = 0; extra[i] != '\0'; i++) {
        if (c == extra[i]) {
            return true;
        }
    }
    return false;
}

/* Where the extension starts: after the last dot, unless that is the first
 * character; `len` when there is none. */
static uint32_t extension_at(const char *name, uint32_t len) {
    for (uint32_t i = len; i > 1u; i--) {
        if (name[i - 1u] == '.') {
            return i - 1u;
        }
    }
    return len;
}

bool fat_short_exact(const char *name, uint32_t len, uint8_t out[11]) {
    uint32_t dot = extension_at(name, len), ext = dot < len ? len - dot - 1u : 0;
    if (len == 0 || dot == 0 || dot > 8u || ext > 3u || (dot < len && ext == 0)) {
        return false;
    }
    for (uint32_t i = 0; i < 11u; i++) {
        out[i] = ' ';
    }
    for (uint32_t i = 0; i < len; i++) {
        if (i != dot && !short_char(name[i])) {
            return false;
        }
        if (i < dot) {
            out[i] = (uint8_t)name[i];
        } else if (i > dot) {
            out[8u + (i - dot - 1u)] = (uint8_t)name[i];
        }
    }
    return out[0] != 0xe5u;
}

/* Up to `room` 8.3 characters from `name[from..to)`, upper-cased, with what
 * an 8.3 name cannot hold dropped. */
static uint32_t squeeze(const char *name, uint32_t from, uint32_t to, uint8_t *out,
                        uint32_t room) {
    uint32_t n = 0;
    for (uint32_t i = from; i < to && n < room; i++) {
        char c = to_upper(name[i]);
        if (short_char(c)) {
            out[n++] = (uint8_t)c;
        }
    }
    return n;
}

/* Whether `dir` has an entry with this exact short name. */
static int short_taken(struct fat *fs, const struct fat_node *dir, const uint8_t *cand,
                       bool *taken) {
    struct fat_walk w;
    uint8_t e[FAT_ENTRY];
    uint64_t off;
    int rc;
    *taken = false;
    fat_walk_start(fs, dir, &w);
    while ((rc = fat_walk_next(fs, &w, e, &off)) == KERR_OK && e[0] != FAT_DE_END) {
        bool same = e[0] != FAT_DE_DELETED && e[FAT_DE_ATTR] != FAT_ATTR_LFN;
        for (uint32_t i = 0; same && i < 11u; i++) {
            same = e[i] == cand[i];
        }
        if (same) {
            *taken = true;
            return KERR_OK;
        }
    }
    return rc == KERR_NOENT || rc == KERR_OK ? KERR_OK : rc;
}

/* Writes `~n` after at most `base` characters of the candidate's name. */
static void number_it(uint8_t *cand, uint32_t base, uint32_t n) {
    char digits[8];
    uint32_t d = 0, at;
    for (uint32_t v = n; v > 0 || d == 0; v /= 10u) {
        digits[d++] = (char)('0' + v % 10u);
    }
    at = base + 1u + d > 8u ? 8u - 1u - d : base;
    cand[at++] = '~';
    while (d > 0) {
        cand[at++] = (uint8_t)digits[--d];
    }
    while (at < 8u) {
        cand[at++] = ' ';
    }
}

int fat_short_for(struct fat *fs, const struct fat_node *dir, const char *name, uint32_t len,
                  uint8_t out[11]) {
    uint32_t dot = extension_at(name, len), base;
    for (uint32_t i = 0; i < 11u; i++) {
        out[i] = ' ';
    }
    base = squeeze(name, 0, dot, out, 6u);
    if (base == 0) {
        out[0] = '_';
        base = 1;
    }
    (void)squeeze(name, dot < len ? dot + 1u : len, len, out + 8u, 3u);
    for (uint32_t n = 1; n < 1000000u; n++) {
        bool taken = true;
        int rc;
        number_it(out, base, n);
        rc = short_taken(fs, dir, out, &taken);
        if (rc != KERR_OK || !taken) {
            return rc;
        }
    }
    return KERR_EXIST;
}

uint8_t fat_short_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11u; i++) {
        sum = (uint8_t)(((sum & 1u) << 7) + (sum >> 1) + name[i]);
    }
    return sum;
}

/* One code point from UTF-8 at `*at`, or false for a malformed sequence. */
static bool utf8_next(const char *in, uint32_t len, uint32_t *at, uint32_t *cp) {
    uint8_t c = (uint8_t)in[*at];
    uint32_t extra = c < 0x80u ? 0 : (c >> 5) == 6u ? 1u : (c >> 4) == 14u ? 2u : (c >> 3) == 30u ? 3u : 9u;
    if (extra == 9u || *at + extra >= len) {
        return false; /* not a lead byte, or a sequence cut off by the end */
    }
    *cp = extra == 0 ? c : (uint32_t)(c & (0x3fu >> extra));
    for (uint32_t i = 1; i <= extra; i++) {
        uint8_t k = (uint8_t)in[*at + i];
        if ((k & 0xc0u) != 0x80u) {
            return false;
        }
        *cp = (*cp << 6) | (k & 0x3fu);
    }
    *at += 1u + extra;
    return *cp <= 0x10ffffu && (*cp < 0xd800u || *cp > 0xdfffu);
}

int fat_utf8_to_utf16(const char *in, uint32_t len, uint16_t *out, uint32_t *units) {
    uint32_t at = 0;
    *units = 0;
    while (at < len) {
        uint32_t cp = 0;
        if (!utf8_next(in, len, &at, &cp) || *units + (cp > 0xffffu ? 2u : 1u) > FAT_NAME_MAX) {
            return KERR_INVAL;
        }
        if (cp > 0xffffu) {
            cp -= 0x10000u;
            out[(*units)++] = (uint16_t)(0xd800u + (cp >> 10));
            out[(*units)++] = (uint16_t)(0xdc00u + (cp & 0x3ffu));
        } else {
            out[(*units)++] = (uint16_t)cp;
        }
    }
    return KERR_OK;
}

/* One code point as UTF-8 at `out`, returning its length. */
static uint32_t utf8_put(uint32_t cp, char *out) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xc0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xe0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (cp & 0x3fu));
        return 3;
    }
    out[0] = (char)(0xf0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3fu));
    out[3] = (char)(0x80u | (cp & 0x3fu));
    return 4;
}

uint32_t fat_utf16_to_utf8(const uint16_t *in, uint32_t units, char *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < units; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xd800u && cp < 0xdc00u && i + 1u < units && in[i + 1u] >= 0xdc00u &&
            in[i + 1u] < 0xe000u) {
            cp = 0x10000u + ((cp - 0xd800u) << 10) + (in[++i] - 0xdc00u);
        }
        n += utf8_put(cp, out + n);
    }
    out[n] = '\0';
    return n;
}

bool fat_same_name(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    if (alen != blen) {
        return false;
    }
    for (uint32_t i = 0; i < alen; i++) {
        if (to_upper(a[i]) != to_upper(b[i])) {
            return false;
        }
    }
    return true;
}

uint32_t fat_short_render(const uint8_t *e, char *out) {
    uint32_t n = 0;
    bool lower_base = (e[FAT_DE_NTRES] & 0x08u) != 0, lower_ext = (e[FAT_DE_NTRES] & 0x10u) != 0;
    for (uint32_t i = 0; i < 8u && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05u ? 0xe5u : e[i]);
        out[n++] = lower_base && upper_ascii(c) ? (char)(c - 'A' + 'a') : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (uint32_t i = 8; i < 11u && e[i] != ' '; i++) {
            char c = (char)e[i];
            out[n++] = lower_ext && upper_ascii(c) ? (char)(c - 'A' + 'a') : c;
        }
    }
    out[n] = '\0';
    return n;
}

/* Days from 1970-01-01 to the start of `year`. */
static uint32_t days_before(uint32_t year) {
    uint32_t y = year - 1u;
    return y * 365u + y / 4u - y / 100u + y / 400u - 719162u;
}

static bool leap(uint32_t year) {
    return (year % 4u == 0 && year % 100u != 0) || year % 400u == 0;
}

void fat_dos_time(uint64_t now_s, uint16_t *time, uint16_t *date) {
    static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint32_t days = (uint32_t)(now_s / 86400u), secs = (uint32_t)(now_s % 86400u), year = 1980;
    uint32_t month = 0;
    days = days < days_before(1980) ? 0 : days - days_before(1980);
    while (year < 2107u && days >= (leap(year) ? 366u : 365u)) {
        days -= leap(year) ? 366u : 365u;
        year++;
    }
    while (month < 11u && days >= mdays[month] + (month == 1u && leap(year) ? 1u : 0u)) {
        days -= mdays[month] + (month == 1u && leap(year) ? 1u : 0u);
        month++;
    }
    *date = (uint16_t)(((year - 1980u) << 9) | ((month + 1u) << 5) | (days + 1u));
    *time = (uint16_t)(((secs / 3600u) << 11) | (((secs / 60u) % 60u) << 5) | ((secs % 60u) / 2u));
}
