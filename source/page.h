/* page.h - turns HTML (or plain text) into positioned boxes, draws them and
 * hit-tests links and form fields. Parsing and layout happen in one pass, and
 * re-run whenever the width or text size changes (e.g. 4:3 <-> 16:9). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"
#include "url.h"

enum { BOX_TEXT, BOX_RULE, BOX_BULLET, BOX_INPUT, BOX_SUBMIT, BOX_IMAGE };
enum { BF_BOLD = 1, BF_DIM = 2, BF_HEADING = 4, BF_PRE = 8 };

typedef struct {
    float x, y, w, h;
    float size;
    unsigned char type, flags;
    short field;        /* form field index or -1 */
    int link;           /* link index or -1 */
    int text, len;      /* offset/length in the text arena (BOX_IMAGE: text = image index) */
} Box;

enum { FIELD_TEXT, FIELD_PASSWORD, FIELD_HIDDEN, FIELD_SUBMIT };

typedef struct {
    int form;
    unsigned char type;
    char name[64];
    char value[256];
} Field;

typedef struct {
    char action[URL_MAX];
    bool post;
} Form;

enum { IMG_NONE, IMG_LOADING, IMG_READY, IMG_FAILED, IMG_SKIPPED };

typedef struct {
    char *url;              /* absolute image address */
    short decl_w, decl_h;   /* width/height attributes, 0 if absent */
    unsigned char state;
    GfxImage img;
} PageImage;

#define PAGE_MAX_IMAGES 64
#define PAGE_MAX_FIELDS 96
#define PAGE_MAX_FORMS 24

typedef struct {
    /* source */
    char *src;
    size_t src_len;
    bool plain;
    char url[URL_MAX];
    char base[URL_MAX];
    char title[128];
    bool secure, verified, truncated;
    /* layout output */
    Box *boxes;
    int nboxes, capboxes;
    char *arena;
    int arena_len, arena_cap;
    int *links;          /* arena offsets of link URLs */
    int nlinks, caplinks;
    Field fields[PAGE_MAX_FIELDS];
    int nfields;
    Form forms[PAGE_MAX_FORMS];
    int nforms;
    PageImage images[PAGE_MAX_IMAGES];
    int nimages;
    size_t image_bytes;
    bool relayout;          /* an image arrived or failed: lay out again */
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
int  page_hit(Page *p, float px, float py, int *field_out);   /* page coords -> link index or -1 */
const char *page_link_url(Page *p, int link);
/* Builds the URL (GET) or body (POST) for submitting the form containing field_idx.
 * submit_field is the clicked submit button or -1. Returns false if no form. */
bool page_build_submit(Page *p, int field_idx, int submit_field, char *url_out, size_t url_len,
                       char **post_body_out);
