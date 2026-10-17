/* browser.h - the main browsing screen: toolbar, address bar, page view,
 * status bar, history and bookmarks. */
#pragma once
#include <stdbool.h>

void browser_init(void);
void browser_frame(bool interactive);   /* update + draw */
bool browser_take_settings_request(void);
void browser_navigate(const char *url);
void browser_clear_history(void);
void browser_clear_bookmarks(void);
void browser_open_music(void);   /* shows the music player panel */
