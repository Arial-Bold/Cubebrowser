/* css.h - a practical CSS subset: parsing (with @media and var()), selector
 * matching (type, class, id, attribute, :first-child/:last-child/:not/:root,
 * descendant/child/sibling combinators), cascade with specificity and
 * !important, inheritance, and HTML presentational attributes. */
#pragma once
#include "gfx.h"
#include "dom.h"

#define CSS_VIEWPORT_W 480.0f   /* pretend phone-sized screen so sites use mobile layouts */
#define CSS_VIEWPORT_H 640.0f

enum { U_UNSET, U_AUTO, U_PX, U_EM, U_REM, U_PCT, U_VW, U_VH };
typedef struct { float v; unsigned char u; } Len;

enum { D_NONE, D_INLINE, D_BLOCK, D_LIST_ITEM, D_TABLE, D_ROW, D_CELL, D_ROWGROUP, D_CAPTION };
enum { TA_LEFT, TA_CENTER, TA_RIGHT };
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PREWRAP, WS_PRELINE };
enum { FL_NONE, FL_LEFT, FL_RIGHT };
enum { CL_NONE = 0, CL_LEFT = 1, CL_RIGHT = 2, CL_BOTH = 3 };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LALPHA, LS_UALPHA, LS_NONE };
enum { VA_MIDDLE, VA_TOP, VA_BOTTOM };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP };

/* Alpha-0 colours are placeholders, resolved at layout against the background
 * so text always stays readable. */
static inline bool color_is_default(Color c) { return c.a == 0; }
#define DEFAULT_TEXT_R 0
#define DEFAULT_LINK_R 1

typedef struct Style {
    unsigned char display, bold, italic, mono, underline, strike, align, ws, transform;
    unsigned char fl, clear, list, valign, has_bg, collapse, bfc, abspos;
    Color color, bg, bcolor, eff_bg;
    float fsize;             /* CSS px */
    float border[4];         /* px: top right bottom left */
    float spacing;           /* table cell spacing, px */
    Len margin[4], padding[4], width, maxw, height;
} Style;

typedef struct CssSheet CssSheet;

bool css_media_matches(const char *query);
CssSheet *css_parse(const char *text, int len);
void css_free(CssSheet *s);
int css_rule_count(const CssSheet *s);

/* Computes node->style for every element (allocated from styles).
 * author=false applies only the browser's default stylesheet.
 * default_bg is the page colour when the page doesn't set one. */
void css_style_tree(Dom *dom, CssSheet *const *sheets, int nsheets, bool author, Color default_bg, Arena *styles);

/* Resolves a length to px. pct_base for %, auto_val for auto/unset. */
float css_len(Len l, float pct_base, float fsize, float auto_val);
