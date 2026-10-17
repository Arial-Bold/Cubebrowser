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
#include <ogc/lwp_watchdog.h>
#else
#include <time.h>
#endif

#define NVOICES 24          /* hardware voices used for music (the DSP has 32) */
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
enum { REQ_NONE, REQ_PLAY, REQ_STOP, REQ_PAUSE, REQ_RESUME, REQ_RELOAD };
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

/* ===================================================== hardware voices */

#ifdef GEKKO

#define STAGE_SAMPLES 1152    /* one DSP stream block is 576 samples; the first fill takes two */

typedef struct {
    AESNDPB *pb;
    /* written by the sequencer; a new note is published by bumping gen */
    const int16_t *data;
    uint32_t len, loop_start, loop_end;
    bool loop;
    volatile uint32_t gen;
    volatile uint32_t freq;
    volatile uint16_t vl, vr;
    /* owned by the voice callback (runs in the audio interrupt) */
    uint32_t applied;
    const int16_t *c_data;
    uint32_t c_len, c_ls, c_le, pos;
    bool c_loop, done;
    volatile bool running;
    int16_t staging[STAGE_SAMPLES] __attribute__((aligned(32)));
} HwVoice;

static HwVoice hw[NVOICES] __attribute__((aligned(32)));
static volatile bool hw_stop_req;
static bool hw_on, hw_ok;

/* Copies the next n samples of the note into dst, wrapping at the loop. */
static void hw_fill(HwVoice *h, int16_t *dst, int n) {
    while (n > 0) {
        if (!h->c_data || h->done) { memset(dst, 0, (size_t)n * 2); return; }
        uint32_t end = h->c_loop ? h->c_le : h->c_len;
        if (h->pos >= end) {
            if (h->c_loop) { h->pos = h->c_ls; continue; }
            h->done = true;
            continue;
        }
        uint32_t k = end - h->pos;
        if (k > (uint32_t)n) k = (uint32_t)n;
        memcpy(dst, h->c_data + h->pos, k * 2);
        dst += k;
        n -= (int)k;
        h->pos += k;
    }
}

/* Called by AESND for each voice every 2 ms (RUNNING) and whenever the
 * voice has used up its block of samples (STREAM). Integer work only. */
static void hw_callback(AESNDPB *pb, u32 st) {
    HwVoice *h = AESND_GetVoiceUserData(pb);
    if (!h) return;
    if (st == VOICE_STATE_RUNNING) {
        h->running = true;
        if (hw_stop_req) {
            AESND_SetVoiceStop(pb, true);
            h->running = false;
            return;
        }
        if (h->gen != h->applied) {   /* a new note on this voice */
            h->applied = h->gen;
            h->c_data = h->data;
            h->c_len = h->len;
            h->c_ls = h->loop_start;
            h->c_le = h->loop_end;
            h->c_loop = h->loop;
            h->pos = 0;
            h->done = false;
            hw_fill(h, h->staging, STAGE_SAMPLES);
            AESND_PlayVoice(pb, VOICE_MONO16, h->staging, STAGE_SAMPLES * 2, h->freq, 0, true);
            AESND_SetVoiceStream(pb, true);
        }
        AESND_SetVoiceFrequency(pb, h->freq);
        AESND_SetVoiceVolume(pb, h->vl, h->vr);
    } else if (st == VOICE_STATE_STREAM) {
        hw_fill(h, h->staging, STAGE_SAMPLES / 2);
        AESND_SetVoiceBuffer(pb, h->staging, STAGE_SAMPLES);
    }
}

static void hw_init(void) {
    for (int i = 0; i < NVOICES; i++) {
        memset(&hw[i], 0, sizeof(hw[i]));
        hw[i].pb = AESND_AllocateVoice(hw_callback);
        if (!hw[i].pb) return;
        AESND_SetVoiceUserData(hw[i].pb, &hw[i]);
    }
    hw_ok = true;
}

/* Starts all music voices, silent (only while music plays, to spare the DSP). */
static void hw_begin(void) {
    if (!hw_ok || hw_on) return;
    hw_stop_req = false;
    for (int i = 0; i < NVOICES; i++) {
        HwVoice *h = &hw[i];
        h->data = h->c_data = NULL;
        h->vl = h->vr = 0;
        h->freq = 8000;
        h->applied = h->gen;
        memset(h->staging, 0, sizeof(h->staging));
        /* stopped voices aren't touched by the interrupt, so this is safe */
        AESND_PlayVoice(h->pb, VOICE_MONO16, h->staging, STAGE_SAMPLES * 2, 8000, 0, true);
        AESND_SetVoiceVolume(h->pb, 0, 0);
        AESND_SetVoiceStream(h->pb, true);
        h->running = true;
        AESND_SetVoiceStop(h->pb, false);
    }
    hw_on = true;
}

/* Stops the voices from inside their own callback, then waits for it. */
static void hw_end(void) {
    if (!hw_on) return;
    hw_stop_req = true;
    for (int k = 0; k < 40; k++) {
        bool any = false;
        for (int i = 0; i < NVOICES; i++) if (hw[i].running) any = true;
        if (!any) break;
        thr_sleep_ms(5);
    }
    thr_sleep_ms(10);
    hw_on = false;
}

static void hw_start(int i, const Zone *z, uint32_t freq, uint16_t vl, uint16_t vr) {
    HwVoice *h = &hw[i];
    h->data = z->data;
    h->len = z->len;
    h->loop_start = z->loop_start;
    h->loop_end = z->loop_end;
    h->loop = z->loop;
    h->freq = freq;
    h->vl = vl;
    h->vr = vr;
    __asm__ volatile("sync" ::: "memory");
    h->gen++;
}

static void hw_set(int i, uint32_t freq, uint16_t vl, uint16_t vr) {
    hw[i].freq = freq;
    hw[i].vl = vl;
    hw[i].vr = vr;
}

static void hw_silence(int i) {
    HwVoice *h = &hw[i];
    h->vl = h->vr = 0;
    h->data = NULL;
    h->freq = 8000;
    __asm__ volatile("sync" ::: "memory");
    h->gen++;
}

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
static void hw_start(int i, const Zone *z, uint32_t freq, uint16_t vl, uint16_t vr) {
    sw[i] = (SwVoice){ z->data, z->len, z->loop_start, z->loop_end, z->loop, true, 0, freq, vl / 256.0f, vr / 256.0f };
}
static void hw_set(int i, uint32_t freq, uint16_t vl, uint16_t vr) { sw[i].freq = freq; sw[i].vl = vl / 256.0f; sw[i].vr = vr / 256.0f; }
static void hw_silence(int i) { sw[i].on = false; }

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

static void voice_volume(const Voice *v, bool paused, uint16_t *vl, uint16_t *vr) {
    if (paused) { *vl = *vr = 0; return; }
    const Channel *c = &chans[v->ch];
    float master = g_set.music_volume / 10.0f;
    master *= master;
    float amp = v->level * v->vel_gain * v->z.gain * c->volume * c->expression * master * OUT_GAIN;
    float p = v->z.pan + c->pan;
    if (p < -1) p = -1;
    if (p > 1) p = 1;
    float a = (p + 1) * 0.25f * (float)M_PI;
    float l = amp * cosf(a) * 1.4142f, r = amp * sinf(a) * 1.4142f;
    if (l > 2) l = 2;
    if (r > 2) r = 2;
    *vl = (uint16_t)(l * 256);
    *vr = (uint16_t)(r * 256);
}

static int voice_pick(void) {
    int best = -1;
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
                if (voices[i].active && voices[i].ch == ch && voices[i].z.exclusive == zs[k].exclusive && voices[i].started < song_pos)
                    voice_free(i);
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
        hw_set(i, voice_freq(v), vl, vr);
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
static int neighbour(int dir) {
    if (nsongs <= 0) return -1;
    if (g_set.music_shuffle && nsongs > 1) {
        int r;
        do r = rand() % nsongs; while (r == current);
        return r;
    }
    int n = current + dir;
    if (n >= nsongs) return g_set.music_repeat ? 0 : -1;
    if (n < 0) return g_set.music_repeat ? nsongs - 1 : 0;
    return n;
}

/* Starts song i (sequencer thread, lock held). Skips unreadable files. */
static void start_song(int i) {
    all_voices_off();
    for (int tries = 0; tries < nsongs && i >= 0; tries++) {
        current = i;
        message[0] = 0;
        if (load_song(i)) {
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
            if (n >= 0) start_song(n);
            else stop_song();
        }
    }
}

static void process_request(void) {
    int r = req, s = req_song;
    req = REQ_NONE;
    switch (r) {
    case REQ_PLAY:
        if (s < 0 || s >= nsongs) break;
        all_voices_off();
        state = MIDI_LOADING;
        thr_unlock(&lock);
        ensure_instruments();   /* may take a while for a big SoundFont */
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
        ensure_instruments();
        thr_lock(&lock);
        state = MIDI_STOPPED;
        if (was && cur >= 0 && req == REQ_NONE) start_song(cur);
        break;
    }
    case REQ_STOP: stop_song(); break;
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
        bool busy = (state == MIDI_PLAYING);
        thr_unlock(&lock);
        thr_sleep_ms(busy ? 3 : 20);
    }
    return NULL;
}
#endif

void midi_init(void) {
    lock_ok = thr_mutex_init(&lock);
    channels_reset();
    hw_init();
#ifdef GEKKO
    if (lock_ok) thr_create(&seq_thread, seq_main, NULL, 64 * 1024, 90);
#endif
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
    else if (state == MIDI_STOPPED && nsongs > 0) request(REQ_PLAY, current >= 0 ? current : 0);
}

void midi_next(void) {
    int n = neighbour(+1);
    if (n < 0) n = 0;
    if (nsongs) request(REQ_PLAY, n);
}

void midi_prev(void) {
    if (state != MIDI_STOPPED && song_pos > 3 && current >= 0) { request(REQ_PLAY, current); return; }
    int n = current < 0 ? 0 : neighbour(-1);
    if (n < 0) n = 0;
    if (nsongs) request(REQ_PLAY, n);
}

void midi_instruments_changed(void) { request(REQ_RELOAD, 0); }

void midi_set_shuffle(bool on) { g_set.music_shuffle = on; }
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
    start_song(i);
    return state == MIDI_PLAYING;
}

/* Renders frames of 32 kHz stereo; returns false once playback stopped. */
bool midi_pc_render(int16_t *out, int frames) {
    const int rate = 32000, step = 128;   /* the sequencer runs every 4 ms here */
    for (int f = 0; f < frames; f += step) {
        int n = frames - f < step ? frames - f : step;
        if (state == MIDI_PLAYING) advance((double)n / rate);
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
