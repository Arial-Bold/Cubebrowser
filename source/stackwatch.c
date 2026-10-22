/* stackwatch.c - see stackwatch.h. */
#include "stackwatch.h"
#include "http.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef GEKKO
#include <malloc.h>
#endif

#define FILL 0x5A17C0DEu
#define MAX_WATCHED 8

static struct { uint32_t *base; size_t words; const char *name; int told; } watched[MAX_WATCHED];
static int nwatched;

void *stack_new(size_t size, const char *name) {
#ifdef GEKKO
    if (nwatched >= MAX_WATCHED) return NULL;
    uint32_t *s = memalign(32, size);
    if (!s) return NULL;
    size_t words = size / 4;
    for (size_t i = 0; i < words; i++) s[i] = FILL;
    watched[nwatched].base = s;
    watched[nwatched].words = words;
    watched[nwatched].name = name;
    nwatched++;
    return s;
#else
    (void)size; (void)name;
    return NULL;
#endif
}

void stack_check(void) {
    for (int i = 0; i < nwatched; i++) {
        /* stacks grow down: the untouched part is at the bottom */
        size_t free_words = 0;
        while (free_words < watched[i].words && watched[i].base[free_words] == FILL) free_words++;
        size_t used = (watched[i].words - free_words) * 4, total = watched[i].words * 4;
        int level = free_words * 4 < 256 ? 2 : free_words * 4 < 1024 ? 1 : 0;
        if (level > watched[i].told) {
            watched[i].told = level;
            http_log_note("threads | the %s thread %s its stack: %u of %u KB used", watched[i].name,
                          level == 2 ? "ran out of (or nearly out of)" : "came close to the end of",
                          (unsigned)(used / 1024), (unsigned)(total / 1024));
        }
    }
}
