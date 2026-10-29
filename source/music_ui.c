/* music_ui.c - the music player panel: what's playing, the controls and the
 * song list. The music keeps playing after the panel is closed. */
#include "music_ui.h"
#include "midi.h"
#include "ui.h"
#include "gfx.h"
#include "theme.h"
#include "input.h"
#include "settings.h"
#include "sound.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int list_scroll;
static bool scrolled_to_current;
static int saved_vol, saved_shuf, saved_rep;

void music_ui_reset(void) {
    midi_scan();
    saved_vol = g_set.music_volume;
    saved_shuf = g_set.music_shuffle;
    saved_rep = g_set.music_repeat;
    list_scroll = 0;
    scrolled_to_current = false;
}

static void fmt_time(char *out, size_t cap, float s) {
    int t = (int)s;
    if (t < 0) t = 0;
    snprintf(out, cap, "%d:%02d", t / 60, t % 60);
}

/* An icon button that shows a highlight ring when its option is on. */
/* Shuffle / repeat: when on, the button lights up in the accent colour like
 * the toolbar's note button does while music plays, but steady (no pulse). */
static bool toggle_button(const char *id, float cx, float cy, int icon, bool on) {
    const Theme *t = theme();
    const float r = 15;
    if (t->system) {   /* System: the glass button turns gold */
        bool hit = ui_icon_button(ui_id(id, 0), cx, cy, r, icon, true);
        if (on) {
            float w = r * 1.7f, h = r * 1.45f;
            gfx_rrect_vgrad(cx - w * 0.5f, cy - h * 0.5f, w, h, 2, rgba(255, 248, 178, 240), rgba(222, 192, 70, 240));
            gfx_rect(cx - w * 0.5f + 2, cy - h * 0.5f + 1, w - 4, 1, rgba(255, 255, 230, 204));
            ui_draw_icon(icon, cx, cy, r * 0.95f, t->text_on_accent);
        }
        return hit;
    }
    if (on)   /* soft light around it */
        for (int k = 3; k >= 1; k--)
            gfx_circle(cx, cy, r + 2 + k * 2.5f, color_alpha(t->accent, 0.2f / k));
    bool hit = ui_icon_button(ui_id(id, 0), cx, cy, r, icon, true);
    if (on) {   /* the face itself lit, with a white symbol */
        gfx_circle(cx, cy, r - 1, color_alpha(t->accent, 0.8f));
        gfx_circle(cx, cy - r * 0.35f, r * 0.5f, rgba(255, 255, 255, 55));
        ui_draw_icon(icon, cx, cy, r * 1.1f, rgba(255, 255, 255, 255));
    }
    return hit;   /* off: a plain button like the others */
}

bool music_ui_draw(bool interactive) {
    const Theme *t = theme();
    float sx, sy, sw, sh;
    ui_safe_area(&sx, &sy, &sw, &sh);
    float open = ui_anim(ui_id("mus-open", 0), 1.0f, 0.25f);
    gfx_rect(0, 0, gfx_width(), 480, color_alpha(t->shadow, 0.6f * open));
    float pw = fminf(580, sw - 20), ph = fminf(410, sh - 10);
    float px = sx + (sw - pw) * 0.5f, py = sy + (sh - ph) * 0.5f + (1 - open) * 60;
    ui_set_input(interactive);
    ui_panel(px, py, pw, ph, 20);
    bool close = false;

    /* header */
    ui_draw_icon(ICON_MUSIC, px + 32, py + 28, 26, t->accent);
    gfx_text(FONT_UI, 24, px + 54, py + 12, t->text, "Music Player", -1);
    if (ui_icon_button(ui_id("mus-close", 0), px + pw - 30, py + 28, 16, ICON_CLOSE, true)) close = true;

    /* now playing */
    MidiState st = midi_state();
    float bx = px + 18, bw = pw - 36, by = py + 54, bh = 92;
    if (t->system) ui_field_frame(bx, by, bw, bh);
    else {
        gfx_rrect(bx, by, bw, bh, 12, t->field_bg);
        gfx_rrect_outline(bx, by, bw, bh, 12, 1.2f, t->panel_edge);
    }
    const char *ttl = midi_title();
    const char *status = st == MIDI_LOADING ? "Loading instruments..." : st == MIDI_PAUSED ? "Paused" :
                         st == MIDI_PLAYING ? "Now playing" : "Stopped";
    gfx_text(FONT_UI, 14, bx + 14, by + 8, color_alpha(t->field_text, 0.65f), status, -1);
    const char *show = ttl[0] ? ttl : (midi_count() ? "Choose a song below" : "No songs yet");
    int n = gfx_text_fit(FONT_UI, 20, show, -1, bw - 28);
    gfx_text(FONT_UI, 20, bx + 14, by + 26, t->field_text, show, n);
    char info[160];
    snprintf(info, sizeof(info), "Instruments: %s", midi_instruments());
    n = gfx_text_fit(FONT_BODY, 14, info, -1, bw - 120);
    gfx_text(FONT_BODY, 14, bx + 14, by + 52, color_alpha(t->field_text, 0.65f), info, n);
    /* time and progress */
    float len = midi_length(), pos = midi_position();
    if (st == MIDI_STOPPED) pos = 0;
    if (pos > len) pos = len;
    char a[16], b[16], tm[40];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), len);
    snprintf(tm, sizeof(tm), "%s / %s", a, b);
    float tw = gfx_text_width(FONT_UI, 14, tm, -1);
    gfx_text(FONT_UI, 14, bx + bw - 14 - tw, by + 52, color_alpha(t->field_text, 0.65f), tm, -1);
    float gx = bx + 14, gw = bw - 28, gy = by + bh - 16;
    gfx_rrect(gx, gy, gw, 6, ui_r(3), color_alpha(t->panel_edge, 0.5f));
    if (len > 0) gfx_rrect(gx, gy, fmaxf(6, gw * pos / len), 6, ui_r(3), t->accent);

    /* controls */
    float cy = by + bh + 34, mid = px + pw * 0.5f;
    if (toggle_button("mus-shuf", bx + 22, cy, ICON_SHUFFLE, midi_shuffle())) {
        midi_set_shuffle(!midi_shuffle());
        ui_toast(midi_shuffle() ? "Shuffle on" : "Shuffle off");
    }
    if (ui_icon_button(ui_id("mus-prev", 0), mid - 70, cy, 18, ICON_PREV, midi_count() > 0)) midi_prev();
    int play_icon = st == MIDI_PLAYING ? ICON_PAUSE : ICON_PLAY;
    if (ui_icon_button(ui_id("mus-play", 0), mid, cy, 24, play_icon, midi_count() > 0)) midi_toggle_pause();
    if (ui_icon_button(ui_id("mus-next", 0), mid + 70, cy, 18, ICON_NEXT, midi_count() > 0)) midi_next();
    if (toggle_button("mus-rep", bx + 70, cy, ICON_REPEAT, midi_repeat())) {
        midi_set_repeat(!midi_repeat());
        ui_toast(midi_repeat() ? "Repeat on: the list starts over at the end" : "Repeat off");
    }
    if (ui_icon_button(ui_id("mus-stop", 0), mid - 128, cy, 14, ICON_HALT, st != MIDI_STOPPED) && st != MIDI_STOPPED) midi_stop();
    /* volume */
    float vx = bx + bw - 120;
    if (ui_icon_button(ui_id("mus-vdn", 0), vx, cy, 12, ICON_LEFT, g_set.music_volume > 0)) g_set.music_volume--;
    if (ui_icon_button(ui_id("mus-vup", 0), vx + 100, cy, 12, ICON_RIGHT, g_set.music_volume < 10)) g_set.music_volume++;
    float vbx = vx + 18, vbw = 64;
    gfx_rrect(vbx, cy - 3, vbw, 6, ui_r(3), color_alpha(t->panel_edge, 0.6f));
    gfx_rrect(vbx, cy - 3, fmaxf(6, vbw * g_set.music_volume / 10.0f), 6, ui_r(3), t->accent);
    gfx_text_centered(FONT_UI, 12, vbx + vbw * 0.5f, cy + 7, t->text_dim, "VOLUME");

    /* song list */
    float ly = cy + 34, row_h = 32, gap = 4;
    const char *msg = midi_message();
    float list_bottom = py + ph - (msg[0] ? 40 : 16);
    int visible = (int)((list_bottom - ly) / (row_h + gap));
    if (visible < 1) visible = 1;
    int count = midi_count(), cur = midi_current();
    if (!scrolled_to_current && cur >= 0) {
        list_scroll = cur - visible / 2;
        scrolled_to_current = true;
    }
    if (count == 0) {
        gfx_text_centered(FONT_UI, 16, mid, ly + 10, t->text_dim, "Put .mid files in a folder called midi");
        gfx_text_centered(FONT_UI, 16, mid, ly + 32, t->text_dim, "on your SD card, then open this again.");
        gfx_text_centered(FONT_UI, 14, mid, ly + 62, t->text_dim, "A SoundFont (.sf2) there gives real instruments.");
    } else {
        int dir = ui_dpad_unhandled();
        static float scroll_acc;
        list_scroll += ui_scroll_rows(&scroll_acc, row_h + gap);
        if (dir == DIR_DOWN) list_scroll++;
        if (dir == DIR_UP) list_scroll--;
        if (list_scroll > count - visible) list_scroll = count - visible;
        if (list_scroll < 0) list_scroll = 0;
        for (int i = list_scroll; i < count && i < list_scroll + visible; i++) {
            float y = ly + (i - list_scroll) * (row_h + gap);
            char label[160];
            snprintf(label, sizeof(label), "%s", midi_file(i));
            char *dot = strrchr(label, '.');
            if (dot && dot != label) *dot = 0;
            if (ui_button(ui_id("mus-row", i), bx, y, bw - (count > visible ? 14 : 0), row_h, label, 15,
                          i == cur ? UIB_ACCENT : UIB_FLAT)) {
                sound_play(SFX_SELECT);
                midi_play(i);
            }
        }
        if (count > visible) {   /* scroll bar */
            float th = visible * (row_h + gap) - gap;
            float kh = fmaxf(16, th * visible / count), ky = ly + (th - kh) * list_scroll / (count - visible);
            gfx_rrect(bx + bw - 6, ly, 5, th, ui_r(2.5f), color_alpha(t->panel_edge, 0.5f));
            gfx_rrect(bx + bw - 6, ky, 5, kh, ui_r(2.5f), t->accent);
        }
    }
    if (msg[0]) {
        int mn = gfx_text_fit(FONT_BODY, 13, msg, -1, pw - 36);
        gfx_text(FONT_BODY, 13, px + 18, py + ph - 30, t->bad, msg, mn);
    }
    if (interactive && input_pressed(ACT_BACK)) {
        input_consume(ACT_BACK);
        close = true;
    }
    if (close) {
        sound_play(SFX_CLOSE);
        /* keep volume, shuffle and repeat for next time */
        if (saved_vol != g_set.music_volume || saved_shuf != g_set.music_shuffle || saved_rep != g_set.music_repeat)
            settings_save();
    }
    return !close;
}
