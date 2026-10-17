/* sound.c - soft "blip" and chime sounds generated at startup and played on
 * the DSP's hardware voices through libogc2's AESND library (which the music
 * player uses too). Everything is synthesised, nothing is sampled. */
#include "sound.h"
#include "settings.h"
#include <gccore.h>
#include <aesndlib.h>
#include <ogc/lwp_watchdog.h>
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

/* A few voices of their own; each plays one sound once and stops itself. */
#define SFX_VOICES 4
static AESNDPB *sfx_voice[SFX_VOICES];
static u64 sfx_busy_until[SFX_VOICES];
static int sfx_next;

void sound_init(void) {
    AESND_Init();
    AESND_Pause(false);
    for (int i = 0; i < SFX_VOICES; i++) sfx_voice[i] = AESND_AllocateVoice(NULL);
    for (int i = 0; i < SFX_COUNT; i++) synth(i);
    ready = true;
}

void sound_shutdown(void) {
    if (!ready) return;
    ready = false;
    AESND_Pause(true);
    AESND_Reset();
}

void sound_play(Sfx s) {
    if (!ready || !g_set.sounds || g_set.volume == 0 || !buffers[s]) return;
    /* prefer a voice whose last sound has finished: the audio interrupt
     * isn't using it, so starting it can't collide */
    u64 now = gettime();
    int v = -1;
    for (int k = 0; k < SFX_VOICES && v < 0; k++) {
        int i = (sfx_next + k) % SFX_VOICES;
        if (sfx_voice[i] && now >= sfx_busy_until[i]) v = i;
    }
    if (v < 0) return;
    sfx_next = (v + 1) % SFX_VOICES;
    u16 vol = (u16)(g_set.volume * 255 / 10);
    AESND_SetVoiceVolume(sfx_voice[v], vol, vol);
    AESND_PlayVoice(sfx_voice[v], VOICE_MONO16, buffers[s], sizes[s], RATE, 0, false);
    sfx_busy_until[v] = now + millisecs_to_ticks((sizes[s] / 2) * 1000 / RATE + 30);
}
