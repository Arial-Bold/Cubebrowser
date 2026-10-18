/* memstat.h - how much memory is left, so big jobs (pictures, music
 * samples) can hold back before the console runs out. */
#pragma once
#include <stddef.h>

/* Bytes malloc can still hand out: free space inside the heap plus the part
 * of main memory the heap hasn't grown into yet. (Not all of it in one
 * piece, so leave a margin.) */
size_t mem_free(void);
