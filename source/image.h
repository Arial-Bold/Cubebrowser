/* image.h - decodes JPEG/PNG/GIF/BMP (stb_image), shrinks to fit, and
 * converts to the GameCube's RGB5A3 tiled texture format. Runs on the
 * network worker thread. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    void *tex;          /* 32-byte aligned RGB5A3 tiled data, already cache-flushed */
    int w, h;           /* picture size */
    int tex_w, tex_h;   /* texture size (multiples of 4) */
    size_t bytes;
} DecodedImage;

/* max_src_pixels guards memory: larger images are refused. */
bool image_decode(const unsigned char *data, size_t len, int max_w, int max_h, long max_src_pixels,
                  DecodedImage *out, char *err, size_t errlen);
void image_free(DecodedImage *img);
