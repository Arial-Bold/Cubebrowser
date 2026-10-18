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
#include "dolload.h"
#include "cookies.h"
#include "update.h"
#include "version.h"
#include "midi.h"
#include "picache.h"
#include <dirent.h>
#include <stddef.h>
#include <strings.h>
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
static bool leave_now;            /* an action wants the browser shown */

/* list picker: the Swiss .dol, or the music player's SoundFont */
#define PICK_MAX 48
static char (*pick_list)[256];
static int pick_count = -1;       /* -1 = closed */
static int pick_scroll;
static bool pick_sf2;             /* choosing instruments rather than a .dol */

/* Lists the choices for the Instruments setting: automatic, built-in, then
 * every .sf2 file in the midi folder. */
static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void open_sf2_picker(void) {
    if (!pick_list) pick_list = malloc(sizeof(*pick_list) * PICK_MAX);
    if (!pick_list) return;
    int n = 0;
    snprintf(pick_list[n++], 256, "%s", "");
    snprintf(pick_list[n++], 256, "%s", "builtin");
    DIR *d = storage_ok() ? opendir(MIDI_DIR) : NULL;
    if (d) {
        struct dirent *e;
        int first = n;
        while ((e = readdir(d)) && n < PICK_MAX) {
            size_t l = strlen(e->d_name);
            if (e->d_name[0] != '.' && l > 4 && !strcasecmp(e->d_name + l - 4, ".sf2") && l + sizeof(MIDI_DIR) + 1 < 256)
                snprintf(pick_list[n++], 256, "%s/%s", MIDI_DIR, e->d_name);
        }
        closedir(d);
        qsort(pick_list + first, (size_t)(n - first), sizeof(*pick_list), cmp_name);
    }
    pick_count = n;
    pick_scroll = 0;
    pick_sf2 = true;
}

/* How an Instruments value is shown. */
static const char *sf2_label(const char *v) {
    if (!v[0]) return "Automatic (first .sf2 in the midi folder)";
    if (!strcmp(v, "builtin")) return "Built-in instruments";
    const char *b = strrchr(v, '/');
    return b ? b + 1 : v;
}

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
    bool needs_confirm = (d->min == SA_CLEAR_BOOKMARKS || d->min == SA_RESET_ALL || d->min == SA_CLEAR_COOKIES ||
                          d->min == SA_MOVE_DATA);
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
    case SA_CLEAR_COOKIES: cookies_clear(); ui_toast("Signed out of all sites"); break;
    case SA_CLEAR_PIC_CACHE: {
        if (!storage_ok()) { ui_toast("No SD card found"); break; }
        unsigned long long freed = picache_clear();
        char msg[96];
        snprintf(msg, sizeof(msg), "Cache cleared (%.1f MB freed)", (double)freed / (1024.0 * 1024.0));
        ui_toast(msg);
        break;
    }
    case SA_FIND_DOL:
        if (!storage_ok()) { ui_toast("No SD card found"); break; }
        if (!pick_list) pick_list = malloc(sizeof(*pick_list) * PICK_MAX);
        if (!pick_list) break;
        pick_count = dol_find("sd:/", pick_list, PICK_MAX);
        pick_scroll = 0;
        pick_sf2 = false;
        if (!pick_count) { ui_toast("No .dol files found on the SD card"); pick_count = -1; }
        break;
    case SA_MOVE_DATA: {
        if (!storage_ok()) { ui_toast("No SD card found"); break; }
        /* save first so the newest settings, bookmarks and cookies go along */
        settings_save();
        cookies_save_if_needed();
        char old[300], msg[300];
        snprintf(old, sizeof(old), "%s/", storage_dir());
        bool to_root = !storage_dir_is_root();
        picache_clear();   /* saved pictures aren't worth moving; they come back as you browse */
        if (storage_move_data(to_root, msg, sizeof(msg))) {
            /* a certificate file inside the old folder moved with it */
            if (!strncmp(g_set.ca_path, old, strlen(old))) {
                char name[128];
                snprintf(name, sizeof(name), "%s", g_set.ca_path + strlen(old));
                storage_path(g_set.ca_path, sizeof(g_set.ca_path), name);
            }
            settings_save();
            dirty = false;
            sound_play(SFX_SELECT);
        } else {
            sound_play(SFX_ERROR);
        }
        ui_toast(msg);
        break;
    }
    case SA_OPEN_MUSIC:
        browser_open_music();
        leave_now = true;
        break;
    case SA_CHECK_SWISS:
        browser_navigate("about:swiss");
        leave_now = true;
        break;
    case SA_CHECK_UPDATE:
        update_request_check(false);
        browser_navigate("about:update");
        leave_now = true;
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

/* Draws text inside maxw: shrinks it a little first (down to 13 px), then
 * cuts it short with "..." if it still doesn't fit. Needed in 4:3, where
 * the option column is narrow. cx is the centre when centered, else the left. */
static void text_fit(float x, float mid_y, float maxw, float size, bool centered, Color c, const char *s) {
    if (maxw < 10) return;
    float w = gfx_text_width(FONT_UI, size, s, -1);
    if (w > maxw) {
        size = fmaxf(13.0f, size * maxw / w);
        w = gfx_text_width(FONT_UI, size, s, -1);
    }
    char cut[160];
    if (w > maxw) {
        float dots = gfx_text_width(FONT_UI, size, "...", -1);
        int n = gfx_text_fit(FONT_UI, size, s, -1, maxw - dots);
        if (n > (int)sizeof(cut) - 4) n = (int)sizeof(cut) - 4;
        while (n > 0 && s[n - 1] == ' ') n--;
        memcpy(cut, s, (size_t)n);
        strcpy(cut + n, "...");
        s = cut;
        w = gfx_text_width(FONT_UI, size, s, -1);
    }
    float y = mid_y - gfx_line_height(FONT_UI, size) * 0.5f;
    gfx_text(FONT_UI, size, centered ? x - w * 0.5f : x, y, c, s, -1);
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
    snprintf(info, sizeof(info), "v%s  |  %s  |  SD: %s", CB_VERSION,
             netio_state() == NET_UP ? netio_ip() : "offline", storage_ok() ? storage_device_name() : "none");
    float ts = 14;
    float iw = gfx_text_width(FONT_UI, ts, info, -1);
    gfx_text(FONT_UI, ts, x + w - iw, y, t->text_dim, info, -1);
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
    gfx_text(FONT_UI, 22, px + 24, py + 14, t->text, pick_sf2 ? "Choose the instruments" : "Choose the Swiss program", -1);
    const int visible = 6;
    float row_h = 40, ly = py + 56;
    int dir = ui_dpad_unhandled();
    static float scroll_acc;
    pick_scroll += ui_scroll_rows(&scroll_acc, row_h + 6);
    if (dir == DIR_DOWN) pick_scroll++;
    if (dir == DIR_UP) pick_scroll--;
    if (pick_scroll > pick_count - visible) pick_scroll = pick_count - visible;
    if (pick_scroll < 0) pick_scroll = 0;
    for (int i = pick_scroll; i < pick_count && i < pick_scroll + visible; i++) {
        const char *shown = pick_sf2 ? sf2_label(pick_list[i]) : pick_list[i] + (strncmp(pick_list[i], "sd:", 3) ? 0 : 3);
        bool current = !strcmp(pick_list[i], pick_sf2 ? g_set.music_sf2 : g_set.swiss_dol);
        if (ui_button(ui_id("pick", i), px + 20, ly + (i - pick_scroll) * (row_h + 6), pw - 40, row_h, shown, 16,
                      current ? UIB_ACCENT : UIB_FLAT)) {
            if (pick_sf2) {
                if (strcmp(g_set.music_sf2, pick_list[i])) {
                    snprintf(g_set.music_sf2, sizeof(g_set.music_sf2), "%s", pick_list[i]);
                    dirty = true;
                    midi_instruments_changed();
                    ui_toast(midi_state() == MIDI_STOPPED ? "These instruments will be used for the next song"
                                                          : "Loading the new instruments...");
                }
                pick_count = -1;
                return;
            }
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
    float cat_w = gfx_width() < 700 ? 148 : 180;   /* narrower in 4:3 so option values fit */
    float bh = CAT_COUNT > 7 ? 35 : 38, gap = CAT_COUNT > 7 ? 5 : 6;   /* room for every section */
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
        if (g_setdefs[i].cat == category && g_setdefs[i].label) idxs[n++] = i;
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
    float ctrl_w = fminf(250, ow * 0.48f);
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
        float mid = y + row_h * 0.5f;
        float label_w = ctrl_x - (ox + 20) - 12;     /* room left of the control */
        UiId rid = ui_id("row", i);
        switch (d->type) {
        case ST_BOOL:
            text_fit(ox + 20, mid, label_w + ctrl_w - 56, 17, false, t->text, d->label);
            if (ui_click(rid, ox + 8, y + 2, ow - 16, row_h - 4)) {
                *setdef_int(d) = !*setdef_int(d);
                sound_play(SFX_SELECT);
                apply_one(d);
            }
            draw_toggle(rid ^ 0x1234u, ctrl_x + ctrl_w - 56, y + 9, *setdef_int(d) != 0);
            break;
        case ST_INT:
        case ST_CHOICE: {
            text_fit(ox + 20, mid, label_w, 17, false, t->text, d->label);
            float cy = y + row_h * 0.5f;
            if (ui_icon_button(ui_id("dec", i), ctrl_x + 14, cy, 13, ICON_LEFT, true)) step_value(d, -1);
            if (ui_icon_button(ui_id("inc", i), ctrl_x + ctrl_w - 14, cy, 13, ICON_RIGHT, true)) step_value(d, +1);
            /* between the two arrow buttons */
            text_fit(ctrl_x + ctrl_w * 0.5f, mid - 3, ctrl_w - 56, 17, true, t->text, value);
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
            text_fit(ox + 20, mid, label_w, 17, false, t->text, d->label);
            bool hov = ui_hover(rid, ox + 8, y + 2, ow - 16, row_h - 4);
            gfx_rrect(ctrl_x, y + 8, ctrl_w, row_h - 16, 9, t->field_bg);
            gfx_rrect_outline(ctrl_x, y + 8, ctrl_w, row_h - 16, 9, hov ? 2.0f : 1.0f, hov ? t->accent : t->panel_edge);
            bool is_sf2 = d->off == offsetof(Settings, music_sf2);
            const char *vshow = is_sf2 ? sf2_label(g_set.music_sf2) : value;
            int vn = gfx_text_fit(FONT_BODY, 16, vshow, -1, ctrl_w - 20);
            gfx_text(FONT_BODY, 16, ctrl_x + 10, y + 13, t->field_text, vshow, vn);
            if (hov && input_ok && visible && input_pressed(ACT_CLICK) && is_sf2) {
                input_consume(ACT_CLICK);
                sound_play(SFX_SELECT);
                open_sf2_picker();
            } else if (hov && input_ok && visible && input_pressed(ACT_CLICK)) {
                input_consume(ACT_CLICK);
                sound_play(SFX_SELECT);
                int mode = d->min == OSK_TEXTMODE_IP ? OSK_IP : (d->min == OSK_TEXTMODE_URL ? OSK_URL : OSK_TEXT);
                osk_open(d->label, setdef_str(d), d->max - 1, mode, str_done, (void *)(intptr_t)i);
            }
            if (hov) help = d->help;
            break;
        }
        case ST_BIND: {
            text_fit(ox + 20, mid, label_w, 17, false, t->text, d->label);
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
            if (d->min == SA_MOVE_DATA && confirm_action != i)
                lbl = storage_dir_is_root() ? "Move data folder next to CubeBrowser" : "Move data folder to SD card root";
            if (ui_button(rid, ox + 20, y + 6, ow - 40, row_h - 12, lbl, 17,
                          confirm_action == i ? UIB_ACCENT : UIB_NORMAL))
                run_action(i);
            break;
        }
        }
        if (!help && ui_input() && input_cursor_y() >= y && input_cursor_y() < y + row_h &&
            input_cursor_x() >= ox && input_cursor_x() < ox + ow) {
            help = d->help;
            if (d->type == ST_ACTION && d->min == SA_MOVE_DATA) {
                static char where[360];
                snprintf(where, sizeof(where), "Settings, bookmarks and cookies are in %s. Press twice to confirm.",
                         storage_ok() ? storage_dir() : "(no SD card)");
                help = where;
            }
        }
    }
    gfx_clear_clip();
    ui_set_input(input_ok);

    /* scroll indicator */
    if (content_h > rows_h) {
        float bh2 = fmaxf(24, rows_h * rows_h / content_h);
        float by2 = rows_y + (rows_h - bh2) * (scroll / (content_h - rows_h));
        gfx_rrect(ox + ow - 8, by2, 4, bh2, 2, t->accent);
    }

    /* help line */
    float hy = oy + oh - help_h;
    gfx_rect(ox + 14, hy - 2, ow - 28, 1.5f, color_alpha(t->panel_edge, 0.6f));
    if (help) {
        /* Too long for the line: wait a moment, glide along to show the rest,
         * pause at the end, then start over. Restarts for each new item. */
        static const char *shown_help;
        static float help_since;
        if (help != shown_help) { shown_help = help; help_since = ui_time(); }
        float avail = ow - 40, tw = gfx_text_width(FONT_UI, 15, help, -1);
        if (tw <= avail) {
            gfx_text(FONT_UI, 15, ox + 20, hy + 10, t->text_dim, help, -1);
        } else {
            const float pause = 1.5f, speed = 45.0f;   /* seconds, units per second */
            float over = tw - avail + 8, run = over / speed;
            float k = fmodf(ui_time() - help_since, pause + run + pause);
            float off = k < pause ? 0 : (k < pause + run ? (k - pause) * speed : over);
            gfx_set_clip(ox + 20, hy, avail, help_h);
            gfx_text(FONT_UI, 15, ox + 20 - off, hy + 10, t->text_dim, help, -1);
            gfx_clear_clip();
        }
    }

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
    } else if (input_ok && (input_pressed(ACT_BACK) || input_pressed(ACT_MENU))) {
        input_consume(ACT_BACK);
        input_consume(ACT_MENU);
        leave = true;
    }

    if (pick_count >= 0) draw_picker(interactive && !osk_active());
    if (leave_now) { leave_now = false; leave = true; }

    if (leave) {
        capture = -1;
        cookies_save_if_needed();
        if (dirty) {
            if (settings_save()) ui_toast("Settings saved");
            else ui_toast("Couldn't save settings - is the SD card inserted?");
            dirty = false;
        }
        sound_play(SFX_BACK);
    }
    return leave;
}
