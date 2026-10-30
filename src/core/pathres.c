#include "core/pathres.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"

#define WORK_MAX PATHRES_WORK

/* The walk: what is resolved so far (`out`, always absolute and free of
 * links) and what is still to go (`todo`, a queue of components). */
struct walk {
    char *out;
    uint32_t room;
    uint32_t len;
    char *todo;   /* WORK_MAX bytes of the caller's scratch */
    char *target; /* and the next WORK_MAX */
    char *spare;  /* and the last: where a requeue is built */
    uint32_t todo_len;
    uint32_t todo_at;
    uint32_t links;
};

static uint32_t length(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* Puts `head` (len bytes) then "/" then the rest of the queue as the new
 * queue: how a link's target takes the place of the component it was. */
static int requeue(struct walk *w, const char *head, uint32_t len) {
    char *next = w->spare;
    uint32_t n = 0, rest = w->todo_len - w->todo_at;
    if (len + 1u + rest + 1u > WORK_MAX) {
        return KERR_NAMETOOLONG;
    }
    for (uint32_t i = 0; i < len; i++) {
        next[n++] = head[i];
    }
    next[n++] = '/';
    for (uint32_t i = 0; i < rest; i++) {
        next[n++] = w->todo[w->todo_at + i];
    }
    for (uint32_t i = 0; i < n; i++) {
        w->todo[i] = next[i];
    }
    w->todo_len = n;
    w->todo_at = 0;
    return KERR_OK;
}

/* The next component off the queue: where it is and how long; 0 at the end.
 * Empty components ("//") are skipped. */
static uint32_t take(struct walk *w, const char **name) {
    uint32_t n = 0;
    while (w->todo_at < w->todo_len && w->todo[w->todo_at] == '/') {
        w->todo_at++;
    }
    *name = w->todo + w->todo_at;
    while (w->todo_at + n < w->todo_len && w->todo[w->todo_at + n] != '/') {
        n++;
    }
    w->todo_at += n;
    return n;
}

static bool more_to_come(const struct walk *w) {
    for (uint32_t i = w->todo_at; i < w->todo_len; i++) {
        if (w->todo[i] != '/') {
            return true;
        }
    }
    return false;
}

/* Drops the last component of what is resolved: `..`, at the root a no-op
 * as on Linux. */
static void pop(struct walk *w) {
    while (w->len > 1u && w->out[w->len - 1u] != '/') {
        w->len--;
    }
    if (w->len > 1u) {
        w->len--;
    }
    w->out[w->len] = '\0';
}

static int push(struct walk *w, const char *name, uint32_t n) {
    uint32_t need = (w->len > 1u ? 1u : 0u) + n + 1u;
    if (w->len + need > w->room) {
        return KERR_NAMETOOLONG;
    }
    if (w->len > 1u) {
        w->out[w->len++] = '/';
    }
    for (uint32_t i = 0; i < n; i++) {
        w->out[w->len++] = name[i];
    }
    w->out[w->len] = '\0';
    return KERR_OK;
}

/* Asks whether the component just pushed is a link, and if so takes it
 * back off and queues its target in its place. */
static int expand_if_link(struct walk *w, pathres_readlink_fn readlink, void *self) {
    char *target = w->target;
    uint32_t got = 0;
    int rc = readlink(self, w->out, target, WORK_MAX, &got);
    if (rc != KERR_OK) {
        /* Not a link (or not there, or on no filesystem at all): the walk
         * goes on, and the call itself reports whatever is wrong. */
        return KERR_OK;
    }
    if (++w->links > PATHRES_MAX_LINKS) {
        return KERR_LOOP;
    }
    pop(w);
    if (got > 0 && target[0] == '/') {
        w->len = 1u; /* an absolute target starts again from the root */
        w->out[1] = '\0';
    }
    return requeue(w, target, got);
}

static int step(struct walk *w, const char *name, uint32_t n, pathres_readlink_fn readlink,
                void *self) {
    int rc;
    if (n == 1u && name[0] == '.') {
        return KERR_OK;
    }
    if (n == 2u && name[0] == '.' && name[1] == '.') {
        pop(w);
        return KERR_OK;
    }
    rc = push(w, name, n);
    return rc != KERR_OK || !more_to_come(w) ? rc : expand_if_link(w, readlink, self);
}

int path_resolve(const char *in, char *out, uint32_t room, pathres_readlink_fn readlink,
                 void *self, char *scratch) {
    struct walk w;
    uint32_t n = length(in);
    const char *name;
    int rc = KERR_OK;
    if (in[0] != '/' || room < 2u) {
        return KERR_INVAL;
    }
    if (n >= WORK_MAX) {
        return KERR_NAMETOOLONG;
    }
    w = (struct walk){.out = out, .room = room, .len = 1u, .todo = scratch,
                      .target = scratch + WORK_MAX, .spare = scratch + 2u * WORK_MAX,
                      .todo_len = n};
    for (uint32_t i = 0; i < n; i++) {
        w.todo[i] = in[i];
    }
    out[0] = '/';
    out[1] = '\0';
    while (rc == KERR_OK && (n = take(&w, &name)) != 0) {
        rc = step(&w, name, n, readlink, self);
    }
    return rc;
}
