/* osk.h - on-screen keyboard used for the address bar, page forms and settings. */
#pragma once
#include <stdbool.h>

enum { OSK_TEXT = 0, OSK_URL = 1, OSK_IP = 2, OSK_PASSWORD = 3 };

typedef void (*OskDone)(bool accepted, const char *text, void *user);

void osk_open(const char *title, const char *initial, int maxlen, int mode, OskDone done, void *user);
bool osk_active(void);
void osk_frame(void);    /* update + draw; call after drawing the screen underneath */
