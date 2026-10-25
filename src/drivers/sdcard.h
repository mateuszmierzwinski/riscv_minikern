/* sdcard — the SD protocol, with no controller in it.
 *
 * The card's side of the conversation is the same whatever host controller is
 * underneath: go idle, ask whether it understands the modern protocol, wait
 * for it to finish powering up, give it an address, read its capacity, select
 * it, widen the bus. What differs is only *how a command is sent*.
 *
 * That distinction became load-bearing when a real **VisionFive 2** turned
 * out not to have the controller this project had been told it had. The
 * documentation says Cadence SDHCI; the board's own device tree says
 * `starfive,jh7110-mmc` with `biu`/`ciu` clocks and a `fifo-depth` — a
 * Synopsys DesignWare mobile-storage controller, a different register set
 * entirely. The SDHCI driver written against an emulator's model was simply
 * the wrong driver for the hardware.
 *
 * So the protocol lives here, behind a transport port, and there are two
 * transports: `sdhci.c` for what an emulator models and `dwmmc.c` for what
 * the board has. Writing the sequence twice would have been the obvious
 * thing and the wrong one.
 */
#ifndef MINIKERN_DRIVERS_SDCARD_H
#define MINIKERN_DRIVERS_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

/* What a command expects back. A transport translates these into whatever
 * its own registers call them. */
#define SD_RESP_NONE 0u
#define SD_RESP_SHORT 1u  /* 48-bit with a CRC: one word of answer */
#define SD_RESP_LONG 2u   /* 136-bit with a CRC: the CID or the CSD */
#define SD_RESP_BUSY 3u   /* 48-bit with a CRC, and the card holds the line */

/* 48-bit **without** a CRC, which is what the OCR (an R3 response) is: the
 * card sends reserved bits where a CRC would go.
 *
 * It needs its own kind because a controller that is told to check the CRC
 * of a response that has none reports a CRC failure every time. On an
 * emulator this is invisible — QEMU's card model does not compute response
 * CRCs, so checking them always passes — and on a real VisionFive 2 it
 * stopped the card exactly one command into powering up: CMD8 echoed its
 * pattern, and then ACMD41 came back `response-crc`. That one distinction is
 * the difference between a card that works and a card that is reported
 * absent. */
#define SD_RESP_SHORT_NOCRC 4u

/* The commands this protocol uses, by their numbers. */
#define CMD_GO_IDLE 0u
#define CMD_ALL_SEND_CID 2u
#define CMD_SEND_RELATIVE_ADDR 3u
#define CMD_SELECT_CARD 7u
#define CMD_SEND_IF_COND 8u
#define CMD_SEND_CSD 9u
#define CMD_SET_BLOCKLEN 16u
#define CMD_READ_SINGLE 17u
#define CMD_READ_MULTIPLE 18u
#define CMD_STOP_TRANSMISSION 12u
/* `SEND_STATUS`: the card's own R1 status word. Worth having because it is
 * the card's answer to "why did you refuse that" — the write-protect
 * violation and the current state both live in it, and a driver guessing at
 * either from a controller-side timeout is guessing. */
#define CMD_SEND_STATUS 13u
#define CMD_WRITE_SINGLE 24u
#define CMD_WRITE_MULTIPLE 25u
#define CMD_APP_COMMAND 55u
#define ACMD_SET_BUS_WIDTH 6u
#define ACMD_SEND_OP_COND 41u

#define SD_BLOCK_BYTES 512u

/* How a transport is asked to do things. `self` is its own state. */
struct sd_transport {
    void *self;
    /* Sends one command and waits for it to complete. `response` receives
     * one word for a short answer and four for a long one; NULL when the
     * caller does not want it.
     *
     * **A long response is spec-aligned**: `response[3]` holds the card's
     * bits 127:96 and `response[0]` its bits 31:0, so a field documented at
     * bit B is at bit B of this array. A transport whose registers present
     * it otherwise is the one that normalises it — and SDHCI's do, which is
     * the whole reason this is written down. */
    int (*command)(void *self, uint8_t index, uint32_t argument, uint32_t kind,
                   uint32_t *response);
    /* Moves one block, after a read or write command has been sent. */
    int (*move_block)(void *self, bool reading, void *buf);
    /* The card is addressed and ready: widen the bus to four bits and raise
     * the clock from the identification rate to the operating one. NULL when
     * a transport has nothing to change. */
    int (*run_faster)(void *self);
};

/* What was found. */
struct sd_card {
    uint64_t blocks;      /* 512-byte blocks */
    uint32_t rca;         /* the address the card answers to */
    bool high_capacity;   /* addressed by block rather than by byte */
    /* What the card last said about its power state, and how many times it
     * was asked. Kept because "the card never finished powering up" and
     * "the card stopped answering half way through" are different faults
     * with the same symptom, and the OCR tells them apart. */
    uint32_t last_ocr;
    uint32_t polls;
};

/* Takes a powered controller through the card's identification sequence.
 *
 * KERR_NOTSUP when nothing answers or the card is older than the modern
 * protocol, KERR_TIMEDOUT when it never finishes powering up. */
int sdcard_bring_up(const struct sd_transport *transport, struct sd_card *out);

/* Reads the card's capacity out of a spec-aligned CSD.
 *
 * Every field is at the bit the specification gives it, which is a contract
 * the transport upholds rather than a fact about controllers. **They do not
 * agree.** An SDHCI controller presents the 136-bit response with its
 * trailing CRC byte dropped, so every field lands 8 bits low; a DesignWare
 * controller presents the 128-bit payload as it is. Reading one layout from
 * the other does not fail — it gives a plausible number: a 14.5 GiB card
 * read with the shift applied reported **465 blocks**, and a 64 MiB card
 * read without it reported 30 GiB and accepted a write past its end.
 *
 * So the shift belongs to the controller that needs it, and this function
 * reads the documented positions. Exposed so a test can check the
 * arithmetic without a controller of either kind. */
uint64_t sdcard_capacity_from_csd(const uint32_t csd[4]);

#endif
