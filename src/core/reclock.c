#include "core/reclock.h"

#include <stddef.h>

#include "core/errno.h"

struct entry {
    bool used;
    uint64_t key;
    struct reclock_range r;
};

static struct entry table[RECLOCK_MAX];

void reclock_reset(void) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        table[i].used = false;
    }
}

uint64_t reclock_identity(const char *path) {
    uint64_t h = 0xcbf29ce484222325ull; /* FNV-1a offset basis */
    for (uint32_t i = 0; path[i] != '\0'; i++) {
        h = (h ^ (uint8_t)path[i]) * 0x100000001b3ull;
    }
    return h == 0 ? 1u : h;
}

static bool overlaps(const struct reclock_range *r, uint64_t start, uint64_t end) {
    return r->start < end && start < r->end;
}

static bool owned_by(const struct entry *e, uint64_t key, uint32_t owner) {
    return e->used && e->key == key && e->r.owner == owner;
}

bool reclock_conflict(uint64_t key, uint32_t owner, uint32_t type, uint64_t start, uint64_t end,
                      struct reclock_range *out) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        const struct entry *e = &table[i];
        if (!e->used || e->key != key || e->r.owner == owner || !overlaps(&e->r, start, end)) {
            continue;
        }
        if (type == RECLOCK_WRITE || e->r.type == RECLOCK_WRITE) {
            if (out != NULL) {
                *out = e->r;
            }
            return true;
        }
    }
    return false;
}

static uint32_t free_slots(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        n += table[i].used ? 0u : 1u;
    }
    return n;
}

/* Whether cutting `[start, end)` out of owner's locks leaves two pieces of
 * one of them — the only case that needs a slot rather than freeing one. */
static bool splits_one(uint64_t key, uint32_t owner, uint64_t start, uint64_t end) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        if (owned_by(&table[i], key, owner) && table[i].r.start < start && table[i].r.end > end) {
            return true;
        }
    }
    return false;
}

static void add(uint64_t key, struct reclock_range r) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        if (!table[i].used) {
            table[i] = (struct entry){.used = true, .key = key, .r = r};
            return;
        }
    }
}

/* Removes `[start, end)` from owner's locks, keeping what lies either side.
 * A piece put back can land in a slot not yet visited; it lies outside the
 * range, so the loop passes over it. */
static void cut(uint64_t key, uint32_t owner, uint64_t start, uint64_t end) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        struct reclock_range r = table[i].r;
        if (!owned_by(&table[i], key, owner) || !overlaps(&r, start, end)) {
            continue;
        }
        table[i].used = false;
        if (r.start < start) {
            add(key, (struct reclock_range){r.start, start, r.type, owner});
        }
        if (r.end > end) {
            add(key, (struct reclock_range){end, r.end, r.type, owner});
        }
    }
}

int reclock_set(uint64_t key, uint32_t owner, uint32_t type, uint64_t start, uint64_t end) {
    uint32_t need;
    if (start >= end || type > RECLOCK_UNLOCK) {
        return KERR_INVAL;
    }
    if (type != RECLOCK_UNLOCK && reclock_conflict(key, owner, type, start, end, NULL)) {
        return KERR_AGAIN;
    }
    need = (splits_one(key, owner, start, end) ? 1u : 0u) + (type != RECLOCK_UNLOCK ? 1u : 0u);
    if (free_slots() < need) {
        return KERR_NOLCK;
    }
    cut(key, owner, start, end);
    if (type != RECLOCK_UNLOCK) {
        add(key, (struct reclock_range){start, end, type, owner});
    }
    return KERR_OK;
}

void reclock_release(uint64_t key, uint32_t owner) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        if (owned_by(&table[i], key, owner)) {
            table[i].used = false;
        }
    }
}

void reclock_release_owner(uint32_t owner) {
    for (uint32_t i = 0; i < RECLOCK_MAX; i++) {
        if (table[i].used && table[i].r.owner == owner) {
            table[i].used = false;
        }
    }
}

uint32_t reclock_count(void) {
    return RECLOCK_MAX - free_slots();
}
