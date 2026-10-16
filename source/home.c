/* home.c - see home.h. Flat, angular early-2000s console-site look: solid
 * colour blocks with thin dark outlines, dark navy bars with gold capitals,
 * cut corners, dotted dividers and square site tiles with caption bars.
 * Everything is drawn from shapes (no picture files) and fits the content
 * area in 4:3 and 16:9. */
#include "home.h"
#include "ui.h"
#include "gfx.h"
#include "theme.h"
#include "input.h"
#include "settings.h"
#include "version.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { IC_SEARCH, IC_BOOK, IC_NEWS, IC_RETRO, IC_CHAT, IC_GLOBE, IC_PENCIL };

typedef struct {
    const char *name, *blurb, *url;
    int icon;
    unsigned char r, g, b;
} Site;

static const Site sites[] = {
    { "DUCKDUCKGO",  "Private web search, light version",        "https://lite.duckduckgo.com/lite/",       IC_SEARCH, 30, 122, 170 },
    { "WIKIPEDIA",   "The free encyclopedia, mobile pages",      "https://en.m.wikipedia.org/wiki/GameCube", IC_BOOK,   78, 92, 178 },
    { "NINTENDO",    "Nintendo.com as it looked in 2001", "https://web.archive.org/web/20010822153959if_/http://www.nintendo.com/", IC_GLOBE, 70, 146, 46 },
    { "DC JUNKYARD", "The Dreamcast Junkyard, a Dreamcast blog", "https://dcjy.reye.me/",          IC_PENCIL, 222, 104, 30 },
    { "68K.NEWS",    "Headlines made for vintage computers",     "http://68k.news/",                        IC_RETRO,  112, 66, 168 },
    { "NPR TEXT",    "News headlines, text only",                "https://text.npr.org/",                   IC_NEWS,   184, 50, 62 },
    { "CNN LITE",    "World news, text only",                    "https://lite.cnn.com/",                   IC_NEWS,   46, 132, 82 },
    { "HACKER NEWS", "Technology news and discussion",           "https://news.ycombinator.com/",           IC_CHAT,   214, 132, 20 },
};
#define NSITES ((int)(sizeof(sites) / sizeof(sites[0])))

static const char about_text[] =
    "CubeBrowser is a web browser for your GameCube, with secure sites, pictures, forms and downloads "
    "to the SD card. Point with the Control Stick and press A to open a link. X types an address or "
    "searches. Light, text-based sites like these work best.";

static const char *const tips[] = {
    "Hold Z to move the pointer slowly.",
    "Hold Z and press A on a link to save it.",
    "The D-Pad jumps between links.",
    "Y opens bookmarks. The star saves a page.",
    "START opens settings: themes, text size...",
};
#define NTIPS ((int)(sizeof(tips) / sizeof(tips[0])))

static float intro_start = -100;

void home_reset(void) { intro_start = ui_time(); }

/* 0..1 entrance progress for item i (staggered fade) */
static float intro(int i) {
    if (!g_set.animations) return 1;
    float k = (ui_time() - intro_start - 0.04f * i) / 0.25f;
    if (k <= 0) return 0;
    if (k >= 1) return 1;
    return k;
}

/* Tile colouring:
 *   0 = each site its own colour
 *   1 = one periwinkle for every tile, white emblems (like the old site's panels)
 *   2 = one pale lavender for every tile, purple emblems (like its news rows) */
#ifndef HOME_TILE_STYLE
#define HOME_TILE_STYLE 1
#endif

static Color site_col(const Site *s, float m, float a) {
    float r = s->r * m, g = s->g * m, b = s->b * m;
    return rgba(r > 255 ? 255 : (u8)r, g > 255 ? 255 : (u8)g, b > 255 ? 255 : (u8)b, (u8)(255 * a));
}

static Color opaque(Color c) { c.a = 255; return c; }

/* ---- shapes */

/* Rectangle with the top-right and bottom-left corners cut at 45 degrees
 * (cut = 0 gives a plain rectangle). Drawn as a fan of triangles. */
static void cut_rect(float x, float y, float w, float h, float cut, Color c) {
    float p[12] = { x, y, x + w - cut, y, x + w, y + cut, x + w, y + h, x + cut, y + h, x, y + h - cut };
    float cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        gfx_tri(cx, cy, p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], c);
    }
}

static void cut_outline(float x, float y, float w, float h, float cut, float t, Color c) {
    float p[12] = { x, y, x + w - cut, y, x + w, y + cut, x + w, y + h, x + cut, y + h, x, y + h - cut };
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        gfx_line(p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1], t, c);
    }
    for (int i = 0; i < 6; i++) gfx_rect(p[i * 2] - t / 2, p[i * 2 + 1] - t / 2, t, t, c);   /* square joints */
}

static void dotted_hline(float x, float y, float w, Color c) {
    for (float d = 0; d < w; d += 4) gfx_rect(x + d, y, 1.6f, 1.6f, c);
}

static void dashed_rect(float x, float y, float w, float h, Color c) {
    for (float d = 0; d < w; d += 6) {
        float e = fminf(d + 3, w);
        gfx_rect(x + d, y, e - d, 1.5f, c);
        gfx_rect(x + d, y + h - 1.5f, e - d, 1.5f, c);
    }
    for (float d = 0; d < h; d += 6) {
        float e = fminf(d + 3, h);
        gfx_rect(x, y + d, 1.5f, e - d, c);
        gfx_rect(x + w - 1.5f, y + d, 1.5f, e - d, c);
    }
}

/* ---- spaced capitals */
static float caps_width(float size, float spacing, const char *s) {
    float w = 0;
    for (const char *p = s; *p; p++) w += gfx_text_width(FONT_UI, size, p, 1) + spacing;
    return *s ? w - spacing : 0;
}

/* shadow.a == 0 means no shadow; the shadow is a hard 1 px offset (flat look) */
static void caps(float x, float y, float size, float spacing, Color c, Color shadow, const char *s) {
    for (int pass = shadow.a ? 0 : 1; pass < 2; pass++) {
        float cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 1);
        Color col = pass ? c : shadow;
        for (const char *p = s; *p; p++) {
            gfx_text(FONT_UI, size, cx, cy, col, p, 1);
            gfx_text(FONT_UI, size, cx + 0.5f, cy, col, p, 1);   /* a touch bolder */
            cx += gfx_text_width(FONT_UI, size, p, 1) + spacing;
        }
    }
}

/* Word-wrapped text; returns the height used. With draw false it only measures. */
static float wrap(FontId f, float size, float x, float y, float w, Color c, const char *s, bool draw) {
    float lh = gfx_line_height(f, size) * 1.05f, yy = y;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        int n = gfx_text_fit(f, size, s, -1, w);
        if (s[n]) {                       /* break at the last space that fits */
            int b = n;
            while (b > 0 && s[b] != ' ') b--;
            if (b > 0) n = b;
        }
        if (n <= 0) n = 1;
        if (draw) gfx_text(f, size, x, yy, c, s, n);
        yy += lh;
        s += n;
    }
    return yy - y;
}

/* ---- emblems, flat single colour (plus a darker colour for details) */
static void draw_emblem(int icon, float cx, float cy, float s, Color c, Color dark) {
    switch (icon) {
    case IC_SEARCH:
        gfx_ring(cx - s * 0.08f, cy - s * 0.08f, s * 0.28f, s * 0.1f, 0, 6.2832f, c);
        gfx_line(cx + s * 0.12f, cy + s * 0.12f, cx + s * 0.38f, cy + s * 0.38f, s * 0.13f, c);
        break;
    case IC_BOOK: {
        float w = s * 0.42f, h = s * 0.56f;
        float l[8] = { cx - w, cy - h * 0.42f, cx - 1, cy - h * 0.5f, cx - 1, cy + h * 0.5f, cx - w, cy + h * 0.58f };
        float r[8] = { cx + 1, cy - h * 0.5f, cx + w, cy - h * 0.42f, cx + w, cy + h * 0.58f, cx + 1, cy + h * 0.5f };
        gfx_quad(l, c);
        gfx_quad(r, c);
        for (int i = 0; i < 3; i++) {
            float yy = cy - h * 0.22f + i * h * 0.22f;
            gfx_line(cx - w * 0.78f, yy, cx - w * 0.2f, yy - 2, 1.6f, dark);
            gfx_line(cx + w * 0.2f, yy - 2, cx + w * 0.78f, yy, 1.6f, dark);
        }
        break;
    }
    case IC_NEWS: {
        float w = s * 0.8f, h = s * 0.62f, x = cx - w / 2, y = cy - h / 2;
        gfx_rect(x, y, w, h, c);
        gfx_rect(x + w * 0.1f, y + h * 0.14f, w * 0.8f, h * 0.18f, dark);
        gfx_rect(x + w * 0.1f, y + h * 0.42f, w * 0.36f, h * 0.42f, dark);
        for (int i = 0; i < 3; i++) gfx_rect(x + w * 0.54f, y + h * (0.44f + i * 0.15f), w * 0.36f, 2, dark);
        break;
    }
    case IC_GLOBE: {
        float R = s * 0.36f;
        gfx_circle(cx, cy, R, c);
        for (int i = -1; i <= 1; i++) {   /* latitude lines */
            float yy = cy + i * R * 0.5f, half = sqrtf(R * R - (i * R * 0.5f) * (i * R * 0.5f)) * 0.92f;
            gfx_line(cx - half, yy, cx + half, yy, 1.6f, dark);
        }
        for (int e = 0; e < 2; e++) {     /* meridians */
            float rx = e ? R * 0.45f : R * 0.92f;
            for (int side = -1; side <= 1; side += 2) {
                float px = cx, py = cy - R * 0.92f;
                for (int k = 1; k <= 20; k++) {
                    float a = -1.5708f + side * 3.1416f * k / 20, nx = cx + cosf(a) * rx, ny = cy + sinf(a) * R * 0.92f;
                    gfx_line(px, py, nx, ny, 1.6f, dark);
                    px = nx;
                    py = ny;
                }
            }
        }
        break;
    }
    case IC_PENCIL: {   /* a single pencil, writing: the blog */
        float dx = 0.7071f, dy = -0.7071f, R = s * 0.11f, L = s * 0.86f;
        float tx = cx - dx * L / 2, ty = cy - dy * L / 2;          /* tip, lower left */
        float nx = -dy * R, ny = dx * R;
        float bx = tx + dx * s * 0.2f, by = ty + dy * s * 0.2f;    /* where the sharpened part starts */
        float ex = tx + dx * L, ey = ty + dy * L;                  /* top end */
        float ux = ex - dx * s * 0.13f, uy = ey - dy * s * 0.13f;  /* eraser band */
        float body[8] = { bx + nx, by + ny, ux + nx, uy + ny, ux - nx, uy - ny, bx - nx, by - ny };
        float eraser[8] = { ux + nx, uy + ny, ex + nx, ey + ny, ex - nx, ey - ny, ux - nx, uy - ny };
        gfx_quad(body, c);
        gfx_quad(eraser, dark);
        gfx_tri(tx, ty, bx + nx, by + ny, bx - nx, by - ny, c);
        float lx = tx + dx * s * 0.08f, ly = ty + dy * s * 0.08f;  /* lead */
        gfx_tri(tx, ty, lx + nx * 0.4f, ly + ny * 0.4f, lx - nx * 0.4f, ly - ny * 0.4f, dark);
        break;
    }
    case IC_RETRO: {
        float w = s * 0.72f, h = s * 0.52f, x = cx - w / 2, y = cy - h / 2 - s * 0.06f;
        gfx_rect(x, y, w, h, c);
        gfx_rect(x + 4, y + 4, w - 8, h - 8, dark);
        gfx_text(FONT_MONO, s * 0.22f, x + 8, y + h * 0.22f, c, ">_", 2);
        gfx_rect(cx - s * 0.08f, y + h, s * 0.16f, s * 0.08f, c);
        gfx_rect(cx - s * 0.26f, y + h + s * 0.08f, s * 0.52f, s * 0.07f, c);
        break;
    }
    case IC_CHAT: {
        float w = s * 0.74f, h = s * 0.46f, x = cx - w / 2, y = cy - h / 2 - s * 0.06f;
        gfx_rect(x, y, w, h, c);
        gfx_tri(x + w * 0.22f, y + h - 1, x + w * 0.44f, y + h - 1, x + w * 0.16f, y + h + s * 0.16f, c);
        for (int i = 0; i < 3; i++) gfx_rect(cx - s * 0.2f + i * s * 0.16f, y + h * 0.42f, s * 0.08f, s * 0.08f, dark);
        break;
    }
    }
}

static bool clicked(bool hov, bool input_ok) {
    if (hov && input_ok && input_pressed(ACT_CLICK)) {
        input_consume(ACT_CLICK);
        input_rumble(3);
        return true;
    }
    return false;
}

HomeAction home_draw(float x, float y, float w, float h, bool input_ok, const char **url, const char **hover_url) {
    const Theme *t = theme();
    HomeAction act = HOME_NONE;
    *url = NULL;
    *hover_url = NULL;
    const char *hover_blurb = NULL;
    float now = ui_time();

    /* palette: flat versions of the theme colours plus fixed navy and orange */
    Color base = opaque(color_lerp(t->panel_bottom, t->bg_top, 0.25f));   /* page background */
    Color panel = opaque(t->panel_top);                                     /* lighter blocks */
    Color navy = rgba(26, 24, 66, 255);                                     /* bars, outlines */
    Color gold = opaque(t->accent);
    Color orange = rgba(240, 132, 24, 255);
    Color white = rgba(255, 255, 255, 255);
    Color shadow = rgba(20, 16, 50, 200);

    gfx_rrect(x, y, w, h, 14, base);
    gfx_set_clip(x, y, w, h);

    float m = 10, hx = x + m, hw = w - 2 * m, cy = y + m;

    /* ---- header bar: navy band, gold title, welcome bubble */
    float a0 = intro(0);
    float bar_h = 34;
    cut_rect(hx, cy, hw, bar_h, 12, color_alpha(navy, a0));
    /* emblem: a small flat cube (three faces) */
    {
        float ex = hx + 22, ey = cy + bar_h / 2, s = 11;
        float top[8] = { ex, ey - s, ex + s, ey - s * 0.5f, ex, ey, ex - s, ey - s * 0.5f };
        float left[8] = { ex - s, ey - s * 0.5f, ex, ey, ex, ey + s, ex - s, ey + s * 0.5f };
        float right[8] = { ex, ey, ex + s, ey - s * 0.5f, ex + s, ey + s * 0.5f, ex, ey + s };
        gfx_quad(top, color_alpha(rgba(170, 160, 240, 255), a0));
        gfx_quad(left, color_alpha(rgba(110, 96, 210, 255), a0));
        gfx_quad(right, color_alpha(rgba(76, 62, 168, 255), a0));
    }
    float tsz = w < 700 ? 21 : 23;
    caps(hx + 42, cy + (bar_h - gfx_line_height(FONT_UI, tsz)) / 2, tsz, 0.8f, color_alpha(gold, a0),
         color_alpha(rgba(0, 0, 0, 255), a0), "CubeBrowser");
    float title_end = hx + 42 + caps_width(tsz, 0.8f, "CubeBrowser");
    const char *bubble = "WELCOME TO v" CB_VERSION;
    float bub_w = caps_width(9.5f, 1.1f, bubble) + 20, bub_h = 22;
    float bub_x = hx + hw - 22 - bub_w, bub_y = cy + (bar_h - bub_h) / 2;
    if (bub_x > title_end + 20) {
        gfx_rect(bub_x, bub_y, bub_w, bub_h, color_alpha(white, a0));
        gfx_tri(bub_x + 1, bub_y + 6, bub_x + 1, bub_y + 15, bub_x - 8, bub_y + 12, color_alpha(white, a0));
        dashed_rect(bub_x + 3, bub_y + 3, bub_w - 6, bub_h - 6, color_alpha(navy, a0));
        caps(bub_x + 10, bub_y + (bub_h - gfx_line_height(FONT_UI, 9.5f)) / 2, 9.5f, 1.1f, color_alpha(navy, a0),
             rgba(0, 0, 0, 0), bubble);
    }
    cy += bar_h + 8;

    /* ---- about panel */
    float a1 = intro(1);
    float tsz_body = w < 700 ? 13.0f : 14.5f;
    float text_w = hw - 24;
    float text_h = wrap(FONT_BODY, tsz_body, 0, 0, text_w - 4, white, about_text, false);   /* same width as drawn */
    float about_h = text_h + 14;
    cut_rect(hx, cy, hw, about_h, 8, color_alpha(color_lerp(panel, white, 0.62f), a1));   /* pale, for easy reading */
    cut_outline(hx, cy, hw, about_h, 8, 1.5f, color_alpha(navy, a1));
    gfx_rect(hx + 1, cy + 1, 5, about_h - 2 - 8, color_alpha(orange, a1));   /* accent edge */
    wrap(FONT_BODY, tsz_body, hx + 15, cy + 7, text_w - 4, color_alpha(navy, a1), about_text, true);
    cy += about_h + 7;

    /* ---- featured sites */
    float a2 = intro(2);
    caps(hx + 2, cy, 15, 1.0f, color_alpha(white, a2), color_alpha(shadow, a2), "FEATURED SITES");
    float dots_x = hx + 2 + caps_width(15, 1.0f, "FEATURED SITES") + 14;
    dotted_hline(dots_x, cy + 9, hx + hw - dots_x, color_alpha(panel, 0.8f * a2));
    cy += 21;

    float row_h = 24, gap = 8;
    int cols = 4, rows = (NSITES + cols - 1) / cols;
    float bottom = y + h - m;
    float grid_bottom = bottom - row_h - 8;
    float th = (grid_bottom - cy - gap * (rows - 1)) / rows;
    if (th > 100) th = 100;
    if (th < 44) th = 44;
    float tw = (hw - gap * (cols - 1)) / cols;
    float label_h = fminf(22, th * 0.36f);

    for (int i = 0; i < NSITES; i++) {
        const Site *s = &sites[i];
        float ap = intro(3 + i);
        if (ap <= 0) continue;
        float bx = hx + (i % cols) * (tw + gap), by = cy + (i / cols) * (th + gap);
        UiId id = ui_id("home-tile", i);
        bool hov = ui_hover(id, bx, by, tw, th);
        float art_h = th - label_h;
        /* hard drop shadow, then the flat art block */
        gfx_rect(bx + 3, by + 3, tw, th, color_alpha(rgba(10, 8, 30, 140), ap));
        Color fill, edge, em, em_dark;
#if HOME_TILE_STYLE == 1
        fill = hov ? rgba(146, 154, 222, 255) : rgba(122, 130, 202, 255);
        edge = rgba(176, 182, 236, 255);
        em = white;
        em_dark = rgba(70, 74, 150, 255);
#elif HOME_TILE_STYLE == 2
        fill = hov ? rgba(255, 255, 255, 255) : rgba(228, 229, 244, 255);
        edge = rgba(255, 255, 255, 255);
        em = rgba(88, 76, 170, 255);
        em_dark = rgba(228, 229, 244, 255);
#else
        fill = site_col(s, hov ? 1.15f : 1.0f, 1);
        edge = site_col(s, 1.35f, 1);
        em = white;
        em_dark = site_col(s, 0.6f, 1);
#endif
        gfx_rect(bx, by, tw, art_h, color_alpha(fill, ap));
        gfx_rect(bx, by, tw, 3, color_alpha(edge, ap));                       /* light top edge */
        float es = fminf(art_h * 0.9f, tw * 0.45f);
        draw_emblem(s->icon, bx + tw / 2, by + art_h / 2 + 1, es, color_alpha(em, ap), color_alpha(em_dark, ap));
        /* caption bar with the orange arrow square */
        gfx_rect(bx, by + art_h, tw, label_h, color_alpha(navy, ap));
        float as = label_h - 6, ax = bx + tw - as - 3, ay = by + art_h + 3;
        gfx_rect(ax, ay, as, as, color_alpha(hov ? gold : orange, ap));
        gfx_tri(ax + as * 0.34f, ay + as * 0.22f, ax + as * 0.34f, ay + as * 0.78f, ax + as * 0.76f, ay + as * 0.5f,
                color_alpha(navy, ap));
        float ns = 11, nsp = 1.0f, room = tw - 8 - as - 8;
        while (ns > 8 && caps_width(ns, nsp, s->name) > room) ns -= 0.5f;
        caps(bx + 7, by + art_h + (label_h - gfx_line_height(FONT_UI, ns)) / 2, ns, nsp,
             color_alpha(hov ? gold : white, ap), rgba(0, 0, 0, 0), s->name);
        /* outline: thin navy, or a thick gold frame when pointed at */
        if (hov) {
            float k = 3;
            gfx_rect(bx - k, by - k, tw + 2 * k, k, gold);
            gfx_rect(bx - k, by + th, tw + 2 * k, k, gold);
            gfx_rect(bx - k, by, k, th, gold);
            gfx_rect(bx + tw, by, k, th, gold);
        } else {
            gfx_rect(bx, by, tw, 1.5f, color_alpha(navy, ap));
            gfx_rect(bx, by + th - 1.5f, tw, 1.5f, color_alpha(navy, ap));
            gfx_rect(bx, by, 1.5f, th, color_alpha(navy, ap));
            gfx_rect(bx + tw - 1.5f, by, 1.5f, th, color_alpha(navy, ap));
        }
        if (hov) { *hover_url = s->url; hover_blurb = s->blurb; }
        if (clicked(hov, input_ok)) { act = HOME_OPEN_URL; *url = s->url; }
    }

    /* ---- bottom row: Controls & Tips button, then the tip line */
    float a3 = intro(3 + NSITES);
    float ry = bottom - row_h;
    const char *blabel = "CONTROLS & TIPS";
    float bw = caps_width(10.5f, 1.2f, blabel) + 40;
    UiId bid = ui_id("home-help", 0);
    bool bhov = ui_hover(bid, hx, ry, bw, row_h);
    cut_rect(hx, ry, bw, row_h, 7, color_alpha(bhov ? gold : panel, a3));
    cut_outline(hx, ry, bw, row_h, 7, 1.5f, color_alpha(navy, a3));
    {   /* arrow in a navy circle, as on the old "take our survey" buttons */
        float ax = hx + 14, ay = ry + row_h / 2;
        gfx_circle(ax, ay, 7, color_alpha(navy, a3));
        gfx_tri(ax - 2.5f, ay - 4, ax - 2.5f, ay + 4, ax + 4, ay, color_alpha(bhov ? navy : white, a3));
        if (!bhov) gfx_tri(ax - 2.5f, ay - 4, ax - 2.5f, ay + 4, ax + 4, ay, color_alpha(gold, a3));
    }
    caps(hx + 28, ry + (row_h - gfx_line_height(FONT_UI, 10.5f)) / 2, 10.5f, 1.2f, color_alpha(navy, a3), rgba(0, 0, 0, 0), blabel);
    if (clicked(bhov, input_ok)) act = HOME_HELP;

    float lx = hx + bw + 10, lw = hw - bw - 10;
    gfx_rect(lx, ry, lw, row_h, color_alpha(navy, 0.85f * a3));
    const char *tag = hover_blurb ? "SITE" : "TIP";
    float tag_w = caps_width(9.5f, 1.2f, tag) + 14;
    gfx_rect(lx + 3, ry + 3, tag_w, row_h - 6, color_alpha(orange, a3));
    caps(lx + 10, ry + (row_h - gfx_line_height(FONT_UI, 9.5f)) / 2, 9.5f, 1.2f, color_alpha(navy, a3), rgba(0, 0, 0, 0), tag);
    char line[200];
    float fade = 1;
    if (hover_blurb) {
        snprintf(line, sizeof(line), "%s", hover_blurb);
    } else {
        float period = 6.0f, ph = fmodf(now, period);
        snprintf(line, sizeof(line), "%s", tips[(int)(now / period) % NTIPS]);
        if (g_set.animations) fade = fminf(1, fminf(ph / 0.4f, (period - ph) / 0.4f));
    }
    float txs = 13;
    float tx = lx + 3 + tag_w + 8, tw2 = lw - (tx - lx) - 8;
    gfx_text(FONT_UI, txs, tx, ry + (row_h - gfx_line_height(FONT_UI, txs)) / 2, color_alpha(white, a3 * fade), line,
             gfx_text_fit(FONT_UI, txs, line, -1, tw2));

    gfx_clear_clip();
    return act;
}
