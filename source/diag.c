/* diag.c - see diag.h */
#include "diag.h"
#include "storage.h"
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
        char buf[1024];
        int n = snprintf(buf, sizeof(buf),
                         "CubeBrowser freeze report\n"
                         "frames drawn: %u\n"
                         "main:   %s\n"
                         "page:   %s\n"
                         "worker: %s\n",
                         frames, notes[DIAG_MAIN], notes[DIAG_PAGE], notes[DIAG_WORKER]);
        if (n > 0) {
            FILE *f = fopen(STORAGE_DIR "/freeze.log", "wb");
            if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }
        }
    }
    return NULL;
}

void diag_init(void) {
    /* above page layout (100), below the network stack's own thread */
    LWP_CreateThread(&watchdog, watchdog_main, NULL, NULL, 16 * 1024, 120);
}
#else
void diag_init(void) {}
#endif
