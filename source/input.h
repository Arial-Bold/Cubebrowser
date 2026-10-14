/* input.h - GameCube controller: pointer physics, remappable actions, rumble. */
#pragma once
#include <gccore.h>
#include <stdbool.h>

typedef enum {
    ACT_CLICK, ACT_BACK, ACT_ADDRESS, ACT_BOOKMARKS, ACT_MENU, ACT_SLOW, ACT_PAGE_UP, ACT_PAGE_DOWN,
    ACT_COUNT
} Action;

/* Buttons that may be bound to actions (the D-pad is reserved for snapping). */
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_Z, BTN_L, BTN_R, BTN_START, BTN_COUNT };

extern const char *const action_names[ACT_COUNT];
extern const char *const button_names[BTN_COUNT];
extern const u32 button_masks[BTN_COUNT];

enum { DIR_NONE = -1, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

void  input_init(void);
void  input_update(void);          /* call once per frame */
bool  input_pressed(Action a);
bool  input_held(Action a);
void  input_consume(Action a);     /* swallow a press so later code ignores it */
u32   input_raw_pressed(void);     /* PAD_BUTTON_* masks */
u32   input_raw_held(void);
bool  input_raw_repeat(u32 mask);  /* pressed, or held long enough to auto-repeat */
int   input_dpad(void);            /* DIR_* with auto-repeat */
int   input_pressed_bindable(void);/* BTN_* pressed this frame, or -1 */

float input_cursor_x(void);
float input_cursor_y(void);
void  input_set_cursor(float x, float y);
void  input_glide_cursor(float x, float y);   /* animated move */
void  input_clamp_cursor(void);
bool  input_stick_idle(int frames);
float input_scroll_delta(void);    /* C-stick + page actions, in virtual pixels */
bool  input_pointer_moved(void);

void  input_rumble(int frames);
void  input_stop_rumble(void);
