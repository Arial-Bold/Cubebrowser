/* diag.c - see diag.h */
#include "stackwatch.h"
#include "diag.h"
#include "storage.h"
#include "midi.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char notes[DIAG_SLOTS][160];
static volatile unsigned frames;

void diag_note(int slot, const char *fmt, ...) {
    if (slot < 0 || slot >= DIAG_SLOTS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notes[slot], sizeof(notes[slot]), fmt, ap);
    va_end(ap);
}

void diag_frame(void) { frames++; }

const char *diag_get(int slot) { return (slot >= 0 && slot < DIAG_SLOTS) ? notes[slot] : ""; }

#ifdef GEKKO
#include <gccore.h>
#include <unistd.h>

static lwp_t watchdog;

static void *watchdog_main(void *arg) {
    (void)arg;
    unsigned last = frames, still = 0;
    bool written = false;
    for (;;) {
        usleep(500 * 1000);
        if (frames != last) { last = frames; still = 0; written = false; continue; }
        if (++still < 20 || written) continue;   /* 10 seconds without a new frame */
        written = true;
        /* is the audio interrupt still running? (if not, interrupts are
         * stuck off, not just the screen's thread) */
        unsigned a0 = midi_audio_blocks();
        usleep(200 * 1000);
        unsigned a1 = midi_audio_blocks();
        /* and the graphics chip: busy with a frame, or stuck with its queue full? */
        u8 over = 0, under = 0, ridle = 0, cidle = 0, brk = 0;
        GX_GetGPStatus(&over, &under, &ridle, &cidle, &brk);
        GXFifoObj fo;
        GX_GetCPUFifo(&fo);
        unsigned fifo_kb = (unsigned)(GX_GetFifoCount(&fo) / 1024);
        char buf[1024];
        int n = snprintf(buf, sizeof(buf),
                         "CubeBrowser freeze report\n"
                         "frames drawn: %u\n"
                         "main:   %s\n"
                         "page:   %s\n"
                         "worker: %s\n"
                         "drawn:  %s\n"
                         "music:  %s\n"
                         "audio:  %s (%u blocks in 0.2 s)\n"
                         "graphics: %u KB queued, %s, %s%s\n",
                         frames, notes[DIAG_MAIN], notes[DIAG_PAGE], notes[DIAG_WORKER], notes[DIAG_DRAW],
                         notes[DIAG_MUSIC], a1 != a0 ? "still running" : "stopped", a1 - a0,
                         fifo_kb, cidle ? "idle" : "busy", ridle ? "reading done" : "still reading", over ? ", queue over its limit" : "");
        if (n > 0) {
            char path[400];
            storage_path(path, sizeof(path), "freeze.log");
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }
        }
    }
    return NULL;
}

void diag_init(void) {
    /* above page layout (100), below the network stack's own thread */
    LWP_CreateThread(&watchdog, watchdog_main, NULL, stack_new(16 * 1024, "watchdog"), 16 * 1024, 120);
}
#else
void diag_init(void) {}
#endif
