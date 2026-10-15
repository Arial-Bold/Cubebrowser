/* gfx.h - video setup, 2D drawing primitives and text rendering on GX.
 *
 * All drawing uses a *virtual* coordinate space that is always 480 units tall.
 * Its width is 640 in 4:3 mode and 854 in 16:9 mode. In 16:9 the GameCube
 * still outputs 640 pixels per line and the TV stretches the picture, so we
 * squeeze 854 virtual units into 640 pixels and everything ends up with the
 * correct proportions on a widescreen set (anamorphic widescreen).
 */
#pragma once
#include <gccore.h>
#include <stdbool.h>

typedef struct { u8 r, g, b, a; } Color;

static inline Color rgba(u8 r, u8 g, u8 b, u8 a) { Color c = { r, g, b, a }; return c; }
Color color_alpha(Color c, float mul);
Color color_lerp(Color a, Color b, float t);

typedef enum { FONT_UI = 0, FONT_BODY = 1, FONT_ITALIC = 2, FONT_MONO = 3, FONT_COUNT } FontId;

enum { VMODE_AUTO = 0, VMODE_480I = 1, VMODE_480P = 2 };

void  gfx_init(int video_mode, bool widescreen);
bool  gfx_apply_video_mode(int video_mode);  /* false if 480p requested without component cable */
void  gfx_set_widescreen(bool wide);
bool  gfx_widescreen(void);
float gfx_width(void);   /* 640 or 854 */
float gfx_height(void);  /* 480 */
bool  gfx_has_component_cable(void);
const char *gfx_mode_name(void);

void gfx_begin_frame(void);
void gfx_end_frame(void);

/* Transform applied to everything drawn: x' = x*sx + tx, y' = y*sy + ty */
void gfx_set_transform(float sx, float sy, float tx, float ty);
void gfx_reset_transform(void);

void gfx_set_clip(float x, float y, float w, float h);
void gfx_clear_clip(void);

void gfx_rect(float x, float y, float w, float h, Color c);
void gfx_rect_vgrad(float x, float y, float w, float h, Color top, Color bottom);
void gfx_rect_hgrad(float x, float y, float w, float h, Color left, Color right);
void gfx_rrect(float x, float y, float w, float h, float r, Color c);
void gfx_rrect_vgrad(float x, float y, float w, float h, float r, Color top, Color bottom);
void gfx_rrect_outline(float x, float y, float w, float h, float r, float thick, Color c);
void gfx_shadow(float x, float y, float w, float h, float r, float spread, Color c);
void gfx_circle(float cx, float cy, float r, Color c);
void gfx_ring(float cx, float cy, float r, float thick, float a0, float a1, Color c);
void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, Color c);
void gfx_quad(const float *xy8, Color c);   /* 4 points, clockwise */
void gfx_line(float x1, float y1, float x2, float y2, float thick, Color c);
/* Shaded 3D cube, rx/ry rotation in radians. */
void gfx_cube(float cx, float cy, float size, float rx, float ry, Color base);

float gfx_line_height(FontId f, float size);
float gfx_text_width(FontId f, float size, const char *s, int len);
float gfx_text(FontId f, float size, float x, float y, Color c, const char *s, int len);
void  gfx_text_centered(FontId f, float size, float cx, float y, Color c, const char *s);
/* Textures for web page images (RGB5A3 tiled data, see image.c). */
typedef struct { GXTexObj obj; void *data; u16 w, h, tex_w, tex_h; } GfxImage;
void  gfx_image_init(GfxImage *img, void *data, int w, int h, int tex_w, int tex_h);
void  gfx_image_free(GfxImage *img);
void  gfx_image_draw(GfxImage *img, float x, float y, float w, float h, Color tint);

/* Index of the character boundary closest to x (for caret placement). */
int   gfx_text_fit(FontId f, float size, const char *s, int len, float maxw);
