/* inflate.c - small streaming DEFLATE decoder (canonical Huffman, 32 KB window). */
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ CRC32 */

static uint32_t crc_table[256];
static int crc_ready;

uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------------------------------------------------------------- decoder */

#define WSIZE 32768u
#define WMASK (WSIZE - 1)
#define INBUF 32768

typedef struct { uint16_t counts[16]; uint16_t symbols[288]; } Tree;

typedef struct {
    InflateRead rd; void *ru;
    InflateWrite wr; void *wu;
    unsigned char in[INBUF];
    int inpos, inlen;
    int eof;
    uint32_t bitbuf;
    int bitcnt;
    unsigned char win[WSIZE];
    uint32_t wpos;            /* bytes produced, modulo 2^32 (only low bits index the window) */
    uint32_t flushed;         /* window position already handed to the writer */
    uint64_t total;
    uint32_t crc;
    int err;
    Tree lt, dt;
} St;

static int next_byte(St *s) {
    if (s->inpos >= s->inlen) {
        if (s->eof) { s->err = -2; return 0; }
        int n = s->rd(s->ru, s->in, INBUF);
        if (n <= 0) { s->eof = 1; s->err = -2; return 0; }
        s->inlen = n;
        s->inpos = 0;
    }
    return s->in[s->inpos++];
}

static unsigned getbits(St *s, int n) {
    while (s->bitcnt < n) {
        s->bitbuf |= (uint32_t)next_byte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    unsigned v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void flush(St *s) {
    uint32_t pending = s->wpos - s->flushed;
    if (!pending || s->err) return;
    uint32_t start = s->flushed & WMASK;
    s->crc = crc32_update(s->crc, s->win + start, pending);
    if (s->wr(s->wu, s->win + start, pending) < 0) s->err = -3;
    s->total += pending;
    s->flushed = s->wpos;
}

static inline void put(St *s, unsigned char c) {
    s->win[s->wpos & WMASK] = c;
    s->wpos++;
    if ((s->wpos & WMASK) == 0) flush(s);   /* window wrapped: hand the full 32 KB on */
}

static void build(Tree *t, const unsigned char *lengths, int num) {
    uint16_t offs[16];
    memset(t->counts, 0, sizeof(t->counts));
    for (int i = 0; i < num; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    uint16_t sum = 0;
    for (int i = 0; i < 16; i++) { offs[i] = sum; sum += t->counts[i]; }
    for (int i = 0; i < num; i++)
        if (lengths[i]) t->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

static int decode(St *s, const Tree *t) {
    int base = 0, offs = 0;
    for (int len = 1; len < 16; len++) {
        offs = 2 * offs + (int)getbits(s, 1);
        if (offs < t->counts[len]) return t->symbols[base + offs];
        base += t->counts[len];
        offs -= t->counts[len];
    }
    s->err = -1;
    return 0;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_bits[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_bits[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                       11, 11, 12, 12, 13, 13 };

static int block_data(St *s) {
    for (;;) {
        int sym = decode(s, &s->lt);
        if (s->err) return s->err;
        if (sym < 256) { put(s, (unsigned char)sym); if (s->err) return s->err; continue; }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return s->err = -1;
        unsigned len = len_base[sym] + getbits(s, len_bits[sym]);
        int ds = decode(s, &s->dt);
        if (s->err) return s->err;
        if (ds >= 30) return s->err = -1;
        unsigned dist = dist_base[ds] + getbits(s, dist_bits[ds]);
        if (s->err) return s->err;
        uint64_t produced = s->total + (uint64_t)(s->wpos - s->flushed);
        if (dist > produced) return s->err = -1;   /* refers to data before the start */
        for (unsigned i = 0; i < len; i++) {
            put(s, s->win[(s->wpos - dist) & WMASK]);
            if (s->err) return s->err;
        }
    }
}

static int block_stored(St *s) {
    s->bitbuf = 0;   /* skip to the next byte boundary */
    s->bitcnt = 0;
    unsigned len = (unsigned)next_byte(s);
    len |= (unsigned)next_byte(s) << 8;
    unsigned nlen = (unsigned)next_byte(s);
    nlen |= (unsigned)next_byte(s) << 8;
    if (s->err) return s->err;
    if ((len ^ 0xFFFFu) != nlen) return s->err = -1;
    while (len--) {
        put(s, (unsigned char)next_byte(s));
        if (s->err) return s->err;
    }
    return 0;
}

static void fixed_trees(St *s) {
    unsigned char l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    build(&s->lt, l, 288);
    unsigned char d[30];
    for (int i = 0; i < 30; i++) d[i] = 5;
    build(&s->dt, d, 30);
}

static int dynamic_trees(St *s) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    unsigned hlit = getbits(s, 5) + 257, hdist = getbits(s, 5) + 1, hclen = getbits(s, 4) + 4;
    if (s->err || hlit > 286 || hdist > 30) return s->err ? s->err : (s->err = -1);
    unsigned char cl[19] = { 0 };
    for (unsigned i = 0; i < hclen; i++) cl[order[i]] = (unsigned char)getbits(s, 3);
    Tree ct;
    build(&ct, cl, 19);
    unsigned char lengths[320];
    unsigned n = 0;
    while (n < hlit + hdist) {
        int sym = decode(s, &ct);
        if (s->err) return s->err;
        unsigned rep = 0;
        unsigned char val = 0;
        if (sym < 16) { lengths[n++] = (unsigned char)sym; continue; }
        if (sym == 16) { if (!n) return s->err = -1; val = lengths[n - 1]; rep = 3 + getbits(s, 2); }
        else if (sym == 17) rep = 3 + getbits(s, 3);
        else rep = 11 + getbits(s, 7);
        if (n + rep > hlit + hdist) return s->err = -1;
        while (rep--) lengths[n++] = val;
    }
    if (lengths[256] == 0) return s->err = -1;   /* no end-of-block code */
    build(&s->lt, lengths, (int)hlit);
    build(&s->dt, lengths + hlit, (int)hdist);
    return s->err;
}

int inflate_stream(InflateRead rd, void *ru, InflateWrite wr, void *wu, uint64_t *out_bytes, uint32_t *out_crc) {
    St *s = calloc(1, sizeof(St));
    if (!s) return -4;
    s->rd = rd; s->ru = ru; s->wr = wr; s->wu = wu;
    int final = 0;
    while (!final && !s->err) {
        final = (int)getbits(s, 1);
        unsigned type = getbits(s, 2);
        if (s->err) break;
        if (type == 0) block_stored(s);
        else if (type == 1) { fixed_trees(s); block_data(s); }
        else if (type == 2) { if (!dynamic_trees(s)) block_data(s); }
        else s->err = -1;
    }
    if (!s->err) flush(s);
    int err = s->err;
    if (out_bytes) *out_bytes = s->total;
    if (out_crc) *out_crc = s->crc;
    free(s);
    return err;
}
