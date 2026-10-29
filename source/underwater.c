/* underwater.c - see underwater.h.
 *
 * The scene, back to front:
 *   - the water: a gradient from sunlit blue at the top to deep navy;
 *   - the surface seen from below: a repeating pattern of bright lines (like
 *     the light network on a pool floor) drawn twice, sliding in different
 *     directions and added together, so the bright patches shimmer. It's drawn
 *     on a bent grid that makes the pattern smaller further down (the surface
 *     going away from you) and gently wobbles it;
 *   - light rays: long faint wedges from the surface, swaying and fading;
 *   - specks drifting in the water, and bubbles rising to the surface;
 *   - "Loading...".
 * Light is drawn additively (it brightens what's under it). */
#include "underwater.h"
#include "gfx.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PAT 64          /* the pattern is PAT x PAT */
#define CELLS 3         /* 3 x 3 cells, one light "knot" in each */
#define GRID_C 12
#define GRID_R 7

static GfxPattern pattern;
static bool pattern_tried;

/* small repeatable random numbers, so the scene is the same every time */
static float rnd(unsigned i) {
    i = (i ^ 61u) ^ (i >> 16);
    i *= 9u;
    i ^= i >> 4;
    i *= 0x27d4eb2du;
    i ^= i >> 15;
    return (float)(i & 0xffffu) / 65535.0f;
}

/* Bright where two cells meet: for each pixel the distance to the nearest
 * and second-nearest knot (wrapping around, so the pattern tiles) - where
 * they're nearly equal is a cell edge. */
static void make_pattern(void) {
    pattern_tried = true;
    static unsigned char px[PAT * PAT];
    float kx[CELLS * CELLS], ky[CELLS * CELLS];
    float cell = (float)PAT / CELLS;
    for (int i = 0; i < CELLS * CELLS; i++) {
        kx[i] = ((i % CELLS) + 0.15f + 0.7f * rnd(i * 2 + 1)) * cell;
        ky[i] = ((i / CELLS) + 0.15f + 0.7f * rnd(i * 2 + 2)) * cell;
    }
    for (int y = 0; y < PAT; y++)
        for (int x = 0; x < PAT; x++) {
            float d1 = 1e9f, d2 = 1e9f;
            for (int i = 0; i < CELLS * CELLS; i++) {
                float dx = fabsf(x + 0.5f - kx[i]), dy = fabsf(y + 0.5f - ky[i]);
                if (dx > PAT * 0.5f) dx = PAT - dx;
                if (dy > PAT * 0.5f) dy = PAT - dy;
                float d = sqrtf(dx * dx + dy * dy);
                if (d < d1) { d2 = d1; d1 = d; }
                else if (d < d2) d2 = d;
            }
            float v = 1.0f - (d2 - d1) / 8.0f;
            if (v < 0) v = 0;
            v = v * v * v;
            px[y * PAT + x] = (unsigned char)(v * 255.0f + 0.5f);
        }
    gfx_pattern_init(&pattern, px, PAT, PAT);
}

static Color fade(Color c, float a) { return color_alpha(c, a); }

/* the shimmering surface: one layer of the pattern on the bent grid */
static void surface_layer(float x, float y, float w, float band, float t, float scale, float du, float dv, float alpha) {
    static float xy[(GRID_C + 1) * (GRID_R + 1) * 2], uv[(GRID_C + 1) * (GRID_R + 1) * 2];
    static Color col[(GRID_C + 1) * (GRID_R + 1)];
    float v = 0, prev_s = 0;
    for (int r = 0; r <= GRID_R; r++) {
        float s = (float)r / GRID_R;                    /* 0 at the top, 1 at the bottom of the band */
        float dens = (0.55f + 0.7f * s * s) * scale;    /* repeats per 100 units: smaller further away */
        if (r > 0) v += (s - prev_s) * band / 100.0f * dens * 2.4f;   /* squashed: seen at a slant */
        prev_s = s;
        float a = powf(1.0f - s, 2.2f) * alpha;
        for (int c = 0; c <= GRID_C; c++) {
            int i = r * (GRID_C + 1) + c;
            float px = x + w * c / GRID_C, py = y + band * s;
            xy[i * 2] = px;
            xy[i * 2 + 1] = py;
            float wob = 0.05f * sinf(t * 1.3f + s * 5.0f + c * 0.7f);
            uv[i * 2] = (px - (x + w * 0.5f)) / 100.0f * dens + du + wob;
            uv[i * 2 + 1] = v + dv + 0.05f * cosf(t * 1.1f + c * 0.9f + s * 3.0f);
            col[i] = fade(rgba(200, 235, 255, 255), a);
        }
    }
    gfx_pattern_grid(&pattern, GRID_C, GRID_R, xy, uv, col);
}

void underwater_draw(float x, float y, float w, float h, float alpha, float t) {
    if (alpha <= 0.003f || w < 20 || h < 20) return;
    if (!pattern_tried) make_pattern();

    /* the water, which also marks its rounded shape: everything else is
     * drawn only inside it, right up to the edges and around the corners */
    gfx_mask_begin();
    gfx_rrect_vgrad(x, y, w, h, ui_r(14), fade(rgba(38, 112, 190, 255), alpha), fade(rgba(3, 20, 58, 255), alpha));
    gfx_mask_use();
    gfx_set_clip(x, y, w, h);
    gfx_rect_vgrad(x, y + h * 0.35f, w, h * 0.65f, fade(rgba(10, 50, 110, 0), alpha), fade(rgba(2, 12, 40, 150), alpha));

    gfx_set_additive(true);
    /* glow just under the surface */
    gfx_rect_vgrad(x, y, w, h * 0.22f, fade(rgba(110, 190, 255, 110), alpha), fade(rgba(60, 140, 230, 0), alpha));

    /* the surface */
    if (pattern.data) {
        float band = h * 0.36f;
        surface_layer(x, y, w, band, t, 1.0f, fmodf(t * 0.035f, 1.0f), fmodf(t * 0.06f, 1.0f), 0.85f * alpha);
        surface_layer(x, y, w, band, t, 1.35f, fmodf(0.37f - t * 0.045f, 1.0f), fmodf(0.61f + t * 0.04f, 1.0f), 0.6f * alpha);
    }

    /* light rays, slanting a little to the right like sunlight from the side */
    for (int i = 0; i < 8; i++) {
        float fx = 0.04f + i * 0.13f + 0.06f * rnd(100 + i);
        float tx = x + w * fx, ty = y - 20;
        float tw = 5 + 9 * rnd(110 + i);
        float len = h * (0.7f + 0.3f * rnd(120 + i));
        float sway = sinf(t * 0.23f + i * 1.7f) * w * 0.035f;
        float bx = tx + len * 0.28f + sway, by = y + len;
        float bw = w * (0.05f + 0.07f * rnd(130 + i));
        float flick = 0.5f + 0.5f * sinf(t * (0.45f + 0.2f * rnd(140 + i)) + i * 2.3f);
        float a = (0.10f + 0.16f * flick) * alpha;
        float q[8] = { tx - tw, ty, tx + tw, ty, bx + bw, by, bx - bw, by };
        Color top = rgba(150, 205, 255, (unsigned char)(255 * a)), bot = rgba(90, 160, 240, 0);
        Color c4[4] = { top, top, bot, bot };
        gfx_quad_colors(q, c4);
    }

    /* specks drifting in the water */
    for (int i = 0; i < 28; i++) {
        float sx = x + w * rnd(200 + i) + sinf(t * 0.2f + i) * 10;
        float sy = y + fmodf(h * rnd(240 + i) + t * (3 + 4 * rnd(280 + i)), h);
        float a = (0.10f + 0.18f * rnd(320 + i)) * alpha * (0.4f + 0.6f * sy / (y + h));
        gfx_circle(sx, sy, 1.0f + 0.8f * rnd(360 + i), rgba(200, 225, 255, (unsigned char)(255 * a)));
    }

    /* bubbles rising; they fade as they reach the surface */
    for (int i = 0; i < 9; i++) {
        float speed = 22 + 30 * rnd(400 + i), r = 2.5f + 4.0f * rnd(420 + i);
        float travel = h + 20;
        float k = fmodf(t * speed + travel * rnd(440 + i), travel);
        float by = y + h + 10 - k;
        float bx = x + w * (0.06f + 0.88f * rnd(460 + i)) + sinf(t * 2.0f + i * 1.9f) * 4;
        float top_fade = fminf(1.0f, (by - y) / (h * 0.25f));
        if (top_fade <= 0) continue;
        float a = 0.5f * alpha * top_fade;
        gfx_ring(bx, by, r, 1.3f, 0, 2 * (float)M_PI, rgba(210, 235, 255, (unsigned char)(255 * a)));
        gfx_circle(bx - r * 0.35f, by - r * 0.35f, r * 0.28f, rgba(255, 255, 255, (unsigned char)(255 * a)));
    }
    gfx_set_additive(false);

    /* darker towards the sides */
    float vw = w * 0.16f;
    gfx_rect_hgrad(x, y, vw, h, fade(rgba(0, 8, 30, 90), alpha), fade(rgba(0, 8, 30, 0), alpha));
    gfx_rect_hgrad(x + w - vw, y, vw, h, fade(rgba(0, 8, 30, 0), alpha), fade(rgba(0, 8, 30, 90), alpha));

    /* Loading... */
    float ty = y + h * 0.6f;
    float full = gfx_text_width(FONT_UI, 30, "Loading...", -1);
    float lx = x + (w - full) * 0.5f;
    float lw = gfx_text(FONT_UI, 30, lx, ty, fade(rgba(255, 255, 255, 235), alpha), "Loading", -1);
    int dots = (int)fmodf(t * 2.5f, 4.0f);
    gfx_text(FONT_UI, 30, lx + lw, ty, fade(rgba(255, 255, 255, 235), alpha), "...", dots);
    gfx_clear_clip();
    gfx_mask_end();
}
