/* picache.h - keeps downloaded pictures on the SD card (in its own folder,
 * sd:/webcache, like the music player's sd:/midi), so a page seen before shows its pictures without
 * downloading them again.
 *
 * What's saved is the picture file exactly as it was downloaded (usually a
 * small JPEG or PNG from the image helper), filed under the address it was
 * requested from. Pictures older than a week are downloaded again in case
 * they changed. When the cache is bigger than its limit (Settings >
 * Browsing > Picture cache), the oldest pictures are removed. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define PICACHE_DIR "sd:/webcache"

/* Worker thread. get: a malloc'd copy of the saved picture, or NULL. */
void *picache_get(const char *url, size_t *len);
void  picache_put(const char *url, const void *data, size_t len);

/* UI thread: removes everything. Returns how many bytes were freed. */
unsigned long long picache_clear(void);

bool picache_enabled(void);
