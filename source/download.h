/* download.h - saves a file from the web straight to the SD card.
 * Network reading and SD writing overlap (a separate writer thread with two
 * 256 KB buffers), interrupted downloads resume from a ".part" file, and
 * free space is checked first. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    volatile long long done, total;   /* bytes; total -1 if unknown */
    volatile int speed;               /* bytes per second (smoothed) */
    volatile bool resumed;
    volatile int connections;         /* connections in use (0 or 1 = single) */
} DownloadProgress;

/* dest_path is the final file name; data goes to dest_path + ".part" until complete. */
bool download_to_file(const char *url, const char *dest_path, DownloadProgress *pg, volatile int *cancel,
                      char *err, size_t errlen);

/* Bytes free on the card holding path, or -1 if unknown. */
long long free_space(const char *path);
