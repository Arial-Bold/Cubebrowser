/* page.h - a web page: document tree + styles -> positioned boxes, drawing,
 * hit testing, forms, pictures and stylesheets. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"
#include "url.h"
#include "dom.h"
#include "css.h"

enum { BOX_TEXT, BOX_FILL, BOX_BULLET, BOX_INPUT, BOX_SUBMIT, BOX_IMAGE, BOX_CHECK, BOX_SELECT };
enum { BF_UNDERLINE = 1, BF_STRIKE = 2, BF_BG = 4, BF_CIRCLE = 8, BF_SQUARE = 16, BF_RADIO = 32 };

typedef struct {
    float x, y, w, h;
    float size;
    unsigned char type, font, flags;
    Color color, bg;
    short field;        /* form field index or -1 */
    int link;           /* link index or -1 */
    int text, len;      /* text arena offset/length; BOX_IMAGE: text = image index */
} Box;

enum { FIELD_TEXT, FIELD_PASSWORD, FIELD_HIDDEN, FIELD_SUBMIT, FIELD_CHECK, FIELD_RADIO, FIELD_SELECT };

typedef struct {
    int form;
    unsigned char type, checked;
    char name[64];
    char value[256];
    char label[64];     /* submit/select caption */
} Field;

typedef struct {
    char action[URL_MAX];
    bool post;
} Form;

enum { IMG_NONE, IMG_LOADING, IMG_READY, IMG_FAILED, IMG_SKIPPED };

typedef struct {
    char *url;
    short decl_w, decl_h;
    unsigned char state;
    GfxImage img;
} PageImage;

enum { SHEET_NONE, SHEET_LOADING, SHEET_DONE };

typedef struct {
    char *url;          /* external sheets only */
    char *text;         /* downloaded text (owned) */
    CssSheet *css;
    unsigned char state;
} PageSheet;

#define PAGE_MAX_IMAGES 64
#define PAGE_MAX_FIELDS 128
#define PAGE_MAX_FORMS 24
#define PAGE_MAX_FLOATS 96

typedef struct { float x, y, w, h; unsigned char side; } Float;

typedef struct {
    /* source */
    char *src;
    size_t src_len;
    bool plain;
    char url[URL_MAX];
    char base[URL_MAX];
    char title[128];
    bool secure, verified, truncated;
    Dom *dom;
    Arena style_arena;
    PageSheet sheets[DOM_MAX_SHEETS];
    int nsheets;
    bool has_author_styles;
    bool restyle;        /* styles changed: recompute before layout */
    bool relayout;       /* an image arrived: lay out again */
    bool styled_author;  /* last styling included the page's own styles */
    /* layout output */
    Box *boxes;
    int nboxes, capboxes;
    char *arena;
    int arena_len, arena_cap;
    char **links;
    int nlinks, caplinks;
    int nframes;
    Field fields[PAGE_MAX_FIELDS];
    int nfields;
    Form forms[PAGE_MAX_FORMS];
    int nforms;
    PageImage images[PAGE_MAX_IMAGES];
    int nimages;
    size_t image_bytes;
    Float floats[PAGE_MAX_FLOATS];
    int nfloats;
    Color bg;            /* page background */
    float width, scale, content_h;
    /* view state */
    float scroll, scroll_target;
} Page;

void page_init(Page *p);
/* Takes ownership of src. */
void page_set_source(Page *p, char *src, size_t len, bool plain, const char *url);
void page_layout(Page *p, float width, float text_scale);
void page_free(Page *p);
void page_free_images(Page *p);
void page_draw(Page *p, float x, float y, float w, float h, int hover_link, int hover_field);
int  page_hit(Page *p, float px, float py, int *field_out);
const char *page_link_url(Page *p, int link);
bool page_build_submit(Page *p, int field_idx, int submit_field, char *url_out, size_t url_len, char **post_out);
void page_toggle_field(Page *p, int field);

/* Stylesheets: next external sheet to download (-1 if none), and delivery. */
int  page_next_sheet(Page *p);
void page_sheet_loaded(Page *p, int idx, char *text, size_t len);   /* text may be NULL on failure */
