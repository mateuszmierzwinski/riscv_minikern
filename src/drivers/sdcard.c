#include "drivers/sdcard.h"

#include <stddef.h>

#include "core/errno.h"

/* CMD8's argument and the pattern it echoes back: 2.7–3.6 V, and a byte the
 * card must return unchanged. A floating bus reads as all ones, which is why
 * the pattern is checked rather than just the absence of an error. */
#define IF_COND_VOLTAGE 0x100u
#define IF_COND_PATTERN 0xaau

/* ACMD41's argument and the bits of its answer. */
#define OCR_VOLTAGE_WINDOW 0x00ff8000u
#define OCR_HIGH_CAPACITY 0x40000000u
#define OCR_POWERED_UP 0x80000000u

static int send(const struct sd_transport *t, uint8_t index, uint32_t argument, uint32_t kind,
                uint32_t *response) {
    return t->command(t->self, index, argument, kind, response);
}

/* An application command: CMD55 to say one is coming, then the command.
 *
 * **CMD55 carries the card's address**, and that is not a formality. While a
 * card is still powering up it has no address and zero is right; once it has
 * been selected it is in the transfer state and a CMD55 addressed to zero is
 * addressed to nobody — the card ignores it and the controller reports a
 * response timeout.
 *
 * The SDHCI driver this protocol was extracted from passed the address and
 * the extraction dropped it, which broke exactly one command: the bus-width
 * request, the only application command sent after the card is selected. The
 * card had already reported itself powered up and high-capacity, so the
 * symptom was a working initialisation that fell over on its last step. */
static int send_app(const struct sd_transport *t, uint32_t rca, uint8_t index, uint32_t argument,
                    uint32_t kind, uint32_t *response) {
    int rc = send(t, CMD_APP_COMMAND, rca << 16, SD_RESP_SHORT, NULL);
    return rc != KERR_OK ? rc : send(t, index, argument, kind, response);
}

/* Idle, then the modern-protocol handshake, then wait for the card to finish
 * powering up — which takes as long as it takes, so it is a poll with a
 * bound rather than a fixed delay. */
static int power_up(const struct sd_transport *t, struct sd_card *card) {
    uint32_t response = 0;
    int rc = send(t, CMD_GO_IDLE, 0, SD_RESP_NONE, NULL);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = send(t, CMD_SEND_IF_COND, IF_COND_VOLTAGE | IF_COND_PATTERN, SD_RESP_SHORT, &response);
    if (rc != KERR_OK || (response & 0xffu) != IF_COND_PATTERN) {
        return KERR_NOTSUP;
    }
    for (uint32_t tries = 0; tries < 300u; tries++) {
        /* **R3, which has no CRC.** See `SD_RESP_SHORT_NOCRC`. */
        /* No address yet: the card is in the idle state, where zero is the
         * only right answer. */
        rc = send_app(t, 0u, ACMD_SEND_OP_COND, OCR_VOLTAGE_WINDOW | OCR_HIGH_CAPACITY,
                      SD_RESP_SHORT_NOCRC, &response);
        card->polls = tries + 1u;
        card->last_ocr = response;
        if (rc != KERR_OK) {
            return rc;
        }
        if ((response & OCR_POWERED_UP) != 0) {
            card->high_capacity = (response & OCR_HIGH_CAPACITY) != 0;
            return KERR_OK;
        }
        /* **Paced.** The card is initialising its own supply and the
         * specification expects to be asked about once a millisecond; asking
         * as fast as a processor can issue commands is a different thing
         * entirely, and a card that is busy answers the next command with
         * nothing at all. */
        for (uint32_t spin = 0; spin < 300000u; spin++) {
            __asm__ volatile("" ::: "memory");
        }
    }
    return KERR_TIMEDOUT;
}

uint64_t sdcard_capacity_from_csd(const uint32_t csd[4]) {
    uint32_t version = csd[3] >> 30; /* CSD_STRUCTURE, bits 127:126 */
    if (version == 1u) {
        /* SDHC/SDXC: C_SIZE is bits 69:48 — the top six in `csd[2]` and the
         * rest in `csd[1]`. Capacity is (C_SIZE + 1) * 512 KiB, which is
         * (C_SIZE + 1) * 1024 blocks. */
        uint32_t c_size = ((csd[2] & 0x3fu) << 16) | (csd[1] >> 16);
        return ((uint64_t)c_size + 1u) * 1024u;
    }
    /* Standard capacity, up to 2 GiB: a geometry to multiply out rather than
     * a block count. C_SIZE is bits 73:62, C_SIZE_MULT 49:47, READ_BL_LEN
     * 83:80. Reading the high-capacity layout from one of these gives a
     * working driver that sees a fraction of the storage. */
    {
        uint32_t c_size = ((csd[2] & 0x3ffu) << 2) | (csd[1] >> 30);
        uint32_t mult = (csd[1] >> 15) & 0x7u;
        uint32_t read_len = (csd[2] >> 16) & 0xfu;
        return ((uint64_t)c_size + 1u) * (1ull << (mult + 2u)) * (1ull << read_len) /
               SD_BLOCK_BYTES;
    }
}

/* Gives the card an address, reads its capacity, selects it, and asks for
 * four-bit transfers. */
static int identify(const struct sd_transport *t, struct sd_card *card) {
    uint32_t csd[4] = {0};
    uint32_t response = 0;
    int rc = send(t, CMD_ALL_SEND_CID, 0, SD_RESP_LONG, NULL);
    rc = rc != KERR_OK ? rc : send(t, CMD_SEND_RELATIVE_ADDR, 0, SD_RESP_SHORT, &response);
    if (rc != KERR_OK) {
        return rc;
    }
    card->rca = response >> 16;
    rc = send(t, CMD_SEND_CSD, card->rca << 16, SD_RESP_LONG, csd);
    if (rc != KERR_OK) {
        return rc;
    }
    card->blocks = sdcard_capacity_from_csd(csd);
    rc = send(t, CMD_SELECT_CARD, card->rca << 16, SD_RESP_BUSY, NULL);
    rc = rc != KERR_OK ? rc : send(t, CMD_SET_BLOCKLEN, SD_BLOCK_BYTES, SD_RESP_SHORT, NULL);
    if (rc != KERR_OK) {
        return rc;
    }
    /* Four-bit transfers. The card must be told before the controller is, or
     * it answers the next command on one line while the controller listens
     * on four. */
    rc = send_app(t, card->rca, ACMD_SET_BUS_WIDTH, 2u, SD_RESP_SHORT, &response);
    if (rc == KERR_OK && t->run_faster != NULL) {
        rc = t->run_faster(t->self);
    }
    return rc;
}

int sdcard_bring_up(const struct sd_transport *transport, struct sd_card *out) {
    int rc;
    *out = (struct sd_card){0};
    rc = power_up(transport, out);
    return rc != KERR_OK ? rc : identify(transport, out);
}
