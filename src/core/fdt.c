#include "core/fdt.h"

#include <stdbool.h>

#include "core/errno.h"

/* Header and token values are the Devicetree Specification's (an ABI). */
#define FDT_MAGIC          0xd00dfeedu
#define FDT_HEADER_SIZE    40u
#define FDT_LAST_COMP_V16  16u
#define FDT_BEGIN_NODE     1u
#define FDT_END_NODE       2u
#define FDT_PROP           3u
#define FDT_NOP            4u
#define FDT_END            9u
#define FDT_MAX_DEPTH      16u  /* deeper than any real board tree; bounds the walk */
#define FDT_MAX_PATH_PARTS 8u

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t align4(uint32_t n) {
    return (n + 3u) & ~3u;
}

/* True when [off, off+len) lies inside a block of the given size. */
static bool fits(uint32_t off, uint32_t len, uint32_t size) {
    return off <= size && len <= size - off;
}

int fdt_open(struct fdt *fdt, const void *base, size_t max_size) {
    const uint8_t *b = base;
    struct fdt f;
    if (b == NULL || max_size < FDT_HEADER_SIZE || be32(b) != FDT_MAGIC) {
        return KERR_INVAL;
    }
    f = (struct fdt){
        .base = b,
        .total_size = be32(b + 4),
        .struct_off = be32(b + 8),
        .strings_off = be32(b + 12),
        .strings_size = be32(b + 32),
        .struct_size = be32(b + 36),
    };
    if (f.total_size < FDT_HEADER_SIZE || f.total_size > max_size || be32(b + 24) < FDT_LAST_COMP_V16 ||
        (f.struct_off & 3u) != 0 || !fits(f.struct_off, f.struct_size, f.total_size) ||
        !fits(f.strings_off, f.strings_size, f.total_size)) {
        return KERR_INVAL;
    }
    *fdt = f;
    return KERR_OK;
}

/* One component of a lookup path: name and length, not NUL-terminated. */
struct path_part {
    const char *name;
    uint32_t len;
};

/* Splits "/a/b@1" into parts; returns the count, or -1 when it is not
 * absolute or has too many parts. "/" is zero parts. */
static int split_path(const char *path, struct path_part *parts) {
    int n = 0;
    if (path == NULL || path[0] != '/') {
        return -1;
    }
    for (const char *p = path + 1; *p != '\0';) {
        uint32_t len = 0;
        while (p[len] != '\0' && p[len] != '/') {
            len++;
        }
        if (len > 0) {
            if (n == (int)FDT_MAX_PATH_PARTS) {
                return -1;
            }
            parts[n++] = (struct path_part){.name = p, .len = len};
        }
        p += len + (p[len] == '/');
    }
    return n;
}

/* A node name matches a part exactly, or by its base name when the part
 * carries no unit address ("memory" matches "memory@40000000"). */
static bool name_matches(const char *name, uint32_t name_len, const struct path_part *part) {
    for (uint32_t i = 0; i < part->len; i++) {
        if (i >= name_len || name[i] != part->name[i]) {
            return false;
        }
    }
    return part->len == name_len || name[part->len] == '@';
}

/* Length of the NUL-terminated string at off within limit, or -1. */
static int32_t bounded_strlen(const uint8_t *s, uint32_t off, uint32_t limit) {
    for (uint32_t i = off; i < limit; i++) {
        if (s[i] == '\0') {
            return (int32_t)(i - off);
        }
    }
    return -1;
}

/* The walk's state: where it is, how deep, and how much of the path the
 * current node chain has matched. */
struct walk {
    const struct fdt *fdt;
    const uint8_t *blk;  /* the structure block */
    uint32_t at;         /* offset of the next token in blk */
    uint32_t depth;      /* nodes currently open */
    uint32_t matched;    /* leading path parts matched by the open chain */
    const struct path_part *parts;
    uint32_t nparts;
};

static bool str_equal(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Opens a node. Its level is the depth before entering it (the root is 0);
 * it extends the match when its parent chain matched every earlier part. */
static int walk_begin_node(struct walk *w) {
    const char *name = (const char *)w->blk + w->at;
    int32_t len = bounded_strlen(w->blk, w->at, w->fdt->struct_size);
    uint32_t level = w->depth;
    if (len < 0 || w->depth == FDT_MAX_DEPTH) {
        return KERR_INVAL;
    }
    w->at = align4(w->at + (uint32_t)len + 1u);
    if (level >= 1 && w->matched == level - 1 && w->matched < w->nparts &&
        name_matches(name, (uint32_t)len, &w->parts[w->matched])) {
        w->matched = level;
    }
    w->depth++;
    return KERR_OK;
}

/* Closes a node, giving back the path part it matched, if any. */
static int walk_end_node(struct walk *w) {
    uint32_t level;
    if (w->depth == 0) {
        return KERR_INVAL;
    }
    level = w->depth - 1;
    if (level >= 1 && w->matched == level) {
        w->matched = level - 1;
    }
    w->depth--;
    return KERR_OK;
}

/* Reads a property token; *hit is set when it is prop on the wanted node. */
static int walk_prop(struct walk *w, const char *prop, struct fdt_prop *out, bool *hit) {
    const struct fdt *f = w->fdt;
    uint32_t len, nameoff;
    if (w->depth == 0 || !fits(w->at, 8, f->struct_size)) {
        return KERR_INVAL;
    }
    len = be32(w->blk + w->at);
    nameoff = be32(w->blk + w->at + 4);
    if (!fits(w->at + 8, len, f->struct_size) ||
        bounded_strlen(f->base + f->strings_off, nameoff, f->strings_size) < 0) {
        return KERR_INVAL;
    }
    *hit = w->depth - 1 == w->nparts && w->matched == w->nparts &&
           str_equal((const char *)f->base + f->strings_off + nameoff, prop);
    *out = (struct fdt_prop){.value = w->blk + w->at + 8, .len = len};
    w->at = align4(w->at + 8 + len);
    return KERR_OK;
}

/* Advances over one token. Returns KERR_OK to continue, KERR_NOENT at
 * FDT_END, KERR_INVAL on a malformed block; *hit reports a found prop. */
static int walk_step(struct walk *w, const char *prop, struct fdt_prop *out, bool *hit) {
    uint32_t token;
    *hit = false;
    if (!fits(w->at, 4, w->fdt->struct_size)) {
        return KERR_INVAL;
    }
    token = be32(w->blk + w->at);
    w->at += 4;
    switch (token) {
    case FDT_BEGIN_NODE:
        return walk_begin_node(w);
    case FDT_END_NODE:
        return walk_end_node(w);
    case FDT_PROP:
        return walk_prop(w, prop, out, hit);
    case FDT_NOP:
        return KERR_OK;
    case FDT_END:
        return w->depth == 0 ? KERR_NOENT : KERR_INVAL;
    default:
        return KERR_INVAL;
    }
}

int fdt_get_prop(const struct fdt *fdt, const char *path, const char *prop, struct fdt_prop *out) {
    struct path_part parts[FDT_MAX_PATH_PARTS];
    int nparts = split_path(path, parts);
    struct walk w = {.fdt = fdt, .blk = fdt->base + fdt->struct_off, .parts = parts};
    struct fdt_prop found;
    bool hit = false;
    int rc = KERR_OK;
    if (nparts < 0 || prop == NULL) {
        return KERR_INVAL;
    }
    w.nparts = (uint32_t)nparts;
    while (rc == KERR_OK && !hit) {
        rc = walk_step(&w, prop, &found, &hit);
    }
    if (hit) {
        *out = found;
    }
    return hit ? KERR_OK : rc;
}

int fdt_prop_cells(const struct fdt_prop *p, uint32_t at, uint32_t cells, uint64_t *out) {
    uint32_t off = at * 4u;
    if (cells < 1 || cells > 2 || at > p->len / 4u || !fits(off, cells * 4u, p->len)) {
        return KERR_INVAL;
    }
    /* A two-cell value is a full 64 bits here. The kernel this grew from
     * refused one whose high word was set, which was safe on a 512 MiB
     * board and would reject the memory node of an 8 GiB VisionFive 2
     * outright (core/types.h). */
    if (false) {
        return KERR_INVAL;
    }
    *out = cells == 2u ? ((uint64_t)be32(p->value + off) << 32) | be32(p->value + off + 4u)
                       : be32(p->value + off);
    return KERR_OK;
}

uint32_t fdt_get_u32(const struct fdt *fdt, const char *path, const char *prop, uint32_t fallback) {
    struct fdt_prop p;
    uint64_t v;
    if (fdt_get_prop(fdt, path, prop, &p) != KERR_OK || p.len != 4 || fdt_prop_cells(&p, 0, 1, &v) != KERR_OK) {
        return fallback;
    }
    return (uint32_t)v;
}

/* ── finding a device rather than a path ─────────────────────────────────── */

/* One open node, while scanning for a device: its name, so a path can be
 * rebuilt when a match is found. */
struct open_node {
    const char *name;
    uint32_t len;
};

/* True when a `compatible` value — a list of NUL-separated strings — names
 * `want`. A node is compatible with several things, most specific first, and
 * matching any of them is the point: a driver for `ns16550a` must bind to a
 * node that says "snps,dw-apb-uart", "ns16550a". */
static bool compatible_lists(const struct fdt_prop *p, const char *want) {
    uint32_t at = 0;
    while (at < p->len) {
        const char *s = (const char *)p->value + at;
        uint32_t n = 0;
        while (at + n < p->len && s[n] != '\0') {
            n++;
        }
        if (at + n >= p->len && s[n] != '\0') {
            return false; /* unterminated: a malformed value matches nothing */
        }
        if (str_equal(s, want)) {
            return true;
        }
        at += n + 1u;
    }
    return false;
}

/* Writes "/a/b@1" for the currently open chain. False when it does not fit,
 * which is a refusal rather than a truncation: half a path would name the
 * wrong node. */
static bool build_path(const struct open_node *stack, uint32_t depth, char *out, uint32_t cap) {
    uint32_t at = 0;
    if (depth == 0) {
        return cap >= 2u && (out[0] = '/', out[1] = '\0', true);
    }
    for (uint32_t i = 0; i < depth; i++) {
        if (at + 1u + stack[i].len + 1u > cap) {
            return false;
        }
        out[at++] = '/';
        for (uint32_t k = 0; k < stack[i].len; k++) {
            out[at++] = stack[i].name[k];
        }
    }
    out[at] = '\0';
    return true;
}

/* What a scan is looking for: a property name, and a test on its value. */
struct matcher {
    const char *prop;
    bool (*matches)(const struct fdt_prop *value, const void *want);
    const void *want;
};

static bool matches_compatible(const struct fdt_prop *value, const void *want) {
    return compatible_lists(value, want);
}

static bool matches_phandle(const struct fdt_prop *value, const void *want) {
    uint64_t v = 0;
    return value->len == 4u && fdt_prop_cells(value, 0, 1, &v) == KERR_OK &&
           v == *(const uint32_t *)want;
}

/* Walks every node until the matcher is satisfied, and writes the path of
 * the node whose property matched. One implementation, because "find the
 * node that says X" is the only question a driver ever asks of a tree. */
static int scan_for(const struct fdt *fdt, const struct matcher *m, uint32_t skip, char *path,
                    uint32_t cap) {
    struct open_node stack[FDT_MAX_DEPTH];
    const uint8_t *blk = fdt->base + fdt->struct_off;
    uint32_t at = 0, depth = 0;
    if (m->prop == NULL || path == NULL || cap == 0) {
        return KERR_INVAL;
    }
    for (;;) {
        uint32_t token;
        if (!fits(at, 4, fdt->struct_size)) {
            return KERR_INVAL;
        }
        token = be32(blk + at);
        at += 4;
        if (token == FDT_END) {
            return depth == 0 ? KERR_NOENT : KERR_INVAL;
        }
        if (token == FDT_NOP) {
            continue;
        }
        if (token == FDT_BEGIN_NODE) {
            int32_t len = bounded_strlen(blk, at, fdt->struct_size);
            if (len < 0 || depth == FDT_MAX_DEPTH) {
                return KERR_INVAL;
            }
            stack[depth] = (struct open_node){.name = (const char *)blk + at, .len = (uint32_t)len};
            depth++;
            at = align4(at + (uint32_t)len + 1u);
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0) {
                return KERR_INVAL;
            }
            depth--;
            continue;
        }
        if (token != FDT_PROP) {
            return KERR_INVAL;
        }
        {
            uint32_t len, nameoff;
            struct fdt_prop value;
            if (depth == 0 || !fits(at, 8, fdt->struct_size)) {
                return KERR_INVAL;
            }
            len = be32(blk + at);
            nameoff = be32(blk + at + 4);
            if (!fits(at + 8, len, fdt->struct_size) ||
                bounded_strlen(fdt->base + fdt->strings_off, nameoff, fdt->strings_size) < 0) {
                return KERR_INVAL;
            }
            value = (struct fdt_prop){.value = blk + at + 8, .len = len};
            at = align4(at + 8 + len);
            if (str_equal((const char *)fdt->base + fdt->strings_off + nameoff, m->prop) &&
                m->matches(&value, m->want)) {
                if (skip > 0) {
                    skip--;        /* a match, but not the one asked for */
                } else {
                    /* The root is depth 1 with an empty name, so the chain
                     * above the match is what names it. */
                    return build_path(&stack[1], depth - 1u, path, cap) ? KERR_OK : KERR_NOMEM;
                }
            }
        }
    }
}

/* The nth child of a node, by path.
 *
 * Nodes that are a *list* are the reason this exists: an operating-point
 * table is a parent whose children are the operating points, named
 * `opp-1500000000` and such, and nothing about a name says how many there
 * are or what the next one is called. A reader that can only find a node by
 * its compatible string or its path cannot read a list at all.
 *
 * `KERR_NOENT` when the parent has no nth child, which is how a caller ends
 * a loop. */
int fdt_nth_child(const struct fdt *fdt, const char *parent, uint32_t n, char *path,
                  uint32_t cap) {
    const uint8_t *blk;
    uint32_t at, depth = 0, seen = 0, parent_depth = 0;
    bool inside = false;
    struct open_node stack[FDT_MAX_DEPTH];
    if (fdt == NULL || parent == NULL || path == NULL || cap == 0) {
        return KERR_INVAL;
    }
    blk = fdt->base + fdt->struct_off;
    at = 0;
    for (;;) {
        uint32_t token;
        if (!fits(at, 4, fdt->struct_size)) {
            return KERR_INVAL;
        }
        token = be32(blk + at);
        at += 4;
        if (token == FDT_END) {
            return KERR_NOENT;
        }
        if (token == FDT_NOP) {
            continue;
        }
        if (token == FDT_BEGIN_NODE) {
            int32_t len = bounded_strlen(blk, at, fdt->struct_size);
            if (len < 0 || depth == FDT_MAX_DEPTH) {
                return KERR_INVAL;
            }
            stack[depth] = (struct open_node){.name = (const char *)blk + at, .len = (uint32_t)len};
            depth++;
            at = align4(at + (uint32_t)len + 1u);
            if (!inside) {
                char here[128];
                if (depth >= 1u && build_path(&stack[1], depth - 1u, here, sizeof here) &&
                    str_equal(here, parent)) {
                    inside = true;
                    parent_depth = depth;
                }
            } else if (depth == parent_depth + 1u) {
                /* A direct child, which is the only kind that counts. */
                if (seen == n) {
                    return build_path(&stack[1], depth - 1u, path, cap) ? KERR_OK : KERR_NOMEM;
                }
                seen++;
            }
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0) {
                return KERR_INVAL;
            }
            depth--;
            if (inside && depth < parent_depth) {
                return KERR_NOENT; /* out of the parent without finding it */
            }
            continue;
        }
        if (token != FDT_PROP) {
            return KERR_INVAL;
        }
        {
            uint32_t len;
            if (depth == 0 || !fits(at, 8, fdt->struct_size)) {
                return KERR_INVAL;
            }
            len = be32(blk + at);
            if (!fits(at + 8, len, fdt->struct_size)) {
                return KERR_INVAL;
            }
            at = align4(at + 8 + len);
        }
    }
}

int fdt_find_compatible(const struct fdt *fdt, const char *compat, char *path, uint32_t cap) {
    struct matcher m = {.prop = "compatible", .matches = matches_compatible, .want = compat};
    return compat == NULL ? KERR_INVAL : scan_for(fdt, &m, 0, path, cap);
}

int fdt_find_compatible_nth(const struct fdt *fdt, const char *compat, uint32_t skip, char *path,
                            uint32_t cap) {
    struct matcher m = {.prop = "compatible", .matches = matches_compatible, .want = compat};
    return compat == NULL ? KERR_INVAL : scan_for(fdt, &m, skip, path, cap);
}

int fdt_find_phandle(const struct fdt *fdt, uint32_t phandle, char *path, uint32_t cap) {
    struct matcher m = {.prop = "phandle", .matches = matches_phandle, .want = &phandle};
    return scan_for(fdt, &m, 0, path, cap);
}

/* Cuts the last component off a path, in place: "/a/b/c" becomes "/a/b".
 * False for a path with nothing to cut. */
bool fdt_parent_path(char *path) {
    uint32_t last = 0;
    for (uint32_t i = 0; path[i] != '\0'; i++) {
        last = path[i] == '/' ? i : last;
    }
    if (last == 0) {
        return false;
    }
    path[last] = '\0';
    return true;
}


int fdt_device_reg(const struct fdt *fdt, const char *path, uint32_t which, phys_t *base,
                   uint64_t *size) {
    uint32_t acells = fdt_get_u32(fdt, "/", "#address-cells", 2);
    uint32_t scells = fdt_get_u32(fdt, "/", "#size-cells", 2);
    struct fdt_prop reg;
    uint32_t at = which * (acells + scells);
    int rc = fdt_get_prop(fdt, path, "reg", &reg);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = fdt_prop_cells(&reg, at, acells, base);
    return rc != KERR_OK ? rc : fdt_prop_cells(&reg, at + acells, scells, size);
}
