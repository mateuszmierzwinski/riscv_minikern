/* A file's bytes: read along its cluster chain, written along it and past
 * its end, and cut back.
 *
 * FAT has no holes. Growing a file — by a write past its end or a truncate
 * upward — gives it every cluster up to the new end, and those clusters
 * come zeroed from the table code; the one part that does not is the tail
 * of the file's old last cluster, past its old size, which may hold what a
 * longer version of the file left there. That tail is zeroed explicitly
 * before the size moves over it. */
#include "core/errno.h"
#include "core/fat/fat_internal.h"

int fat_read(struct fat *fs, const struct fat_node *node, uint64_t off, void *buf, uint32_t len,
             uint32_t *got) {
    uint8_t *out = buf;
    uint32_t c = 0, done = 0;
    int rc;
    *got = 0;
    if (node->dir) {
        return KERR_ISDIR;
    }
    if (off >= node->size || len == 0) {
        return KERR_OK;
    }
    len = node->size - off < len ? (uint32_t)(node->size - off) : len;
    rc = fat_nth_cluster(fs, node->first, (uint32_t)(off / fs->cluster), &c);
    while (rc == KERR_OK && done < len) {
        uint32_t within = (uint32_t)((off + done) % fs->cluster);
        uint32_t n = fs->cluster - within < len - done ? fs->cluster - within : len - done;
        rc = fat_pread(fs, fat_cluster_offset(fs, c) + within, out + done, n);
        done += rc == KERR_OK ? n : 0;
        if (rc == KERR_OK && done < len) {
            rc = fat_nth_cluster(fs, c, 1u, &c);
        }
    }
    *got = done;
    return rc == KERR_NOENT ? KERR_IO : rc; /* a chain shorter than its file's size */
}

/* Rewrites the node's short entry: first cluster, size, and the time. */
static int update_entry(struct fat *fs, const struct fat_node *node, uint64_t now_s) {
    uint8_t e[FAT_ENTRY];
    uint16_t t = 0, d = 0;
    int rc = node->entry == 0 ? KERR_INVAL : fat_pread(fs, node->entry, e, sizeof e);
    if (rc != KERR_OK) {
        return rc;
    }
    fat_dos_time(now_s, &t, &d);
    fat_put16(e + FAT_DE_HICLUST, fs->type == 32 ? (uint16_t)(node->first >> 16) : 0);
    fat_put16(e + FAT_DE_LOCLUST, (uint16_t)node->first);
    fat_put32(e + FAT_DE_SIZE, node->dir ? 0 : node->size);
    fat_put16(e + FAT_DE_MTIME, t);
    fat_put16(e + FAT_DE_MDATE, d);
    fat_put16(e + FAT_DE_ADATE, d);
    return fat_pwrite(fs, node->entry, e, sizeof e);
}

/* How many clusters a chain has, and its last. */
static int chain_length(struct fat *fs, uint32_t first, uint32_t *count, uint32_t *last) {
    uint32_t c = first;
    *count = 0;
    *last = 0;
    for (; fat_cluster_ok(fs, c) && *count <= fs->clusters;) {
        uint32_t next = 0;
        int rc = fat_get(fs, c, &next);
        if (rc != KERR_OK) {
            return rc;
        }
        (*count)++;
        *last = c;
        if (fat_is_end(fs, next)) {
            return KERR_OK;
        }
        c = next;
    }
    return first == 0 ? KERR_OK : KERR_IO;
}

/* Gives the file clusters until it has `want`. */
static int grow_chain(struct fat *fs, struct fat_node *node, uint32_t want) {
    uint32_t have = 0, last = 0;
    int rc = chain_length(fs, node->first, &have, &last);
    for (; rc == KERR_OK && have < want; have++) {
        uint32_t fresh = 0;
        rc = fat_take_cluster(fs, last, &fresh);
        if (rc == KERR_OK && node->first == 0) {
            node->first = fresh;
        }
        last = fresh;
    }
    return rc;
}

static uint32_t clusters_for(const struct fat *fs, uint64_t bytes) {
    return (uint32_t)((bytes + fs->cluster - 1u) / fs->cluster);
}

/* Zeroes the file's old last cluster past its old size, up to `upto`. */
static int zero_tail(struct fat *fs, const struct fat_node *node, uint64_t upto) {
    static const uint8_t zeroes[512];
    uint32_t c = 0, within = node->size % fs->cluster;
    uint64_t end = (uint64_t)node->size - within + fs->cluster;
    int rc = KERR_OK;
    if (within == 0 || node->first == 0) {
        return KERR_OK; /* the old end is on a cluster boundary: nothing stale */
    }
    end = upto < end ? upto : end;
    rc = fat_nth_cluster(fs, node->first, node->size / fs->cluster, &c);
    for (uint64_t at = node->size; rc == KERR_OK && at < end;) {
        uint32_t n = end - at < sizeof zeroes ? (uint32_t)(end - at) : (uint32_t)sizeof zeroes;
        rc = fat_pwrite(fs, fat_cluster_offset(fs, c) + at % fs->cluster, zeroes, n);
        at += n;
    }
    return rc;
}

/* Makes the file able to hold `end` bytes: zeroed past its old end, and
 * with the clusters for all of it. */
static int extend_to(struct fat *fs, struct fat_node *node, uint64_t end) {
    int rc = end > node->size ? zero_tail(fs, node, end) : KERR_OK;
    return rc != KERR_OK ? rc : grow_chain(fs, node, clusters_for(fs, end));
}

/* Writes `len` bytes at `off` into clusters the file already has. */
static int write_chain(struct fat *fs, const struct fat_node *node, uint64_t off,
                       const uint8_t *in, uint32_t len) {
    uint32_t c = 0, done = 0;
    int rc = fat_nth_cluster(fs, node->first, (uint32_t)(off / fs->cluster), &c);
    while (rc == KERR_OK && done < len) {
        uint32_t within = (uint32_t)((off + done) % fs->cluster);
        uint32_t n = fs->cluster - within < len - done ? fs->cluster - within : len - done;
        rc = fat_pwrite(fs, fat_cluster_offset(fs, c) + within, in + done, n);
        done += rc == KERR_OK ? n : 0;
        if (rc == KERR_OK && done < len) {
            rc = fat_nth_cluster(fs, c, 1u, &c);
        }
    }
    return rc;
}

int fat_write(struct fat *fs, struct fat_node *node, uint64_t off, const void *buf, uint32_t len,
              uint32_t *put) {
    uint64_t end = off + len;
    int rc = node->dir ? KERR_ISDIR : (fs->writable ? KERR_OK : KERR_ROFS);
    *put = 0;
    if (rc != KERR_OK || len == 0) {
        return rc;
    }
    if (end > 0xffffffffu) {
        return KERR_FBIG; /* a FAT file's size is 32 bits */
    }
    rc = extend_to(fs, node, end);
    rc = rc != KERR_OK ? rc : write_chain(fs, node, off, buf, len);
    if (rc != KERR_OK) {
        return rc;
    }
    *put = len;
    node->size = end > node->size ? (uint32_t)end : node->size;
    return update_entry(fs, node, fs->now_s);
}

/* Ends the chain after `keep` clusters and frees the rest. */
static int cut_chain(struct fat *fs, struct fat_node *node, uint32_t keep) {
    uint32_t last = 0, rest = 0;
    int rc;
    if (keep == 0) {
        rc = fat_free_chain(fs, node->first);
        node->first = 0;
        return rc;
    }
    rc = fat_nth_cluster(fs, node->first, keep - 1u, &last);
    rc = rc != KERR_OK ? rc : fat_get(fs, last, &rest);
    rc = rc != KERR_OK ? rc : fat_set(fs, last, fat_end_mark(fs));
    return rc != KERR_OK || fat_is_end(fs, rest) ? rc : fat_free_chain(fs, rest);
}

int fat_truncate(struct fat *fs, struct fat_node *node, uint32_t size, uint64_t now_s) {
    int rc = node->dir ? KERR_ISDIR : (fs->writable ? KERR_OK : KERR_ROFS);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = size < node->size ? cut_chain(fs, node, clusters_for(fs, size)) : extend_to(fs, node, size);
    if (rc != KERR_OK) {
        return rc;
    }
    node->size = size;
    return update_entry(fs, node, now_s);
}

int fat_update_entry(struct fat *fs, const struct fat_node *node, uint64_t now_s) {
    return update_entry(fs, node, now_s);
}
