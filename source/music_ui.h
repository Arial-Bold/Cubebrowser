/* music_ui.h - the music player panel opened from the toolbar's note button. */
#pragma once
#include <stdbool.h>

void music_ui_reset(void);              /* call when opening: re-reads the song list */
bool music_ui_draw(bool interactive);   /* returns false when it should close */
