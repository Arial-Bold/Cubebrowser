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

void gfx_fill(int x, int y, int w, int h, Col c);
void gfx_vgrad(int y0, int y1, Col top, Col bottom);               /* full width */
void gfx_mask(int x, int y, const Mask *m, Col c);                 /* draw a mask in a colour */
void gfx_rrect(int x, int y, int w, int h, int r, Col c);          /* r: 3, 5, 6, 8 or 10 */
void gfx_rrect_line(int x, int y, int w, int h, int r, Col line, Col fill);
int  gfx_text_w(const Font *f, const char *s);
/* Draws s with its top at y; cut short with "..." to fit max_w (0 = no limit). Returns the end x. */
int  gfx_text(const Font *f, int x, int y, Col c, const char *s, int max_w);
void gfx_text_c(const Font *f, int cx, int y, Col c, const char *s);   /* centred on cx */
Col  gfx_mix(Col a, Col b, int k16);   /* a..b, k16 = 0..16 */
