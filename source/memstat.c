/* memstat.c - see memstat.h. */
#include "memstat.h"
#ifdef GEKKO
#include <gccore.h>
#include <malloc.h>

size_t mem_free(void) {
    struct mallinfo mi = mallinfo();
    size_t heap_free = (size_t)mi.fordblks;
    unsigned long lo = (unsigned long)SYS_GetArena1Lo(), hi = (unsigned long)SYS_GetArena1Hi();
    size_t arena_free = hi > lo ? (size_t)(hi - lo) : 0;
    return heap_free + arena_free;
}
#else
size_t mem_free(void) { return 64u * 1024u * 1024u; }   /* test programs on a PC */
#endif
