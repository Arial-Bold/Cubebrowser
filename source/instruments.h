/* instruments.h - where the music player's instrument sounds come from:
 * a SoundFont (.sf2) file when one is on the SD card, otherwise a small set
 * of instruments generated at start-up. Either way a note becomes one or two
 * "zones": a recorded sample with its loop, tuning, loudness and envelope,
 * which the GameCube's sound processor then plays at the right pitch. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const int16_t *data;      /* samples in the console's byte order */
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

/* Loads a SoundFont from path; on failure keeps (or falls back to) the
 * built-in instruments and returns false with a reason. */
bool instruments_load_sf2(const char *path, char *err, size_t errlen);
/* Makes the built-in set ready (cheap; call once). */
void instruments_builtin_init(void);
void instruments_unload_sf2(void);
bool instruments_have_sf2(void);
const char *instruments_name(void);   /* SoundFont's name, or "Built-in" */

/* Fills up to max zones for a note. drums: the note is on the percussion
 * channel (bank is then the drum kit's program). Returns how many. */
int instruments_find(int bank, int program, bool drums, int key, int vel, Zone *out, int max);
