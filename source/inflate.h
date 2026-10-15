/* inflate.h - streaming DEFLATE decompressor (RFC 1951) for unzipping.
 * Pulls compressed bytes through a read callback and pushes output through a
 * write callback, using a fixed 32 KB window, so files of any size can be
 * unpacked with very little memory. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int (*InflateRead)(void *user, unsigned char *buf, int max);           /* bytes read, 0 = end */
typedef int (*InflateWrite)(void *user, const unsigned char *data, size_t len); /* 0 = ok, <0 = stop */

/* Returns 0 on success, or a negative error:
 * -1 corrupt data, -2 unexpected end of data, -3 write failed/stopped, -4 out of memory. */
int inflate_stream(InflateRead rd, void *ru, InflateWrite wr, void *wu, uint64_t *out_bytes, uint32_t *out_crc);

uint32_t crc32_update(uint32_t crc, const unsigned char *data, size_t len);
