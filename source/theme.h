/* theme.h - colour schemes named after the console's own colours. */
#pragma once
#include "gfx.h"

typedef struct {
    const char *name;
    Color bg_top, bg_bottom;        /* backdrop gradient */
    Color bg_cube;                  /* drifting background cubes */
    Color panel_top, panel_bottom;  /* chrome panels */
    Color panel_edge;
    Color button_top, button_bottom;
    Color accent, accent_hi;        /* focus glow and selected state */
    Color text, text_dim, text_on_accent;
    Color field_bg, field_text;
    Color page_bg, page_text, page_dim, page_link, page_link_hover, page_heading, page_rule;
    Color cursor_fill, cursor_edge;
    Color shadow;
    Color good, bad;
} Theme;

enum { THEME_INDIGO, THEME_JET, THEME_PLATINUM, THEME_SPICE, THEME_COUNT };

extern const char *const theme_names[THEME_COUNT];
const Theme *theme(void);
void theme_set(int idx);
