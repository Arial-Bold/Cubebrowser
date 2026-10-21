/* midi.c - see midi.h.
 *
 * How it plays, in the manner of a GameCube game's sound driver:
 *  - The sequencer (a thread here) walks the song's events in time.
 *  - Each note is given one of NVOICES hardware voices on the DSP, the
 *    console's sound processor (libogc2's AESND library). The voice plays the
 *    instrument's sample at a pitch set by its playback rate; its volume
 *    follows the instrument's envelope and the channel's controls.
 *  - The DSP pulls sample data in small blocks, which the voice callback
 *    supplies from the instrument (wrapping around the sample's loop), and
 *    mixes all voices together into the TV's audio.
 *
 * On a PC (the test programs) the same sequencer drives a simple software
 * mixer instead, so songs can be rendered to a .wav file and listened to. */
#include "midi.h"
#include "midifile.h"
#include "instruments.h"
#include "settings.h"
#include "storage.h"
#include "thr.h"
#include "http.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef GEKKO
#include <gccore.h>
#include <aesndlib.h>
#include <ogc/machine/processor.h>
#include <ogc/aram.h>
#include <ogc/lwp_watchdog.h>
#else
#include <time.h>
#endif

#define NVOICES 24          /* hardware voices for music (the DSP has 32; only playing notes use one) */
#define MAX_SONGS 400
#define NAME_LEN 128
#define OUT_GAIN 0.9f       /* keeps many loud notes from clipping */
#define MAX_RATE 140000u    /* the DSP's per-voice limit is about 3x 48 kHz */

/* ---------------------------------------------------------------- state */

typedef struct {
    int program, bank;
    float volume, expression, pan;    /* 0..1, 0..1, -1..1 */
    bool sustain;
    float bend, bend_range;           /* -1..1, semitones */
    int rpn_msb, rpn_lsb;
} Channel;

enum { ENV_ATTACK, ENV_HOLD, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_DONE };

typedef struct {
    bool active;
    int ch, key;
    float vel_gain;
    Zone z;
    int stage;
    float level, stage_t;
    bool released, held;      /* held: note-off came while the sustain pedal was down */
    double started, ends;     /* seconds of song time; ends: one-shot sample runs out */
    uint32_t order;
    /* worked out once and reused while unchanged (sin/cos/pow are slow) */
    float pan_for, gl, gr;
    float bend_for;
    uint32_t freq;
} Voice;

static thr_mutex lock;
static bool lock_ok;

static Channel chans[16];
static Voice voices[NVOICES];
static uint32_t voice_order;

static MidiSong song;
static bool song_loaded;
static int ev_next;
static double song_pos;           /* seconds into the song */
static double tail_until;         /* after the last event: wait for notes to ring out */

static char (*songs)[NAME_LEN];
static int nsongs;
static volatile int current = -1;
static volatile MidiState state = MIDI_STOPPED;
static char title[96];
static char message[160];
static char loaded_sf2[256];      /* what's loaded: "" = built-in */
static bool instruments_checked;

/* requests from the UI thread, carried out by the sequencer */
static int *order;                /* shuffle order (see neighbour) */
static int order_n, order_pos = -1;

enum { REQ_NONE, REQ_PLAY, REQ_STOP, REQ_PAUSE, REQ_RESUME, REQ_RELOAD, REQ_NEXT, REQ_PREV };
static int req, req_song;

/* -------------------------------------------------------------- helpers */

static double now_seconds(void) {
#ifdef GEKKO
    return (double)ticks_to_microsecs(gettime()) / 1e6;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static bool is_song(const char *name) {
    return has_ext(name, ".mid") || has_ext(name, ".midi") || has_ext(name, ".kar") || has_ext(name, ".rmi");
}

static int cmp_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* Reading samples from the SD card can take seconds; it's done at a low
 * priority so the browser stays responsive, then playing resumes at the
 * sequencer's high one. */
static void slow_work(bool on) {
#ifdef GEKKO
    LWP_SetThreadPriority(LWP_GetSelf(), on ? 50 : 110);
#else
    (void)on;
#endif
}

/* ===================================================== hardware voices */

#ifdef GEKKO

#define STAGE_SAMPLES 1152    /* one DSP stream block is 576 samples; the first fill takes two */
#define LOOP_CACHE 2048       /* loops up to this long are copied from audio memory once per note */

/* A voice is only switched on while it plays a note, so the DSP mixes just
 * the notes sounding now (mixing every voice all the time can take longer
 * than the 2 ms the DSP has per block, which is heard as skips).
 *
 * Who may touch a voice: while it runs, AESND copies its settings out at the
 * start of each 2 ms block and back at the end, so changes must be made in
 * its callback. Once stopped, it's left alone, and after a few blocks the
 * sequencer may start it again directly. */
enum { HV_STOPPED, HV_RUNNING, HV_STOPPING };

typedef struct {
    AESNDPB *pb;
    /* a new note for a running voice, published by bumping gen */
    const int16_t *data;
    uint8_t fmt;                   /* or packed (packsamp.h), or in audio memory: */
    const void *packed, *aux;
    uint32_t aram;
    uint32_t first;
    uint32_t len, loop_start, loop_end;
    bool loop;
    volatile uint32_t gen;
    volatile uint32_t freq;
    volatile uint16_t vl, vr;
    volatile bool want_stop;      /* the note ended: switch the voice off */
    volatile int state;
    volatile uint32_t stop_frame;
    /* the note being played, owned by the callback while the voice runs */
    uint32_t applied;
    const int16_t *c_data;
    uint8_t c_fmt;
    const void *c_packed, *c_aux;
    uint32_t c_aram;
    uint32_t c_first;
    AdpcmDec dec;                  /* where the ADPCM decoding has got to */
    uint32_t c_len, c_ls, c_le, pos;
    bool c_loop, done;
    bool loop_cached;              /* ARAM: a short loop kept in loopbuf */
    uint16_t since_stream;         /* blocks since the voice last asked for more samples */
    int16_t staging[STAGE_SAMPLES] __attribute__((aligned(32)));
    int16_t bounce[STAGE_SAMPLES + 32] __attribute__((aligned(32)));   /* ARAM copies land here */
    int16_t loopbuf[LOOP_CACHE + 32] __attribute__((aligned(32)));
} HwVoice;

static HwVoice hw[NVOICES] __attribute__((aligned(32)));
static volatile bool hw_stop_req;
static volatile uint32_t frame_no;   /* counts the 2 ms audio blocks */
static volatile uint32_t stalls;     /* voices found stuck and restarted (see hw_callback) */
static bool hw_ok;

static void audio_block(void *buf, u32 len) { (void)buf; (void)len; frame_no++; }

/* Copies n samples starting at sample `at` of the note's ARAM sample into
 * dst (n <= STAGE_SAMPLES), through the voice's bounce buffer. The copy engine
 * is shared with the sound library, which only starts its copies from
 * interrupts: with interrupts off, wait for any copy in progress, make ours,
 * and wait for it. (The library's own completion still reaches it: the
 * engine reports "done" once for both.) */
/* The copy engine raises a "copy finished" interrupt for our copies too. The
 * sound library's copy queue takes that interrupt as the end of ITS copy,
 * and when its own copy was still running it would start the next one on
 * top of it, so a voice's sound memory could be left half written. So when
 * the flag wasn't already up before our copy (someone else's finish, which
 * must still be delivered), it's cleared again after ours. */
#define DSP_CSR (*(volatile u16 *)0xCC00500A)
static inline bool aram_irq_pending(void) { return (DSP_CSR & 0x0020) != 0; }
static inline void aram_irq_clear(void) {
    u16 c = DSP_CSR & (u16)~(0x0080 | 0x0008);   /* (don't acknowledge the DSP's or audio's interrupts) */
    DSP_CSR = (u16)(c | 0x0020);
}

static bool aram_copy(HwVoice *h, int16_t *dst, uint32_t at, uint32_t n, int16_t *bounce) {
    uint32_t src = h->c_aram + at * 2;
    uint32_t a0 = src & ~31u, a1 = (src + n * 2 + 31) & ~31u, len = a1 - a0;
    u32 level;
    _CPU_ISR_Disable(level);
    int k;
    for (k = 0; k < 200000 && AR_GetDMAStatus(); k++) {}
    if (k == 200000) { _CPU_ISR_Restore(level); return false; }
    bool flagged = aram_irq_pending();
    DCInvalidateRange(bounce, len);
    AR_StartDMA(AR_ARAMTOMRAM, (u32)MEM_VIRTUAL_TO_PHYSICAL(bounce), a0, len);
    for (k = 0; k < 200000 && AR_GetDMAStatus(); k++) {}
    if (k < 200000 && !flagged) aram_irq_clear();
    _CPU_ISR_Restore(level);
    if (k == 200000) return false;
    memcpy(dst, (const char *)bounce + (src - a0), n * 2);
    return true;
}

static void aram_read(HwVoice *h, int16_t *dst, uint32_t pos, uint32_t n) {
    if (h->loop_cached && pos >= h->c_ls) {   /* inside a short loop: from the copy made at note start */
        memcpy(dst, h->loopbuf + (pos - h->c_ls), n * 2);
        return;
    }
    if (!aram_copy(h, dst, h->c_first + pos, n, h->bounce)) memset(dst, 0, n * 2);
}

/* Copies the next n samples of the note into dst, wrapping at the loop. */
static void hw_fill(HwVoice *h, int16_t *dst, int n) {
    while (n > 0) {
        if ((!h->c_data && !h->c_packed && !h->c_aram) || h->done) { memset(dst, 0, (size_t)n * 2); return; }
        uint32_t end = h->c_loop ? h->c_le : h->c_len;
        if (h->pos >= end) {
            if (h->c_loop) {
                h->pos = h->c_ls;
                if (h->c_fmt == SFMT_ADPCM) adpcm_seek(&h->dec, h->c_first + h->pos);
                continue;
            }
            h->done = true;
            continue;
        }
        uint32_t k = end - h->pos;
        if (k > (uint32_t)n) k = (uint32_t)n;
        if (h->c_fmt == SFMT_ARAM) aram_read(h, dst, h->pos, k);
        else if (h->c_fmt == SFMT_ADPCM) adpcm_read(&h->dec, dst, k);
        else if (h->c_fmt == SFMT_BLOCK8) blk8_read(h->c_packed, h->c_aux, h->c_first + h->pos, dst, k);
        else memcpy(dst, h->c_data + h->pos, k * 2);
        dst += k;
        n -= (int)k;
        h->pos += k;
    }
}

static void hw_take_note(HwVoice *h) {
    h->applied = h->gen;
    h->c_data = h->data;
    h->c_fmt = h->fmt;
    h->c_packed = h->packed;
    h->c_aux = h->aux;
    h->c_aram = h->aram;
    h->c_first = h->first;
    h->loop_cached = false;
    if (h->c_fmt == SFMT_ADPCM) {
        h->dec.data = h->c_packed;
        h->dec.ck = h->c_aux;
        adpcm_seek(&h->dec, h->c_first);
    }
    h->c_len = h->len;
    h->c_ls = h->loop_start;
    h->c_le = h->loop_end;
    h->c_loop = h->loop;
    h->pos = 0;
    h->done = false;
    /* a short loop is copied out of audio memory once, rather than in many
     * tiny pieces every time it comes round */
    if (h->c_fmt == SFMT_ARAM && h->c_loop && h->c_le > h->c_ls && h->c_le - h->c_ls <= LOOP_CACHE) {
        uint32_t llen = h->c_le - h->c_ls;
        /* copy in pieces the bounce buffer can hold */
        bool ok = true;
        for (uint32_t done_n = 0; done_n < llen && ok; ) {
            uint32_t piece = llen - done_n > STAGE_SAMPLES ? STAGE_SAMPLES : llen - done_n;
            ok = aram_copy(h, h->loopbuf + done_n, h->c_first + h->c_ls + done_n, piece, h->bounce);
            done_n += piece;
        }
        h->loop_cached = ok;
    }
    hw_fill(h, h->staging, STAGE_SAMPLES);
}

/* Called by AESND (in the audio interrupt) at the start of each block for a
 * running voice (RUNNING), and when it has used up its samples (STREAM).
 * Integer work only. */
static void hw_callback(AESNDPB *pb, u32 st) {
    HwVoice *h = AESND_GetVoiceUserData(pb);
    if (!h) return;
    if (st == VOICE_STATE_RUNNING) {
        /* AESND copies every voice's samples to sound memory through one
         * shared buffer. If the previous copy is still going, the next voice
         * would overwrite it (heard as clicks and stray noises), so wait
         * for it here; it takes a few microseconds at most. */
        for (int k = 0; k < 20000 && AR_GetDMAStatus(); k++) {}
        if (hw_stop_req || h->want_stop) {
            AESND_SetVoiceStop(pb, true);
            h->state = HV_STOPPING;
            h->stop_frame = frame_no;
            return;
        }
        if (h->gen != h->applied) {   /* a new note on this running voice */
            hw_take_note(h);
            AESND_PlayVoice(pb, VOICE_MONO16, h->staging, STAGE_SAMPLES * 2, h->freq, 0, true);
            AESND_SetVoiceStream(pb, true);
            h->since_stream = 0;
        } else {
            /* A voice that keeps playing but stops asking for more samples
             * goes round and round the last 24 ms it was given: a loud,
             * steady buzz until the note ends. It should ask every
             * 576 samples; if it's gone well past that, restart it from
             * where the note has got to. */
            uint32_t per = h->freq > 144 ? 576u * 500u / h->freq : 2000;   /* blocks per request */
            if (++h->since_stream > 2 * per + 12) {
                hw_fill(h, h->staging, STAGE_SAMPLES);
                AESND_PlayVoice(pb, VOICE_MONO16, h->staging, STAGE_SAMPLES * 2, h->freq, 0, true);
                AESND_SetVoiceStream(pb, true);
                h->since_stream = 0;
                stalls++;
            }
        }
        AESND_SetVoiceFrequency(pb, h->freq);
        AESND_SetVoiceVolume(pb, h->vl, h->vr);
    } else if (st == VOICE_STATE_STREAM) {
        h->since_stream = 0;
        hw_fill(h, h->staging, STAGE_SAMPLES / 2);
        AESND_SetVoiceBuffer(pb, h->staging, STAGE_SAMPLES);
    }
}

/* A stopped voice is safe to start once AESND has finished with it. */
static bool hw_settled(const HwVoice *h) {
    return h->state == HV_STOPPED || (h->state == HV_STOPPING && frame_no - h->stop_frame >= 3);
}

static bool hw_ready(int i) { return hw_ok && hw_settled(&hw[i]); }

static void hw_init(void) {
    for (int i = 0; i < NVOICES; i++) {
        memset(&hw[i], 0, sizeof(hw[i]));
        hw[i].pb = AESND_AllocateVoice(hw_callback);
        if (!hw[i].pb) return;
        AESND_SetVoiceUserData(hw[i].pb, &hw[i]);
    }
    AESND_RegisterAudioCallback(audio_block);
    hw_ok = true;
}

static void hw_begin(void) { hw_stop_req = false; }

/* Switches every voice off (from inside their callbacks) and waits for it. */
static void hw_end(void) {
    if (!hw_ok) return;
    hw_stop_req = true;
    for (int k = 0; k < 60; k++) {
        bool busy = false;
        for (int i = 0; i < NVOICES; i++) if (!hw_settled(&hw[i])) busy = true;
        if (!busy) break;
        thr_sleep_ms(2);
    }
    for (int i = 0; i < NVOICES; i++) hw[i].state = HV_STOPPED;
    hw_stop_req = false;
}

static void hw_start(int i, const Zone *z, uint32_t freq, uint16_t vl, uint16_t vr) {
    if (!hw_ok) return;
    HwVoice *h = &hw[i];
    u32 level;
    _CPU_ISR_Disable(level);
    if (h->state == HV_RUNNING) {
        /* still running: hand the note to its callback */
        h->data = z->data;
        h->fmt = z->fmt;
        h->packed = z->packed;
        h->aux = z->aux;
        h->aram = z->aram;
        h->first = z->first;
        h->len = z->len;
        h->loop_start = z->loop_start;
        h->loop_end = z->loop_end;
        h->loop = z->loop;
        h->freq = freq;
        h->vl = vl;
        h->vr = vr;
        h->want_stop = false;
        h->gen++;
        _CPU_ISR_Restore(level);
        return;
    }
    _CPU_ISR_Restore(level);
    /* switched off: wait until AESND is done with it (a few ms at most) */
    for (int k = 0; k < 10 && !hw_settled(h); k++) thr_sleep_ms(1);
    if (!hw_settled(h)) return;
    h->data = z->data;
    h->fmt = z->fmt;
    h->packed = z->packed;
    h->aux = z->aux;
    h->aram = z->aram;
    h->first = z->first;
    h->len = z->len;
    h->loop_start = z->loop_start;
    h->loop_end = z->loop_end;
    h->loop = z->loop;
    h->freq = freq;
    h->vl = vl;
    h->vr = vr;
    h->want_stop = false;
    hw_take_note(h);
    h->since_stream = 0;
    h->state = HV_RUNNING;
    AESND_SetVoiceVolume(h->pb, vl, vr);
    AESND_PlayVoice(h->pb, VOICE_MONO16, h->staging, STAGE_SAMPLES * 2, freq, 0, true);
    AESND_SetVoiceStream(h->pb, true);
}

static void hw_set(int i, uint32_t freq, uint16_t vl, uint16_t vr) {
    hw[i].freq = freq;
    hw[i].vl = vl;
    hw[i].vr = vr;
}

static void hw_silence(int i) {
    hw[i].vl = hw[i].vr = 0;
    hw[i].want_stop = true;
}

float midi_dsp_load(void) { return AESND_GetDSPProcessUsage(); }

#else   /* ------------------------------------------- PC software mixer */

typedef struct {
    const int16_t *data;
    uint32_t len, ls, le;
    bool loop, on;
    double pos;
    uint32_t freq;
    float vl, vr;
} SwVoice;
static SwVoice sw[NVOICES];
static void hw_init(void) {}
static void hw_begin(void) {}
static void hw_end(void) { memset(sw, 0, sizeof(sw)); }
static bool hw_ready(int i) { return !sw[i].on; }
static void hw_start(int i, const Zone *z, uint32_t freq, uint16_t vl, uint16_t vr) {
    sw[i] = (SwVoice){ z->data, z->len, z->loop_start, z->loop_end, z->loop, true, 0, freq, vl / 256.0f, vr / 256.0f };
}
static void hw_set(int i, uint32_t freq, uint16_t vl, uint16_t vr) { sw[i].freq = freq; sw[i].vl = vl / 256.0f; sw[i].vr = vr / 256.0f; }
static void hw_silence(int i) { sw[i].on = false; }
float midi_dsp_load(void) { return 0; }

#endif

/* =========================================================== the synth */

static void channels_reset(void) {
    for (int c = 0; c < 16; c++) {
        chans[c] = (Channel){ 0, 0, 100 / 127.0f, 1, 0, false, 0, 2, 127, 127 };
        chans[c].volume = powf(100 / 127.0f, 2);
    }
}

static void voice_free(int i) {
    voices[i].active = false;
    hw_silence(i);
}

static void all_voices_off(void) {
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].active) voice_free(i);
}

static uint32_t voice_freq(const Voice *v) {
    const Channel *c = &chans[v->ch];
    float semis = (v->key - v->z.root) * v->z.scale + v->z.tune + c->bend * c->bend_range;
    double f = v->z.rate * pow(2.0, semis / 12.0);
    if (f < 100) f = 100;
    if (f > MAX_RATE) f = MAX_RATE;
    return (uint32_t)f;
}

static void voice_volume(Voice *v, bool paused, uint16_t *vl, uint16_t *vr) {
    if (paused) { *vl = *vr = 0; return; }
    const Channel *c = &chans[v->ch];
    float master = g_set.music_volume / 10.0f;
    master *= master;
    float amp = v->level * v->vel_gain * v->z.gain * c->volume * c->expression * master * OUT_GAIN;
    if (v->pan_for != c->pan) {
        v->pan_for = c->pan;
        float p = v->z.pan + c->pan;
        if (p < -1) p = -1;
        if (p > 1) p = 1;
        float a = (p + 1) * 0.25f * (float)M_PI;
        v->gl = cosf(a) * 1.4142f;
        v->gr = sinf(a) * 1.4142f;
    }
    float l = amp * v->gl, r = amp * v->gr;
    if (l > 2) l = 2;
    if (r > 2) r = 2;
    *vl = (uint16_t)(l * 256);
    *vr = (uint16_t)(r * 256);
}

static int voice_pick(void) {
    int best = -1;
    /* a free voice the DSP has finished with, else any free voice */
    for (int i = 0; i < NVOICES; i++)
        if (!voices[i].active && hw_ready(i)) return i;
    for (int i = 0; i < NVOICES; i++)
        if (!voices[i].active) return i;
    /* all busy: take the quietest released note, else the oldest note */
    float lowest = 2;
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].released && voices[i].level < lowest) { lowest = voices[i].level; best = i; }
    if (best >= 0) return best;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < NVOICES; i++)
        if (voices[i].order < oldest) { oldest = voices[i].order; best = i; }
    return best;
}

static void note_on(int ch, int key, int vel) {
    bool drums = (ch == 9);
    Zone zs[2];
    int n = instruments_find(chans[ch].bank, chans[ch].program, drums, key, vel, zs, 2);
    for (int k = 0; k < n; k++) {
        /* exclusive class: e.g. a closed hi-hat cuts an open one */
        if (zs[k].exclusive)
            for (int i = 0; i < NVOICES; i++)
                if (voices[i].active && voices[i].ch == ch && voices[i].z.exclusive == zs[k].exclusive && voices[i].started < song_pos) {
                    voices[i].released = true;   /* a very quick fade rather than a click */
                    voices[i].stage = ENV_RELEASE;
                    voices[i].z.release = 0.03f;
                }
        int i = voice_pick();
        if (i < 0) return;
        Voice *v = &voices[i];
        memset(v, 0, sizeof(*v));
        v->active = true;
        v->ch = ch;
        v->key = key;
        v->vel_gain = powf(vel / 127.0f, 1.6f);
        v->z = zs[k];
        v->stage = ENV_ATTACK;
        v->level = v->z.attack <= 0.001f ? 1 : 0;
        if (v->z.attack <= 0.001f) v->stage = ENV_HOLD;
        v->started = song_pos;
        v->order = ++voice_order;
        uint32_t f = voice_freq(v);
        v->freq = f;
        v->bend_for = chans[ch].bend;
        v->pan_for = 1e9f;   /* work out the panning on first use */
        v->ends = v->z.loop ? 1e30 : song_pos + (double)v->z.len / f + 0.02;
        uint16_t vl, vr;
        voice_volume(v, false, &vl, &vr);
        hw_start(i, &v->z, f, vl, vr);
    }
}

static void note_off(int ch, int key) {
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &voices[i];
        if (!v->active || v->ch != ch || v->key != key || v->released) continue;
        if (chans[ch].sustain) v->held = true;
        else { v->released = true; v->stage = ENV_RELEASE; }
    }
}

static void control(int ch, int cc, int val) {
    Channel *c = &chans[ch];
    switch (cc) {
    case 0: c->bank = val; break;
    case 7: c->volume = powf(val / 127.0f, 2); break;
    case 10: c->pan = (val - 64) / 64.0f; break;
    case 11: c->expression = powf(val / 127.0f, 2); break;
    case 64:
        c->sustain = val >= 64;
        if (!c->sustain)
            for (int i = 0; i < NVOICES; i++)
                if (voices[i].active && voices[i].ch == ch && voices[i].held) {
                    voices[i].held = false;
                    voices[i].released = true;
                    voices[i].stage = ENV_RELEASE;
                }
        break;
    case 100: c->rpn_lsb = val; break;
    case 101: c->rpn_msb = val; break;
    case 6:   /* data entry: pitch bend range */
        if (c->rpn_msb == 0 && c->rpn_lsb == 0) c->bend_range = val > 24 ? 24 : val;
        break;
    case 120: case 123:   /* all sound / notes off */
        for (int i = 0; i < NVOICES; i++)
            if (voices[i].active && voices[i].ch == ch) {
                if (cc == 120) voice_free(i);
                else { voices[i].released = true; voices[i].stage = ENV_RELEASE; }
            }
        break;
    case 121:   /* reset controllers */
        c->expression = 1;
        c->sustain = false;
        c->bend = 0;
        c->pan = 0;
        break;
    }
}

static void handle_event(const MidiEvent *e) {
    int ch = e->status & 15;
    switch (e->status & 0xF0) {
    case 0x90: note_on(ch, e->a, e->b); break;
    case 0x80: note_off(ch, e->a); break;
    case 0xB0: control(ch, e->a, e->b); break;
    case 0xC0: chans[ch].program = e->a; break;
    case 0xE0: chans[ch].bend = ((e->b << 7 | e->a) - 8192) / 8192.0f; break;
    }
}

/* Advances every playing note's envelope by dt and sends its pitch and volume. */
static void voices_update(float dt, bool paused) {
    for (int i = 0; i < NVOICES; i++) {
        Voice *v = &voices[i];
        if (!v->active) continue;
        if (!paused) {
            const Zone *z = &v->z;
            v->stage_t += dt;
            switch (v->stage) {
            case ENV_ATTACK:
                v->level += dt / (z->attack > 0.001f ? z->attack : 0.001f);
                if (v->level >= 1) { v->level = 1; v->stage = ENV_HOLD; v->stage_t = 0; }
                break;
            case ENV_HOLD:
                if (v->stage_t >= z->hold) { v->stage = ENV_DECAY; v->stage_t = 0; }
                break;
            case ENV_DECAY:
                if (z->decay <= 0.001f) v->level = z->sustain;
                else v->level = z->sustain + (v->level - z->sustain) * expf(-dt * 5.0f / z->decay);
                if (fabsf(v->level - z->sustain) < 0.002f) { v->level = z->sustain; v->stage = ENV_SUSTAIN; }
                if (v->level < 0.001f) v->stage = ENV_DONE;
                break;
            case ENV_SUSTAIN:
                if (v->level < 0.001f) v->stage = ENV_DONE;
                break;
            case ENV_RELEASE:
                v->level *= expf(-dt * 6.9f / (z->release > 0.01f ? z->release : 0.01f));
                if (v->level < 0.0007f) v->stage = ENV_DONE;
                break;
            }
            if (song_pos >= v->ends) v->stage = ENV_DONE;
            if (v->stage == ENV_DONE) { voice_free(i); continue; }
        }
        uint16_t vl, vr;
        voice_volume(v, paused, &vl, &vr);
        if (v->bend_for != chans[v->ch].bend) { v->bend_for = chans[v->ch].bend; v->freq = voice_freq(v); }
        hw_set(i, v->freq, vl, vr);
    }
}

static int voices_active(void) {
    int n = 0;
    for (int i = 0; i < NVOICES; i++) n += voices[i].active;
    return n;
}

/* ======================================================= the song list */

static void wanted_sf2(char *out, size_t cap);
static char planned[128];

int midi_scan(void) {
    if (!songs) songs = malloc(sizeof(*songs) * MAX_SONGS);
    if (!songs) return 0;
    char keep[NAME_LEN] = "";
    if (lock_ok) thr_lock(&lock);
    if (current >= 0 && current < nsongs) snprintf(keep, sizeof(keep), "%s", songs[current]);
    int n = 0;
    if (storage_ok()) {
        mkdir(MIDI_DIR, 0777);   /* so people can see where songs go */
        DIR *d = opendir(MIDI_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) && n < MAX_SONGS)
                if (e->d_name[0] != '.' && is_song(e->d_name) && strlen(e->d_name) < NAME_LEN)
                    snprintf(songs[n++], NAME_LEN, "%s", e->d_name);
            closedir(d);
        }
    }
    qsort(songs, (size_t)n, NAME_LEN, cmp_names);
    if (!instruments_checked) {
        char w[256];
        wanted_sf2(w, sizeof(w));
        const char *b = strrchr(w, '/');
        snprintf(planned, sizeof(planned), "%s", w[0] ? (b ? b + 1 : w) : "");
    }
    nsongs = n;
    order_n = 0;   /* the list changed: shuffle afresh */
    current = -1;
    for (int i = 0; i < n && keep[0]; i++)
        if (!strcmp(songs[i], keep)) current = i;
    if (lock_ok) thr_unlock(&lock);
    return n;
}

int midi_count(void) { return nsongs; }
const char *midi_file(int i) { return (i >= 0 && i < nsongs) ? songs[i] : ""; }
int midi_current(void) { return current; }

/* The SoundFont the settings ask for: "" when the built-in set is wanted. */
static void wanted_sf2(char *out, size_t cap) {
    out[0] = 0;
    if (!strcmp(g_set.music_sf2, "builtin")) return;
    if (g_set.music_sf2[0]) { snprintf(out, cap, "%s", g_set.music_sf2); return; }
    /* automatic: the first .sf2 in the midi folder */
    DIR *d = opendir(MIDI_DIR);
    if (!d) return;
    char best[NAME_LEN] = "";
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && has_ext(e->d_name, ".sf2") && strlen(e->d_name) < NAME_LEN &&
            (!best[0] || strcasecmp(e->d_name, best) < 0))
            snprintf(best, sizeof(best), "%s", e->d_name);
    closedir(d);
    if (best[0]) snprintf(out, cap, "%s/%s", MIDI_DIR, best);
}

/* =================================================== loading and playing */

/* Loads the wanted instruments if they differ from what's loaded. Called
 * on the sequencer thread with the lock released (it can take seconds). */
static void ensure_instruments(void) {
    char want[256];
    wanted_sf2(want, sizeof(want));
    if (instruments_checked && !strcmp(want, loaded_sf2)) return;
    instruments_checked = true;
    state = MIDI_LOADING;
    hw_end();   /* no voice may still be reading the old samples */
    instruments_unload_sf2();
    instruments_builtin_init();
    snprintf(loaded_sf2, sizeof(loaded_sf2), "%s", want);
    if (want[0]) {
        char err[160];
        if (!instruments_load_sf2(want, err, sizeof(err))) {
            const char *base = strrchr(want, '/');
            snprintf(message, sizeof(message), "Couldn't use %s: %s. Using the built-in instruments.", base ? base + 1 : want, err);
        }
    }
}

static bool load_song(int i) {
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", MIDI_DIR, songs[i]);
    size_t len = 0;
    char *data = storage_read(path, &len);
    if (!data) { snprintf(message, sizeof(message), "Couldn't read %s", songs[i]); return false; }
    char err[120];
    MidiSong s;
    bool ok = midifile_parse((const unsigned char *)data, len, &s, err, sizeof(err));
    free(data);
    if (!ok) { snprintf(message, sizeof(message), "Couldn't play %s: %s", songs[i], err); return false; }
    if (song_loaded) midifile_free(&song);
    song = s;
    song_loaded = true;
    ev_next = 0;
    song_pos = 0;
    tail_until = 0;
    channels_reset();
    if (s.title[0]) snprintf(title, sizeof(title), "%s", s.title);
    else {
        snprintf(title, sizeof(title), "%s", songs[i]);
        char *dot = strrchr(title, '.');
        if (dot) *dot = 0;
    }
    return true;
}

/* Which song comes after (dir +1) or before (-1) the current one; -1 = none. */
/* Shuffle plays every song once, in a random order, before any repeats
 * (like shuffling a deck of cards); with Repeat on, a new order follows
 * that doesn't start with the song that just ended. */

static void shuffle_order(int first, int avoid_first) {
    free(order);
    order = malloc(sizeof(int) * (size_t)(nsongs > 0 ? nsongs : 1));
    order_n = order ? nsongs : 0;
    for (int i = 0; i < order_n; i++) order[i] = i;
    for (int i = order_n - 1; i > 0; i--) {   /* Fisher-Yates */
        int j = rand() % (i + 1), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    if (first >= 0 && first < order_n)
        for (int i = 0; i < order_n; i++)
            if (order[i] == first) { order[i] = order[0]; order[0] = first; break; }
    if (avoid_first >= 0 && order_n > 1 && order[0] == avoid_first) { order[0] = order[1]; order[1] = avoid_first; }
    order_pos = (first >= 0) ? 0 : -1;
}

/* Keeps the shuffle order in step with the song now starting. */
static void shuffle_follow(int song_index) {
    if (!g_set.music_shuffle) return;
    if (order_n != nsongs || !order) { shuffle_order(song_index, -1); return; }
    if (order_pos + 1 < order_n && order[order_pos + 1] == song_index) { order_pos++; return; }
    if (order_pos > 0 && order[order_pos - 1] == song_index) { order_pos--; return; }
    if (order_pos >= 0 && order_pos < order_n && order[order_pos] == song_index) return;
    shuffle_order(song_index, -1);   /* picked by hand: a fresh order from here */
}

/* Which song comes after (dir +1) or before (-1) the current one; -1 = none. */
static int neighbour(int dir) {
    if (nsongs <= 0) return -1;
    if (g_set.music_shuffle && nsongs > 1) {
        if (order_n != nsongs || !order || order_pos < 0) shuffle_order(current, -1);
        if (!order) return -1;
        if (dir > 0) {
            if (order_pos + 1 < order_n) return order[order_pos + 1];
            if (!g_set.music_repeat) return -1;
            shuffle_order(-1, current);   /* everything has played: deal again */
            order_pos = -1;
            return order[0];
        }
        return order_pos > 0 ? order[order_pos - 1] : current;
    }
    int n = current + dir;
    if (n >= nsongs) return g_set.music_repeat ? 0 : -1;
    if (n < 0) return g_set.music_repeat ? nsongs - 1 : 0;
    return n;
}

/* Lists every note the song plays so the SoundFont can load just the
 * samples it needs, then loads them (with the lock released: reading the
 * SD card can take a few seconds). */
static void load_song_instruments(void) {
    if (!instruments_have_sf2()) return;
    int prog[16] = { 0 }, bank[16] = { 0 };
    instruments_need_begin();
    for (int i = 0; i < song.count; i++) {
        const MidiEvent *e = &song.ev[i];
        int ch = e->status & 15;
        switch (e->status & 0xF0) {
        case 0xC0: prog[ch] = e->a; break;
        case 0xB0: if (e->a == 0) bank[ch] = e->b; break;
        case 0x90: instruments_need(bank[ch], prog[ch], ch == 9, e->a, e->b); break;
        }
    }
    state = MIDI_LOADING;
    thr_unlock(&lock);
    hw_end();   /* no voice may read a sample while samples are swapped */
    char msg[160];
    slow_work(true);
    bool ok = instruments_need_load(msg, sizeof(msg));
    slow_work(false);
    thr_lock(&lock);
    if (!ok) snprintf(message, sizeof(message), "%s", msg);
}

/* Starts song i (sequencer thread, lock held). Skips unreadable files. */
static void start_song(int i) {
    all_voices_off();
    for (int tries = 0; tries < nsongs && i >= 0; tries++) {
        current = i;
        message[0] = 0;
        shuffle_follow(i);
        if (load_song(i)) {
            load_song_instruments();
            hw_begin();
            state = MIDI_PLAYING;
            return;
        }
        i = (i + 1 < nsongs) ? i + 1 : -1;
    }
    state = MIDI_STOPPED;
    hw_end();
}

static void stop_song(void) {
    all_voices_off();
    hw_end();
    state = MIDI_STOPPED;
    song_pos = 0;
}

/* One step of the sequencer: plays the events that are due by now. */
static void advance(double dt) {
    song_pos += dt;
    uint32_t pos_us = (uint32_t)(song_pos * 1e6);
    while (ev_next < song.count && song.ev[ev_next].t_us <= pos_us) handle_event(&song.ev[ev_next++]);
    voices_update((float)dt, false);
    if (ev_next >= song.count) {
        if (tail_until == 0) tail_until = song_pos + 4;
        if (voices_active() == 0 || song_pos > tail_until) {
            int n = neighbour(+1);
            if (n < 0) { stop_song(); instruments_free_samples(); }
            else if (req == REQ_NONE) {   /* the next song starts on the next pass */
                all_voices_off();
                req = REQ_PLAY;
                req_song = n;
            }
        }
    }
}

static void process_request(void) {
    int r = req, s = req_song;
    req = REQ_NONE;
    switch (r) {
    case REQ_NEXT:
    case REQ_PREV:
        if (r == REQ_PREV && state != MIDI_STOPPED && song_pos > 3 && current >= 0) s = current;   /* back to its start */
        else s = neighbour(r == REQ_NEXT ? +1 : -1);
        if (s < 0) s = (r == REQ_NEXT && g_set.music_shuffle) ? -1 : 0;
        if (s < 0) { stop_song(); break; }
        /* fall through: play it */
    case REQ_PLAY:
        if (s < 0 || s >= nsongs) break;
        all_voices_off();
        state = MIDI_LOADING;
        thr_unlock(&lock);
        slow_work(true);
        ensure_instruments();   /* may take a while for a big SoundFont */
        slow_work(false);
        thr_lock(&lock);
        if (req != REQ_NONE) break;   /* something else was asked meanwhile */
        start_song(s);
        break;
    case REQ_RELOAD: {
        bool was = (state == MIDI_PLAYING || state == MIDI_PAUSED);
        int cur = current;
        stop_song();
        instruments_checked = false;
        message[0] = 0;
        thr_unlock(&lock);
        slow_work(true);
        ensure_instruments();
        slow_work(false);
        thr_lock(&lock);
        state = MIDI_STOPPED;
        if (was && cur >= 0 && req == REQ_NONE) start_song(cur);
        break;
    }
    case REQ_STOP: stop_song(); instruments_free_samples(); break;   /* music off: its memory goes back to web pages */
    case REQ_PAUSE:
        if (state == MIDI_PLAYING) { state = MIDI_PAUSED; voices_update(0, true); }
        break;
    case REQ_RESUME:
        if (state == MIDI_PAUSED) state = MIDI_PLAYING;
        break;
    }
}

/* ============================================================= threads */

#ifdef GEKKO
static thr_t seq_thread;

static void *seq_main(void *arg) {
    (void)arg;
    double last = now_seconds();
    for (;;) {
        thr_lock(&lock);
        if (req != REQ_NONE) process_request();
        double now = now_seconds(), dt = now - last;
        last = now;
        if (dt > 0.1) dt = 0.1;   /* after a long stall, don't rush through notes */
        if (state == MIDI_PLAYING && song_loaded) advance(dt);
        static uint32_t logged;
        if (stalls != logged) {   /* (in page.log, so it shows if the buzz guard ever had to act) */
            logged = stalls;
            http_log_note("music   | a stuck voice was restarted (%u so far), in %s", (unsigned)logged, title);
        }
        bool busy = (state == MIDI_PLAYING);
        thr_unlock(&lock);
        thr_sleep_ms(busy ? 4 : 20);
    }
    return NULL;
}
#endif

void midi_init(void) {
    lock_ok = thr_mutex_init(&lock);
#ifdef GEKKO
    srand((unsigned)gettime());   /* a different shuffle each time */
#else
    srand((unsigned)time(NULL));
#endif
    channels_reset();
    hw_init();
#ifdef GEKKO
    if (lock_ok) thr_create(&seq_thread, seq_main, NULL, 64 * 1024, 110);   /* above page layout (100) so music never stalls */
#endif
}

/* Stops the music and frees the loaded instrument samples, for jobs that
 * need a lot of memory (unpacking a Swiss update). */
void midi_release_memory(void) {
    if (!lock_ok) return;
    thr_lock(&lock);
    stop_song();
    instruments_free_samples();
    thr_unlock(&lock);
}

void midi_shutdown(void) {
    if (!lock_ok) return;
    thr_lock(&lock);
    stop_song();
    thr_unlock(&lock);
}

static void request(int r, int s) {
    if (!lock_ok) return;
    thr_lock(&lock);
    req = r;
    req_song = s;
    thr_unlock(&lock);
}

void midi_play(int i) { if (i >= 0 && i < nsongs) request(REQ_PLAY, i); }
void midi_stop(void) { request(REQ_STOP, 0); }

void midi_toggle_pause(void) {
    if (state == MIDI_PLAYING) request(REQ_PAUSE, 0);
    else if (state == MIDI_PAUSED) request(REQ_RESUME, 0);
    else if (state == MIDI_STOPPED && nsongs > 0) {
        if (current < 0 && g_set.music_shuffle) request(REQ_NEXT, 0);   /* shuffle: a random first song */
        else request(REQ_PLAY, current >= 0 ? current : 0);
    }
}

void midi_next(void) { if (nsongs) request(REQ_NEXT, 0); }

void midi_prev(void) { if (nsongs) request(REQ_PREV, 0); }

void midi_instruments_changed(void) { request(REQ_RELOAD, 0); }

void midi_set_shuffle(bool on) {
    if (lock_ok) thr_lock(&lock);
    g_set.music_shuffle = on;
    order_pos = -1;   /* a new order starting from the current song */
    order_n = 0;
    if (lock_ok) thr_unlock(&lock);
}
void midi_set_repeat(bool on) { g_set.music_repeat = on; }
bool midi_shuffle(void) { return g_set.music_shuffle != 0; }
bool midi_repeat(void) { return g_set.music_repeat != 0; }

MidiState midi_state(void) { return state; }
const char *midi_title(void) { return state == MIDI_STOPPED && current < 0 ? "" : title; }
float midi_position(void) { return (float)song_pos; }
float midi_length(void) { return song_loaded ? song.length_us / 1e6f : 0; }
int midi_voices_in_use(void) { return voices_active(); }
/* What's loaded; before the first song, what will be loaded. */
const char *midi_instruments(void) {
    if (instruments_checked) return instruments_name();
    return planned[0] ? planned : "Built-in";
}
const char *midi_message(void) { return message; }

#ifndef GEKKO
/* ---- for the PC test program: play song i into a stereo buffer */
bool midi_pc_start(int i) {
    if (i < 0 || i >= nsongs) return false;
    ensure_instruments();
    thr_lock(&lock);
    start_song(i);
    thr_unlock(&lock);
    return state == MIDI_PLAYING;
}

/* Renders frames of 32 kHz stereo; returns false once playback stopped. */
bool midi_pc_render(int16_t *out, int frames) {
    const int rate = 32000, step = 128;   /* the sequencer runs every 4 ms here */
    for (int f = 0; f < frames; f += step) {
        int n = frames - f < step ? frames - f : step;
        thr_lock(&lock);
        if (req != REQ_NONE) process_request();
        if (state == MIDI_PLAYING) advance((double)n / rate);
        thr_unlock(&lock);
        for (int k = 0; k < n; k++) {
            float l = 0, r = 0;
            for (int i = 0; i < NVOICES; i++) {
                SwVoice *v = &sw[i];
                if (!v->on || !v->data) continue;
                uint32_t end = v->loop ? v->le : v->len;
                uint32_t p = (uint32_t)v->pos;
                if (p >= end) {
                    if (v->loop) { v->pos = v->ls + (v->pos - end); p = (uint32_t)v->pos; }
                    else { v->on = false; continue; }
                }
                float s = v->data[p] / 32768.0f;
                l += s * v->vl;
                r += s * v->vr;
                v->pos += (double)v->freq / rate;
            }
            int li = (int)(l * 32767), ri = (int)(r * 32767);
            out[(f + k) * 2] = (int16_t)(li > 32767 ? 32767 : li < -32768 ? -32768 : li);
            out[(f + k) * 2 + 1] = (int16_t)(ri > 32767 ? 32767 : ri < -32768 ? -32768 : ri);
        }
    }
    return state != MIDI_STOPPED;
}
#endif
