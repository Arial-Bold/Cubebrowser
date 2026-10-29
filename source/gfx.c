/* gfx.c - GX setup, 2D primitives, procedural cube and bitmap-font text. */
#include "gfx.h"
#include "font_data.h"
#include "diag.h"
#include "storage.h"
#include "midi.h"
#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The ring the graphics chip reads its drawing commands from while we write
 * them. When we get too far ahead, an interrupt pauses our writing until it
 * catches up. That interrupt can be held up for a moment by the music (the
 * audio interrupts do their work with other interrupts off), and with the
 * library's usual margin of 16 KB - well under a millisecond of drawing - we
 * could run right round the ring and overwrite commands not yet read: the
 * graphics chip then stops, and the frame is lost (a black flash, noted in
 * gpu-reset.log). So the ring is bigger and the pause comes at half full,
 * leaving 384 KB of margin: several milliseconds even at full speed. */
#define FIFO_SIZE (768 * 1024)
#define XFB_BYTES (640 * 576 * VI_DISPLAY_PIX_SZ)  /* big enough for any mode */

static void *fifo;
static void *xfb[2];
static int fb;
static GXRModeObj *rmode;
static bool wide;
static float vw = 640.0f;
static float t_sx = 1, t_sy = 1, t_tx = 0, t_ty = 0;
static int cur_mode = -1;     /* 0 = color, 1 = text, 2 = image */
static bool tex_invalidate;
static int cur_font = -1;
static GXTexObj font_tex[FONT_COUNT];
static unsigned frame_verts, frame_images, gpu_resets;
static volatile bool gpu_done;

#define BEGIN(prim, count) do { frame_verts += (unsigned)(count); GX_Begin(prim, GX_VTXFMT0, (u16)(count)); } while (0)

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

static GXRModeObj custom_mode;
static int applied_pref = -1, applied_full = -1;

/* The GPU always draws a 640-pixel-wide frame. In full-width mode the video
 * interface stretches it to 704 pixels, the whole width of the TV picture
 * (as commercial games do), which also gives correct proportions: TV pixels
 * are slightly narrower than square, so 640 of them look ~9% too narrow. */
static GXRModeObj *pick_mode(int pref, bool full_width) {
    bool pal = (VIDEO_GetCurrentTvMode() == VI_PAL);
    GXRModeObj *base;
    if (pref == VMODE_480P && VIDEO_HaveComponentCable())
        base = pal ? &TVEurgb60Hz480Prog : &TVNtsc480Prog;
    else if (pref == VMODE_480I)
        base = pal ? &TVPal528IntDf : &TVNtsc480IntDf;
    else
        base = VIDEO_GetPreferredMode(NULL);
    custom_mode = *base;
    if (full_width) {
        custom_mode.viWidth = 704;
        custom_mode.viXOrigin = (720 - 704) / 2;   /* centred in the 720-pixel video line */
    }
    return &custom_mode;
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

static void drawdone_cb(void) { gpu_done = true; }

/* Drawing state that never changes; re-applied after a GPU reset. */
static void apply_state(void) {
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
}

void gfx_init(int video_mode, bool widescreen, bool full_width) {
    VIDEO_Init();
    xfb[0] = MEM_K0_TO_K1(memalign(32, XFB_BYTES));
    xfb[1] = MEM_K0_TO_K1(memalign(32, XFB_BYTES));
    configure_vi(pick_mode(video_mode, full_width));
    applied_pref = video_mode;
    applied_full = full_width;

    fifo = memalign(32, FIFO_SIZE);
    memset(fifo, 0, FIFO_SIZE);
    GXFifoObj *fo = GX_Init(fifo, FIFO_SIZE);
    GX_InitFifoLimits(fo, FIFO_SIZE / 2, FIFO_SIZE / 4);   /* pause at half full, carry on at a quarter */
    GX_SetCPUFifo(fo);
    GX_SetGPFifo(fo);
    GXColor black = { 0, 0, 0, 255 };
    GX_SetCopyClear(black, 0x00FFFFFF);
    setup_gx_for_mode();
    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(xfb[0], GX_TRUE);
    apply_state();
    GX_SetDrawDoneCallback(drawdone_cb);


    const unsigned char *texdata[FONT_COUNT] = { font_ui_tex, font_body_tex, font_italic_tex, font_mono_tex };
    const u32 texsize[FONT_COUNT] = { sizeof(font_ui_tex), sizeof(font_body_tex), sizeof(font_italic_tex),
                                      sizeof(font_mono_tex) };
    for (int i = 0; i < FONT_COUNT; i++) {
        DCFlushRange((void *)texdata[i], texsize[i]);
        GX_InitTexObj(&font_tex[i], (void *)texdata[i], FONT_TEX_W, FONT_TEX_H, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&font_tex[i], GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    }
    GX_InvalidateTexAll();
    GX_InvVtxCache();

    gfx_set_widescreen(widescreen);
}

bool gfx_apply_video_mode(int video_mode, bool full_width) {
    bool ok = !(video_mode == VMODE_480P && !VIDEO_HaveComponentCable());
    if (video_mode == applied_pref && (int)full_width == applied_full) return ok;
    applied_pref = video_mode;
    applied_full = full_width;
    GXRModeObj *m = pick_mode(video_mode, full_width);
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
/* One screen pixel in drawing units, allowing for the current transform.
 * Lines thinner than this can fall between the GPU's pixel centres and
 * vanish on some sides (in 16:9 a drawing unit is only 3/4 of a pixel). */
float gfx_pixel_w(void) { float s = fabsf(t_sx); return vw / rmode->fbWidth / (s > 1e-4f ? s : 1); }
float gfx_pixel_h(void) { float s = fabsf(t_sy); return 480.0f / rmode->efbHeight / (s > 1e-4f ? s : 1); }
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
    gfx_set_additive(false);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    if (tex_invalidate) { GX_InvalidateTexAll(); tex_invalidate = false; }
}

/* Shape masks, using the depth buffer (otherwise unused: it's cleared to
 * "far" every frame and nothing else writes to it). Shapes drawn between
 * begin and use mark their pixels; after use, drawing only lands on marked
 * pixels, so effects can follow a rounded shape exactly. */
void gfx_mask_begin(void) { GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE); }
void gfx_mask_use(void) { GX_SetZMode(GX_TRUE, GX_EQUAL, GX_FALSE); }
void gfx_mask_end(void) { GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE); }

/* Light-like drawing: what's drawn is added to what's already there, so
 * overlapping shapes brighten instead of covering each other. */
void gfx_set_additive(bool on) {

    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, on ? GX_BL_ONE : GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

/* Waits for the GPU to finish, but never forever: true if it finished. */
static bool gpu_wait(int ms) {
    gpu_done = false;
    GX_SetDrawDone();
    for (int i = 0; i < ms; i++) {
        if (gpu_done) return true;
        usleep(1000);
    }
    return gpu_done;
}

/* The GPU stopped responding: abort the frame, restore our drawing state,
 * and note what was being drawn so the cause can be found. */
static void gpu_recover(void) {
    GX_AbortFrame();
    setup_gx_for_mode();
    GX_SetCullMode(GX_CULL_NONE);
    apply_state();
    cur_mode = -1;
    cur_font = -1;
    tex_invalidate = true;
    gpu_resets++;
    char line[400];
    int n = snprintf(line, sizeof(line), "GPU reset %u: %u vertices, %u pictures this frame, music %s, audio interrupt longest %u us | %s | %s\n",
                     gpu_resets, frame_verts, frame_images, midi_state() == MIDI_PLAYING ? "playing" : "off",
                     midi_audio_irq_worst_us(), diag_get(DIAG_PAGE), diag_get(DIAG_DRAW));
    char path[400];
    storage_path(path, sizeof(path), "gpu-reset.log");
    FILE *f = fopen(path, "ab");
    if (f) { fwrite(line, 1, (size_t)n, f); fclose(f); }
}

void gfx_end_frame(void) {
    bool ok = gpu_wait(1500);
    if (ok) {
        GX_SetColorUpdate(GX_TRUE);
        GX_CopyDisp(xfb[fb], GX_TRUE);
        ok = gpu_wait(1500);
    }
    if (!ok) {
        gpu_recover();   /* skip showing this frame; the next one starts clean */
    } else {
        VIDEO_SetNextFramebuffer(xfb[fb]);
        VIDEO_Flush();
        fb ^= 1;
    }
    VIDEO_WaitVSync();
    frame_verts = frame_images = 0;
}

void gfx_set_transform(float sx, float sy, float tx, float ty) {
    t_sx = sx; t_sy = sy; t_tx = tx; t_ty = ty;
    load_matrix();
}

void gfx_reset_transform(void) { gfx_set_transform(1, 1, 0, 0); }

void gfx_set_clip(float x, float y, float w, float h) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) { gfx_clear_clip(); return; }
    float x0 = x * t_sx + t_tx, y0 = y * t_sy + t_ty;
    float x1 = (x + w) * t_sx + t_tx, y1 = (y + h) * t_sy + t_ty;
    float kx = rmode->fbWidth / vw, ky = rmode->efbHeight / 480.0f;
    int px = (int)(x0 * kx), py = (int)(y0 * ky);
    int pw = (int)(x1 * kx) - px, ph = (int)(y1 * ky) - py;
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (px > rmode->fbWidth - 1) px = rmode->fbWidth - 1;
    if (py > rmode->efbHeight - 1) py = rmode->efbHeight - 1;
    if (px + pw > rmode->fbWidth) pw = rmode->fbWidth - px;
    if (py + ph > rmode->efbHeight) ph = rmode->efbHeight - py;
    if (pw < 1) pw = 1;   /* a zero-sized scissor is invalid for the GPU */
    if (ph < 1) ph = 1;
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

/* finite and within a range the GPU's clipper handles comfortably */
static inline bool sane(float v) { return isfinite(v) && v > -100000.0f && v < 100000.0f; }

static inline void vtx(float x, float y, Color c) {
    GX_Position2f32(x, y);
    GX_Color4u8(c.r, c.g, c.b, c.a);
}

/* ------------------------------------------------------------- primitives */

/* The GPU mis-draws shapes whose corners lie thousands of units off screen
 * (one triangle of a rectangle can come out as a big diagonal wedge). Long
 * web pages produce exactly that: a tall coloured block scrolled mostly out
 * of view. So rectangles are trimmed to the screen plus a margin first.
 * The bounds are in drawing units, before the current transform. */
#define GUARD 64.0f
static void guard_bounds(float *x0, float *y0, float *x1, float *y1) {
    float sx = t_sx > 0.01f ? t_sx : 0.01f, sy = t_sy > 0.01f ? t_sy : 0.01f;
    *x0 = (-GUARD - t_tx) / sx;
    *x1 = (vw + GUARD - t_tx) / sx;
    *y0 = (-GUARD - t_ty) / sy;
    *y1 = (480.0f + GUARD - t_ty) / sy;
}

void gfx_rect(float x, float y, float w, float h, Color c) { gfx_rect_vgrad(x, y, w, h, c, c); }

void gfx_rect_vgrad(float x, float y, float w, float h, Color top, Color bottom) {
    if (!(w > 0) || !(h > 0) || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) return;
    float gx0, gy0, gx1, gy1;
    guard_bounds(&gx0, &gy0, &gx1, &gy1);
    if (x >= gx1 || y >= gy1 || x + w <= gx0 || y + h <= gy0) return;   /* entirely off screen */
    if (x < gx0) { w -= gx0 - x; x = gx0; }
    if (x + w > gx1) w = gx1 - x;
    if (y < gy0 || y + h > gy1) {   /* trim top/bottom, keeping the gradient where it was */
        float ny0 = fmaxf(y, gy0), ny1 = fminf(y + h, gy1);
        Color nt = color_lerp(top, bottom, (ny0 - y) / h), nb = color_lerp(top, bottom, (ny1 - y) / h);
        top = nt;
        bottom = nb;
        y = ny0;
        h = ny1 - ny0;
    }
    mode_color();
    BEGIN(GX_QUADS, 4);
    vtx(x, y, top);
    vtx(x + w, y, top);
    vtx(x + w, y + h, bottom);
    vtx(x, y + h, bottom);
    GX_End();
}

void gfx_rect_hgrad(float x, float y, float w, float h, Color left, Color right) {
    if (!(w > 0) || !(h > 0) || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) return;
    float gx0, gy0, gx1, gy1;
    guard_bounds(&gx0, &gy0, &gx1, &gy1);
    if (x >= gx1 || y >= gy1 || x + w <= gx0 || y + h <= gy0) return;
    if (y < gy0) { h -= gy0 - y; y = gy0; }
    if (y + h > gy1) h = gy1 - y;
    if (x < gx0 || x + w > gx1) {
        float nx0 = fmaxf(x, gx0), nx1 = fminf(x + w, gx1);
        Color nl = color_lerp(left, right, (nx0 - x) / w), nr = color_lerp(left, right, (nx1 - x) / w);
        left = nl;
        right = nr;
        x = nx0;
        w = nx1 - nx0;
    }
    mode_color();
    BEGIN(GX_QUADS, 4);
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

/* Rounded shapes: an edge far off screen is pulled in to just beyond the
 * screen margin (plus the corner radius, so the rounding stays invisible). */
static bool guard_rrect(float *x, float *y, float *w, float *h, float r, Color *top, Color *bottom) {
    float gx0, gy0, gx1, gy1;
    guard_bounds(&gx0, &gy0, &gx1, &gy1);
    float pad = (r > 0 ? r : 0) + 2;
    gx0 -= pad; gy0 -= pad; gx1 += pad; gy1 += pad;
    if (*x >= gx1 || *y >= gy1 || *x + *w <= gx0 || *y + *h <= gy0) return false;
    if (*x < gx0) { *w -= gx0 - *x; *x = gx0; }
    if (*x + *w > gx1) *w = gx1 - *x;
    if (*y < gy0 || *y + *h > gy1) {
        float ny0 = fmaxf(*y, gy0), ny1 = fminf(*y + *h, gy1);
        if (top && bottom) {
            Color nt = color_lerp(*top, *bottom, (ny0 - *y) / *h), nb = color_lerp(*top, *bottom, (ny1 - *y) / *h);
            *top = nt;
            *bottom = nb;
        }
        *y = ny0;
        *h = ny1 - ny0;
    }
    return *w > 0 && *h > 0;
}

void gfx_rrect_vgrad(float x, float y, float w, float h, float r, Color top, Color bottom) {
    if (!(w > 0) || !(h > 0) || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || !isfinite(r)) return;
    if (!guard_rrect(&x, &y, &w, &h, r, &top, &bottom)) return;
    float px[RR_PTS], py[RR_PTS];
    int n = rr_points(x, y, w, h, r, px, py);
    mode_color();
    BEGIN(GX_TRIANGLEFAN, n + 2);
    vtx(x + w * 0.5f, y + h * 0.5f, color_lerp(top, bottom, 0.5f));
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        vtx(px[k], py[k], color_lerp(top, bottom, (py[k] - y) / h));
    }
    GX_End();
}

void gfx_rrect(float x, float y, float w, float h, float r, Color c) { gfx_rrect_vgrad(x, y, w, h, r, c, c); }

void gfx_rrect_outline(float x, float y, float w, float h, float r, float t, Color c) {
    if (!(w > 0) || !(h > 0) || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || !isfinite(t)) return;
    if (!guard_rrect(&x, &y, &w, &h, r + t, NULL, NULL)) return;
    /* never thinner than a pixel (a hair more, so a line that lands exactly
     * between two pixel centres still covers one of them) */
    float ux = gfx_pixel_w() * 1.02f, uy = gfx_pixel_h() * 1.02f;
    float tx = t < ux ? ux : t, ty = t < uy ? uy : t;
    if (2 * tx >= w || 2 * ty >= h) { gfx_rrect(x, y, w, h, r, c); return; }
    float ox[RR_PTS], oy[RR_PTS], ix[RR_PTS], iy[RR_PTS];
    int n = rr_points(x, y, w, h, r, ox, oy);
    float ri = r - (tx > ty ? tx : ty);
    rr_points(x + tx, y + ty, w - 2 * tx, h - 2 * ty, ri > 0 ? ri : 0, ix, iy);
    mode_color();
    BEGIN(GX_TRIANGLESTRIP, (n + 1) * 2);
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
    if (!sane(cx) || !sane(cy) || !sane(r)) return;
    const int N = 24;
    mode_color();
    BEGIN(GX_TRIANGLEFAN, N + 2);
    vtx(cx, cy, c);
    for (int i = 0; i <= N; i++) {
        float a = 2.0f * (float)M_PI * i / N;
        vtx(cx + cosf(a) * r, cy + sinf(a) * r, c);
    }
    GX_End();
}

void gfx_ring(float cx, float cy, float r, float t, float a0, float a1, Color c) {
    if (!sane(cx) || !sane(cy) || !sane(r) || !sane(t) || !sane(a0) || !sane(a1)) return;
    const int N = 24;
    mode_color();
    BEGIN(GX_TRIANGLESTRIP, (N + 1) * 2);
    for (int i = 0; i <= N; i++) {
        float a = a0 + (a1 - a0) * i / N;
        vtx(cx + cosf(a) * r, cy + sinf(a) * r, c);
        vtx(cx + cosf(a) * (r - t), cy + sinf(a) * (r - t), c);
    }
    GX_End();
}

void gfx_tri(float x1, float y1, float x2, float y2, float x3, float y3, Color c) {
    if (!sane(x1) || !sane(y1) || !sane(x2) || !sane(y2) || !sane(x3) || !sane(y3)) return;
    mode_color();
    BEGIN(GX_TRIANGLES, 3);
    vtx(x1, y1, c);
    vtx(x2, y2, c);
    vtx(x3, y3, c);
    GX_End();
}

void gfx_quad(const float *p, Color c) {
    for (int i = 0; i < 8; i++) if (!sane(p[i])) return;
    mode_color();
    BEGIN(GX_QUADS, 4);
    for (int i = 0; i < 4; i++) vtx(p[i * 2], p[i * 2 + 1], c);
    GX_End();
}

void gfx_quad_colors(const float *p, const Color *c) {
    for (int i = 0; i < 8; i++) if (!sane(p[i])) return;
    mode_color();
    BEGIN(GX_QUADS, 4);
    for (int i = 0; i < 4; i++) vtx(p[i * 2], p[i * 2 + 1], c[i]);
    GX_End();
}

void gfx_line(float x1, float y1, float x2, float y2, float t, Color c) {
    if (!sane(x1) || !sane(y1) || !sane(x2) || !sane(y2) || !sane(t)) return;
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    float q[8] = { x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny };
    gfx_quad(q, c);
}

void gfx_cube(float cx, float cy, float size, float rx, float ry, Color base) {
    if (!sane(cx) || !sane(cy) || !sane(size) || !sane(rx) || !sane(ry)) return;
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

static const FontGlyph *glyph_table(FontId f) {
    switch (f) {
    case FONT_UI: return font_ui_glyphs;
    case FONT_ITALIC: return font_italic_glyphs;
    case FONT_MONO: return font_mono_glyphs;
    default: return font_body_glyphs;
    }
}
static float font_px(FontId f) {
    switch (f) {
    case FONT_UI: return FONT_UI_PX;
    case FONT_ITALIC: return FONT_ITALIC_PX;
    case FONT_MONO: return FONT_MONO_PX;
    default: return FONT_BODY_PX;
    }
}
static float font_line(FontId f) {
    switch (f) {
    case FONT_UI: return FONT_UI_LINE;
    case FONT_ITALIC: return FONT_ITALIC_LINE;
    case FONT_MONO: return FONT_MONO_LINE;
    default: return FONT_BODY_LINE;
    }
}

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
    if (len > 2000) len = 2000;   /* keeps the vertex count well inside GX limits */
    if (!isfinite(x) || !isfinite(y) || !isfinite(size) || size <= 0 || size > 400) return 0;
    const FontGlyph *G = glyph_table(f);
    float sc = size / font_px(f);
    int n = 0;
    for (int i = 0; i < len; i++)
        if (glyph_for(G, (unsigned char)s[i])->w) n++;
    float cx = x;
    if (n > 0) {
        mode_text(f);
        const float iw = 1.0f / FONT_TEX_W, ih = 1.0f / FONT_TEX_H;
        BEGIN(GX_QUADS, n * 4);
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

/* ----------------------------------------------------------------- images */

void gfx_image_init(GfxImage *img, void *data, int w, int h, int tex_w, int tex_h) {
    if (!data || w <= 0 || h <= 0 || tex_w <= 0 || tex_h <= 0 || tex_w > 1024 || tex_h > 1024 ||
        (tex_w & 3) || (tex_h & 3) || ((unsigned long)data & 31)) {
        free(data);   /* not a texture the GPU can safely read */
        img->data = NULL;
        return;
    }
    img->data = data;
    img->w = (u16)w; img->h = (u16)h; img->tex_w = (u16)tex_w; img->tex_h = (u16)tex_h;
    GX_InitTexObj(&img->obj, data, (u16)tex_w, (u16)tex_h, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&img->obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    tex_invalidate = true;   /* memory may have held an older texture */
}

void gfx_image_free(GfxImage *img) {
    free(img->data);
    img->data = NULL;
}

/* colour = texture x vertex colour */
static void mode_image(void) {
    if (cur_mode != 2) {
        cur_mode = 2;
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    }
    cur_font = -1;
}

void gfx_image_draw(GfxImage *img, float x, float y, float w, float h, Color c) {
    if (!img->data || !img->tex_w || !img->tex_h) return;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w <= 0 || h <= 0) return;
    mode_image();
    frame_images++;
    GX_LoadTexObj(&img->obj, GX_TEXMAP0);
    float u1 = (float)img->w / img->tex_w, v1 = (float)img->h / img->tex_h;
    BEGIN(GX_QUADS, 4);
    GX_Position2f32(x, y);         GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(0, 0);
    GX_Position2f32(x + w, y);     GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u1, 0);
    GX_Position2f32(x + w, y + h); GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(u1, v1);
    GX_Position2f32(x, y + h);     GX_Color4u8(c.r, c.g, c.b, c.a); GX_TexCoord2f32(0, v1);
    GX_End();
}

/* --------------------------------------------------------------- patterns */

/* A small greyscale texture that repeats in both directions; `data` is
 * w x h bytes, one per pixel, row by row (it's rearranged into the GPU's
 * 8x4 tiles here). w and h are powers of two, 8 to 256. */
bool gfx_pattern_init(GfxPattern *pt, const unsigned char *data, int w, int h) {
    pt->data = NULL;
    if (w < 8 || h < 4 || w > 256 || h > 256 || (w & (w - 1)) || (h & (h - 1))) return false;
    unsigned char *t = memalign(32, (size_t)w * h);
    if (!t) return false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            t[((y >> 2) * (w >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)] = data[y * w + x];
    DCFlushRange(t, (u32)(w * h));
    pt->data = t;
    pt->w = (u16)w;
    pt->h = (u16)h;
    GX_InitTexObj(&pt->obj, t, (u16)w, (u16)h, GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjLOD(&pt->obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    tex_invalidate = true;
    return true;
}

void gfx_pattern_free(GfxPattern *pt) {
    free(pt->data);
    pt->data = NULL;
}

/* A grid of (cols+1) x (rows+1) points, each with its own place, texture
 * position (1.0 = once across the pattern) and colour. */
void gfx_pattern_grid(GfxPattern *pt, int cols, int rows, const float *xy, const float *uv, const Color *c) {
    if (!pt->data || cols < 1 || rows < 1) return;
    int n = (cols + 1) * (rows + 1);
    for (int i = 0; i < n * 2; i++) if (!sane(xy[i]) || !isfinite(uv[i])) return;
    mode_image();
    GX_LoadTexObj(&pt->obj, GX_TEXMAP0);
    for (int r = 0; r < rows; r++) {
        BEGIN(GX_TRIANGLESTRIP, (cols + 1) * 2);
        for (int k = 0; k <= cols; k++) {
            for (int d = 0; d < 2; d++) {
                int i = (r + d) * (cols + 1) + k;
                GX_Position2f32(xy[i * 2], xy[i * 2 + 1]);
                GX_Color4u8(c[i].r, c[i].g, c[i].b, c[i].a);
                GX_TexCoord2f32(uv[i * 2], uv[i * 2 + 1]);
            }
        }
        GX_End();
    }
}
