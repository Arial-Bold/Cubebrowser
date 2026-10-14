/* settings_ui.h - the in-program Settings screen. */
#pragma once
#include <stdbool.h>

void settings_ui_open(void);
bool settings_ui_frame(bool interactive);   /* returns true when the user leaves */
void settings_apply_all(void);              /* push g_set into theme/video/etc. */
