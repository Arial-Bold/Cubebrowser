/* dom.h - HTML -> document tree. Forgiving parser with the common implied
 * end-tag rules (p, li, td, tr, option...), raw text elements, and text
 * converted to ASCII for the bitmap fonts. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct ArenaBlock ArenaBlock;
typedef struct { ArenaBlock *head; size_t used; } Arena;
void *arena_alloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);
void arena_free(Arena *a);

enum {
    TAG_UNKNOWN, TAG_ROOT, TAG_HTML, TAG_HEAD, TAG_BODY, TAG_TITLE, TAG_STYLE, TAG_SCRIPT, TAG_LINK, TAG_META,
    TAG_BASE, TAG_A, TAG_IMG, TAG_BR, TAG_HR, TAG_P, TAG_DIV, TAG_SPAN, TAG_UL, TAG_OL, TAG_LI, TAG_DL, TAG_DT,
    TAG_DD, TAG_TABLE, TAG_THEAD, TAG_TBODY, TAG_TFOOT, TAG_TR, TAG_TD, TAG_TH, TAG_CAPTION, TAG_COL,
    TAG_COLGROUP, TAG_FORM, TAG_INPUT, TAG_TEXTAREA, TAG_BUTTON, TAG_SELECT, TAG_OPTION, TAG_OPTGROUP,
    TAG_LABEL, TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6, TAG_PRE, TAG_BLOCKQUOTE, TAG_CENTER, TAG_FONT,
    TAG_NOSCRIPT, TAG_SVG, TAG_MATH, TAG_TEMPLATE, TAG_IFRAME, TAG_OBJECT, TAG_EMBED, TAG_WBR, TAG_AREA,
    TAG_SOURCE, TAG_PARAM, TAG_TRACK, TAG_XMP, TAG_LISTING, TAG_PLAINTEXT, TAG_SECTION, TAG_ARTICLE,
    TAG_HEADER, TAG_FOOTER, TAG_NAV, TAG_MAIN, TAG_ASIDE, TAG_FIGURE, TAG_ADDRESS, TAG_FIELDSET, TAG_DETAILS,
    TAG_MENU, TAG_PICTURE, TAG_VIDEO, TAG_AUDIO, TAG_CANVAS, TAG_DIALOG, TAG_COUNT
};

enum { NT_ELEM, NT_TEXT };

typedef struct Attr { const char *name, *value; struct Attr *next; } Attr;
struct Style;

typedef struct Node {
    unsigned char type, tag;
    const char *name;               /* lowercase tag name */
    struct Node *parent, *first, *last, *next, *prev;
    Attr *attrs;
    const char *text;               /* NT_TEXT: ASCII text */
    int len;
    const char *id;
    const char **classes;
    int nclasses;
    int index;                      /* link / field / image / form index, -1 if none */
    int sub;                        /* extra index (e.g. form of a field) */
    struct Style *style;
} Node;

#define DOM_MAX_SHEETS 8

typedef struct {
    const char *text;   /* <style> contents, or NULL for <link> until downloaded */
    int len;
    const char *href;   /* <link href>, NULL for <style> */
    const char *media;
} DomSheet;

typedef struct {
    Arena arena;
    Node *root;
    int nodes;
    char title[128];
    DomSheet sheets[DOM_MAX_SHEETS];
    int nsheets;
} Dom;

Dom *dom_parse(const char *src, size_t len, bool plain);
void dom_free(Dom *d);
const char *dom_attr(const Node *n, const char *name);
int dom_tag_id(const char *lowercase_name);

/* Text conversion shared with other modules */
void dom_decode_inplace(char *s, size_t cap);   /* entities + UTF-8 -> ASCII */
