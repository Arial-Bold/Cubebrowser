/* instruments.c - see instruments.h. The SoundFont reader follows the
 * SoundFont 2.01 layout: presets point at instruments, instruments at
 * samples, and generators at both levels set ranges, tuning, loudness and
 * the volume envelope. Modulators, filters and LFOs are not used. */
#include "instruments.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GEKKO
#include <malloc.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===================================================== SoundFont reader */

#define SF2_MAX_BYTES (24u * 1024u * 1024u)

typedef struct {
    unsigned char *file;          /* the whole file */
    size_t file_len;
    int16_t *smpl;                /* sample data inside file, in console byte order */
    uint32_t nsmpl;
    const unsigned char *phdr, *pbag, *pgen, *inst, *ibag, *igen, *shdr;
    uint32_t nphdr, npbag, npgen, ninst, nibag, nigen, nshdr;
    char name[64];
} Sf2;

static Sf2 sf;
static bool sf_loaded;

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool host_big_endian(void) {
    const uint16_t one = 1;
    return *(const unsigned char *)&one == 0;
}

void instruments_unload_sf2(void) {
    if (sf_loaded) free(sf.file);
    memset(&sf, 0, sizeof(sf));
    sf_loaded = false;
}

bool instruments_have_sf2(void) { return sf_loaded; }

const char *instruments_name(void) { return sf_loaded ? sf.name : "Built-in"; }

/* Finds the pdta sub-chunks inside a LIST. */
static void scan_pdta(Sf2 *s, const unsigned char *p, const unsigned char *end) {
    while (p + 8 <= end) {
        uint32_t cl = rd32(p + 4);
        const unsigned char *d = p + 8;
        if (cl > (uint32_t)(end - d)) cl = (uint32_t)(end - d);
#define TAKE(tag, ptr, cnt, sz) if (!memcmp(p, tag, 4)) { s->ptr = d; s->cnt = cl / (sz); }
        TAKE("phdr", phdr, nphdr, 38)
        TAKE("pbag", pbag, npbag, 4)
        TAKE("pgen", pgen, npgen, 4)
        TAKE("inst", inst, ninst, 22)
        TAKE("ibag", ibag, nibag, 4)
        TAKE("igen", igen, nigen, 4)
        TAKE("shdr", shdr, nshdr, 46)
#undef TAKE
        p = d + cl + (cl & 1);
    }
}

bool instruments_load_sf2(const char *path, char *err, size_t errlen) {
    instruments_unload_sf2();
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "couldn't open the SoundFont"); return false; }
    fseek(f, 0, SEEK_END);
    long fl = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fl < 64) { fclose(f); snprintf(err, errlen, "the SoundFont file is empty or damaged"); return false; }
    if ((unsigned long)fl > SF2_MAX_BYTES) {
        fclose(f);
        snprintf(err, errlen, "the SoundFont is too big (%ld MB, most this can hold is %u MB)", fl >> 20, SF2_MAX_BYTES >> 20);
        return false;
    }
#ifdef GEKKO
    unsigned char *buf = memalign(32, (size_t)fl);
#else
    unsigned char *buf = malloc((size_t)fl);
#endif
    if (!buf) { fclose(f); snprintf(err, errlen, "not enough memory for this SoundFont (%ld MB)", fl >> 20); return false; }
    size_t got = 0;
    while (got < (size_t)fl) {
        size_t r = fread(buf + got, 1, (size_t)fl - got > 65536 ? 65536 : (size_t)fl - got, f);
        if (r == 0) break;
        got += r;
    }
    fclose(f);
    if (got != (size_t)fl || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "sfbk", 4)) {
        free(buf);
        snprintf(err, errlen, "not a SoundFont 2 file");
        return false;
    }
    Sf2 s;
    memset(&s, 0, sizeof(s));
    s.file = buf;
    s.file_len = got;
    const unsigned char *p = buf + 12, *end = buf + got;
    while (p + 8 <= end) {
        uint32_t cl = rd32(p + 4);
        const unsigned char *d = p + 8;
        if (cl > (uint32_t)(end - d)) cl = (uint32_t)(end - d);
        if (!memcmp(p, "LIST", 4) && cl >= 4) {
            const unsigned char *q = d + 4, *qe = d + cl;
            if (!memcmp(d, "INFO", 4)) {
                while (q + 8 <= qe) {
                    uint32_t l2 = rd32(q + 4);
                    if (l2 > (uint32_t)(qe - q - 8)) l2 = (uint32_t)(qe - q - 8);
                    if (!memcmp(q, "INAM", 4)) {
                        size_t k = 0;
                        for (uint32_t i = 0; i < l2 && q[8 + i] && k + 1 < sizeof(s.name); i++)
                            if (q[8 + i] >= 32 && q[8 + i] < 127) s.name[k++] = (char)q[8 + i];
                        s.name[k] = 0;
                    }
                    q += 8 + l2 + (l2 & 1);
                }
            } else if (!memcmp(d, "sdta", 4)) {
                while (q + 8 <= qe) {
                    uint32_t l2 = rd32(q + 4);
                    if (l2 > (uint32_t)(qe - q - 8)) l2 = (uint32_t)(qe - q - 8);
                    if (!memcmp(q, "smpl", 4)) {
                        /* samples must be 2-byte aligned to use in place */
                        unsigned char *sp = (unsigned char *)q + 8;
                        if (((uintptr_t)sp & 1)) { memmove(sp - 1, sp, l2); sp--; }
                        s.smpl = (int16_t *)sp;
                        s.nsmpl = l2 / 2;
                    }
                    q += 8 + l2 + (l2 & 1);
                }
            } else if (!memcmp(d, "pdta", 4)) {
                scan_pdta(&s, q, qe);
            }
        }
        p = d + cl + (cl & 1);
    }
    if (!s.smpl || !s.nsmpl || s.nphdr < 2 || s.npbag < 1 || s.ninst < 2 || s.nibag < 1 || s.nshdr < 2) {
        free(buf);
        snprintf(err, errlen, "this SoundFont is damaged or uses a format this player can't read");
        return false;
    }
    if (host_big_endian())
        for (uint32_t i = 0; i < s.nsmpl; i++) {
            uint16_t v = (uint16_t)s.smpl[i];
            s.smpl[i] = (int16_t)(uint16_t)((v >> 8) | (v << 8));
        }
    if (!s.name[0]) {
        const char *base = strrchr(path, '/');
        snprintf(s.name, sizeof(s.name), "%s", base ? base + 1 : path);
    }
    sf = s;
    sf_loaded = true;
    return true;
}

/* generator numbers used here */
enum {
    G_START = 0, G_END = 1, G_LSTART = 2, G_LEND = 3, G_START_C = 4, G_END_C = 12, G_PAN = 17,
    G_ATTACK = 34, G_HOLD = 35, G_DECAY = 36, G_SUSTAIN = 37, G_RELEASE = 38, G_INSTRUMENT = 41,
    G_KEYRANGE = 43, G_VELRANGE = 44, G_LSTART_C = 45, G_KEYNUM = 46, G_ATTEN = 48, G_LEND_C = 50,
    G_COARSE = 51, G_FINE = 52, G_SAMPLE = 53, G_MODES = 54, G_SCALE = 56, G_EXCL = 57, G_ROOT = 58,
    G_COUNT = 61
};

typedef struct { int16_t v[G_COUNT]; bool set[G_COUNT]; } Gens;

static void gens_inst_defaults(Gens *g) {
    memset(g, 0, sizeof(*g));
    g->v[G_ATTACK] = g->v[G_HOLD] = g->v[G_DECAY] = g->v[G_RELEASE] = -12000;
    g->v[G_KEYRANGE] = (int16_t)(127 << 8);   /* lo 0, hi 127 */
    g->v[G_VELRANGE] = (int16_t)(127 << 8);
    g->v[G_SCALE] = 100;
    g->v[G_ROOT] = -1;
    g->v[G_KEYNUM] = -1;
}

static void gens_preset_defaults(Gens *g) {
    memset(g, 0, sizeof(*g));
    g->v[G_KEYRANGE] = (int16_t)(127 << 8);
    g->v[G_VELRANGE] = (int16_t)(127 << 8);
}

/* Applies the generators of bag [b] (gens list gen with count ngen, bag table bag). */
static void apply_bag(Gens *g, const unsigned char *bag, uint32_t nbag, const unsigned char *gen, uint32_t ngen, uint32_t b) {
    if (b + 1 >= nbag + 1) return;
    uint32_t g0 = rd16(bag + b * 4), g1 = (b + 1 < nbag) ? rd16(bag + (b + 1) * 4) : ngen;
    if (g1 > ngen) g1 = ngen;
    for (uint32_t i = g0; i < g1; i++) {
        uint16_t op = rd16(gen + i * 4);
        if (op >= G_COUNT) continue;
        g->v[op] = (int16_t)rd16(gen + i * 4 + 2);
        g->set[op] = true;
    }
}

/* Does bag b end with the given terminal generator (instrument / sampleID)? */
static bool bag_has(const unsigned char *bag, uint32_t nbag, const unsigned char *gen, uint32_t ngen, uint32_t b, int op) {
    uint32_t g0 = rd16(bag + b * 4), g1 = (b + 1 < nbag) ? rd16(bag + (b + 1) * 4) : ngen;
    if (g1 > ngen) g1 = ngen;
    for (uint32_t i = g0; i < g1; i++)
        if (rd16(gen + i * 4) == op) return true;
    return false;
}

static bool in_range(int16_t r, int v) {
    int lo = (uint16_t)r & 0xFF, hi = ((uint16_t)r >> 8) & 0xFF;
    return v >= lo && v <= hi;
}

static float tc_seconds(int tc) { return tc <= -12000 ? 0.0f : powf(2.0f, tc / 1200.0f); }

static int find_preset(int bank, int prog) {
    for (uint32_t i = 0; i + 1 < sf.nphdr; i++) {   /* the last header is the terminator */
        const unsigned char *h = sf.phdr + i * 38;
        if (rd16(h + 20) == prog && rd16(h + 22) == bank) return (int)i;
    }
    return -1;
}

static bool make_zone(const Gens *pz, const Gens *iz, int key, Zone *z) {
    int sid = iz->v[G_SAMPLE];
    if (sid < 0 || (uint32_t)sid + 1 >= sf.nshdr) return false;
    const unsigned char *sh = sf.shdr + (uint32_t)sid * 46;
    if (rd16(sh + 44) & 0x8000) return false;   /* ROM sample */
    int32_t v[G_COUNT];
    for (int i = 0; i < G_COUNT; i++) v[i] = iz->v[i] + (pz ? pz->v[i] : 0);
    int64_t start = (int64_t)rd32(sh + 20) + iz->v[G_START] + (int64_t)iz->v[G_START_C] * 32768;
    int64_t end = (int64_t)rd32(sh + 24) + iz->v[G_END] + (int64_t)iz->v[G_END_C] * 32768;
    int64_t ls = (int64_t)rd32(sh + 28) + iz->v[G_LSTART] + (int64_t)iz->v[G_LSTART_C] * 32768;
    int64_t le = (int64_t)rd32(sh + 32) + iz->v[G_LEND] + (int64_t)iz->v[G_LEND_C] * 32768;
    if (start < 0) start = 0;
    if (end > (int64_t)sf.nsmpl) end = sf.nsmpl;
    if (end - start < 8) return false;
    memset(z, 0, sizeof(*z));
    z->data = sf.smpl + start;
    z->len = (uint32_t)(end - start);
    int modes = iz->v[G_MODES] & 3;
    if ((modes == 1 || modes == 3) && ls >= start && le <= end && le - ls >= 4) {
        z->loop = true;
        z->loop_start = (uint32_t)(ls - start);
        z->loop_end = (uint32_t)(le - start);
    }
    z->rate = rd32(sh + 36);
    if (z->rate < 400 || z->rate > 200000) z->rate = 22050;
    int root = iz->v[G_ROOT] >= 0 ? iz->v[G_ROOT] : sh[40];
    if (root > 127) root = 60;
    z->root = (float)root;
    z->tune = (float)v[G_COARSE] + ((float)v[G_FINE] + (float)(int8_t)sh[41]) / 100.0f;
    z->scale = v[G_SCALE] / 100.0f;
    int atten = v[G_ATTEN];
    if (atten < 0) atten = 0;
    if (atten > 1440) atten = 1440;
    z->gain = powf(10.0f, -atten * 0.4f / 200.0f);
    float pan = v[G_PAN] / 500.0f;
    z->pan = pan < -1 ? -1 : pan > 1 ? 1 : pan;
    z->attack = tc_seconds(v[G_ATTACK]);
    z->hold = tc_seconds(v[G_HOLD]);
    z->decay = tc_seconds(v[G_DECAY]);
    int sus = v[G_SUSTAIN];
    if (sus < 0) sus = 0;
    z->sustain = sus >= 1000 ? 0.0f : powf(10.0f, -sus / 200.0f);
    z->release = tc_seconds(v[G_RELEASE]);
    if (z->release > 8) z->release = 8;
    if (z->attack > 8) z->attack = 8;
    z->exclusive = iz->v[G_EXCL];
    (void)key;
    return true;
}

static int sf2_find(int bank, int prog, bool drums, int key, int vel, Zone *out, int max) {
    int pi = drums ? find_preset(128, prog) : find_preset(bank, prog);
    if (pi < 0 && drums) pi = find_preset(128, 0);
    if (pi < 0 && !drums) pi = find_preset(0, prog);
    if (pi < 0 && !drums) pi = find_preset(bank, 0);
    if (pi < 0) pi = drums ? -1 : find_preset(0, 0);
    if (pi < 0) return 0;
    const unsigned char *h = sf.phdr + (uint32_t)pi * 38;
    uint32_t b0 = rd16(h + 24), b1 = rd16(h + 38 + 24);
    if (b1 > sf.npbag) b1 = sf.npbag;
    int n = 0;
    Gens pglobal;
    gens_preset_defaults(&pglobal);
    for (uint32_t b = b0; b < b1 && n < max; b++) {
        if (!bag_has(sf.pbag, sf.npbag, sf.pgen, sf.npgen, b, G_INSTRUMENT)) {
            if (b == b0) apply_bag(&pglobal, sf.pbag, sf.npbag, sf.pgen, sf.npgen, b);
            continue;
        }
        Gens pz = pglobal;
        apply_bag(&pz, sf.pbag, sf.npbag, sf.pgen, sf.npgen, b);
        if (!in_range(pz.v[G_KEYRANGE], key) || !in_range(pz.v[G_VELRANGE], vel)) continue;
        int ii = pz.v[G_INSTRUMENT];
        if (ii < 0 || (uint32_t)ii + 1 >= sf.ninst) continue;
        const unsigned char *ih = sf.inst + (uint32_t)ii * 22;
        uint32_t ib0 = rd16(ih + 20), ib1 = rd16(ih + 22 + 20);
        if (ib1 > sf.nibag) ib1 = sf.nibag;
        Gens iglobal;
        gens_inst_defaults(&iglobal);
        /* preset-level ranges and the instrument choice aren't added on */
        pz.v[G_KEYRANGE] = pz.v[G_VELRANGE] = 0;
        pz.v[G_INSTRUMENT] = 0;
        for (uint32_t c = ib0; c < ib1 && n < max; c++) {
            if (!bag_has(sf.ibag, sf.nibag, sf.igen, sf.nigen, c, G_SAMPLE)) {
                if (c == ib0) apply_bag(&iglobal, sf.ibag, sf.nibag, sf.igen, sf.nigen, c);
                continue;
            }
            Gens iz = iglobal;
            apply_bag(&iz, sf.ibag, sf.nibag, sf.igen, sf.nigen, c);
            if (!in_range(iz.v[G_KEYRANGE], key) || !in_range(iz.v[G_VELRANGE], vel)) continue;
            /* additive preset values; sample-address offsets are instrument-only */
            Gens add = pz;
            add.v[G_START] = add.v[G_END] = add.v[G_LSTART] = add.v[G_LEND] = 0;
            add.v[G_START_C] = add.v[G_END_C] = add.v[G_LSTART_C] = add.v[G_LEND_C] = 0;
            add.v[G_SAMPLE] = add.v[G_MODES] = add.v[G_EXCL] = add.v[G_ROOT] = add.v[G_KEYNUM] = 0;
            if (make_zone(&add, &iz, key, &out[n])) n++;
        }
    }
    return n;
}

/* ================================================== built-in instruments */

#define CYC_A 256   /* samples per cycle for low notes */
#define CYC_B 64
#define CYC_C 32
#define DRUM_RATE 32000

typedef struct {
    float harm[16];
    float attack, decay, sustain, release;
} Family;

/* one per group of 8 General MIDI programs */
static const Family families[16] = {
    { { 1, .5f, .33f, .25f, .2f, .12f, .08f, .05f }, .002f, 1.6f, 0, .35f },             /* piano */
    { { 1, 0, 0, .45f, 0, 0, 0, .2f }, .001f, 1.0f, 0, .4f },                            /* chromatic percussion */
    { { 1, 1, .6f, 0, .4f, 0, 0, .3f }, .01f, 1, 1, .08f },                              /* organ */
    { { 1, .6f, .4f, .3f, .25f, .2f, .15f, .1f }, .002f, 1.3f, 0, .25f },                /* guitar */
    { { 1, .5f, .25f, .12f, .06f }, .004f, .8f, .45f, .12f },                            /* bass */
    { { 1, .5f, .33f, .25f, .2f, .17f, .14f, .12f, .11f, .1f }, .12f, 1, .9f, .35f },     /* strings */
    { { 1, .5f, .33f, .25f, .2f, .17f, .14f, .12f }, .18f, 1, .9f, .45f },               /* ensemble */
    { { 1, .58f, .42f, .33f, .28f, .24f, .21f, .19f, .17f, .15f }, .035f, 1, .85f, .15f }, /* brass */
    { { 1, 0, .33f, 0, .2f, 0, .14f, 0, .11f }, .025f, 1, .9f, .1f },                     /* reed */
    { { 1, .2f, .05f }, .05f, 1, .9f, .15f },                                            /* pipe */
    { { 1, 0, .33f, 0, .2f, 0, .14f, 0, .11f, 0, .09f, 0, .08f }, .004f, 1, 1, .08f },    /* synth lead */
    { { 1, .4f, .2f, .12f, .08f, .05f }, .35f, 1, .9f, .9f },                            /* synth pad */
    { { 1, .4f, .2f, .12f, .08f, .05f }, .2f, 1, .8f, .8f },                             /* synth effects */
    { { 1, .6f, .4f, .3f, .25f, .2f, .15f, .1f }, .002f, 1.1f, 0, .25f },                /* ethnic */
    { { 1, 0, 0, .45f, 0, 0, 0, .2f }, .001f, .8f, 0, .3f },                             /* percussive */
    { { 1, .4f, .2f, .12f, .08f, .05f }, .1f, 1, .8f, .6f },                             /* sound effects */
};

static int16_t *cycles[16][3];
static const int cyc_len[3] = { CYC_A, CYC_B, CYC_C };
static const int cyc_root[3] = { 60, 72, 96 };   /* key at which each table plays at its rate */
static uint32_t cyc_rate[3];

enum { D_KICK, D_SNARE, D_CLAP, D_HATC, D_HATO, D_CRASH, D_RIDE, D_TOM, D_COWBELL, D_CLICK, D_SHAKER, D_TAMB, D_COUNT };
static int16_t *drum_data[D_COUNT];
static uint32_t drum_len[D_COUNT];
static bool builtin_ready;

static uint32_t rng = 22222;
static float noise(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(int32_t)rng / 2147483648.0f;
}

static void *alloc_samples(size_t n) {
#ifdef GEKKO
    return memalign(32, n * 2 + 32);
#else
    return malloc(n * 2 + 32);
#endif
}

static void make_cycle(int fam, int t) {
    int n = cyc_len[t];
    int16_t *c = alloc_samples((size_t)n);
    if (!c) return;
    float tmp[CYC_A], peak = 0;
    int maxh = n / 2 - 1;
    for (int i = 0; i < n; i++) {
        float v = 0;
        for (int h = 0; h < 16 && h + 1 <= maxh; h++)
            if (families[fam].harm[h]) v += families[fam].harm[h] * sinf(2 * (float)M_PI * (h + 1) * i / n);
        tmp[i] = v;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    for (int i = 0; i < n; i++) c[i] = (int16_t)(tmp[i] / (peak > 0 ? peak : 1) * 16000);
    cycles[fam][t] = c;
}

/* Fills a drum sample from a little sound recipe. */
static void make_drum(int id) {
    static const float secs[D_COUNT] = { .4f, .28f, .3f, .07f, .45f, 1.6f, 1.1f, .45f, .3f, .06f, .12f, .3f };
    uint32_t n = (uint32_t)(secs[id] * DRUM_RATE);
    int16_t *d = alloc_samples(n);
    if (!d) return;
    float ph = 0, ph2 = 0, prev = 0, hp = 0;
    for (uint32_t i = 0; i < n; i++) {
        float t = (float)i / DRUM_RATE, v = 0;
        float nz = noise();
        hp = nz - prev;   /* rough high-pass for cymbals */
        prev = nz;
        switch (id) {
        case D_KICK: {
            float f = 45 + 95 * expf(-t * 28);
            ph += f / DRUM_RATE;
            v = sinf(2 * (float)M_PI * ph) * expf(-t * 8) + nz * 0.15f * expf(-t * 120);
            break;
        }
        case D_SNARE:
            ph += 185.0f / DRUM_RATE;
            v = nz * 0.75f * expf(-t * 17) + 0.5f * sinf(2 * (float)M_PI * ph) * expf(-t * 28);
            break;
        case D_CLAP: {
            float e = 0;
            for (int k = 0; k < 3; k++) { float tk = t - k * 0.011f; if (tk >= 0) e += expf(-tk * (k == 2 ? 14 : 90)); }
            v = nz * 0.8f * e;
            break;
        }
        case D_HATC: v = hp * 0.6f * expf(-t * 55); break;
        case D_HATO: v = hp * 0.55f * expf(-t * 7); break;
        case D_CRASH: v = hp * 0.6f * expf(-t * 2.4f); break;
        case D_RIDE:
            ph += 3100.0f / DRUM_RATE;
            ph2 += 4730.0f / DRUM_RATE;
            v = hp * 0.3f * expf(-t * 3) + (sinf(2 * (float)M_PI * ph) + sinf(2 * (float)M_PI * ph2)) * 0.12f * expf(-t * 4);
            break;
        case D_TOM: {
            float f = 110 + 60 * expf(-t * 20);
            ph += f / DRUM_RATE;
            v = sinf(2 * (float)M_PI * ph) * expf(-t * 7.5f);
            break;
        }
        case D_COWBELL:
            ph += 562.0f / DRUM_RATE;
            ph2 += 845.0f / DRUM_RATE;
            v = ((ph - floorf(ph) < .5f ? .4f : -.4f) + (ph2 - floorf(ph2) < .5f ? .4f : -.4f)) * expf(-t * 13);
            break;
        case D_CLICK:
            ph += 1700.0f / DRUM_RATE;
            v = sinf(2 * (float)M_PI * ph) * expf(-t * 70) + nz * 0.3f * expf(-t * 300);
            break;
        case D_SHAKER: v = hp * 0.5f * (t < 0.01f ? t / 0.01f : 1) * expf(-t * 30); break;
        case D_TAMB:
            ph += 6200.0f / DRUM_RATE;
            v = (hp * 0.4f + sinf(2 * (float)M_PI * ph) * 0.15f) * expf(-t * 9);
            break;
        }
        if (i > n - 200) v *= (float)(n - i) / 200.0f;   /* no click at the end */
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        d[i] = (int16_t)(v * 22000);
    }
    drum_data[id] = d;
    drum_len[id] = n;
}

void instruments_builtin_init(void) {
    if (builtin_ready) return;
    for (int t = 0; t < 3; t++)
        cyc_rate[t] = (uint32_t)(cyc_len[t] * 440.0f * powf(2.0f, (cyc_root[t] - 69) / 12.0f) + 0.5f);
    for (int f = 0; f < 16; f++)
        for (int t = 0; t < 3; t++) make_cycle(f, t);
    for (int d = 0; d < D_COUNT; d++) make_drum(d);
    builtin_ready = true;
}

/* General MIDI percussion keys -> built-in drum, pitched or not, and loudness */
static void drum_for_key(int key, int *id, float *root, float *scale, float *gain, int *excl) {
    *root = (float)key;
    *scale = 0;
    *gain = 1;
    *excl = 0;
    switch (key) {
    case 35: case 36: *id = D_KICK; *gain = 1.2f; break;
    case 37: case 75: case 76: case 77: *id = D_CLICK; *gain = .7f; break;
    case 38: case 40: *id = D_SNARE; break;
    case 39: *id = D_CLAP; break;
    case 42: case 44: *id = D_HATC; *excl = 1; *gain = .8f; break;
    case 46: *id = D_HATO; *excl = 1; *gain = .8f; break;
    case 49: case 52: case 55: case 57: *id = D_CRASH; *gain = .8f; break;
    case 51: case 53: case 59: *id = D_RIDE; *gain = .8f; break;
    case 54: *id = D_TAMB; *gain = .7f; break;
    case 56: *id = D_COWBELL; *gain = .6f; break;
    case 41: case 43: case 45: case 47: case 48: case 50:
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 68: case 78: case 79:
        *id = D_TOM; *root = 45; *scale = .5f; break;
    case 69: case 70: case 73: case 74: case 82: *id = D_SHAKER; *gain = .7f; break;
    default: *id = D_CLICK; *gain = .5f; break;
    }
}

static int builtin_find(int program, bool drums, int key, Zone *out) {
    instruments_builtin_init();
    Zone *z = out;
    memset(z, 0, sizeof(*z));
    if (drums) {
        int id, excl;
        float root, scale, gain;
        drum_for_key(key, &id, &root, &scale, &gain, &excl);
        if (!drum_data[id]) return 0;
        z->data = drum_data[id];
        z->len = drum_len[id];
        z->rate = DRUM_RATE;
        z->root = root;
        z->scale = scale;
        z->gain = gain;
        z->decay = 1;
        z->sustain = 1;
        z->release = 1.5f;   /* drums ring out to the end of their sample */
        z->exclusive = excl;
        return 1;
    }
    int fam = (program >> 3) & 15;
    int t = key < 66 ? 0 : key < 90 ? 1 : 2;
    if (!cycles[fam][t]) return 0;
    const Family *f = &families[fam];
    z->data = cycles[fam][t];
    z->len = (uint32_t)cyc_len[t];
    z->loop = true;
    z->loop_start = 0;
    z->loop_end = (uint32_t)cyc_len[t];
    z->rate = cyc_rate[t];
    z->root = (float)cyc_root[t];
    z->scale = 1;
    z->gain = 0.8f;
    z->attack = f->attack;
    z->decay = f->decay;
    z->sustain = f->sustain;
    z->release = f->release;
    /* decaying instruments fade faster on high notes, like real strings */
    if (f->sustain == 0) z->decay *= powf(2.0f, -(key - 60) / 24.0f);
    return 1;
}

int instruments_find(int bank, int program, bool drums, int key, int vel, Zone *out, int max) {
    if (max <= 0) return 0;
    if (sf_loaded) {
        int n = sf2_find(bank, program, drums, key, vel, out, max);
        if (n > 0) return n;
    }
    return builtin_find(program, drums, key, out);
}
