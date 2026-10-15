/* osk.c - chunky rounded on-screen keyboard.
 * Pointer or D-pad (snaps key to key) to type; shortcuts:
 * B backspace, Y space, X shift, L/R move caret, START accept. */
#include "osk.h"
#include "ui.h"
#include "gfx.h"
#include "theme.h"
#include "input.h"
#include "settings.h"
#include "sound.h"
#include "keyboard.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEXT_MAX 512

static bool active;
static char title[64];
static char text[TEXT_MAX];
static int len, caret, maxlen, mode, view_start;
static bool shift, caps, sym;
static OskDone done_cb;
static void *done_user;
static int open_frame;

static const char *const rows_qwerty[2][4] = {
    { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'", "zxcvbnm,./" },
    { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"", "ZXCVBNM<>?" },
};
static const char *const rows_abc[2][4] = {
    { "1234567890-=", "abcdefghij'/", "klmnopqrst,.", "uvwxyz;:?!" },
    { "!@#$%^&*()_+", "ABCDEFGHIJ\"\\", "KLMNOPQRST<>", "UVWXYZ{}[]" },
};
static const char *const rows_sym[4] = { "!@#$%^&*()_+", "`~\\|{}[]<>-=", "\"';:/?.,%&*", "1234567890" };
static const char *const rows_ip[4] = { "123", "456", "789", ".0" };
typedef struct { const char *label; float w; int id; int style; } BottomKey;

static const char *const quick_url[] = { "https://", "www.", ".com", ".org", ".net", "/" };

void osk_open(const char *t, const char *initial, int maxl, int m, OskDone done, void *user) {
    snprintf(title, sizeof(title), "%s", t);
    snprintf(text, sizeof(text), "%s", initial ? initial : "");
    maxlen = (maxl > 0 && maxl < TEXT_MAX) ? maxl : TEXT_MAX - 1;
    if ((int)strlen(text) > maxlen) text[maxlen] = 0;
    len = (int)strlen(text);
    caret = len;
    view_start = 0;
    mode = m;
    shift = caps = sym = false;
    done_cb = done;
    done_user = user;
    active = true;
    open_frame = ui_frame();
    kbd_clear();   /* don't type keys pressed before the box opened */
    sound_play(SFX_OPEN);
}

bool osk_active(void) { return active; }

static void insert(const char *s) {
    int n = (int)strlen(s);
    if (len + n > maxlen) n = maxlen - len;
    if (n <= 0) { sound_play(SFX_ERROR); return; }
    memmove(text + caret + n, text + caret, (size_t)(len - caret + 1));
    memcpy(text + caret, s, (size_t)n);
    len += n;
    caret += n;
    sound_play(SFX_KEY);
    if (shift && !caps) shift = false;
}

static void backspace(void) {
    if (caret == 0) return;
    memmove(text + caret - 1, text + caret, (size_t)(len - caret + 1));
    caret--;
    len--;
    sound_play(SFX_KEY);
}

static void delete_forward(void) {
    if (caret >= len) return;
    memmove(text + caret, text + caret + 1, (size_t)(len - caret));
    len--;
    sound_play(SFX_KEY);
}

static void finish(bool accepted);
static void draw_field(float x, float y, float w, float h, float ts);

/* Keys from a real keyboard. Returns true if the box was closed. */
static bool keyboard_input(void) {
    KeyEvent ev;
    while (kbd_next(&ev)) {
        switch (ev.key) {
        case KEY_ENTER: finish(true); return true;
        case KEY_ESC: finish(false); return true;
        case KEY_BACKSPACE: backspace(); break;
        case KEY_DELETE: delete_forward(); break;
        case KEY_LEFT: if (caret > 0) caret--; break;
        case KEY_RIGHT: if (caret < len) caret++; break;
        case KEY_HOME: case KEY_UP: caret = 0; break;
        case KEY_END: case KEY_DOWN: caret = len; break;
        default:
            if (ev.key >= 32 && ev.key < 127 && !ev.ctrl && !ev.alt) {
                if (mode == OSK_IP && !(ev.key == '.' || (ev.key >= '0' && ev.key <= '9'))) break;
                char ch[2] = { (char)ev.key, 0 };
                bool saved = shift;
                shift = false;
                insert(ch);
                shift = saved;
            } else if (ev.ctrl && (ev.key == 'a' || ev.key == 'A')) {
                caret = len;
            } else if (ev.ctrl && (ev.key == 'u' || ev.key == 'U')) {
                text[0] = 0; len = caret = 0;   /* clear the box */
            }
            break;
        }
    }
    return false;
}

/* With a keyboard connected: just the text box, no on-screen keys. */
static void compact_frame(void) {
    const Theme *t = theme();
    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    float open = ui_anim(ui_id("osk-open", open_frame), 1.0f, 0.22f);
    gfx_rect(0, 0, gfx_width(), 480, color_alpha(t->shadow, 0.5f * open));
    float pw = fminf(sw, 640), ph = 170;
    float px = sx + (sw - pw) * 0.5f, py = sy + 60 - (1.0f - open) * 40;
    ui_panel(px, py, pw, ph, 18);
    gfx_text(FONT_UI, 20, px + 20, py + 12, t->text, title, -1);
    const char *hint = "Type on your keyboard.  Enter: OK   Esc: Cancel";
    gfx_text(FONT_UI, 14, px + pw - 20 - gfx_text_width(FONT_UI, 14, hint, -1), py + 18, t->text_dim, hint, -1);
    draw_field(px + 16, py + 44, pw - 32, 46, 22);
    float bw = 140, by = py + ph - 54;
    if (ui_button(ui_id("osk-c", 0), px + pw - 2 * bw - 28, by, bw, 38, "Cancel", 17, UIB_NORMAL)) { finish(false); return; }
    if (ui_button(ui_id("osk-c", 1), px + pw - bw - 16, by, bw, 38, "OK", 17, UIB_ACCENT)) { finish(true); return; }
    if (keyboard_input()) return;
    if (ui_frame() != open_frame && (input_raw_pressed() & PAD_BUTTON_START)) finish(true);
}

static void finish(bool accepted) {
    active = false;
    sound_play(accepted ? SFX_SELECT : SFX_CLOSE);
    if (done_cb) done_cb(accepted, text, done_user);
}

static void draw_field(float x, float y, float w, float h, float ts) {
    const Theme *t = theme();
    gfx_rrect(x, y, w, h, 10, t->field_bg);
    gfx_rrect_outline(x, y, w, h, 10, 2, t->accent);
    float pad = 12, fw = w - pad * 2;
    char shown[TEXT_MAX];
    for (int i = 0; i <= len; i++) shown[i] = (mode == OSK_PASSWORD && i < len) ? '*' : text[i];
    if (caret < view_start) view_start = caret;
    while (view_start < caret && gfx_text_width(FONT_BODY, ts, shown + view_start, caret - view_start) > fw - 4)
        view_start++;
    int vis = gfx_text_fit(FONT_BODY, ts, shown + view_start, len - view_start, fw);
    float ty = y + (h - gfx_line_height(FONT_BODY, ts)) * 0.5f;
    gfx_set_clip(x + 4, y, w - 8, h);
    gfx_text(FONT_BODY, ts, x + pad, ty, t->field_text, shown + view_start, vis);
    float cx = x + pad + gfx_text_width(FONT_BODY, ts, shown + view_start, caret - view_start);
    if ((ui_frame() / 20) % 2 == 0) gfx_rect(cx, ty + 2, 2, gfx_line_height(FONT_BODY, ts) - 4, t->accent);
    gfx_clear_clip();
    /* click inside the field to move the caret */
    if (ui_click(ui_id("osk-field", 0), x, y, w, h)) {
        float rel = input_cursor_x() - (x + pad);
        caret = view_start + gfx_text_fit(FONT_BODY, ts, shown + view_start, len - view_start, rel);
    }
}

void osk_frame(void) {
    if (!active) return;
    ui_set_input(true);
    if (kbd_connected()) { compact_frame(); return; }
    const Theme *t = theme();
    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    float open = ui_anim(ui_id("osk-open", open_frame), 1.0f, 0.22f);

    gfx_rect(0, 0, gfx_width(), 480, color_alpha(t->shadow, 0.7f * open));

    bool ip = (mode == OSK_IP);
    int nrows = 4 + (mode == OSK_URL ? 1 : 0);
    float key_h = 38, gap = 6;
    float pw = ip ? 360 : fminf(sw, 640);
    float ph = 70 + 50 + nrows * (key_h + gap) + key_h + 24;
    float px = sx + (sw - pw) * 0.5f;
    float py = sy + sh - ph + (1.0f - open) * (ph + 40);
    ui_panel(px, py, pw, ph, 18);
    gfx_text(FONT_UI, 20, px + 20, py + 12, t->text, title, -1);
    const char *hint = "B: Delete  Y: Space  X: Shift  L/R: Move  START: OK";
    float hs = 13;
    gfx_text(FONT_UI, hs, px + pw - 20 - gfx_text_width(FONT_UI, hs, hint, -1), py + 18, t->text_dim, hint, -1);

    draw_field(px + 16, py + 44, pw - 32, 46, 22);

    /* keys */
    float ky = py + 104;
    float inner = pw - 32;
    if (mode == OSK_URL) {
        int nq = 6;
        float qw = (inner - gap * (nq - 1)) / nq;
        for (int i = 0; i < nq; i++)
            if (ui_button(ui_id("osk-q", i), px + 16 + i * (qw + gap), ky, qw, key_h, quick_url[i], 17, UIB_FLAT))
                insert(quick_url[i]);
        ky += key_h + gap;
    }
    const char *const *rows;
    if (ip) rows = rows_ip;
    else if (sym) rows = rows_sym;
    else rows = (g_set.osk_layout ? rows_abc : rows_qwerty)[shift ? 1 : 0];
    int cols = ip ? 3 : 12;
    float kw = (inner - gap * (cols - 1)) / cols;
    for (int r = 0; r < 4; r++) {
        int n = (int)strlen(rows[r]);
        float rw = n * kw + (n - 1) * gap;
        float rx = px + 16 + (inner - rw) * 0.5f;
        for (int k = 0; k < n; k++) {
            char label[2] = { rows[r][k], 0 };
            if (ui_button(ui_id("osk-k", r * 32 + k), rx + k * (kw + gap), ky, kw, key_h, label, 20, UIB_NORMAL))
                insert(label);
        }
        ky += key_h + gap;
    }

    /* bottom row */
    float bx = px + 16;
    BottomKey bottom[6];
    int nb = 0;
    if (!ip) {
        bottom[nb++] = (BottomKey){ caps ? "CAPS" : "Shift", 1.3f, 1, (shift || caps) ? UIB_ACCENT : UIB_NORMAL };
        bottom[nb++] = (BottomKey){ sym ? "ABC" : "#+=", 1.1f, 2, UIB_NORMAL };
        bottom[nb++] = (BottomKey){ "Space", 2.6f, 3, UIB_NORMAL };
    }
    bottom[nb++] = (BottomKey){ "Delete", 1.4f, 4, UIB_NORMAL };
    bottom[nb++] = (BottomKey){ "Cancel", 1.4f, 5, UIB_NORMAL };
    bottom[nb++] = (BottomKey){ "OK", 1.4f, 6, UIB_ACCENT };
    float total = 0;
    for (int i = 0; i < nb; i++) total += bottom[i].w;
    float unit = (inner - gap * (nb - 1)) / total;
    for (int i = 0; i < nb; i++) {
        float w = bottom[i].w * unit;
        if (ui_button(ui_id("osk-b", bottom[i].id), bx, ky, w, key_h, bottom[i].label, 17, bottom[i].style)) {
            switch (bottom[i].id) {
            case 1:
                if (shift && !caps) caps = true;
                else if (caps) { caps = false; shift = false; }
                else shift = true;
                break;
            case 2: sym = !sym; break;
            case 3: insert(" "); break;
            case 4: backspace(); break;
            case 5: finish(false); return;
            case 6: finish(true); return;
            }
        }
        bx += w + gap;
    }

    /* controller shortcuts (ignored on the frame the keyboard opened) */
    if (ui_frame() != open_frame) {
        if (input_raw_repeat(PAD_BUTTON_B)) backspace();
        if (!ip && (input_raw_pressed() & PAD_BUTTON_Y)) insert(" ");
        if (!ip && (input_raw_pressed() & PAD_BUTTON_X)) { shift = !shift; caps = false; }
        if (input_raw_repeat(PAD_TRIGGER_L) && caret > 0) caret--;
        if (input_raw_repeat(PAD_TRIGGER_R) && caret < len) caret++;
        if (input_raw_pressed() & PAD_BUTTON_START) { finish(true); return; }
    }
}
