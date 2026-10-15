/* unzip.h - extracts .zip files on the SD card (stored and deflate, ZIP64,
 * folders), streaming so even very large files need little memory. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    volatile uint64_t done, total;     /* uncompressed bytes */
    volatile int files_done, files_total;
    char current[128];                 /* file being extracted */
} UnzipProgress;

/* Extracts zip_path into dest_dir (created if needed). */
bool unzip_file(const char *zip_path, const char *dest_dir, UnzipProgress *pg, volatile int *cancel,
                char *err, size_t errlen);

/* Creates a directory and any missing parents. */
bool make_dirs(const char *path);
