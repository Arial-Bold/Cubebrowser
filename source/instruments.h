/* instruments.h - where the music player's instrument sounds come from:
 * a SoundFont (.sf2) file when one is on the SD card, otherwise a small set
 * of instruments generated at start-up. Either way a note becomes one or two
 * "zones": a recorded sample with its loop, tuning, loudness and envelope,
 * which the GameCube's sound processor then plays at the right pitch. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "packsamp.h"

enum { SFMT_PCM16, SFMT_BLOCK8, SFMT_ADPCM };

typedef struct {
    const int16_t *data;      /* 16-bit samples in the console's byte order (SFMT_PCM16) */
    uint8_t fmt;              /* or packed smaller (packsamp.h): */
    const void *packed;       /*   BLOCK8: int8 samples; ADPCM: 4-bit codes */
    const void *aux;          /*   BLOCK8: scales; ADPCM: checkpoints */
    uint32_t first;           /*   where in them this zone's sample 0 is */
    uint32_t len;             /* playable length in samples */
    uint32_t loop_start, loop_end;
    bool loop;
    uint32_t rate;            /* sample rate in Hz */
    float root;               /* MIDI key at which the sample plays at its own rate */
    float tune;               /* extra tuning in semitones */
    float scale;              /* semitones per key (1 = normal, 0 = fixed pitch) */
    float gain;               /* linear loudness from the instrument's attenuation */
    float pan;                /* -1 left .. +1 right */
    float attack, hold, decay, sustain, release;   /* seconds; sustain is a level 0..1 */
    int exclusive;            /* > 0: a new note in this class cuts the others (e.g. hi-hats) */
} Zone;

/* Opens a SoundFont (any size: only its list of instruments is read now;
 * samples are read per song). On failure the built-in instruments are used
 * and false is returned with a reason. */
bool instruments_load_sf2(const char *path, char *err, size_t errlen);
/* Makes the built-in set ready (cheap; call once). */
void instruments_builtin_init(void);
void instruments_unload_sf2(void);
bool instruments_have_sf2(void);
const char *instruments_name(void);   /* SoundFont's name, or "Built-in" */
size_t instruments_memory(void);      /* bytes of SoundFont samples loaded now */
void instruments_free_samples(void);  /* gives their memory back (no voice may be playing) */

/* Before a song: list every note it plays (begin, then need for each), then
 * load the samples those notes use from the SD card. Returns false (with a
 * message) if some didn't fit; those notes use the built-in sounds. */
void instruments_need_begin(void);
void instruments_need(int bank, int program, bool drums, int key, int vel);
bool instruments_need_load(char *msg, size_t msglen);

/* Fills up to max zones for a note. drums: the note is on the percussion
 * channel (bank is then the drum kit's program). Returns how many. */
int instruments_find(int bank, int program, bool drums, int key, int vel, Zone *out, int max);
