/* instruments.c - see instruments.h. The SoundFont reader follows the
 * SoundFont 2.01 layout: presets point at instruments, instruments at
 * samples, and generators at both levels set ranges, tuning, loudness and
 * the volume envelope. Modulators, filters and LFOs are not used. */
#include "instruments.h"
#include "memstat.h"
#include "http.h"
#include "settings.h"
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

/* ===================================================== SoundFont reader
 *
 * SoundFonts can be far bigger than the GameCube's memory, so only the
 * small description part of the file (which presets, instruments and
 * samples exist) is kept in memory. Before each song starts, the player
 * lists the notes the song will play (instruments_need) and only the
 * samples those notes use are read from the SD card (instruments_need_load),
 * like a game loading the sound bank for a level. High-rate samples are
 * stored at half rate, and looping samples without the part after their
 * loop, which the player never reaches. Samples stay cached for the next
 * song while there's room. */

#define SAMPLE_BUDGET (10u * 1024u * 1024u)   /* most memory for loaded samples */
#define SAMPLE_RESERVE (8u * 1024u * 1024u)   /* ...leaving this much for web pages (long ones like Wikipedia need it) */
#define SAMPLE_MIN (3u * 1024u * 1024u)       /* ...yet music always gets at least this much */
#define SAMPLE_SAFETY (1536u * 1024u)         /* ...as long as this much stays free */
#define META_MAX (4u * 1024u * 1024u)

/* How SoundFont samples are kept (Settings > Music > Instrument memory):
 * 16-bit, 8-bit with block scales (half), or 4-bit ADPCM (a quarter). Test
 * programs on a PC always keep 16-bit (their mixer reads samples directly). */
static int wanted_fmt(void) {
#if defined(GEKKO) || defined(SF2_PACK_TEST)
    int m = g_set.music_mem;
    return m == 0 ? SFMT_PCM16 : m == 2 ? SFMT_ADPCM : SFMT_BLOCK8;
#else
    return SFMT_PCM16;
#endif
}

static size_t fmt_bytes(int fmt, uint32_t frames) {
    switch (fmt) {
    case SFMT_BLOCK8: return frames + blk8_scale_count(frames);
    case SFMT_ADPCM: return adpcm_ck_count(frames) * sizeof(AdpcmCk) + adpcm_data_bytes(frames);
    default: return (size_t)frames * 2;
    }
}

typedef struct {
    int16_t *data;        /* SFMT_PCM16: loaded frames, from the sample's start */
    void *block;          /* packed formats: one allocation holding aux then packed */
    const void *packed, *aux;
    uint8_t fmt;
    uint32_t raw_end;     /* how far into the sample (in file frames) is loaded */
    uint32_t need_end;    /* how far the coming song needs */
    uint8_t div;          /* 1, or 2 when stored at half rate */
    bool needed;
    uint32_t bytes;
} SampleSlot;

static bool slot_loaded(const SampleSlot *sl) { return sl->data || sl->block; }

typedef struct {
    char path[256];
    long smpl_off;                /* file offset of the sample data */
    uint32_t nsmpl;
    unsigned char *meta;          /* the pdta chunks, kept in memory */
    const unsigned char *phdr, *pbag, *pgen, *inst, *ibag, *igen, *shdr;
    uint32_t nphdr, npbag, npgen, ninst, nibag, nigen, nshdr;
    SampleSlot *slots;            /* one per sample header */
    size_t loaded_bytes;
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

static void *alloc_samples(size_t n);

void instruments_unload_sf2(void) {
    if (sf_loaded) {
        for (uint32_t i = 0; i < sf.nshdr; i++) { free(sf.slots[i].data); free(sf.slots[i].block); }
        free(sf.slots);
        free(sf.meta);
    }
    memset(&sf, 0, sizeof(sf));
    sf_loaded = false;
}

bool instruments_have_sf2(void) { return sf_loaded; }

const char *instruments_name(void) { return sf_loaded ? sf.name : "Built-in"; }

size_t instruments_memory(void) { return sf_loaded ? sf.loaded_bytes : 0; }

static void slot_free(SampleSlot *sl);
void instruments_free_samples(void) {
    if (!sf_loaded) return;
    for (uint32_t i = 0; i < sf.nshdr; i++) slot_free(&sf.slots[i]);
}

static bool read_at(FILE *f, long off, void *buf, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

bool instruments_load_sf2(const char *path, char *err, size_t errlen) {
    instruments_unload_sf2();
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "couldn't open the SoundFont"); return false; }
    unsigned char h[12];
    if (!read_at(f, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "sfbk", 4)) {
        fclose(f);
        snprintf(err, errlen, "not a SoundFont 2 file");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    Sf2 s;
    memset(&s, 0, sizeof(s));
    long pdta_off = -1;
    uint32_t pdta_len = 0;
    long p = 12;
    /* walk the top-level LISTs without reading the big sample chunk */
    while (p + 12 <= flen) {
        unsigned char ch[12];
        if (!read_at(f, p, ch, 12)) break;
        uint32_t cl = rd32(ch + 4);
        if (cl > (uint32_t)(flen - p - 8)) cl = (uint32_t)(flen - p - 8);
        if (!memcmp(ch, "LIST", 4)) {
            long q = p + 12, qe = p + 8 + cl;
            if (!memcmp(ch + 8, "pdta", 4)) { pdta_off = q; pdta_len = (uint32_t)(qe - q); }
            else if (!memcmp(ch + 8, "sdta", 4) || !memcmp(ch + 8, "INFO", 4)) {
                while (q + 8 <= qe) {
                    unsigned char sc[8];
                    if (!read_at(f, q, sc, 8)) break;
                    uint32_t l2 = rd32(sc + 4);
                    if (l2 > (uint32_t)(qe - q - 8)) l2 = (uint32_t)(qe - q - 8);
                    if (!memcmp(sc, "smpl", 4)) { s.smpl_off = q + 8; s.nsmpl = l2 / 2; }
                    else if (!memcmp(sc, "INAM", 4)) {
                        char nm[64] = "";
                        size_t rl = l2 < sizeof(nm) - 1 ? l2 : sizeof(nm) - 1;
                        if (read_at(f, q + 8, nm, rl)) {
                            size_t k = 0;
                            for (size_t i = 0; i < rl && nm[i]; i++)
                                if (nm[i] >= 32 && nm[i] < 127) s.name[k++] = nm[i];
                            s.name[k] = 0;
                        }
                    }
                    q += 8 + l2 + (l2 & 1);
                }
            }
        }
        p += 8 + cl + (cl & 1);
    }
    if (pdta_off < 0 || pdta_len < 8 || pdta_len > META_MAX || !s.nsmpl) {
        fclose(f);
        snprintf(err, errlen, "this SoundFont is damaged or uses a format this player can't read");
        return false;
    }
    s.meta = malloc(pdta_len);
    if (!s.meta || !read_at(f, pdta_off, s.meta, pdta_len)) {
        fclose(f);
        free(s.meta);
        snprintf(err, errlen, "couldn't read the SoundFont");
        return false;
    }
    fclose(f);
    const unsigned char *m = s.meta, *me = s.meta + pdta_len;
    while (m + 8 <= me) {
        uint32_t cl = rd32(m + 4);
        const unsigned char *d = m + 8;
        if (cl > (uint32_t)(me - d)) cl = (uint32_t)(me - d);
#define TAKE(tag, ptr, cnt, sz) if (!memcmp(m, tag, 4)) { s.ptr = d; s.cnt = cl / (sz); }
        TAKE("phdr", phdr, nphdr, 38)
        TAKE("pbag", pbag, npbag, 4)
        TAKE("pgen", pgen, npgen, 4)
        TAKE("inst", inst, ninst, 22)
        TAKE("ibag", ibag, nibag, 4)
        TAKE("igen", igen, nigen, 4)
        TAKE("shdr", shdr, nshdr, 46)
#undef TAKE
        m = d + cl + (cl & 1);
    }
    if (s.nphdr < 2 || s.npbag < 1 || s.ninst < 2 || s.nibag < 1 || s.nshdr < 2) {
        free(s.meta);
        snprintf(err, errlen, "this SoundFont is damaged or uses a format this player can't read");
        return false;
    }
    s.slots = calloc(s.nshdr, sizeof(SampleSlot));
    if (!s.slots) { free(s.meta); snprintf(err, errlen, "out of memory"); return false; }
    snprintf(s.path, sizeof(s.path), "%s", path);
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

/* Applies the generators of bag b. */
static void apply_bag(Gens *g, const unsigned char *bag, uint32_t nbag, const unsigned char *gen, uint32_t ngen, uint32_t b) {
    if (b >= nbag) return;
    uint32_t g0 = rd16(bag + b * 4), g1 = (b + 1 < nbag) ? rd16(bag + (b + 1) * 4) : ngen;
    if (g1 > ngen) g1 = ngen;
    for (uint32_t i = g0; i < g1; i++) {
        uint16_t op = rd16(gen + i * 4);
        if (op >= G_COUNT) continue;
        g->v[op] = (int16_t)rd16(gen + i * 4 + 2);
        g->set[op] = true;
    }
}

/* Does bag b contain the given terminal generator (instrument / sampleID)? */
static bool bag_has(const unsigned char *bag, uint32_t nbag, const unsigned char *gen, uint32_t ngen, uint32_t b, int op) {
    if (b >= nbag) return false;
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

/* A zone before its samples are attached: positions are file frames. */
typedef struct {
    int sid;
    int64_t base, start, end, ls, le;   /* base = the sample header's start */
    Zone z;                             /* everything except data/len/loops/rate */
    uint32_t rate;
} ZoneSpec;

static bool make_spec(const Gens *pz, const Gens *iz, ZoneSpec *sp) {
    int sid = iz->v[G_SAMPLE];
    if (sid < 0 || (uint32_t)sid + 1 >= sf.nshdr) return false;
    const unsigned char *sh = sf.shdr + (uint32_t)sid * 46;
    if (rd16(sh + 44) & 0x8000) return false;   /* ROM sample */
    int32_t v[G_COUNT];
    for (int i = 0; i < G_COUNT; i++) v[i] = iz->v[i] + (pz ? pz->v[i] : 0);
    int64_t base = rd32(sh + 20), send = rd32(sh + 24);
    if (send > (int64_t)sf.nsmpl) send = sf.nsmpl;
    int64_t start = base + iz->v[G_START] + (int64_t)iz->v[G_START_C] * 32768;
    int64_t end = send + iz->v[G_END] + (int64_t)iz->v[G_END_C] * 32768;
    int64_t ls = (int64_t)rd32(sh + 28) + iz->v[G_LSTART] + (int64_t)iz->v[G_LSTART_C] * 32768;
    int64_t le = (int64_t)rd32(sh + 32) + iz->v[G_LEND] + (int64_t)iz->v[G_LEND_C] * 32768;
    if (start < base) start = base;
    if (end > send) end = send;
    if (end - start < 8) return false;
    memset(sp, 0, sizeof(*sp));
    sp->sid = sid;
    sp->base = base;
    sp->start = start;
    sp->end = end;
    Zone *z = &sp->z;
    int modes = iz->v[G_MODES] & 3;
    if ((modes == 1 || modes == 3) && ls >= start && le <= end && le - ls >= 4) {
        z->loop = true;
        sp->ls = ls;
        sp->le = le;
    }
    sp->rate = rd32(sh + 36);
    if (sp->rate < 400 || sp->rate > 200000) sp->rate = 22050;
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
    return true;
}

/* Finds the zones a note uses (as specs). */
static int sf2_specs(int bank, int prog, bool drums, int key, int vel, ZoneSpec *out, int max) {
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
        /* only these preset values are added on to the instrument's */
        Gens add = pz;
        add.v[G_KEYRANGE] = add.v[G_VELRANGE] = add.v[G_INSTRUMENT] = 0;
        add.v[G_START] = add.v[G_END] = add.v[G_LSTART] = add.v[G_LEND] = 0;
        add.v[G_START_C] = add.v[G_END_C] = add.v[G_LSTART_C] = add.v[G_LEND_C] = 0;
        add.v[G_SAMPLE] = add.v[G_MODES] = add.v[G_EXCL] = add.v[G_ROOT] = add.v[G_KEYNUM] = 0;
        for (uint32_t c = ib0; c < ib1 && n < max; c++) {
            if (!bag_has(sf.ibag, sf.nibag, sf.igen, sf.nigen, c, G_SAMPLE)) {
                if (c == ib0) apply_bag(&iglobal, sf.ibag, sf.nibag, sf.igen, sf.nigen, c);
                continue;
            }
            Gens iz = iglobal;
            apply_bag(&iz, sf.ibag, sf.nibag, sf.igen, sf.nigen, c);
            if (!in_range(iz.v[G_KEYRANGE], key) || !in_range(iz.v[G_VELRANGE], vel)) continue;
            if (make_spec(&add, &iz, &out[n])) n++;
        }
    }
    return n;
}

/* Is this sample stored at half rate? High-rate samples are, unless their
 * loop is so short that halving it would put it out of tune. */
static uint8_t sample_div(int sid) {
    const unsigned char *sh = sf.shdr + (uint32_t)sid * 46;
    uint32_t rate = rd32(sh + 36), ls = rd32(sh + 28), le = rd32(sh + 32);
    if (rate <= 32000) return 1;
    if (le > ls && le - ls < 400) return 1;
    return 2;
}

/* How far into the sample a zone plays: to its loop's end if it loops. */
static uint32_t spec_need(const ZoneSpec *sp) {
    return (uint32_t)((sp->z.loop ? sp->le : sp->end) - sp->base);
}

/* Attaches the loaded samples to a spec; false if they aren't loaded. */
static bool spec_zone(const ZoneSpec *sp, Zone *z) {
    const SampleSlot *sl = &sf.slots[sp->sid];
    uint32_t need = spec_need(sp);
    if (!slot_loaded(sl) || sl->raw_end < need) return false;
    *z = sp->z;
    uint32_t d = sl->div;
    uint32_t off = (uint32_t)(sp->start - sp->base) / d;
    uint32_t avail = sl->raw_end / d;
    z->fmt = sl->fmt;
    z->first = off;
    if (sl->fmt == SFMT_PCM16) {
        z->data = sl->data + off;
        z->packed = z->aux = NULL;
    } else {
        z->data = NULL;
        z->packed = sl->packed;
        z->aux = sl->aux;
    }
    uint32_t end = (uint32_t)(sp->end - sp->base) / d;
    if (end > avail) end = avail;
    if (z->loop) {
        z->loop_start = (uint32_t)(sp->ls - sp->base) / d - off;
        z->loop_end = (uint32_t)(sp->le - sp->base) / d - off;
        if (z->loop_end > avail - off) z->loop_end = avail - off;
        if (z->loop_end <= z->loop_start + 1) z->loop = false;
        end = off + z->loop_end;   /* nothing after the loop is kept */
    }
    if (end <= off + 4) return false;
    z->len = end - off;
    z->rate = sp->rate / d;
    return true;
}

static int sf2_find(int bank, int prog, bool drums, int key, int vel, Zone *out, int max) {
    ZoneSpec specs[4];
    int n = sf2_specs(bank, prog, drums, key, vel, specs, max < 4 ? max : 4), k = 0;
    for (int i = 0; i < n; i++)
        if (spec_zone(&specs[i], &out[k])) k++;
    return k;
}

/* ---- per-song loading */

void instruments_need_begin(void) {
    if (!sf_loaded) return;
    for (uint32_t i = 0; i < sf.nshdr; i++) {
        sf.slots[i].needed = false;
        sf.slots[i].need_end = 0;
    }
}

void instruments_need(int bank, int program, bool drums, int key, int vel) {
    if (!sf_loaded) return;
    ZoneSpec specs[2];
    int n = sf2_specs(bank, program, drums, key, vel, specs, 2);
    for (int i = 0; i < n; i++) {
        SampleSlot *sl = &sf.slots[specs[i].sid];
        uint32_t need = spec_need(&specs[i]);
        sl->needed = true;
        if (need > sl->need_end) sl->need_end = need;
    }
}

static void slot_free(SampleSlot *sl) {
    if (!slot_loaded(sl)) return;
    free(sl->data);
    free(sl->block);
    sf.loaded_bytes -= sl->bytes;
    sl->data = NULL;
    sl->block = NULL;
    sl->packed = sl->aux = NULL;
    sl->bytes = 0;
    sl->raw_end = 0;
}

/* Reads frames [0, need) of sample sid into memory, halving the rate if wanted. */
static bool load_slot(FILE *f, int sid, uint32_t need) {
    SampleSlot *sl = &sf.slots[sid];
    const unsigned char *sh = sf.shdr + (uint32_t)sid * 46;
    uint32_t base = rd32(sh + 20);
    if (base >= sf.nsmpl) return false;
    if (need > sf.nsmpl - base) need = sf.nsmpl - base;
    uint8_t div = sample_div(sid);
    uint32_t frames = need / div;
    if (frames < 4) return false;
    int fmt = wanted_fmt();
    int16_t *dst = NULL;
    void *block = NULL;
    size_t bytes = fmt_bytes(fmt, frames);
    AdpcmEnc enc = { 0 };
    int8_t *b8 = NULL;
    uint8_t *b8s = NULL;
    int16_t pend[BLK8_N];   /* BLOCK8 packs 16 samples at a time */
    uint32_t npend = 0;
    if (fmt == SFMT_PCM16) {
        dst = alloc_samples(frames);
        if (!dst) return false;
    } else {
        block = malloc(bytes);
        if (!block) return false;
        if (fmt == SFMT_ADPCM) {
            enc.ck = block;
            enc.data = (uint8_t *)block + adpcm_ck_count(frames) * sizeof(AdpcmCk);
        } else {
            b8s = block;
            b8 = (int8_t *)((uint8_t *)block + blk8_scale_count(frames));
        }
    }
#define PUT(v) do { int16_t pv_ = (v); \
        if (dst) dst[out] = pv_; \
        else if (enc.data) adpcm_put(&enc, pv_); \
        else { pend[npend++] = pv_; if (npend == BLK8_N) { blk8_pack(pend, BLK8_N, b8 + out + 1 - BLK8_N, b8s + ((out + 1 - BLK8_N) >> BLK8_SHIFT)); npend = 0; } } \
        out++; } while (0)
    static int16_t chunk[16384];
    bool swap = host_big_endian();
    uint32_t got = 0, out = 0;
    int16_t prev = 0;
    if (fseek(f, sf.smpl_off + (long)base * 2, SEEK_SET) != 0) { free(dst); free(block); return false; }
    while (got < need) {
        uint32_t n = need - got;
        if (n > 16384) n = 16384;
        if (fread(chunk, 2, n, f) != n) { free(dst); free(block); return false; }
        for (uint32_t i = 0; i < n; i++) {
            int16_t v = chunk[i];
            if (swap) v = (int16_t)(uint16_t)(((uint16_t)v >> 8) | ((uint16_t)v << 8));
            chunk[i] = v;
        }
        if (div == 1) {
            for (uint32_t i = 0; i < n && out < frames; i++) PUT(chunk[i]);
        } else {
            /* gentle low-pass, then every second frame */
            for (uint32_t i = 0; i < n; i++) {
                uint32_t abs_i = got + i;
                if ((abs_i & 1) == 0 && out < frames) {
                    int32_t nx = (i + 1 < n) ? chunk[i + 1] : chunk[i];
                    PUT((int16_t)((prev + 2 * (int32_t)chunk[i] + nx) / 4));
                }
                prev = chunk[i];
            }
        }
        got += n;
    }
#undef PUT
    if (npend) blk8_pack(pend, npend, b8 + out - npend, b8s + ((out - npend) >> BLK8_SHIFT));   /* the last few */
    slot_free(sl);
    sl->data = dst;
    sl->block = block;
    sl->fmt = (uint8_t)fmt;
    sl->packed = fmt == SFMT_ADPCM ? (const void *)enc.data : (const void *)b8;
    sl->aux = fmt == SFMT_ADPCM ? (const void *)enc.ck : (const void *)b8s;
    sl->div = div;
    sl->raw_end = (uint32_t)out * div;
    sl->bytes = (uint32_t)bytes;
    sf.loaded_bytes += sl->bytes;
    return true;
}

bool instruments_need_load(char *msg, size_t msglen) {
    msg[0] = 0;
    if (!sf_loaded) return true;
    /* drop what the coming song doesn't use, and anything kept in another
     * format than the Instrument memory setting now asks for */
    int fmt = wanted_fmt();
    for (uint32_t i = 0; i < sf.nshdr; i++)
        if (!sf.slots[i].needed || (slot_loaded(&sf.slots[i]) && sf.slots[i].fmt != fmt)) slot_free(&sf.slots[i]);
    /* web pages need memory too: leave SAMPLE_RESERVE free when possible */
    size_t free_now = cb_mem_free();
    size_t budget = sf.loaded_bytes + (free_now > SAMPLE_RESERVE ? free_now - SAMPLE_RESERVE : 0);
    /* ...but music always gets a little room, even next to a huge page (the
     * page then skips some pictures until the music stops); this is where the
     * 8-bit and 4-bit settings make the difference: 4-bit fits four times as
     * many sounds in it */
    size_t most = sf.loaded_bytes + (free_now > SAMPLE_SAFETY ? free_now - SAMPLE_SAFETY : 0);
    size_t floor_bytes = SAMPLE_MIN < most ? SAMPLE_MIN : most;
    if (budget < floor_bytes) budget = floor_bytes;
    if (budget > SAMPLE_BUDGET) budget = SAMPLE_BUDGET;
    FILE *f = fopen(sf.path, "rb");
    if (!f) { snprintf(msg, msglen, "Couldn't open the SoundFont"); return false; }
    int skipped = 0, over_budget = 0, failed = 0;
    size_t needed = 0;
    for (uint32_t i = 0; i < sf.nshdr; i++)
        if (sf.slots[i].needed) needed += fmt_bytes(fmt, sf.slots[i].need_end / sample_div((int)i));
    /* in file order, so the card reads mostly forwards */
    for (uint32_t i = 0; i < sf.nshdr; i++) {
        SampleSlot *sl = &sf.slots[i];
        if (!sl->needed || (slot_loaded(sl) && sl->raw_end >= sl->need_end)) continue;
        size_t bytes = fmt_bytes(fmt, sl->need_end / sample_div((int)i));
        if (sf.loaded_bytes - sl->bytes + bytes > budget) { skipped++; over_budget++; }
        else if (!load_slot(f, (int)i, sl->need_end)) { skipped++; failed++; }
    }
    fclose(f);
    /* page.log: what the music got, so memory problems can be told apart */
    static const char *const fmt_names[] = { "16-bit", "8-bit", "4-bit" };
    http_log_note("music   | %s: needs %u KB, has %u KB, limit %u KB, %u KB free | %d skipped (%d over limit, %d no memory)",
                  fmt_names[fmt], (unsigned)(needed / 1024), (unsigned)(sf.loaded_bytes / 1024), (unsigned)(budget / 1024),
                  (unsigned)(free_now / 1024), skipped, over_budget, failed);
    if (skipped)
        snprintf(msg, msglen, "Low memory: %d sounds use built-in instruments", skipped);
    return skipped == 0;
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
