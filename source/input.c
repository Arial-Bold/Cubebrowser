/* input.c - controller handling. The main stick drives a pointer with a dead
 * zone and an acceleration curve; actions are remappable via settings. */
#include "input.h"
#include "settings.h"
#include "gfx.h"
#include "gc_entropy.h"
#include "keyboard.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const action_names[ACT_COUNT] = {
    "Click", "Back", "Address bar", "Bookmarks", "Settings menu", "Slow pointer", "Page up", "Page down"
};
const char *const button_names[BTN_COUNT] = { "A", "B", "X", "Y", "Z", "L", "R", "START" };
const u32 button_masks[BTN_COUNT] = {
    PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y,
    PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_BUTTON_START
};

static u32 down, held, prev_held, consumed;
static float cx = 320, cy = 240, glide_x, glide_y;
static bool gliding, moved;
static int idle_frames;
static int rumble_left;
static int dpad_dir = DIR_NONE, dpad_timer;
static u32 repeat_mask;
static int repeat_timer;
static s8 last_sx, last_sy;
static int active_chan;

void input_init(void) {
    PAD_Init();
    cx = gfx_width() * 0.5f;
    cy = 240;
}

static u32 action_mask(Action a) { return button_masks[g_set.bind[a]]; }

static float stick_axis(int v) {
    float dz = (float)g_set.deadzone;
    float a = fabsf((float)v);
    if (a <= dz) return 0;
    float n = (a - dz) / (96.0f - dz);
    if (n > 1) n = 1;
    float curve = 1.0f + g_set.accel * 0.22f;
    n = powf(n, curve);
    return v < 0 ? -n : n;
}

/* Controllers are read directly rather than with PAD_ScanPads(), because
 * ScanPads resets every port that doesn't hold a standard controller, every
 * frame. That keeps knocking a keyboard (or BlueRetro's keyboard) offline
 * before it can even be identified. Here, a keyboard's port is never reset,
 * and empty ports are only retried once a second. */
static PADStatus pads[4];
static u16 pad_prev[4];
static int scan_frames;

static void scan_pads(void) {
    PAD_Read(pads);   /* raw readings: same full stick range as before (no PAD_Clamp, which shrinks it) */
    scan_frames++;
    u32 reset = 0;
    for (int c = 0; c < 4; c++) {
        if (pads[c].err == PAD_ERR_NO_CONTROLLER) {
            if (c != kbd_port() - 1 && (scan_frames % 60) == 15) reset |= (PAD_CHAN0_BIT >> c);
            memset(&pads[c], 0, sizeof(pads[c]));
            pads[c].err = PAD_ERR_NO_CONTROLLER;
        } else if (pads[c].err != PAD_ERR_NONE) {
            /* not ready / transfer error: keep last frame's buttons */
            pads[c].button = pad_prev[c];
            pads[c].stickX = pads[c].stickY = pads[c].substickX = pads[c].substickY = 0;
        }
    }
    if (reset) PAD_Reset(reset);
}

void input_update(void) {
    scan_pads();
    kbd_update();
    prev_held = held;
    /* a controller on any port works (Bluetooth adapters may use any of them) */
    held = down = 0;
    s8 sx = 0, sy = 0;
    for (int c = 0; c < 4; c++) {
        u32 h = pads[c].button, d = h & ~pad_prev[c];
        pad_prev[c] = (u16)h;
        s8 x = pads[c].stickX, y = pads[c].stickY, cy2 = pads[c].substickY;
        if (h || abs(x) > 20 || abs(y) > 20 || abs(cy2) > 20) active_chan = c;
        held |= h;
        down |= d;
        if (abs(x) > abs(sx)) sx = x;
        if (abs(y) > abs(sy)) sy = y;
    }
    consumed = 0;

    if (down || sx != last_sx || sy != last_sy) entropy_add_event(((u32)down << 16) ^ ((u8)sx << 8) ^ (u8)sy);
    last_sx = sx; last_sy = sy;

    float ax = stick_axis(sx), ay = stick_axis(sy);
    moved = (ax != 0 || ay != 0);
    if (moved) {
        gliding = false;
        idle_frames = 0;
        float speed = 2.0f + g_set.cursor_speed * 1.1f;
        if (held & action_mask(ACT_SLOW)) speed *= 0.35f;
        cx += ax * speed;
        cy -= ay * speed;   /* stick up = positive Y */
    } else {
        idle_frames++;
    }
    if (gliding) {
        cx += (glide_x - cx) * 0.35f;
        cy += (glide_y - cy) * 0.35f;
        if (fabsf(glide_x - cx) < 0.5f && fabsf(glide_y - cy) < 0.5f) { cx = glide_x; cy = glide_y; gliding = false; }
    }
    input_clamp_cursor();

    /* D-pad with auto-repeat */
    int dir = DIR_NONE;
    if (held & PAD_BUTTON_UP) dir = DIR_UP;
    else if (held & PAD_BUTTON_DOWN) dir = DIR_DOWN;
    else if (held & PAD_BUTTON_LEFT) dir = DIR_LEFT;
    else if (held & PAD_BUTTON_RIGHT) dir = DIR_RIGHT;
    if (dir != dpad_dir) { dpad_dir = dir; dpad_timer = 0; }
    else if (dir != DIR_NONE) dpad_timer++;

    /* generic held-button repeat */
    if (held != prev_held) { repeat_mask = held; repeat_timer = 0; }
    else repeat_timer++;

    if (rumble_left > 0 && --rumble_left == 0) PAD_ControlMotor(active_chan, PAD_MOTOR_STOP);
}

bool input_pressed(Action a) { u32 m = action_mask(a); return (down & m) && !(consumed & m); }
bool input_held(Action a) { return held & action_mask(a); }
void input_consume(Action a) { consumed |= action_mask(a); }
u32 input_raw_pressed(void) { return down & ~consumed; }
u32 input_raw_held(void) { return held; }

static int repeat_period(void) { return 12 - g_set.key_repeat; }

bool input_raw_repeat(u32 mask) {
    if (down & mask) return true;
    if (!(held & mask) || !(repeat_mask & mask)) return false;
    int first = 20;
    return repeat_timer >= first && ((repeat_timer - first) % repeat_period()) == 0;
}

int input_dpad(void) {
    if (dpad_dir == DIR_NONE) return DIR_NONE;
    if (dpad_timer == 0) return dpad_dir;
    if (dpad_timer >= 18 && ((dpad_timer - 18) % 5) == 0) return dpad_dir;
    return DIR_NONE;
}

int input_pressed_bindable(void) {
    for (int b = 0; b < BTN_COUNT; b++)
        if (down & button_masks[b]) return b;
    return -1;
}

float input_cursor_x(void) { return cx; }
float input_cursor_y(void) { return cy; }
void input_set_cursor(float x, float y) { cx = x; cy = y; gliding = false; input_clamp_cursor(); }

void input_glide_cursor(float x, float y) {
    glide_x = x; glide_y = y; gliding = true;
    idle_frames = 0;
}

void input_clamp_cursor(void) {
    float w = gfx_width();
    if (cx < 0) cx = 0;
    if (cx > w - 1) cx = w - 1;
    if (cy < 0) cy = 0;
    if (cy > 479) cy = 479;
}

bool input_stick_idle(int frames) { return idle_frames >= frames; }
bool input_pointer_moved(void) { return moved; }

float input_scroll_delta(void) {
    float k = 1.5f + g_set.scroll_speed * 1.2f;
    s8 cy = 0;
    for (int c = 0; c < 4; c++) { s8 v = pads[c].substickY; if (abs(v) > abs(cy)) cy = v; }
    float d = -stick_axis(cy) * k * 1.6f;
    if (input_raw_repeat(action_mask(ACT_PAGE_DOWN))) d += 300;
    if (input_raw_repeat(action_mask(ACT_PAGE_UP))) d -= 300;
    return d;
}

void input_rumble(int frames) {
    if (!g_set.rumble) return;
    if (rumble_left <= 0) PAD_ControlMotor(active_chan, PAD_MOTOR_RUMBLE);
    if (frames > rumble_left) rumble_left = frames;
}

void input_stop_rumble(void) {
    rumble_left = 0;
    PAD_ControlMotor(active_chan, PAD_MOTOR_STOP);
}
