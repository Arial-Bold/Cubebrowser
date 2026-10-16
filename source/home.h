/* home.h - the start page (about:home), drawn with the menu toolkit rather
 * than as a web page: featured-site tiles, a search box, quick buttons and a
 * rotating tip, in the style of early-2000s console menus. */
#pragma once
#include <stdbool.h>

typedef enum { HOME_NONE, HOME_OPEN_URL, HOME_SEARCH, HOME_BOOKMARKS, HOME_HELP, HOME_SETTINGS } HomeAction;

/* Call when the start page is (re)opened: replays the intro animation. */
void home_reset(void);

/* Draws the start page into the content area and handles its input.
 * *url is set for HOME_OPEN_URL; *hover_url to the pointed-at site (or NULL). */
HomeAction home_draw(float x, float y, float w, float h, bool input_ok, const char **url, const char **hover_url);

/* Controls & Tips (about:help) in the same style. Returns HOME_OPEN_URL when
 * "Back to start page" is chosen. Scrolls by itself. */
void home_help_reset(void);
HomeAction home_help_draw(float x, float y, float w, float h, bool input_ok);
