/* storage.h - SD card (SD2SP2 / SD Gecko) mounting and file helpers. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define STORAGE_DIR "sd:/gcbrowser"

bool storage_init(void);
bool storage_ok(void);
const char *storage_device_name(void);
char *storage_read(const char *path, size_t *len_out);  /* malloc'd, NUL-terminated */
bool storage_write_atomic(const char *path, const char *tmp_path, const void *data, size_t len);
