/* gckbd.h - the GameCube keyboard controller (the ASCII keyboard made for
 * Phantasy Star Online), plugged into any controller port.
 *
 * Once a frame the port is read: it reports up to three keys held down.
 * New presses (and held keys, after a moment, repeating) become events for
 * the on-screen keyboard and the browser. Nothing is sent to a port unless
 * a keyboard answers there, so ordinary controllers are never affected. */
#pragma once
#include <stdbool.h>

typedef enum {
    KB_CHAR,                       /* ch holds the character */
    KB_BACKSPACE, KB_DELETE, KB_ENTER, KB_ESC, KB_TAB,
    KB_LEFT, KB_RIGHT, KB_UP, KB_DOWN, KB_HOME, KB_END, KB_PGUP, KB_PGDN,
    KB_F5,
} KbKey;

typedef struct { KbKey key; char ch; } KbEvent;

void kb_update(void);              /* once a frame, after input_update() */
bool kb_next(KbEvent *e);          /* the next key this frame; false when there are no more */
void kb_clear(void);               /* drop this frame's keys (once they've been used) */
bool kb_connected(void);           /* a keyboard is plugged in (and turned on in Settings) */

#ifndef GEKKO
void kb_pc_type(const char *s);    /* PC tests: type a string (\b backspace, \n enter) */
#endif
