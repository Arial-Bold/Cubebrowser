/* packsamp.h - smaller ways to keep SoundFont samples in memory (used for
 * sounds that don't fit in audio memory), decoded as the music player streams each voice.
 *
 * BLOCK8: 8 bits per sample, with one shared scale for each 16 samples
 *   (about half the memory of 16-bit; on real SoundFonts around 46 dB signal to
 *   noise, which is hard to tell from the original). Any sample can be read
 *   directly.
 *
 * ADPCM: IMA ADPCM, 4 bits per sample (about a quarter of the memory; around
 *   25-30 dB, so some hiss on quiet, clean sounds). A checkpoint (the decoder's
 *   state) is kept every ADPCM_CK samples, so playing can start anywhere (a
 *   zone's start, or its loop start) by decoding at most ADPCM_CK - 1 samples
 *   from the checkpoint before it. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ADPCM_CK_SHIFT 8
#define ADPCM_CK (1u << ADPCM_CK_SHIFT)

typedef struct { int16_t pred; uint8_t idx, pad; } AdpcmCk;

static const int16_t adpcm_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
};
static const int8_t adpcm_idx_adj[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int adpcm_step(int *pred, int *idx, int code) {
    int step = adpcm_steps[*idx];
    int diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    int p = (code & 8) ? *pred - diff : *pred + diff;
    if (p > 32767) p = 32767;
    if (p < -32768) p = -32768;
    *pred = p;
    int i = *idx + adpcm_idx_adj[code];
    *idx = i < 0 ? 0 : (i > 88 ? 88 : i);
    return p;
}

/* ---- encoding (streamed: one sample at a time) */
typedef struct { uint8_t *data; AdpcmCk *ck; uint32_t n; int pred, idx; } AdpcmEnc;

static inline size_t adpcm_data_bytes(uint32_t frames) { return (frames + 1) / 2; }
static inline size_t adpcm_ck_count(uint32_t frames) { return (frames >> ADPCM_CK_SHIFT) + 1; }

static inline void adpcm_put(AdpcmEnc *e, int16_t s) {
    if ((e->n & (ADPCM_CK - 1)) == 0) {
        AdpcmCk *c = &e->ck[e->n >> ADPCM_CK_SHIFT];
        c->pred = (int16_t)e->pred;
        c->idx = (uint8_t)e->idx;
        c->pad = 0;
    }
    int step = adpcm_steps[e->idx], diff = s - e->pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) code |= 1;
    adpcm_step(&e->pred, &e->idx, code);   /* track exactly what the decoder will see */
    uint8_t *b = &e->data[e->n >> 1];
    if (e->n & 1) *b = (uint8_t)((*b & 0x0F) | (code << 4));
    else *b = (uint8_t)code;
    e->n++;
}

/* ---- decoding */
typedef struct { const uint8_t *data; const AdpcmCk *ck; uint32_t pos; int pred, idx; } AdpcmDec;

static inline void adpcm_seek(AdpcmDec *d, uint32_t pos) {
    const AdpcmCk *c = &d->ck[pos >> ADPCM_CK_SHIFT];
    d->pred = c->pred;
    d->idx = c->idx;
    d->pos = pos & ~(ADPCM_CK - 1);
    while (d->pos < pos) {
        int code = (d->data[d->pos >> 1] >> ((d->pos & 1) * 4)) & 15;
        adpcm_step(&d->pred, &d->idx, code);
        d->pos++;
    }
}

static inline void adpcm_read(AdpcmDec *d, int16_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int code = (d->data[d->pos >> 1] >> ((d->pos & 1) * 4)) & 15;
        out[i] = (int16_t)adpcm_step(&d->pred, &d->idx, code);
        d->pos++;
    }
}

/* ---- BLOCK8 */
#define BLK8_SHIFT 4
#define BLK8_N (1u << BLK8_SHIFT)

static inline size_t blk8_scale_count(uint32_t frames) { return (frames + BLK8_N - 1) >> BLK8_SHIFT; }

/* Packs n samples (a whole number of blocks, except at the very end). */
static inline void blk8_pack(const int16_t *in, uint32_t n, int8_t *data, uint8_t *scale) {
    for (uint32_t s = 0; s < n; s += BLK8_N) {
        uint32_t m = n - s < BLK8_N ? n - s : BLK8_N;
        int mx = 0;
        for (uint32_t i = 0; i < m; i++) { int v = in[s + i] < 0 ? -in[s + i] : in[s + i]; if (v > mx) mx = v; }
        int sh = 0;
        while ((mx >> sh) > 127) sh++;
        scale[s >> BLK8_SHIFT] = (uint8_t)sh;
        int half = sh ? 1 << (sh - 1) : 0;
        for (uint32_t i = 0; i < m; i++) {
            int v = in[s + i];
            int q = v >= 0 ? (v + half) >> sh : -((-v + half) >> sh);
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            data[s + i] = (int8_t)q;
        }
    }
}

static inline void blk8_read(const int8_t *data, const uint8_t *scale, uint32_t pos, int16_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, pos++) out[i] = (int16_t)(data[pos] * (1 << scale[pos >> BLK8_SHIFT]));
}
