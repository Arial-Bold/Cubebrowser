/* gfx.h - drawing into the music player's 240x160 picture (15-bit colour).
 * Shapes and text are drawn as coverage masks (gfxdata.c), so edges are
 * smooth even on the small screen. */
#pragma once
#include <stdint.h>
#include "gfxdata.h"

#define SCR_W 240
#define SCR_H 160
typedef uint16_t Col;
#define RGB8(r, g, b) ((Col)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10)))

extern Col *fb;   /* the picture being drawn (SCR_W x SCR_H) */

/* The drawing code runs from the GBA's fast internal RAM as 32-bit ARM code,
 * several times quicker than from the slower RAM the program is loaded into. */
#ifdef __arm__
#define GFX_FAST __attribute__((section(".iwram"), long_call, target("arm"), noinline))
#else
#define GFX_FAST
#endif

GFX_FAST void gfx_fill(int x, int y, int w, int h, Col c);
GFX_FAST void gfx_vgrad(int y0, int y1, Col top, Col bottom);               /* full width */
GFX_FAST void gfx_mask(int x, int y, const Mask *m, Col c);                 /* draw a mask in a colour */
GFX_FAST void gfx_rrect(int x, int y, int w, int h, int r, Col c);          /* r: 3, 5, 6, 8 or 10 */
GFX_FAST void gfx_rrect_line(int x, int y, int w, int h, int r, Col line, Col fill);
GFX_FAST int  gfx_text_w(const Font *f, const char *s);
/* Draws s with its top at y; cut short with "..." to fit max_w (0 = no limit). Returns the end x. */
GFX_FAST int  gfx_text(const Font *f, int x, int y, Col c, const char *s, int max_w);
GFX_FAST void gfx_text_c(const Font *f, int cx, int y, Col c, const char *s);   /* centred on cx */
GFX_FAST Col  gfx_mix(Col a, Col b, int k16);   /* a..b, k16 = 0..16 */
