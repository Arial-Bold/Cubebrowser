/* midifile.c - see midifile.h. Format 0, 1 and 2 files; running status;
 * SMPTE time division; tempo changes in any track. */
#include "midifile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 400000
#define MAX_TEMPOS 4096

typedef struct { uint32_t tick; uint32_t seq; MidiEvent e; } TickEvent;
typedef struct { uint32_t tick; uint32_t seq; uint32_t us_per_qn; } Tempo;

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

static bool read_vlq(const unsigned char **p, const unsigned char *end, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= end) return false;
        unsigned char c = *(*p)++;
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) { *out = v; return true; }
    }
    return false;
}

static int cmp_tick(const void *a, const void *b) {
    const TickEvent *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq);
}

static int cmp_tempo(const void *a, const void *b) {
    const Tempo *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->seq < y->seq ? -1 : (x->seq > y->seq);
}

/* Keeps printable ASCII, trims spaces. */
static void clean_title(char *dst, size_t cap, const unsigned char *src, uint32_t n) {
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = src[i];
        if (c < 32 || c >= 127) c = ' ';
        if (c == ' ' && (!k || dst[k - 1] == ' ')) continue;   /* no runs of spaces */
        dst[k++] = (char)c;
    }
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = 0;
    size_t s = 0;
    while (dst[s] == ' ') s++;
    if (s) memmove(dst, dst + s, k - s + 1);
}

bool midifile_parse(const unsigned char *data, size_t len, MidiSong *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    /* .rmi: a RIFF file whose "data" chunk is the MIDI file */
    if (len >= 20 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "RMID", 4)) {
        size_t p = 12;
        bool found = false;
        while (p + 8 <= len) {
            uint32_t cl = le32(data + p + 4);
            if (!memcmp(data + p, "data", 4)) {
                if (cl > len - p - 8) cl = (uint32_t)(len - p - 8);
                data += p + 8;
                len = cl;
                found = true;
                break;
            }
            p += 8 + cl + (cl & 1);
        }
        if (!found) { snprintf(err, errlen, "no MIDI data in this .rmi file"); return false; }
    }
    /* some files have junk before the header */
    size_t skip = 0;
    while (skip + 14 <= len && skip < 4096 && memcmp(data + skip, "MThd", 4)) skip++;
    if (skip + 14 > len || memcmp(data + skip, "MThd", 4)) { snprintf(err, errlen, "not a MIDI file"); return false; }
    data += skip;
    len -= skip;
    uint32_t hlen = be32(data + 4);
    if (hlen < 6 || 8 + (size_t)hlen > len) { snprintf(err, errlen, "damaged MIDI header"); return false; }
    int ntracks = be16(data + 10);
    uint16_t division = be16(data + 12);
    double us_per_tick_smpte = 0;
    uint32_t tpq = 0;
    if (division & 0x8000) {
        int fps = -(int8_t)(division >> 8);
        int tpf = division & 0xFF;
        if (fps == 29) fps = 30;   /* 29.97 drop-frame: close enough */
        if (fps <= 0 || tpf <= 0) { snprintf(err, errlen, "unsupported timing"); return false; }
        us_per_tick_smpte = 1e6 / ((double)fps * tpf);
    } else {
        tpq = division;
        if (!tpq) { snprintf(err, errlen, "unsupported timing"); return false; }
    }

    int cap = 4096, n = 0;
    TickEvent *te = malloc(sizeof(TickEvent) * (size_t)cap);
    Tempo *tempos = malloc(sizeof(Tempo) * MAX_TEMPOS);
    int ntempo = 0;
    if (!te || !tempos) { free(te); free(tempos); snprintf(err, errlen, "out of memory"); return false; }
    uint32_t seq = 0;
    char title[64] = "";
    size_t pos = 8 + hlen;
    uint32_t track_offset = 0;   /* format 2: tracks play one after another */
    for (int t = 0; t < ntracks && pos + 8 <= len; t++) {
        /* skip unknown chunks */
        while (pos + 8 <= len && memcmp(data + pos, "MTrk", 4)) pos += 8 + be32(data + pos + 4);
        if (pos + 8 > len) break;
        uint32_t tl = be32(data + pos + 4);
        const unsigned char *p = data + pos + 8, *end = p + tl;
        if (end > data + len || end < p) end = data + len;   /* truncated track: read what's there */
        pos = (size_t)(end - data);
        uint32_t tick = track_offset;
        unsigned char running = 0;
        while (p < end) {
            uint32_t delta;
            if (!read_vlq(&p, end, &delta)) break;
            tick += delta;
            if (p >= end) break;
            unsigned char st = *p;
            if (st == 0xFF) {   /* meta event */
                if (p + 2 > end) break;
                unsigned char type = p[1];
                p += 2;
                uint32_t ml;
                if (!read_vlq(&p, end, &ml) || ml > (uint32_t)(end - p)) break;
                if (type == 0x51 && ml == 3 && ntempo < MAX_TEMPOS) {
                    uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
                    if (v) tempos[ntempo++] = (Tempo){ tick, seq++, v };
                } else if (type == 0x03 && !title[0] && (t == 0 || ntracks == 1)) {
                    clean_title(title, sizeof(title), p, ml);
                } else if (type == 0x2F) {
                    p += ml;
                    break;
                }
                p += ml;
                running = 0;
                continue;
            }
            if (st == 0xF0 || st == 0xF7) {   /* sysex: skipped */
                p++;
                uint32_t sl;
                if (!read_vlq(&p, end, &sl) || sl > (uint32_t)(end - p)) break;
                p += sl;
                running = 0;
                continue;
            }
            if (st & 0x80) { running = st; p++; }
            else if (!running) break;   /* data byte with no status: damaged */
            unsigned char type = running & 0xF0;
            int need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (p + need > end) break;
            unsigned char a = p[0] & 0x7F, b = need == 2 ? (p[1] & 0x7F) : 0;
            p += need;
            if (type == 0xF0) continue;   /* system common/realtime: ignored */
            unsigned char status = running;
            if (type == 0x90 && b == 0) status = (unsigned char)(0x80 | (running & 0x0F));
            if (type == 0xA0 || type == 0xD0) continue;   /* aftertouch: not used */
            if (n == cap) {
                if (cap >= MAX_EVENTS) break;
                TickEvent *nt = realloc(te, sizeof(TickEvent) * (size_t)cap * 2);
                if (!nt) break;
                te = nt;
                cap *= 2;
            }
            te[n].tick = tick;
            te[n].seq = seq++;
            te[n].e.status = status;
            te[n].e.a = a;
            te[n].e.b = b;
            te[n].e.pad = 0;
            n++;
        }
        if (be16(data + 8) == 2) track_offset = tick;
    }
    if (n == 0) { free(te); free(tempos); snprintf(err, errlen, "this MIDI file has no notes"); return false; }

    qsort(te, (size_t)n, sizeof(TickEvent), cmp_tick);
    qsort(tempos, (size_t)ntempo, sizeof(Tempo), cmp_tempo);
    MidiEvent *ev = malloc(sizeof(MidiEvent) * (size_t)n);
    if (!ev) { free(te); free(tempos); snprintf(err, errlen, "out of memory"); return false; }
    /* ticks -> microseconds, walking the tempo map */
    double us = 0, us_per_tick = tpq ? 500000.0 / tpq : us_per_tick_smpte;
    uint32_t last_tick = 0;
    int ti = 0;
    for (int i = 0; i < n; i++) {
        uint32_t tk = te[i].tick;
        while (tpq && ti < ntempo && tempos[ti].tick <= tk) {
            us += (double)(tempos[ti].tick - last_tick) * us_per_tick;
            last_tick = tempos[ti].tick;
            us_per_tick = (double)tempos[ti].us_per_qn / tpq;
            ti++;
        }
        us += (double)(tk - last_tick) * us_per_tick;
        last_tick = tk;
        if (us > 4.0e9) us = 4.0e9;
        ev[i] = te[i].e;
        ev[i].t_us = (uint32_t)us;
    }
    free(te);
    free(tempos);
    out->ev = ev;
    out->count = n;
    out->length_us = ev[n - 1].t_us;
    snprintf(out->title, sizeof(out->title), "%s", title);
    return true;
}

void midifile_free(MidiSong *s) {
    free(s->ev);
    memset(s, 0, sizeof(*s));
}
