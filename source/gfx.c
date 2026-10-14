/* gfx.c - GX setup, 2D primitives, procedural cube and bitmap-font text. */
#include "gfx.h"
#include "font_data.h"
#include <malloc.h>
#include <math.h>
#include <string.h>

#define FIFO_SIZE (256 * 1024)
#define XFB_BYTES (640 * 576 * VI_DISPLAY_PIX_SZ)  /* big enough for any mode */

static void *fifo;
static void *xfb[2];
static int fb;
static GXRModeObj *rmode;
static bool wide;
static float vw = 640.0f;
static float t_sx = 1, t_sy = 1, t_tx = 0, t_ty = 0;
static int cur_mode = -1;     /* 0 = color, 1 = textured */
static int cur_font = -1;
static GXTexObj font_tex[2];

Color color_alpha(Color c, float mul) {
    float a = c.a * mul;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    c.a = (u8)a;
    return c;
}

Color color_lerp(Color a, Color b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    Color c;
    c.r = (u8)(a.r + (b.r - a.r) * t);
    c.g = (u8)(a.g + (b.g - a.g) * t);
    c.b = (u8)(a.b + (b.b - a.b) * t);
    c.a = (u8)(a.a + (b.a - a.a) * t);
    return c;
}

/* ------------------------------------------------------------------ video */

static GXRModeObj *pick_mode(int pref) {
    bool pal = (VIDEO_GetCurrentTvMode() == VI_PAL);
    if (pref == VMODE_480P && VIDEO_HaveComponentCable())
        return pal ? &TVEurgb60Hz480Prog : &TVNtsc480Prog;
    if (pref == VMODE_480I)
        return pal ? &TVPal528IntDf : &TVNtsc480IntDf;
    return VIDEO_GetPreferredMode(NULL);
}

static void setup_gx_for_mode(void) {
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfb_h = GX_SetDispCopyYScale(yscale);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfb_h);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    if (rmode->aa)
        GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    else
        GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
}

static void configure_vi(GXRModeObj *m) {
    rmode = m;
    VIDEO_Configure(rmode);
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    fb = 1;
}

void gfx_init(int video_mode, bool widescreen) {
    VIDEO_Init();
    xfb[0] = MEM_K0_TO_K1(memalign(32, XFB_BYTES));
    xfb[1] = MEM_K0_TO_K1(memalign(32, XFB_BYTES));
    configure_vi(pick_mode(video_mode));

    fifo = memalign(32, FIFO_SIZE);
    memset(fifo, 0, FIFO_SIZE);
    GX_Init(fifo, FIFO_SIZE);
    GXColor black = { 0, 0, 0, 255 };
    GX_SetCopyClear(black, 0x00FFFFFF);
    setup_gx_for_mode();
    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(xfb[0], GX_TRUE);

    GX_ClearVtxDesc();
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetNumTevStages(1);

    DCFlushRange((void *)font_ui_tex, sizeof(font_ui_tex));
    DCFlushRange((void *)font_body_tex, sizeof(font_body_tex));
    GX_InitTexObj(&font_tex[0], (void *)font_ui_tex, FONT_TEX_W, FONT_TEX_H, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObj(&font_tex[1], (void *)font_body_tex, FONT_TEX_W, FONT_TEX_H, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
    for (int i = 0; i < 2; i++)
        GX_InitTexObjLOD(&font_tex[i], GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_InvalidateTexAll();
    GX_InvVtxCache();

    gfx_set_widescreen(widescreen);
}

bool gfx_apply_video_mode(int video_mode) {
    bool ok = !(video_mode == VMODE_480P && !VIDEO_HaveComponentCable());
    GXRModeObj *m = pick_mode(video_mode);
    if (m == rmode) return ok;
    GX_DrawDone();
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    configure_vi(m);
    setup_gx_for_mode();
    return ok;
}

void gfx_set_widescreen(bool w) {
    wide = w;
    vw = w ? 854.0f : 640.0f;
}

bool gfx_widescreen(void) { return wide; }
float gfx_width(void) { return vw; }
float gfx_height(void) { return 480.0f; }
bool gfx_has_component_cable(void) { return VIDEO_HaveComponentCable(); }

const char *gfx_mode_name(void) {
    if (rmode->viTVMode & VI_NON_INTERLACE) return "480p";
    return rmode->xfbHeight > 480 ? "576i" : "480i";
}

/* ------------------------------------------------------------------ frame */

static void load_matrix(void) {
    Mtx m;
    guMtxIdentity(m);
    m[0][0] = t_sx; m[1][1] = t_sy;
    m[0][3] = t_tx; m[1][3] = t_ty;
    GX_LoadPosMtxImm(m, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

void gfx_begin_frame(void) {
    Mtx44 proj;
    guOrtho(proj, 0, 480, 0, vw, 0, 300);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    gfx_reset_transform();
    gfx_clear_clip();
    cur_mode = -1;
    cur_font = -1;
}

void gfx_end_frame(void) {
    GX_DrawDone();
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(xfb[fb], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(xfb[fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fb ^= 1;
}

void gfx_set_transform(float sx, float sy, float tx, float ty) {
    t_sx = sx; t_sy = sy; t_tx = tx; t_ty = ty;
    load_matrix();
}

void gfx_reset_transform(void) { gfx_set_transform(1, 1, 0, 0); }

void gfx_set_clip(float x, float y, float w, float h) {
    float x0 = x * t_sx + t_tx, y0 = y * t_sy + t_ty;
    float x1 = (x + w) * t_sx + t_tx, y1 = (y + h) * t_sy + t_ty;
    float kx = rmode->fbWidth / vw, ky = rmode->efbHeight / 480.0f;
    int px = (int)(x0 * kx), py = (int)(y0 * ky);
    int pw = (int)(x1 * kx) - px, ph = (int)(y1 * ky) - py;
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (pw < 0) pw = 0;
    if (ph < 0) ph = 0;
    GX_SetScissor(px, py, pw, ph);
}

void gfx_clear_clip(void) { GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight); }

/* ------------------------------------------------------------------ state */

static void mode_color(void) {
    if (cur_mode == 0) return;
    cur_mode = 0;
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

static void mode_text(int font) {
    if (cur_mode != 1) {
        cur_mode = 1;
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        /* colour = vertex colour, alpha = vertex alpha * glyph coverage */
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        cur_font = -1;
    }
    if (cur_font != font) {
        cur_font = font;
        GX_LoadTexObj(&font_tex[font], GX_TEXMAP0);
    }
}

static inline void vtx(float x, float y, Color c) {
    GX_Position2f32(x, y);
    GX_Color4u8(c.r, c.g, c.b, c.a);
}

/* ------------------------------------------------------------- primitives */

void gfx_rect(float x, float y, float w, float h, Color c) { gfx_rect_vgrad(x, y, w, h, c, c); }

void gfx_rect_vgrad(float x, float y, float w, float h, Color top, Color bottom) {
    if (w <= 0 || h <= 0) return;
    mode_color();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    vtx(x, y, top);
    vtx(x + w, y, top);
    vtx(x + w, y + h, bottom);
    vtx(x, y + h, bottom);
    GX_End();
}

void gfx_rect_hgrad(float x, float y, float w, float h, Color left, Color right) {
    if (w <= 0 || h <= 0) return;
    mode_color();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    vtx(x, y, left);
    vtx(x + w, y, right);
    vtx(x + w, y + h, right);
    vtx(x, y + h, left);
    GX_End();
}

#define RR_SEG 6
#define RR_PTS (4 * (RR_SEG + 1))

static int rr_points(float x, float y, float w, float h, float r, float *px, float *py) {
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0) r = 0;
    const float cx[4] = { x + w - r, x + w - r, x + r, x + r };
    const float cy[4] = { y + r, y + h - r, y + h - r, y + r };
    int n = 0;
    for (int c = 0; c < 4; c++) {
        float a0 = (-90.0f + 90.0f * c) * (float)M_PI / 180.0f;
        for (int i = 0; i <= RR_SEG; i++) {
            float a = a0 + (float)M_PI * 0.5f * i / RR_SEG;
            px[n] = cx[c] + cosf(a) * r;
            py[n] = cy[c] + sinf(a) * r;
            n++;
        }
    }
    return n;
}

void gfx_rrect_vgrad(float x, float y, float w, float h, float r, Color top, Color bottom) {
    if (w <= 0 || h <= 0) return;
    float px[RR_PTS], py[RR_PTS];
    int n = rr_points(x, y, w, h, r, px, py);
    mode_color();
    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, n + 2);
    vtx(x + w * 0.5f, y + h * 0.5f, color_lerp(top, bottom, 0.5f));
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        vtx(px[k], py[k], color_lerp(top, bottom, (py[k] - y) / h));
    }
    GX_End();
}

void gfx_rrect(float x, float y, float w, float h, float r, Color c) { gfx_rrect_vgrad(x, y, w, h, r, c, c); }

void gfx_rrect_outline(float x, float y, float w, float h, float r, float t, Color c) {
    if (w <= 0 || h <= 0) return;
    float ox[RR_PTS], oy[RR_PTS], ix[RR_PTS], iy[RR_PTS];
    int n = rr_points(x, y, w, h, r, ox, oy);
    rr_points(x + t, y + t, w - 2 * t, h - 2 * t, r - t, ix, iy);
    mode_color();
    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, (n + 1) * 2);
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        vtx(ox[k], oy[k], c);
        vtx(ix[k], iy[k], c);
    }
    GX_End();
}

void gfx_shadow(float x, float y, float w, float h, float r, float spread, Color c) {
    for (int i = 4; i >= 1; i--) {
        float s = spread * i / 4.0f;
        gfx_rrect(x - s, y - s, w + 2 * s, h + 2 * s, r + s, color_alpha(c, 0.28f));
    }
}

void gfx_circle(float cx, float cy, float r, Color c) {
    const int N = 24;
    mode_color();
    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, N + 2);
    vtx(cx, cy, c);
    for (int i = 0; i <= N; i++) {
        float a = 2.0f * (float)M_PI * i / N;
        vtx(cx + cosf(a) * r, cy + sinf(a) * r, c);
    }
    GX_End();
}

void gfx_ring(float cx, float cy, float r, float t, float a0, float a1, Color c) {
    const int N = 24;
    mode_color();
    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, (N + 1) * 2);
    for (int i = 0; i <= N; i++) {
        float a = a0 + (a1 - a0) * i / N;
        vtx(cx + cosf(a) * r, cy + sinf(a) * r, c);
        vtx(cx + cosf(a) * (r - t), cy + sinf(a) * (r - t), c);
    }
    GX_End();
}

void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
    mode_color();
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
    vtx(x1, y1, c);
    vtx(x2, y2, c);
    vtx(x3, y3, c);
    GX_End();
}

void gfx_quad(const float *p, Color c) {
    mode_color();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; i++) vtx(p[i * 2], p[i * 2 + 1], c);
    GX_End();
}

void gfx_line(float x1, float y1, float x2, float y2, float t, Color c) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    float q[8] = { x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny };
    gfx_quad(q, c);
}

void gfx_cube(float cx, float cy, float size, float rx, float ry, Color base) {
    static const float V[8][3] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}
    };
    static const int F[6][4] = {
        {4,5,6,7},{1,0,3,2},{0,4,7,3},{5,1,2,6},{0,1,5,4},{7,6,2,3}
    };
    static const float N[6][3] = { {0,0,1},{0,0,-1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0} };
    float cyy = cosf(ry), syy = sinf(ry), cxx = cosf(rx), sxx = sinf(rx);
    float P[8][2], Z[8];
    for (int i = 0; i < 8; i++) {
        float x = V[i][0], y = V[i][1], z = V[i][2];
        float x1 = x * cyy + z * syy, z1 = -x * syy + z * cyy;
        float y1 = y * cxx - z1 * sxx, z2 = y * sxx + z1 * cxx;
        float persp = 4.0f / (4.0f - z2);
        P[i][0] = cx + x1 * size * 0.5f * persp;
        P[i][1] = cy + y1 * size * 0.5f * persp;
        Z[i] = z2;
    }
    (void)Z;
    for (int f = 0; f < 6; f++) {
        float x = N[f][0], y = N[f][1], z = N[f][2];
        float z1 = -x * syy + z * cyy;
        float y1 = y * cxx - z1 * sxx;
        float nz = y * sxx + z1 * cxx;
        if (nz <= 0.02f) continue;
        float light = 0.55f + 0.45f * nz + (y1 < 0 ? -y1 * 0.25f : 0);
        if (light > 1.25f) light = 1.25f;
        Color c = base;
        c.r = (u8)fminf(255, base.r * light);
        c.g = (u8)fminf(255, base.g * light);
        c.b = (u8)fminf(255, base.b * light);
        float q[8];
        for (int k = 0; k < 4; k++) { q[k * 2] = P[F[f][k]][0]; q[k * 2 + 1] = P[F[f][k]][1]; }
        gfx_quad(q, c);
    }
}

/* ------------------------------------------------------------------- text */

static const FontGlyph *glyph_table(FontId f) { return f == FONT_UI ? font_ui_glyphs : font_body_glyphs; }
static float font_px(FontId f) { return f == FONT_UI ? FONT_UI_PX : FONT_BODY_PX; }
static float font_line(FontId f) { return f == FONT_UI ? FONT_UI_LINE : FONT_BODY_LINE; }

static inline const FontGlyph *glyph_for(const FontGlyph *G, unsigned char ch) {
    if (ch < 32 || ch > 126) ch = '?';
    return &G[ch - 32];
}

float gfx_line_height(FontId f, float size) { return font_line(f) * size / font_px(f); }

float gfx_text_width(FontId f, float size, const char *s, int len) {
    if (len < 0) len = (int)strlen(s);
    const FontGlyph *G = glyph_table(f);
    float w = 0;
    for (int i = 0; i < len; i++) w += glyph_for(G, (unsigned char)s[i])->adv;
    return w * size / font_px(f);
}

int gfx_text_fit(FontId f, float size, const char *s, int len, float maxw) {
    if (len < 0) len = (int)strlen(s);
    const FontGlyph *G = glyph_table(f);
    float sc = size / font_px(f), w = 0;
    for (int i = 0; i < len; i++) {
        float a = glyph_for(G, (unsigned char)s[i])->adv * sc;
        if (w + a * 0.5f > maxw) return i;
        w += a;
    }
    return len;
}

float gfx_text(FontId f, float size, float x, float y, Color c, const char *s, int len) {
    if (len < 0) len = (int)strlen(s);
    const FontGlyph *G = glyph_table(f);
    float sc = size / font_px(f);
    int n = 0;
    for (int i = 0; i < len; i++)
        if (glyph_for(G, (unsigned char)s[i])->w) n++;
    float cx = x;
    if (n > 0) {
        mode_text(f);
        const float iw = 1.0f / FONT_TEX_W, ih = 1.0f / FONT_TEX_H;
        GX_Begin(GX_QUADS, GX_VTXFMT0, n * 4);
        for (int i = 0; i < len; i++) {
            const FontGlyph *g = glyph_for(G, (unsigned char)s[i]);
            if (g->w) {
                float x0 = cx + g->xoff * sc, y0 = y + g->yoff * sc;
                float x1 = x0 + g->w * sc, y1 = y0 + g->h * sc;
                float u0 = g->x * iw, v0 = g->y * ih, u1 = (g->x + g->w) * iw, v1 = (g->y + g->h) * ih;
                GX_Position2f32(x0, y0); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u0, v0);
                GX_Position2f32(x1, y0); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u1, v0);
                GX_Position2f32(x1, y1); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u1, v1);
                GX_Position2f32(x0, y1); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u0, v1);
            }
            cx += g->adv * sc;
        }
        GX_End();
    } else {
        for (int i = 0; i < len; i++) cx += glyph_for(G, (unsigned char)s[i])->adv * sc;
    }
    return cx - x;
}

void gfx_text_centered(FontId f, float size, float cx, float y, Color c, const char *s) {
    float w = gfx_text_width(f, size, s, -1);
    gfx_text(f, size, cx - w * 0.5f, y, c, s, -1);
}
