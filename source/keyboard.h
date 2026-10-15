/* keyboard.h - GameCube ASCII Keyboard Controller support. This is also what
 * BlueRetro presents when a Bluetooth keyboard is paired, so any wireless
 * keyboard works through it. The keyboard is found automatically on any
 * controller port and re-checked every second (plugging in or pairing later
 * works). Keys are delivered as events. */
#pragma once
#include <stdbool.h>

enum {
    KEY_NONE = 0,
    /* printable characters are delivered as their ASCII value (32..126) */
    KEY_ENTER = 256, KEY_BACKSPACE, KEY_TAB, KEY_ESC, KEY_DELETE, KEY_INSERT,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12
};

typedef struct {
    int key;
    bool ctrl, shift, alt;
} KeyEvent;

void kbd_update(void);                 /* once per frame */
bool kbd_connected(void);
int  kbd_port(void);                   /* 1..4, or 0 */
bool kbd_next(KeyEvent *ev);           /* pops the next key press, false if none */
void kbd_clear(void);                  /* drop queued presses */
int  kbd_last_code(void);              /* raw code of the last key, for troubleshooting */
void kbd_describe_ports(char *out, int n);   /* what each controller port reports */
void kbd_stats(char *out, int n);            /* read counters and raw data, for troubleshooting */

/* Turns raw keyboard codes into an event. Exposed for testing. */
int  kbd_translate(int code, bool shift, bool japanese_layout);
