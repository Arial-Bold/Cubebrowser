/* settings_ui.c - category list on the left, options on the right. Every
 * change applies immediately; the file is saved when you leave the screen. */
#include "settings_ui.h"
#include "settings.h"
#include "browser.h"
#include "ui.h"
#include "osk.h"
#include "gfx.h"
#include "theme.h"
#include "input.h"
#include "sound.h"
#include "netio.h"
#include "storage.h"
#include "conn.h"
#include "keyboard.h"
#include "dolload.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int category;
static bool dirty;
static float scroll, scroll_target;
static int capture = -1;          /* def index waiting for a button press */
static bool capture_armed;
static int confirm_action = -1;   /* def index pressed once */

/* .dol picker */
#define PICK_MAX 48
static char (*pick_list)[256];
static int pick_count = -1;       /* -1 = closed */
static int pick_scroll;

void settings_apply_all(void) {
    theme_set(g_set.theme);
    gfx_set_widescreen(g_set.widescreen != 0);
    input_clamp_cursor();
    if (!gfx_apply_video_mode(g_set.video_mode, g_set.full_width != 0))
        ui_toast("480p needs component cables - using the default mode");
}

static void apply_one(const SetDef *d) {
    dirty = true;
    if (d->off == offsetof(Settings, theme)) theme_set(g_set.theme);
    else if (d->off == offsetof(Settings, widescreen)) { gfx_set_widescreen(g_set.widescreen != 0); input_clamp_cursor(); }
    else if (d->off == offsetof(Settings, video_mode) || d->off == offsetof(Settings, full_width)) {
        if (!gfx_apply_video_mode(g_set.video_mode, g_set.full_width != 0)) ui_toast("480p needs component cables");
    }
}

void settings_ui_open(void) {
    capture = -1;
    confirm_action = -1;
    dirty = false;
    scroll = scroll_target = 0;
}

static void str_done(bool ok, const char *text, void *user) {
    if (!ok) return;
    const SetDef *d = &g_setdefs[(intptr_t)user];
    char *dst = setdef_str(d);
    if (d->min == OSK_TEXTMODE_IP && text[0]) {
        Settings tmp = g_set;
        snprintf((char *)&tmp + d->off, (size_t)d->max, "%s", text);
        settings_validate(&tmp);
        if (strcmp((char *)&tmp + d->off, text) != 0) {
            ui_toast("That isn't a valid address (example: 192.168.1.20)");
            sound_play(SFX_ERROR);
            return;
        }
    }
    snprintf(dst, (size_t)d->max, "%s", text);
    apply_one(d);
}

static void run_action(int idx) {
    const SetDef *d = &g_setdefs[idx];
    bool needs_confirm = (d->min == SA_CLEAR_BOOKMARKS || d->min == SA_RESET_ALL);
    if (needs_confirm && confirm_action != idx) {
        confirm_action = idx;
        ui_toast("Press again to confirm");
        return;
    }
    confirm_action = -1;
    switch (d->min) {
    case SA_CLEAR_HISTORY: browser_clear_history(); ui_toast("History cleared"); break;
    case SA_CLEAR_BOOKMARKS: browser_clear_bookmarks(); ui_toast("Bookmarks deleted"); break;
    case SA_RESET_ALL:
        settings_defaults(&g_set);
        settings_apply_all();
        dirty = true;
        ui_toast("All settings reset");
        break;
    case SA_FIND_DOL:
        if (!storage_ok()) { ui_toast("No SD card found"); break; }
        if (!pick_list) pick_list = malloc(sizeof(*pick_list) * PICK_MAX);
        if (!pick_list) break;
        pick_count = dol_find("sd:/", pick_list, PICK_MAX);
        pick_scroll = 0;
        if (!pick_count) { ui_toast("No .dol files found on the SD card"); pick_count = -1; }
        break;
    case SA_SAVE_NOW:
        if (settings_save()) { dirty = false; ui_toast("Settings saved"); }
        else ui_toast("Couldn't save - is the SD card inserted?");
        break;
    }
}

static void step_value(const SetDef *d, int dir) {
    int *v = setdef_int(d);
    int nv = *v + dir * (d->step > 0 ? d->step : 1);
    if (d->type == ST_CHOICE) {
        if (nv > d->max) nv = d->min;
        if (nv < d->min) nv = d->max;
    } else {
        if (nv > d->max) nv = d->max;
        if (nv < d->min) nv = d->min;
    }
    if (nv != *v) {
        *v = nv;
        sound_play(SFX_KEY);
        apply_one(d);
    } else {
        sound_play(SFX_ERROR);
    }
}

static void draw_toggle(UiId id, float x, float y, bool on) {
    const Theme *t = theme();
    float k = ui_anim(id, on ? 1.0f : 0.0f, 0.3f);
    float w = 56, h = 28;
    gfx_rrect(x, y, w, h, 14, color_lerp(color_alpha(t->button_bottom, 0.7f), t->accent, k));
    gfx_rrect_outline(x, y, w, h, 14, 1.5f, t->panel_edge);
    float kx = x + 14 + k * (w - 28);
    gfx_circle(kx + 1, y + 16, 11, color_alpha(t->shadow, 0.6f));
    gfx_circle(kx, y + 14, 11, t->button_top);
    gfx_circle(kx, y + 11, 6, rgba(255, 255, 255, 80));
}

static void draw_info_line(float x, float y, float w) {
    const Theme *t = theme();
    char info[160];
    char kb[32] = "";
    if (kbd_connected()) snprintf(kb, sizeof(kb), "  |  Keyboard: port %d", kbd_port());
    snprintf(info, sizeof(info), "%s  %s  |  %s  |  SD: %s%s", gfx_mode_name(),
             g_set.widescreen ? "16:9" : "4:3", netio_state() == NET_UP ? netio_ip() : "offline",
             storage_ok() ? storage_device_name() : "none", kb);
    float ts = 14;
    float iw = gfx_text_width(FONT_UI, ts, info, -1);
    gfx_text(FONT_UI, ts, x + w - iw, y, t->text_dim, info, -1);
}

static bool esc_pressed(void) {
    KeyEvent ev;
    bool esc = false;
    while (!osk_active() && kbd_next(&ev)) if (ev.key == KEY_ESC) esc = true;
    return esc;
}

/* Lists the .dol files found; choosing one sets it as the Swiss program. */
static void draw_picker(bool interactive) {
    const Theme *t = theme();
    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    gfx_rect(0, 0, gfx_width(), 480, color_alpha(t->shadow, 0.6f));
    float pw = fminf(600, sw - 20), ph = 380;
    float px = sx + (sw - pw) * 0.5f, py = sy + (sh - ph) * 0.5f;
    ui_set_input(interactive);
    ui_panel(px, py, pw, ph, 20);
    gfx_text(FONT_UI, 22, px + 24, py + 14, t->text, "Choose the Swiss program", -1);
    const int visible = 6;
    float row_h = 40, ly = py + 56;
    int dir = ui_dpad_unhandled();
    float sd = input_scroll_delta();
    if ((dir == DIR_DOWN || sd > 20) && pick_scroll + visible < pick_count) pick_scroll++;
    if ((dir == DIR_UP || sd < -20) && pick_scroll > 0) pick_scroll--;
    for (int i = pick_scroll; i < pick_count && i < pick_scroll + visible; i++) {
        const char *shown = pick_list[i] + (strncmp(pick_list[i], "sd:", 3) ? 0 : 3);
        bool current = !strcmp(pick_list[i], g_set.swiss_dol);
        if (ui_button(ui_id("pick", i), px + 20, ly + (i - pick_scroll) * (row_h + 6), pw - 40, row_h, shown, 16,
                      current ? UIB_ACCENT : UIB_FLAT)) {
            char err[200];
            if (dol_check(pick_list[i], err, sizeof(err))) {
                snprintf(g_set.swiss_dol, sizeof(g_set.swiss_dol), "%s", pick_list[i]);
                dirty = true;
                pick_count = -1;
                ui_toast("Exit button will start this program");
                return;
            }
            ui_toast(err);
        }
    }
    if (pick_count > visible) {
        char more[48];
        snprintf(more, sizeof(more), "%d-%d of %d", pick_scroll + 1, pick_scroll + visible, pick_count);
        gfx_text_centered(FONT_UI, 14, px + pw * 0.5f, py + ph - 76, t->text_dim, more);
    }
    if (ui_button(ui_id("pick-cancel", 0), px + pw * 0.3f, py + ph - 54, pw * 0.4f, 40, "Cancel", 17, UIB_NORMAL) ||
        (interactive && input_pressed(ACT_BACK))) {
        input_consume(ACT_BACK);
        pick_count = -1;
    }
}

bool settings_ui_frame(bool interactive) {
    const Theme *t = theme();
    bool modal = osk_active() || pick_count >= 0;
    bool input_ok = interactive && !modal;
    ui_set_input(input_ok);
    bool leave = false;

    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);

    /* header */
    float hh = 54;
    ui_panel(sx + 4, sy + 4, sw - 8, hh, 24);
    if (ui_icon_button(ui_id("set-back", 0), sx + 30, sy + 4 + hh * 0.5f, 18, ICON_BACK, true)) leave = true;
    ui_draw_icon(ICON_GEAR, sx + 72, sy + 4 + hh * 0.5f, 26, t->text);
    gfx_text(FONT_UI, 24, sx + 92, sy + 16, t->text, "Settings", -1);
    draw_info_line(sx + 210, sy + 24, sw - 230);

    /* categories */
    float top = sy + hh + 16;
    float cat_w = 180, bh = 38, gap = 6;
    float list_h = sh - hh - 16 - 8;
    ui_panel(sx + 4, top, cat_w + 16, list_h, 18);
    for (int c = 0; c < CAT_COUNT; c++) {
        float y = top + 10 + c * (bh + gap);
        if (ui_button(ui_id("cat", c), sx + 12, y, cat_w, bh, category_names[c], 17,
                      c == category ? UIB_ACCENT : UIB_FLAT) && c != category) {
            category = c;
            scroll = scroll_target = 0;
            capture = -1;
            confirm_action = -1;
        }
    }

    /* option rows */
    float ox = sx + cat_w + 32, ow = sw - cat_w - 36, oy = top, oh = list_h;
    ui_panel(ox, oy, ow, oh, 18);
    float help_h = 44;
    float rows_y = oy + 10, rows_h = oh - help_h - 14;
    float row_h = 46;
    const char *help = NULL;

    int idxs[64], n = 0;
    for (int i = 0; i < g_setdef_count && n < 64; i++)
        if (g_setdefs[i].cat == category) idxs[n++] = i;
    float content_h = n * row_h;
    if (input_ok) {
        scroll_target += input_scroll_delta() * 0.5f;
        int d = ui_dpad_unhandled();
        if (d == DIR_DOWN) scroll_target += row_h;
        if (d == DIR_UP) scroll_target -= row_h;
    }
    float max_s = content_h - rows_h;
    if (max_s < 0) max_s = 0;
    if (scroll_target > max_s) scroll_target = max_s;
    if (scroll_target < 0) scroll_target = 0;
    scroll += (scroll_target - scroll) * (g_set.animations ? 0.3f : 1.0f);

    gfx_set_clip(ox, rows_y, ow, rows_h);
    float ctrl_w = fminf(250, ow * 0.45f);
    float ctrl_x = ox + ow - ctrl_w - 18;
    for (int r = 0; r < n; r++) {
        int i = idxs[r];
        const SetDef *d = &g_setdefs[i];
        float y = rows_y + r * row_h - scroll;
        if (y + row_h < rows_y || y > rows_y + rows_h) continue;
        bool visible = (y >= rows_y - 4 && y + row_h <= rows_y + rows_h + 4);
        ui_set_input(input_ok && visible);
        if (r % 2 == 0) gfx_rrect(ox + 8, y + 2, ow - 16, row_h - 4, 10, rgba(255, 255, 255, 18));
        char value[300];
        settings_format_value(d, value, sizeof(value));
        float ly = y + (row_h - gfx_line_height(FONT_UI, 17)) * 0.5f;
        UiId rid = ui_id("row", i);
        switch (d->type) {
        case ST_BOOL:
            gfx_text(FONT_UI, 17, ox + 20, ly, t->text, d->label, -1);
            if (ui_click(rid, ox + 8, y + 2, ow - 16, row_h - 4)) {
                *setdef_int(d) = !*setdef_int(d);
                sound_play(SFX_SELECT);
                apply_one(d);
            }
            draw_toggle(rid ^ 0x1234u, ctrl_x + ctrl_w - 56, y + 9, *setdef_int(d) != 0);
            break;
        case ST_INT:
        case ST_CHOICE: {
            gfx_text(FONT_UI, 17, ox + 20, ly, t->text, d->label, -1);
            float cy = y + row_h * 0.5f;
            if (ui_icon_button(ui_id("dec", i), ctrl_x + 14, cy, 13, ICON_LEFT, true)) step_value(d, -1);
            if (ui_icon_button(ui_id("inc", i), ctrl_x + ctrl_w - 14, cy, 13, ICON_RIGHT, true)) step_value(d, +1);
            gfx_text_centered(FONT_UI, 17, ctrl_x + ctrl_w * 0.5f, ly - 3, t->text, value);
            if (d->type == ST_INT) {
                float frac = (float)(*setdef_int(d) - d->min) / (float)(d->max - d->min);
                float bx = ctrl_x + 36, bw = ctrl_w - 72;
                gfx_rrect(bx, y + row_h - 12, bw, 5, 2.5f, color_alpha(t->button_bottom, 0.6f));
                gfx_rrect(bx, y + row_h - 12, fmaxf(5, bw * frac), 5, 2.5f, t->accent);
            }
            if (ui_hover(rid, ox + 8, y + 2, ctrl_x - ox - 16, row_h - 4)) help = d->help;
            break;
        }
        case ST_STR: {
            gfx_text(FONT_UI, 17, ox + 20, ly, t->text, d->label, -1);
            bool hov = ui_hover(rid, ox + 8, y + 2, ow - 16, row_h - 4);
            gfx_rrect(ctrl_x, y + 8, ctrl_w, row_h - 16, 9, t->field_bg);
            gfx_rrect_outline(ctrl_x, y + 8, ctrl_w, row_h - 16, 9, hov ? 2.0f : 1.0f, hov ? t->accent : t->panel_edge);
            int vn = gfx_text_fit(FONT_BODY, 16, value, -1, ctrl_w - 20);
            gfx_text(FONT_BODY, 16, ctrl_x + 10, y + 13, t->field_text, value, vn);
            if (hov && input_ok && visible && input_pressed(ACT_CLICK)) {
                input_consume(ACT_CLICK);
                sound_play(SFX_SELECT);
                int mode = d->min == OSK_TEXTMODE_IP ? OSK_IP : (d->min == OSK_TEXTMODE_URL ? OSK_URL : OSK_TEXT);
                osk_open(d->label, setdef_str(d), d->max - 1, mode, str_done, (void *)(intptr_t)i);
            }
            if (hov) help = d->help;
            break;
        }
        case ST_BIND: {
            gfx_text(FONT_UI, 17, ox + 20, ly, t->text, d->label, -1);
            bool waiting = (capture == i);
            const char *lbl = waiting ? "Press a button..." : value;
            if (ui_button(rid, ctrl_x, y + 6, ctrl_w, row_h - 12, lbl, 17, waiting ? UIB_ACCENT : UIB_NORMAL)) {
                capture = i;
                capture_armed = false;
            }
            if (ui_hover(rid ^ 0x99u, ox + 8, y + 2, ctrl_x - ox - 16, row_h - 4) || waiting) help = d->help ? d->help : "Select a binding, then press the button you want.";
            break;
        }
        case ST_ACTION: {
            const char *lbl = (confirm_action == i) ? "Press again to confirm" : d->label;
            if (ui_button(rid, ox + 20, y + 6, ow - 40, row_h - 12, lbl, 17,
                          confirm_action == i ? UIB_ACCENT : UIB_NORMAL))
                run_action(i);
            break;
        }
        }
        if (!help && ui_input() && input_cursor_y() >= y && input_cursor_y() < y + row_h &&
            input_cursor_x() >= ox && input_cursor_x() < ox + ow)
            help = d->help;
    }
    gfx_clear_clip();
    ui_set_input(input_ok);

    /* scroll indicator */
    if (content_h > rows_h) {
        float bh2 = fmaxf(24, rows_h * rows_h / content_h);
        float by2 = rows_y + (rows_h - bh2) * (scroll / (content_h - rows_h));
        gfx_rrect(ox + ow - 8, by2, 4, bh2, 2, t->accent);
    }

    /* Keyboard page: show what each controller port reports */
    if (category == CAT_KEYBOARD) {
        char ports[200];
        kbd_describe_ports(ports, sizeof(ports));
        float py2 = rows_y + n * row_h + 14 - scroll;
        if (py2 < rows_y + rows_h - 40) {
            gfx_text(FONT_UI, 15, ox + 20, py2, t->text, kbd_connected() ? "Keyboard connected" : "No keyboard found yet", -1);
            gfx_text(FONT_BODY, 13, ox + 20, py2 + 24, t->text_dim, ports, gfx_text_fit(FONT_BODY, 13, ports, -1, ow - 40));
            if (kbd_connected()) {
                char st[160];
                kbd_stats(st, sizeof(st));
                gfx_text(FONT_BODY, 13, ox + 20, py2 + 44, t->text_dim, st, gfx_text_fit(FONT_BODY, 13, st, -1, ow - 40));
            }
            if (kbd_last_code()) {
                char lc[48];
                snprintf(lc, sizeof(lc), "Last key code: 0x%02X", kbd_last_code());
                gfx_text(FONT_BODY, 13, ox + 20, py2 + 64, t->text_dim, lc, -1);
            }
        }
    }

    /* help line */
    float hy = oy + oh - help_h;
    gfx_rect(ox + 14, hy - 2, ow - 28, 1.5f, color_alpha(t->panel_edge, 0.6f));
    if (help) gfx_text(FONT_UI, 15, ox + 20, hy + 10, t->text_dim, help, gfx_text_fit(FONT_UI, 15, help, -1, ow - 40));

    /* button capture for remapping */
    if (capture >= 0 && interactive && !modal) {
        if (!capture_armed) {
            if (input_raw_held() == 0) capture_armed = true;   /* wait until A is released */
        } else {
            int b = input_pressed_bindable();
            if (b >= 0) {
                const SetDef *d = &g_setdefs[capture];
                int *v = setdef_int(d);
                for (int a = 0; a < ACT_COUNT; a++)       /* swap with whatever used it */
                    if (&g_set.bind[a] != v && g_set.bind[a] == b) g_set.bind[a] = *v;
                *v = b;
                capture = -1;
                dirty = true;
                sound_play(SFX_SELECT);
                input_consume(ACT_CLICK);
                input_consume(ACT_BACK);
                input_consume(ACT_MENU);
            }
        }
    } else if (input_ok && (input_pressed(ACT_BACK) || input_pressed(ACT_MENU) || esc_pressed())) {
        input_consume(ACT_BACK);
        input_consume(ACT_MENU);
        leave = true;
    }

    if (pick_count >= 0) draw_picker(interactive && !osk_active());

    if (leave) {
        capture = -1;
        if (dirty) {
            if (settings_save()) ui_toast("Settings saved");
            else ui_toast("Couldn't save settings - is the SD card inserted?");
            dirty = false;
        }
        sound_play(SFX_BACK);
    }
    return leave;
}
