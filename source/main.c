/* CubeBrowser - a small HTTPS web browser for the Nintendo GameCube.
 * main.c - start-up and the 60 fps main loop with cube-turn screen transitions. */
#include <gccore.h>
#include <math.h>
#include <stdio.h>
#include "gfx.h"
#include "theme.h"
#include "input.h"
#include "settings.h"
#include "settings_ui.h"
#include "storage.h"
#include "gc_entropy.h"
#include "sound.h"
#include "fetch.h"
#include "ui.h"
#include "osk.h"
#include "browser.h"
#include "diag.h"

enum { SCREEN_BROWSER, SCREEN_SETTINGS };

static int screen = SCREEN_BROWSER;
static int target_screen = SCREEN_BROWSER;
static float trans = 1.0f;   /* 1 = finished */

static void draw_screen(int which, bool interactive) {
    if (which == SCREEN_BROWSER) {
        browser_frame(interactive);
    } else if (settings_ui_frame(interactive) && interactive) {
        target_screen = SCREEN_BROWSER;
        trans = g_set.animations ? 0.0f : 1.0f;
        if (!g_set.animations) screen = SCREEN_BROWSER;
    }
}

/* Draws two screens as adjacent faces of a turning cube. */
static void draw_transition(int from, int to, float p) {
    float e = p * p * (3 - 2 * p);   /* smoothstep */
    float th = e * (float)M_PI * 0.5f;
    float c = cosf(th), s = sinf(th);
    float k = 1.0f / (c + s);
    float W = gfx_width();
    float ty = 240.0f * (1.0f - k);
    bool forward = (to == SCREEN_SETTINGS);
    float out_sx = k * c, in_sx = k * s;
    float out_tx = forward ? 0 : W * k * s;
    float in_tx = forward ? W * k * c : 0;
    const Theme *t = theme();

    gfx_set_transform(out_sx, k, out_tx, ty);
    draw_screen(from, false);
    gfx_rect(0, 0, W, 480, color_alpha(t->shadow, s * 0.6f));
    gfx_set_transform(in_sx, k, in_tx, ty);
    draw_screen(to, false);
    gfx_rect(0, 0, W, 480, color_alpha(t->shadow, c * 0.6f));
    gfx_reset_transform();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    bool sd = storage_init();
    diag_init();
    settings_load();
    gfx_init(g_set.video_mode, g_set.widescreen != 0);
    input_init();

    /* Hold L + R + START while starting to reset all settings */
    bool reset = false;
    for (int i = 0; i < 6; i++) {
        VIDEO_WaitVSync();
        PAD_ScanPads();
        u32 h = PAD_ButtonsHeld(0);
        if ((h & PAD_TRIGGER_L) && (h & PAD_TRIGGER_R) && (h & PAD_BUTTON_START)) reset = true;
    }
    if (reset) {
        settings_defaults(&g_set);
        settings_save();
    }
    theme_set(g_set.theme);
    entropy_init();
    sound_init();
    fetch_init();
    browser_init();

    if (reset) ui_toast("Settings were reset to defaults");
    else if (!sd) ui_toast("No SD card found - settings and bookmarks won't be saved");

    for (;;) {
        diag_frame();
        diag_note(DIAG_MAIN, "reading controller");
        input_update();
        ui_begin_frame();
        gfx_begin_frame();
        ui_draw_background();

        diag_note(DIAG_MAIN, "screen %d", screen);
        if (trans < 1.0f) {
            draw_transition(screen, target_screen, trans);
            trans += 1.0f / 22.0f;
            if (trans >= 1.0f) { trans = 1.0f; screen = target_screen; }
        } else {
            draw_screen(screen, true);
            if (screen == SCREEN_BROWSER && browser_take_settings_request()) {
                settings_ui_open();
                sound_play(SFX_OPEN);
                target_screen = SCREEN_SETTINGS;
                if (g_set.animations) trans = 0.0f;
                else screen = SCREEN_SETTINGS;
            }
        }

        diag_note(DIAG_MAIN, "drawing keyboard and pointer");
        osk_frame();
        ui_draw_toast();
        ui_draw_cursor();
        ui_end_frame();
        diag_note(DIAG_MAIN, "waiting for the GPU to finish the frame");
        gfx_end_frame();
    }
    return 0;
}
