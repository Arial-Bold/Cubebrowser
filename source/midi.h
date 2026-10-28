/* midi.h - the music player: plays .mid files from the "midi" folder on the
 * SD card in the background while you browse. A sequencer thread reads the
 * song and, like a GameCube game's sound driver, plays each note on one of
 * the sound processor's (DSP) hardware voices with a recorded instrument
 * sample (instruments.h), pitched, enveloped and panned. */
#pragma once
#include <stdbool.h>

#define MIDI_DIR "sd:/midi"

typedef enum { MIDI_STOPPED, MIDI_LOADING, MIDI_PLAYING, MIDI_PAUSED } MidiState;

void midi_init(void);          /* after sound_init() */
void midi_shutdown(void);      /* stops everything (before launching another program) */
void midi_release_memory(void);   /* stops the music and frees its samples */

/* the song list (the midi folder, sorted by name) */
int  midi_scan(void);          /* re-reads the folder; returns how many songs */
int  midi_count(void);
const char *midi_file(int i);  /* file name */
int  midi_current(void);       /* index of the playing/selected song, or -1 */

void midi_play(int i);
void midi_toggle_pause(void);  /* play/pause; starts the first song when stopped */
void midi_stop(void);
void midi_next(void);
void midi_prev(void);
void midi_set_shuffle(bool on);
void midi_set_repeat(bool on);
bool midi_shuffle(void);
bool midi_repeat(void);

MidiState midi_state(void);
const char *midi_title(void);     /* song name (from the file, else the file name) */
float midi_position(void);        /* seconds */
float midi_length(void);          /* seconds */
int   midi_voices_in_use(void);
const char *midi_instruments(void);   /* "Built-in" or the SoundFont's name */
const char *midi_message(void);   /* a problem to show, or "" */
float midi_dsp_load(void);         /* % of the sound processor's time in use */
unsigned midi_audio_irq_worst_us(void);   /* the longest the music's audio interrupt has run (us) */

/* The Instruments setting changed: load the new SoundFont (in the background). */
void midi_instruments_changed(void);

#ifndef GEKKO
#include <stdint.h>
bool midi_pc_start(int i);                       /* test programs only */
bool midi_pc_render(int16_t *out, int frames);   /* 32 kHz stereo */
#endif
