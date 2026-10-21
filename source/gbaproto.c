/* gbaproto.c - see gbaproto.h. Plain C with no library calls, so it builds
 * for both the GameCube and the Game Boy Advance. */
#include "gbaproto.h"

void gbp_tx_reset(GbpTx *t) {
    t->head = t->tail = 0;
    t->seq = 0;
    t->sync_due = true;
    t->ask_due = false;
}

void gbp_tx_ask_sync(GbpTx *t) { t->ask_due = true; }

static int used(const GbpTx *t) { return (t->head - t->tail + GBP_TX_BYTES) % GBP_TX_BYTES; }
int gbp_tx_free(const GbpTx *t) { return GBP_TX_BYTES - 1 - used(t); }
bool gbp_tx_pending(const GbpTx *t) { return t->sync_due || t->ask_due || t->head != t->tail; }

static void push(GbpTx *t, uint8_t b) {
    t->buf[t->head] = b;
    t->head = (uint16_t)((t->head + 1) % GBP_TX_BYTES);
}

bool gbp_tx_put(GbpTx *t, int type, const void *data, int len) {
    if (len < 0 || len > 255 || gbp_tx_free(t) < len + 2) return false;
    push(t, (uint8_t)type);
    push(t, (uint8_t)len);
    const uint8_t *p = data;
    for (int i = 0; i < len; i++) push(t, p[i]);
    return true;
}

bool gbp_tx_text(GbpTx *t, int type, const uint8_t *prefix, int plen, const char *text) {
    uint8_t m[8 + GBP_TEXT_MAX];
    int n = 0;
    for (int i = 0; i < plen && n < 8; i++) m[n++] = prefix[i];
    for (int i = 0; text && text[i] && i < GBP_TEXT_MAX; i++) m[n++] = (uint8_t)text[i];
    return gbp_tx_put(t, type, m, n);
}

uint32_t gbp_tx_word(GbpTx *t) {
    if (t->ask_due) {
        t->ask_due = false;
        return 0xFE000000u;
    }
    if (t->sync_due) {
        t->sync_due = false;
        t->seq = 0;
        return 0xFF000000u;
    }
    if (t->head == t->tail) return 0;
    uint32_t w = 0;
    for (int i = 0; i < 3 && t->head != t->tail; i++) {
        w |= (uint32_t)t->buf[t->tail] << (8 * i);
        t->tail = (uint16_t)((t->tail + 1) % GBP_TX_BYTES);
    }
    t->seq = t->seq >= 253 ? 1 : t->seq + 1;
    return w | ((uint32_t)t->seq << 24);
}

void gbp_rx_reset(GbpRx *r) {
    r->seq = 0;
    r->lost = true;
    r->stage = 0;
}

static void feed(GbpRx *r, uint8_t b, GbpHandler h, void *ctx) {
    switch (r->stage) {
    case 0:                              /* type */
        if (b == 0) return;              /* padding */
        r->type = b;
        r->stage = 1;
        break;
    case 1:                              /* length */
        r->len = b;
        r->got = 0;
        if (b == 0) { r->stage = 0; h(ctx, r->type, r->msg, 0); }
        else r->stage = 2;
        break;
    default:
        r->msg[r->got++] = b;
        if (r->got == r->len) { r->stage = 0; h(ctx, r->type, r->msg, r->len); }
        break;
    }
}

bool gbp_rx_word(GbpRx *r, uint32_t w, GbpHandler h, void *ctx) {
    uint8_t s = (uint8_t)(w >> 24);
    if (s == 0) return true;             /* nothing */
    if (s == 0xFE) {                     /* the other side wants a sync word */
        h(ctx, GBP_REQ_SYNC, r->msg, 0);
        return true;
    }
    if (s == 0xFF) {                     /* sync */
        r->lost = false;
        r->seq = 0;
        r->stage = 0;
        return true;
    }
    if (r->lost) return true;            /* still waiting for a sync */
    if (s == r->seq) return true;        /* the same word read twice */
    uint8_t want = r->seq >= 253 ? 1 : r->seq + 1;
    if (s != want) { gbp_rx_reset(r); return false; }
    r->seq = s;
    for (int i = 0; i < 3; i++) feed(r, (uint8_t)(w >> (8 * i)), h, ctx);
    return true;
}

void gbp_state_pack(const GbpState *s, uint8_t *o) {
    o[0] = s->state; o[1] = s->flags; o[2] = s->volume; o[3] = s->sf_cur; o[4] = s->sf_count;
    gbp_put16(o + 5, s->cur); gbp_put16(o + 7, s->count); gbp_put16(o + 9, s->pos); gbp_put16(o + 11, s->len);
}

void gbp_state_unpack(GbpState *s, const uint8_t *i) {
    s->state = i[0]; s->flags = i[1]; s->volume = i[2]; s->sf_cur = i[3]; s->sf_count = i[4];
    s->cur = (uint16_t)gbp_get16(i + 5); s->count = (uint16_t)gbp_get16(i + 7);
    s->pos = (uint16_t)gbp_get16(i + 9); s->len = (uint16_t)gbp_get16(i + 11);
}
