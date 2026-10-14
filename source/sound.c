/* sound.c - soft "blip" and chime sounds generated at startup and played
 * through libogc's ASND mixer. Everything is synthesised, nothing is sampled. */
#include "sound.h"
#include "settings.h"
#include <gccore.h>
#include <asndlib.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#define RATE 32000

typedef struct { float f0, f1, ms, gain; int wave; } Note;   /* wave: 0 sine, 1 triangle */
typedef struct { const Note *notes; int count; float gap_ms; } Recipe;

static const Note n_hover[]  = { { 1320, 1400, 28, 0.18f, 0 } };
static const Note n_select[] = { { 880, 880, 55, 0.35f, 1 }, { 1320, 1320, 90, 0.30f, 1 } };
static const Note n_back[]   = { { 990, 660, 90, 0.30f, 1 } };
static const Note n_error[]  = { { 220, 200, 120, 0.35f, 1 }, { 196, 180, 160, 0.35f, 1 } };
static const Note n_key[]    = { { 1760, 1700, 22, 0.20f, 0 } };
static const Note n_open[]   = { { 660, 990, 110, 0.30f, 0 } };
static const Note n_close[]  = { { 990, 600, 110, 0.28f, 0 } };
static const Note n_loaded[] = { { 1046, 1046, 90, 0.25f, 0 }, { 1318, 1318, 90, 0.25f, 0 }, { 1568, 1568, 220, 0.25f, 0 } };

static const Recipe recipes[SFX_COUNT] = {
    { n_hover, 1, 0 }, { n_select, 2, 0 }, { n_back, 1, 0 }, { n_error, 2, 20 },
    { n_key, 1, 0 }, { n_open, 1, 0 }, { n_close, 1, 0 }, { n_loaded, 3, 0 },
};

static s16 *buffers[SFX_COUNT];
static u32 sizes[SFX_COUNT];
static bool ready;

static float wave_at(int wave, float phase) {
    if (wave == 0) return sinf(phase * 2.0f * (float)M_PI);
    float p = phase - floorf(phase);
    return p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
}

static void synth(int id) {
    const Recipe *r = &recipes[id];
    float total_ms = 0;
    for (int i = 0; i < r->count; i++) total_ms += r->notes[i].ms + r->gap_ms;
    u32 samples = (u32)(RATE * (total_ms + 40) / 1000.0f);
    u32 bytes = (samples * 2 + 31) & ~31u;
    s16 *buf = memalign(32, bytes);
    if (!buf) return;
    memset(buf, 0, bytes);
    u32 pos = 0;
    for (int i = 0; i < r->count; i++) {
        const Note *n = &r->notes[i];
        u32 len = (u32)(RATE * n->ms / 1000.0f);
        float phase = 0;
        for (u32 s = 0; s < len && pos + s < samples; s++) {
            float t = (float)s / len;
            float f = n->f0 + (n->f1 - n->f0) * t;
            phase += f / RATE;
            float attack = fminf(1.0f, s / (RATE * 0.003f));
            float env = attack * expf(-3.5f * t);
            float v = wave_at(n->wave, phase) + 0.25f * wave_at(0, phase * 2.0f);  /* soft bell overtone */
            buf[pos + s] = (s16)(v * env * n->gain * 26000.0f);
        }
        pos += len + (u32)(RATE * r->gap_ms / 1000.0f);
    }
    DCFlushRange(buf, bytes);
    buffers[id] = buf;
    sizes[id] = bytes;
}

void sound_init(void) {
    ASND_Init();
    ASND_Pause(0);
    for (int i = 0; i < SFX_COUNT; i++) synth(i);
    ready = true;
}

void sound_play(Sfx s) {
    if (!ready || !g_set.sounds || g_set.volume == 0 || !buffers[s]) return;
    s32 voice = ASND_GetFirstUnusedVoice();
    if (voice < 0) return;
    int vol = g_set.volume * 255 / 10;
    ASND_SetVoice(voice, VOICE_MONO_16BIT, RATE, 0, buffers[s], (s32)sizes[s], vol, vol, NULL);
}
