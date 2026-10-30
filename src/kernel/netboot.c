#include "kernel/netboot.h"

#include <stdbool.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/net/netboot.h"
#include "kernel/mem.h"
#include "kernel/net.h"
#include "kernel/random.h"
#include "lib/sha256.h"

/* 64 MiB: the server's own limit, far above a root filesystem built for a
 * test, and under 1% of the board's memory. Taken at boot while memory is
 * unfragmented. */
#define IMAGE_CAPACITY (64u * 1024u * 1024u)
#define LEASE_WAIT_MS 15000u

static uint32_t bitmap[NETBOOT_BITMAP_WORDS(IMAGE_CAPACITY)];
static struct netboot client;
static uint8_t expected[32];
static bool expected_set;
static uint8_t trusted_key[32];
static bool key_set;

static uint64_t now_ms(void) {
    return timer_ns() / 1000000u;
}

/* What was fetched, with the digest the server named. */
static void report_image(const struct netboot *nb) {
    char ip[16], hex[65];
    sha256_format(nb->digest, hex);
    hex[16] = '\0'; /* the first 8 bytes are plenty for a boot log */
    kprintf("netboot: %u bytes from %s in %u blocks (sha256 %s...)\n", nb->image_len,
            ip4_format(nb->server, ip), nb->blocks, hex);
}

static void report_failure(const struct netboot *nb) {
    kprintf("netboot: failed: %s\n", netboot_error_text(nb->error));
    kprintf("netboot: %u probes, %u offers (%u refused), %u reads, %u blocks (%u refused), "
            "%u send errors (last %d)\n",
            nb->stats.probes_sent, nb->stats.offers_seen, nb->stats.offers_refused,
            nb->stats.reads_sent, nb->stats.blocks_stored, nb->stats.blocks_refused,
            nb->stats.send_errors, nb->stats.last_send_rc);
}

/* Drives the session to its end. No thread exists yet, so the wait polls:
 * interrupts open for an instant, the receive ring drained, the stack's
 * timers run (the same reasons as netfs_tcp's boot wait). */
static int run_session(struct netboot *nb, void (*poll)(void)) {
    bool announced = false;
    while (!netboot_finished(nb)) {
        netboot_tick(nb, now_ms());
        if (!announced && nb->state == NETBOOT_FETCHING) {
            char ip[16];
            announced = true;
            kprintf("netboot: server at %s offers %u bytes in %u blocks\n",
                    ip4_format(nb->server, ip), nb->image_len, nb->blocks);
        }
        __asm__ volatile("csrsi sstatus, 2\n\tnop\n\tcsrci sstatus, 2" : : : "memory");
        knet_poll_locked(poll);
    }
    netboot_stop(nb);
    if (nb->state != NETBOOT_READY) {
        report_failure(nb);
        return KERR_IO;
    }
    report_image(nb);
    return KERR_OK;
}

/* The board id on the wire: this board has no chip id the kernel reads, so
 * its MAC, zero-padded — enough for the server's per-board image names. */
static void board_id_of(const struct netif *nif, uint8_t out[MKNB_BOARD_ID]) {
    for (uint32_t i = 0; i < MKNB_BOARD_ID; i++) {
        out[i] = i < ETH_ALEN ? nif->mac[i] : 0u;
    }
}

static int begin(struct netif *nif, ip4_t pinned, uint8_t *buf) {
    uint8_t id[MKNB_BOARD_ID];
    uint32_t xid;
    char ip[16];
    int rc;
    board_id_of(nif, id);
    random_fill(&xid, sizeof xid);
    kprintf("netboot: looking for a server %s\n",
            pinned != 0 ? ip4_format(pinned, ip) : "on the local subnet (unicast sweep)");
    rc = netboot_start(&client, nif, buf, IMAGE_CAPACITY, bitmap, xid, nif->mac, id, pinned);
    if (rc == KERR_OK && expected_set) {
        netboot_expect_digest(&client, expected); /* after the start, which clears the session */
    }
    if (rc == KERR_OK && key_set) {
        netboot_require_signature(&client, trusted_key);
    }
    return rc;
}

static int unhex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* 64 hex characters into 32 bytes, or KERR_INVAL. */
static int parse_32(const char *hex, size_t len, uint8_t out[32]) {
    if (len != 64u) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < 32u; i++) {
        int hi = unhex(hex[i * 2u]), lo = unhex(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return KERR_INVAL;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return KERR_OK;
}

int knetboot_expect(const char *hex, size_t len) {
    int rc = parse_32(hex, len, expected);
    expected_set = rc == KERR_OK;
    return rc;
}

int knetboot_require_key(const char *hex, size_t len) {
    int rc = parse_32(hex, len, trusted_key);
    key_set = rc == KERR_OK;
    return rc;
}

int knetboot_fetch(ip4_t pinned, void (*poll)(void), phys_t *at, uint32_t *len) {
    phys_t phys = 0;
    int rc = knet_wait_address(LEASE_WAIT_MS);
    if (rc != KERR_OK) {
        kprintf("netboot: no address after %u s, giving up\n", LEASE_WAIT_MS / 1000u);
        return rc;
    }
    rc = mem_alloc_pages(IMAGE_CAPACITY / PAGE_SIZE, 1, &phys);
    if (rc != KERR_OK) {
        kprintf("netboot: cannot reserve %u MiB for the image\n", IMAGE_CAPACITY >> 20);
        return rc;
    }
    rc = begin(knet_interface(), pinned, (uint8_t *)mmu_phys_to_virt(phys));
    rc = rc != KERR_OK ? rc : run_session(&client, poll);
    if (rc != KERR_OK) {
        mem_free_pages(phys, IMAGE_CAPACITY / PAGE_SIZE);
        return rc;
    }
    *at = phys;
    *len = client.image_len;
    return KERR_OK;
}
