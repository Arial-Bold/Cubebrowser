/* image.c - stb_image decode -> area-average downscale -> RGB5A3 4x4 tiles. */
#include "image.h"
#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"

/* RGB5A3: opaque pixels use 1-RRRRR-GGGGG-BBBBB, translucent 0-AAA-RRRR-GGGG-BBBB */
static inline unsigned short enc_rgb5a3(unsigned r, unsigned g, unsigned b, unsigned a) {
    if (a >= 0xE0) return (unsigned short)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    return (unsigned short)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}

bool image_decode(const unsigned char *data, size_t len, int max_w, int max_h, long max_src_pixels,
                  DecodedImage *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    int sw, sh, comp;
    if (!stbi_info_from_memory(data, (int)len, &sw, &sh, &comp)) {
        snprintf(err, errlen, "unsupported image format");
        return false;
    }
    if (sw <= 0 || sh <= 0 || (long)sw * (long)sh > max_src_pixels) {
        snprintf(err, errlen, "image too large (%dx%d)", sw, sh);
        return false;
    }
    unsigned char *px = stbi_load_from_memory(data, (int)len, &sw, &sh, &comp, 4);
    if (!px) {
        snprintf(err, errlen, "couldn't decode image");
        return false;
    }

    /* fit inside max_w x max_h, never enlarge */
    float scale = 1.0f;
    if (sw > max_w) scale = (float)max_w / sw;
    if (sh * scale > max_h) scale = (float)max_h / sh;
    int dw = (int)(sw * scale + 0.5f), dh = (int)(sh * scale + 0.5f);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int tw = (dw + 3) & ~3, th = (dh + 3) & ~3;
    size_t bytes = (size_t)tw * th * 2;
    unsigned short *tex = memalign(32, bytes);
    if (!tex) {
        stbi_image_free(px);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    memset(tex, 0, bytes);   /* padding = fully transparent */

    for (int y = 0; y < dh; y++) {
        int sy0 = (int)((long)y * sh / dh), sy1 = (int)((long)(y + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = (int)((long)x * sw / dw), sx1 = (int)((long)(x + 1) * sw / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const unsigned char *p = px + ((size_t)yy * sw + sx0) * 4;
                for (int xx = sx0; xx < sx1; xx++, p += 4) {
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            }
            r /= n; g /= n; b /= n; a /= n;
            /* 4x4 tiles, row-major tiles, row-major pixels inside a tile */
            size_t tile = (size_t)(y >> 2) * (tw >> 2) + (x >> 2);
            tex[tile * 16 + (y & 3) * 4 + (x & 3)] = enc_rgb5a3(r, g, b, a);
        }
    }
    stbi_image_free(px);
    DCFlushRange(tex, (u32)bytes);
    out->tex = tex;
    out->w = dw;
    out->h = dh;
    out->tex_w = tw;
    out->tex_h = th;
    out->bytes = bytes;
    return true;
}

void image_free(DecodedImage *img) {
    free(img->tex);
    img->tex = NULL;
}
