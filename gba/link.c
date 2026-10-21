/* link.c - see link.h. */
#include "link.h"
#include "player.h"
#include "gbaproto.h"
#include <stddef.h>

/* words received (filled by the interrupt, emptied by the main loop) */
#define RX_WORDS 1024
static EWRAM_BSS uint32_t rx_ring[RX_WORDS];
static volatile uint16_t rx_head, rx_tail;
/* words to send (filled by the main loop, emptied by the interrupt) */
#define TX_WORDS 64
static uint32_t tx_ring[TX_WORDS];
static volatile uint16_t tx_head, tx_tail;

static volatile uint32_t frame, last_heard;
static GbpTx tx;
static GbpRx rx;
static bool ever_ok;
static uint32_t last_req;

/* commands not yet acknowledged (sent again every half second) */
#define PENDING 8
static struct { uint8_t id, cmd; uint16_t arg; uint32_t at; } pend[PENDING];
static uint8_t next_id = 1;

static void put_cmd(int i) {
    uint8_t m[4] = { pend[i].id, pend[i].cmd, (uint8_t)pend[i].arg, (uint8_t)(pend[i].arg >> 8) };
    if (gbp_tx_put(&tx, GBP_CMD, m, 4)) pend[i].at = frame;
}

void link_init(void) {
    rx_head = rx_tail = tx_head = tx_tail = 0;
    gbp_tx_reset(&tx);
    gbp_rx_reset(&rx);
    frame = 0;
    last_heard = 0;
    ever_ok = false;
    for (int i = 0; i < PENDING; i++) pend[i].id = 0;
}

uint32_t link_first_word(void) { return gbp_tx_word(&tx); }   /* the sync word */

void link_irq_received(uint32_t w) {
    uint16_t n = (uint16_t)((rx_head + 1) % RX_WORDS);
    if (n != rx_tail) { rx_ring[rx_head] = w; rx_head = n; }
    last_heard = frame;
}

uint32_t link_irq_next(void) {
    last_heard = frame;
    if (tx_tail == tx_head) return 0;
    uint32_t w = tx_ring[tx_tail];
    tx_tail = (uint16_t)((tx_tail + 1) % TX_WORDS);
    return w;
}

static void on_message(void *ctx, int type, const uint8_t *d, int len) {
    (void)ctx;
    if (type == GBP_REQ_SYNC) {
        /* start our stream afresh: drop the words already queued (one store, so
         * safe against the interrupt), then a sync word */
        tx_tail = tx_head;
        gbp_tx_reset(&tx);
        for (int i = 0; i < PENDING; i++) if (pend[i].id) pend[i].at = 0;   /* send those again soon */
    } else if (type == GBP_ACK && len >= 1) {
        for (int i = 0; i < PENDING; i++) if (pend[i].id == d[0]) pend[i].id = 0;
    } else player_message(type, d, len);
}

void link_send_cmd(int cmd, int arg) {
    int slot = -1;
    for (int i = 0; i < PENDING && slot < 0; i++) if (!pend[i].id) slot = i;
    if (slot < 0) {   /* all waiting: drop the oldest */
        slot = 0;
        for (int i = 1; i < PENDING; i++) if (pend[i].at < pend[slot].at) slot = i;
    }
    pend[slot].id = next_id;
    next_id = next_id == 255 ? 1 : next_id + 1;
    pend[slot].cmd = (uint8_t)cmd;
    pend[slot].arg = (uint16_t)arg;
    put_cmd(slot);
}

int link_frame(void) {
    frame++;
    while (rx_tail != rx_head) {
        uint32_t w = rx_ring[rx_tail];
        rx_tail = (uint16_t)((rx_tail + 1) % RX_WORDS);
        gbp_rx_word(&rx, w, on_message, NULL);
    }
    bool alive = frame - last_heard < 90;   /* heard from the GameCube in the last 1.5 s */
    if (alive && rx.lost && frame - last_req > 30) {
        gbp_tx_ask_sync(&tx);
        last_req = frame;
    }
    if (alive && !rx.lost) ever_ok = true;
    for (int i = 0; i < PENDING; i++)
        if (pend[i].id && frame - pend[i].at > 30) put_cmd(i);
    if (alive && !rx.lost && frame % 60 == 0) {   /* songs missing from the list? ask again */
        int m = player_missing_song();
        if (m >= 0) { uint8_t b[2] = { (uint8_t)m, (uint8_t)(m >> 8) }; gbp_tx_put(&tx, GBP_WANT, b, 2); }
    }
    /* words to send */
    while (gbp_tx_pending(&tx)) {
        uint16_t n = (uint16_t)((tx_head + 1) % TX_WORDS);
        if (n == tx_tail) break;
        tx_ring[tx_head] = gbp_tx_word(&tx);
        tx_head = n;
    }
    if (!alive) return ever_ok ? LINK_LOST : LINK_WAIT;
    return ever_ok ? LINK_OK : LINK_WAIT;
}
