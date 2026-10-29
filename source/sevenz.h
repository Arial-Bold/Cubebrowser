/* sevenz.h - extracts downloaded .7z files on the SD card (LZMA and LZMA2,
 * the usual 7-Zip methods), with the same progress as unzip.c.
 *
 * A .7z packs its files into blocks that must be unpacked whole, so each
 * block has to fit in memory: archives made with very large solid blocks
 * can be too big to unpack on the GameCube (it says so). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "unzip.h"

/* Extracts archive_path into dest_dir (created if needed). */
bool sevenz_file(const char *archive_path, const char *dest_dir, UnzipProgress *pg, volatile int *cancel,
                 char *err, size_t errlen);
