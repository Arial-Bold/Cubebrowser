/* ui.h - small immediate-mode UI toolkit in a GameCube-era style: soft
 * rounded panels, bouncy focus, glowing pointer, D-pad snapping between
 * widgets, sound and rumble feedback. */
#pragma once
#include "gfx.h"
#include <stdbool.h>

typedef unsigned int UiId;

enum {
    ICON_BACK, ICON_FORWARD, ICON_RELOAD, ICON_HOME, ICON_STAR, ICON_GEAR, ICON_STOP, ICON_LOCK,
    ICON_UNLOCK, ICON_CLOSE, ICON_PLUS, ICON_LEFT, ICON_RIGHT, ICON_POWER, ICON_EXIT, ICON_MUSIC,
    ICON_PLAY, ICON_PAUSE, ICON_PREV, ICON_NEXT, ICON_SHUFFLE, ICON_REPEAT, ICON_HALT, ICON_COUNT
};

enum { UIB_NORMAL = 0, UIB_ACCENT = 1, UIB_FLAT = 2 };

UiId  ui_id(const char *s, int n);
void  ui_begin_frame(void);
void  ui_end_frame(void);
void  ui_set_input(bool enabled);   /* disables hit testing for layers under a modal */
bool  ui_input(void);
int   ui_dpad_unhandled(void);      /* D-pad direction no widget could take, or DIR_NONE */
/* For lists that move a row at a time: rows to move this frame from the
 * C-Stick (and L/R), which give a few pixels per frame. acc keeps the
 * leftover between frames; step is one row's height in pixels. */
int   ui_scroll_rows(float *acc, float step);
float ui_time(void);
int   ui_frame(void);

/* Registers a hit target (for D-pad snapping) and returns true if the pointer is over it. */
bool  ui_hover(UiId id, float x, float y, float w, float h);
bool  ui_click(UiId id, float x, float y, float w, float h);   /* hover + Click action */
float ui_anim(UiId id, float target, float rate);              /* eased per-widget value */

bool  ui_button(UiId id, float x, float y, float w, float h, const char *label, float text_size, int style);
bool  ui_icon_button(UiId id, float cx, float cy, float r, int icon, bool enabled);

void  ui_panel(float x, float y, float w, float h, float r);
void  ui_draw_icon(int icon, float cx, float cy, float size, Color c);
void  ui_draw_background(void);
void  ui_draw_cursor(void);
void  ui_spinner(float cx, float cy, float size);
void  ui_safe_area(float *x, float *y, float *w, float *h);
void  ui_toast(const char *msg);    /* brief message bubble */
void  ui_draw_toast(void);
