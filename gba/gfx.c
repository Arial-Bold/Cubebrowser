/* gfx.c - see gfx.h. The pixel loops are short and simple; the picture is
 * only redrawn when something on it changes. */
#include "gfx.h"
#include <string.h>
#ifdef __arm__
#pragma GCC target("arm")   /* the whole file as 32-bit ARM code (it runs from fast RAM, see gfx.h) */
#define INL static inline __attribute__((always_inline))
#else
#define INL static inline
#endif

Col *fb;


INL Col blend(Col d, Col c, int a /* 0..15 */) {
    if (a >= 15) return c;
    /* coverage 0..15 as a fraction of 256: a * 256 / 15 without dividing
     * (the GBA has no divide instruction, so "/ 15" here was slow) */
    int k = (a << 4) + a + (a >> 3);
    int r = d & 31, g = (d >> 5) & 31, b = (d >> 10) & 31;
    r += ((( c        & 31) - r) * k) >> 8;
    g += ((((c >> 5)  & 31) - g) * k) >> 8;
    b += ((((c >> 10) & 31) - b) * k) >> 8;
    return (Col)(r | (g << 5) | (b << 10));
}

GFX_FAST Col gfx_mix(Col a, Col b, int k) {
    int r = a & 31, g = (a >> 5) & 31, bl = (a >> 10) & 31;
    r += (((b & 31) - r) * k) >> 4;
    g += ((((b >> 5) & 31) - g) * k) >> 4;
    bl += ((((b >> 10) & 31) - bl) * k) >> 4;
    return (Col)(r | (g << 5) | (bl << 10));
}

GFX_FAST void gfx_fill(int x, int y, int w, int h, Col c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) return;
    uint32_t c2 = c | ((uint32_t)c << 16);
    for (int j = 0; j < h; j++) {
        Col *p = fb + (y + j) * SCR_W + x;
        int n = w;
        if (((uintptr_t)p & 2) && n) { *p++ = c; n--; }      /* to a 4-byte boundary */
        uint32_t *q = (uint32_t *)p;
        for (; n >= 2; n -= 2) *q++ = c2;
        if (n) *(Col *)q = c;
    }
}

GFX_FAST void gfx_vgrad(int y0, int y1, Col top, Col bottom) {
    for (int y = y0; y < y1; y++) gfx_fill(0, y, SCR_W, 1, gfx_mix(top, bottom, (y - y0) * 16 / (y1 - y0)));
}

/* coverage of pixel (i, j) of a mask, 0..15 */
INL int cov(const Mask *m, int i, int j) {
    const unsigned char *row = gfx_pool + m->off + j * ((m->w + 1) >> 1);
    unsigned char b = row[i >> 1];
    return (i & 1) ? b >> 4 : b & 15;
}

GFX_FAST void gfx_mask(int x, int y, const Mask *m, Col c) {
    for (int j = 0; j < m->h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCR_H) continue;
        Col *p = fb + py * SCR_W;
        for (int i = 0; i < m->w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCR_W) continue;
            int a = cov(m, i, j);
            if (a) p[px] = blend(p[px], c, a);
        }
    }
}

GFX_FAST static const Mask *corner(int r) {
    switch (r) {
    case 3: return &m_corner3;
    case 5: return &m_corner5;
    case 6: return &m_corner6;
    case 8: return &m_corner8;
    default: return &m_corner10;
    }
}

/* one corner, mirrored as needed (fx/fy: flip) */
GFX_FAST static void corner_draw(int x, int y, const Mask *m, int fx, int fy, Col c) {
    for (int j = 0; j < m->h; j++) {
        int py = y + j;
        if (py < 0 || py >= SCR_H) continue;
        int sj = fy ? m->h - 1 - j : j;
        for (int i = 0; i < m->w; i++) {
            int px = x + i;
            if (px < 0 || px >= SCR_W) continue;
            int a = cov(m, fx ? m->w - 1 - i : i, sj);
            if (a) fb[py * SCR_W + px] = blend(fb[py * SCR_W + px], c, a);
        }
    }
}

GFX_FAST void gfx_rrect(int x, int y, int w, int h, int r, Col c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    const Mask *m = corner(r);
    r = m->w;
    if (r * 2 > h || r * 2 > w) { gfx_fill(x, y, w, h, c); return; }
    gfx_fill(x + r, y, w - 2 * r, h, c);
    gfx_fill(x, y + r, r, h - 2 * r, c);
    gfx_fill(x + w - r, y + r, r, h - 2 * r, c);
    corner_draw(x, y, m, 0, 0, c);
    corner_draw(x + w - r, y, m, 1, 0, c);
    corner_draw(x, y + h - r, m, 0, 1, c);
    corner_draw(x + w - r, y + h - r, m, 1, 1, c);
}

GFX_FAST void gfx_rrect_line(int x, int y, int w, int h, int r, Col line, Col fill) {
    gfx_rrect(x, y, w, h, r, line);
    int r2 = r == 10 ? 8 : r == 8 ? 6 : r == 6 ? 5 : 3;
    gfx_rrect(x + 1, y + 1, w - 2, h - 2, r2, fill);
}

GFX_FAST int gfx_text_w(const Font *f, const char *s) {
    int w = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 130) c = '?';
        w += f->g[c - 32].adv;
    }
    return w;
}

GFX_FAST static void glyph(const Font *f, int x, int y, unsigned c, Col col) {
    const Glyph *g = &f->g[c - 32];
    if (!g->w) return;
    Mask m = { g->w, g->h, g->off };
    gfx_mask(x + g->x, y + g->y, &m, col);
}

GFX_FAST int gfx_text(const Font *f, int x, int y, Col c, const char *s, int max_w) {
    int limit = 1 << 30;
    if (max_w > 0 && gfx_text_w(f, s) > max_w) limit = max_w - 3 * f->g['.' - 32].adv;   /* room for "..." */
    int x0 = x;
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 130) ch = '?';
        int adv = f->g[ch - 32].adv;
        if (x - x0 + adv > limit) {
            for (int k = 0; k < 3; k++) { glyph(f, x, y, '.', c); x += f->g['.' - 32].adv; }
            break;
        }
        glyph(f, x, y, ch, c);
        x += adv;
    }
    return x;
}

GFX_FAST void gfx_text_c(const Font *f, int cx, int y, Col c, const char *s) {
    gfx_text(f, cx - gfx_text_w(f, s) / 2, y, c, s, 0);
}
