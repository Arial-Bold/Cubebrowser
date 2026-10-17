/* midifile.h - reads Standard MIDI Files (.mid, .midi, .kar, and .rmi with
 * its RIFF wrapper) into one time-ordered list of channel events, with the
 * tempo map already applied so each event has a time in microseconds. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t t_us;       /* when, from the start of the song */
    uint8_t status;      /* 0x80..0xEF: event type | channel (note-on with velocity 0 is stored as note-off) */
    uint8_t a, b;        /* data bytes */
    uint8_t pad;
} MidiEvent;

typedef struct {
    MidiEvent *ev;
    int count;
    uint32_t length_us;  /* time of the last event */
    char title[64];      /* the song's name from the file, or "" */
} MidiSong;

/* Parses data (len bytes). On failure returns false with a short reason. */
bool midifile_parse(const unsigned char *data, size_t len, MidiSong *out, char *err, size_t errlen);
void midifile_free(MidiSong *s);
