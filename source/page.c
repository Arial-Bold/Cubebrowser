/* page.c - turns a styled document tree into positioned boxes.
 *
 * Layout model (a practical subset of CSS 2):
 *  - block boxes with margins (adjacent vertical margins collapse), borders,
 *    padding, backgrounds, width/max-width, auto-margin centring
 *  - inline text with wrapping, white-space modes, text-align, colours,
 *    bold/italic/monospace, underline/strike, inline backgrounds
 *  - floats (left/right) that text wraps around, and clear
 *  - tables with real columns (min/max content widths, colspan,
 *    cellspacing/padding, vertical-align, row/cell backgrounds)
 *  - list markers (disc/circle/square/decimal/alpha)
 *  - images, text inputs, buttons, checkboxes, radios, selects, textareas
 * Not supported: flexbox/grid (treated as blocks), positioning, transforms.
 */
#include "page.h"
#include "theme.h"
#include "settings.h"
#include "bigstack.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define WORD_MAX 400
#define MAX_TABLE_COLS 32
#define MAX_TABLE_ROWS 600
#define MAX_TABLE_DEPTH 8
#define SHEET_MAX_BYTES (256 * 1024)

/* ================================================================ arenas */

static int text_add(Page *p, const char *s, int len) {
    if (p->arena_len + len + 1 > p->arena_cap) {
        int nc = p->arena_cap ? p->arena_cap * 2 : 65536;
        while (nc < p->arena_len + len + 1) nc *= 2;
        char *na = realloc(p->arena, (size_t)nc);
        if (!na) return 0;
        p->arena = na;
        p->arena_cap = nc;
    }
    int off = p->arena_len;
    memcpy(p->arena + off, s, (size_t)len);
    p->arena[off + len] = 0;
    p->arena_len += len + 1;
    return off;
}

static Box *box_new(Page *p) {
    if (p->nboxes == p->capboxes) {
        int nc = p->capboxes ? p->capboxes * 2 : 2048;
        Box *nb = realloc(p->boxes, sizeof(Box) * (size_t)nc);
        if (!nb) return NULL;
        p->boxes = nb;
        p->capboxes = nc;
    }
    Box *b = &p->boxes[p->nboxes++];
    memset(b, 0, sizeof(*b));
    b->link = -1;
    b->field = -1;
    return b;
}

static int link_add(Page *p, const char *url) {
    if (p->nlinks == p->caplinks) {
        int nc = p->caplinks ? p->caplinks * 2 : 128;
        char **nl = realloc(p->links, sizeof(char *) * (size_t)nc);
        if (!nl) return -1;
        p->links = nl;
        p->caplinks = nc;
    }
    p->links[p->nlinks] = strdup(url);
    if (!p->links[p->nlinks]) return -1;
    return p->nlinks++;
}

const char *page_link_url(Page *p, int link) {
    if (link < 0 || link >= p->nlinks) return "";
    return p->links[link];
}

/* ================================================================ prepass */

static void inner_text(const Node *n, char *out, size_t cap, size_t *o) {
    for (const Node *c = n->first; c; c = c->next) {
        if (c->type == NT_TEXT) {
            for (int i = 0; i < c->len && *o + 2 < cap; i++) {
                char ch = c->text[i];
                if (isspace((unsigned char)ch) || ch == 1) {
                    if (*o && out[*o - 1] != ' ') out[(*o)++] = ' ';
                } else {
                    out[(*o)++] = ch;
                }
            }
        } else if (c->tag != TAG_SCRIPT && c->tag != TAG_STYLE) {
            inner_text(c, out, cap, o);
        }
    }
    out[*o] = 0;
}

static void text_of(const Node *n, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
    inner_text(n, out, cap, &o);
    while (o && out[o - 1] == ' ') out[--o] = 0;
}

static int field_add(Page *p, int form, int type, const char *name, const char *value) {
    if (p->nfields >= PAGE_MAX_FIELDS) return -1;
    Field *f = &p->fields[p->nfields];
    memset(f, 0, sizeof(*f));
    f->form = form;
    f->type = (unsigned char)type;
    snprintf(f->name, sizeof(f->name), "%s", name ? name : "");
    snprintf(f->value, sizeof(f->value), "%s", value ? value : "");
    return p->nfields++;
}

static __attribute__((noinline)) int image_add(Page *p, const Node *n) {
    if (p->nimages >= PAGE_MAX_IMAGES) return -1;
    PageImage *im = &p->images[p->nimages];
    memset(im, 0, sizeof(*im));
    const char *src = dom_attr(n, "data-src");
    if (!src || !*src) src = dom_attr(n, "data-original");
    if (!src || !*src) src = dom_attr(n, "src");
    char first[URL_MAX];
    if ((!src || !*src || !strncasecmp(src, "data:", 5)) && dom_attr(n, "srcset")) {
        const char *ss = dom_attr(n, "srcset");
        while (isspace((unsigned char)*ss)) ss++;
        size_t l = strcspn(ss, " ,");
        if (l && l < sizeof(first)) { memcpy(first, ss, l); first[l] = 0; src = first; }
    }
    const char *w = dom_attr(n, "width"), *h = dom_attr(n, "height");
    if (w && !strchr(w, '%')) im->decl_w = (short)atoi(w);
    if (h && !strchr(h, '%')) im->decl_h = (short)atoi(h);
    if (im->decl_w < 0 || im->decl_w > 4000) im->decl_w = 0;
    if (im->decl_h < 0 || im->decl_h > 4000) im->decl_h = 0;
    if (!src || !*src || !strncasecmp(src, "data:", 5) || (im->decl_w && im->decl_w <= 2) ||
        (im->decl_h && im->decl_h <= 2)) {
        im->state = IMG_SKIPPED;   /* missing, inline data, or a tracking pixel */
    } else {
        char abs[URL_MAX];
        url_resolve(p->base, src, abs, sizeof(abs));
        im->url = strdup(abs);
        if (!im->url) im->state = IMG_SKIPPED;
    }
    return p->nimages++;
}

static char scratch_url[URL_MAX];   /* prepass helpers run one at a time */

/* Handles one element. Kept out of line so the recursive prepass() stays tiny. */
static __attribute__((noinline)) void prepass_elem(Page *p, Node *c, int *form) {
        const char *v;
        if (dom_attr(c, "style") || dom_attr(c, "bgcolor") || (c->tag == TAG_FONT && dom_attr(c, "color")))
            p->has_author_styles = true;
        switch (c->tag) {
        case TAG_BASE:
            if ((v = dom_attr(c, "href"))) url_resolve(p->url, v, p->base, sizeof(p->base));
            break;
        case TAG_A:
            if ((v = dom_attr(c, "href")) && *v && strncasecmp(v, "javascript:", 11) && strncasecmp(v, "mailto:", 7) &&
                strncasecmp(v, "tel:", 4)) {
                url_resolve(p->base, v, scratch_url, sizeof(scratch_url));
                c->index = link_add(p, scratch_url);
            }
            break;
        case TAG_IMG:
            c->index = image_add(p, c);
            break;
        case TAG_FRAME:
        case TAG_IFRAME:
            if (c->tag == TAG_FRAME && (v = dom_attr(c, "src")) && *v && strncasecmp(v, "javascript:", 11)) {
                url_resolve(p->base, v, scratch_url, sizeof(scratch_url));
                c->index = link_add(p, scratch_url);
                c->sub = ++p->nframes;
            }
            break;
        case TAG_FORM:
            if (p->nforms < PAGE_MAX_FORMS) {
                Form *f = &p->forms[p->nforms];
                url_resolve(p->base, (v = dom_attr(c, "action")) ? v : "", f->action, sizeof(f->action));
                f->post = (v = dom_attr(c, "method")) && !strcasecmp(v, "post");
                c->index = p->nforms++;
                *form = c->index;
            }
            break;
        case TAG_INPUT: {
            const char *type = dom_attr(c, "type");
            const char *name = dom_attr(c, "name"), *val = dom_attr(c, "value");
            int t = FIELD_TEXT;
            if (type) {
                if (!strcasecmp(type, "hidden")) t = FIELD_HIDDEN;
                else if (!strcasecmp(type, "password")) t = FIELD_PASSWORD;
                else if (!strcasecmp(type, "submit") || !strcasecmp(type, "image") || !strcasecmp(type, "button")) t = FIELD_SUBMIT;
                else if (!strcasecmp(type, "checkbox")) t = FIELD_CHECK;
                else if (!strcasecmp(type, "radio")) t = FIELD_RADIO;
                else if (!strcasecmp(type, "reset") || !strcasecmp(type, "file")) t = -1;
            }
            if (t < 0) break;
            if ((t == FIELD_CHECK || t == FIELD_RADIO) && !val) val = "on";
            c->index = field_add(p, *form, t, name, val);
            if (c->index >= 0) {
                Field *f = &p->fields[c->index];
                f->checked = dom_attr(c, "checked") != NULL;
                if (t == FIELD_SUBMIT)
                    snprintf(f->label, sizeof(f->label), "%s", val && *val ? val : (type && !strcasecmp(type, "image") ? "Go" : "Submit"));
                if (t == FIELD_TEXT && (v = dom_attr(c, "placeholder"))) snprintf(f->label, sizeof(f->label), "%s", v);
            }
            break;
        }
        case TAG_TEXTAREA: {
            char content[256];
            text_of(c, content, sizeof(content));
            c->index = field_add(p, *form, FIELD_TEXT, dom_attr(c, "name"), content);
            break;
        }
        case TAG_BUTTON: {
            const char *type = dom_attr(c, "type");
            if (type && strcasecmp(type, "submit")) break;
            c->index = field_add(p, *form, FIELD_SUBMIT, dom_attr(c, "name"), dom_attr(c, "value"));
            if (c->index >= 0) {
                text_of(c, p->fields[c->index].label, sizeof(p->fields[c->index].label));
                if (!p->fields[c->index].label[0]) snprintf(p->fields[c->index].label, 64, "Submit");
            }
            break;
        }
        case TAG_SELECT: {
            const Node *chosen = NULL, *first = NULL;
            for (const Node *o = c->first; o; o = o->next) {
                const Node *list[2] = { o, NULL };
                if (o->tag == TAG_OPTGROUP) list[0] = o->first;
                for (const Node *q = list[0]; q; q = (o->tag == TAG_OPTGROUP) ? q->next : NULL) {
                    if (q->tag != TAG_OPTION) continue;
                    if (!first) first = q;
                    if (dom_attr(q, "selected") && !chosen) chosen = q;
                }
            }
            if (!chosen) chosen = first;
            char label[64] = "";
            const char *val = NULL;
            if (chosen) {
                text_of(chosen, label, sizeof(label));
                val = dom_attr(chosen, "value");
            }
            c->index = field_add(p, *form, FIELD_SELECT, dom_attr(c, "name"), val ? val : label);
            if (c->index >= 0) snprintf(p->fields[c->index].label, 64, "%s", label);
            break;
        }
        }
}

static void prepass(Page *p, Node *n, int *form) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != NT_ELEM) continue;
        prepass_elem(p, c, form);
        if (c->tag == TAG_SELECT || c->tag == TAG_BUTTON || c->tag == TAG_TEXTAREA) continue;
        int saved = *form;
        prepass(p, c, form);
        if (c->tag == TAG_FORM && c->first) *form = saved;   /* properly nested forms end here */
    }
}

/* ================================================================ sheets */

static void sheets_free(Page *p) {
    for (int i = 0; i < p->nsheets; i++) {
        free(p->sheets[i].url);
        free(p->sheets[i].text);
        css_free(p->sheets[i].css);
    }
    p->nsheets = 0;
}

static void sheets_init(Page *p) {
    Dom *d = p->dom;
    for (int i = 0; i < d->nsheets && p->nsheets < DOM_MAX_SHEETS; i++) {
        DomSheet *ds = &d->sheets[i];
        PageSheet *ps = &p->sheets[p->nsheets++];
        memset(ps, 0, sizeof(*ps));
        if (!css_media_matches(ds->media)) { ps->state = SHEET_DONE; continue; }
        if (ds->href) {
            char abs[URL_MAX];
            url_resolve(p->base, ds->href, abs, sizeof(abs));
            ps->url = strdup(abs);
            ps->state = ps->url ? SHEET_NONE : SHEET_DONE;
        } else {
            int len = ds->len < SHEET_MAX_BYTES ? ds->len : SHEET_MAX_BYTES;
            ps->css = css_parse(ds->text, len);
            ps->state = SHEET_DONE;
        }
    }
    if (p->nsheets) p->has_author_styles = true;
}

int page_next_sheet(Page *p) {
    if (!g_set.page_styles) return -1;
    for (int i = 0; i < p->nsheets; i++)
        if (p->sheets[i].state == SHEET_NONE) {
            p->sheets[i].state = SHEET_LOADING;
            return i;
        }
    return -1;
}

void page_sheet_loaded(Page *p, int idx, char *text, size_t len) {
    if (idx < 0 || idx >= p->nsheets) { free(text); return; }
    PageSheet *ps = &p->sheets[idx];
    ps->state = SHEET_DONE;
    if (!text) return;
    if (len > SHEET_MAX_BYTES) len = SHEET_MAX_BYTES;
    ps->text = text;
    ps->css = css_parse(text, (int)len);
    p->restyle = true;
}

static float luminance(Color c) { return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f; }

static void restyle(Page *p) {
    arena_free(&p->style_arena);
    CssSheet *list[DOM_MAX_SHEETS];
    int n = 0;
    for (int i = 0; i < p->nsheets; i++) if (p->sheets[i].css) list[n++] = p->sheets[i].css;
    bool author = g_set.page_styles != 0;
    Color def = (author && p->has_author_styles) ? rgba(255, 255, 255, 255) : theme()->page_bg;
    css_style_tree(p->dom, list, n, author, def, &p->style_arena);
    p->bg = def;
    /* the page background comes from <html>, else <body> */
    for (Node *a = p->dom->root->first; a; a = a->next) {
        if (a->tag != TAG_HTML || !a->style) continue;
        if (a->style->has_bg) { p->bg = a->style->bg; break; }
        for (Node *b = a->first; b; b = b->next)
            if (b->tag == TAG_BODY && b->style && b->style->has_bg) p->bg = b->style->bg;
    }
    p->restyle = false;
    p->styled_author = author;
}

/* ================================================================ colours */

static Color pick_color(Color c, Color bg, bool is_link) {
    const Theme *t = theme();
    bool light = luminance(bg) > 0.5f;
    bool theme_bg = bg.r == t->page_bg.r && bg.g == t->page_bg.g && bg.b == t->page_bg.b;
    if (color_is_default(c)) {
        bool link = c.r == DEFAULT_LINK_R || is_link;
        if (theme_bg) return link ? t->page_link : t->page_text;
        if (link) return light ? rgba(26, 72, 200, 255) : rgba(140, 185, 255, 255);
        return light ? rgba(22, 22, 30, 255) : rgba(232, 232, 238, 255);
    }
    /* keep text readable whatever the page asks for */
    if (fabsf(luminance(c) - luminance(bg)) < 0.22f) {
        if (theme_bg) return is_link ? t->page_link : t->page_text;
        return light ? rgba(22, 22, 30, 255) : rgba(232, 232, 238, 255);
    }
    return c;
}

/* ================================================================ layout */

typedef struct {
    Page *p;
    bool measure;
    float origin, max_line, max_atom;
    float fscale;
    int link;
    int depth;
} L;

typedef struct {
    float left, right;
    float y, x;
    float line_l, line_r, line_h;
    int line_start;
    bool at_start, space;
    float space_w;
    unsigned char align;
    float pending_margin;
    int marker;              /* LS_* or -1 */
    int marker_num;
    float marker_size;
    Color marker_color;
    float empty_lh;          /* height of an empty line for <br> */
} Flow;

static void flow_init(Flow *f, float left, float right, float y, unsigned char align) {
    memset(f, 0, sizeof(*f));
    f->left = left;
    f->right = right < left + 8 ? left + 8 : right;
    f->y = y;
    f->x = left;
    f->line_l = left;
    f->line_r = f->right;
    f->at_start = true;
    f->align = align;
    f->marker = -1;
    f->empty_lh = 20;
}

static float floats_bottom(Page *p, int from, float left, float right, int sides) {
    float b = 0;
    for (int i = from; i < p->nfloats; i++) {
        Float *fl = &p->floats[i];
        if (!(sides & (fl->side == FL_LEFT ? CL_LEFT : CL_RIGHT))) continue;
        if (fl->x >= right || fl->x + fl->w <= left) continue;
        if (fl->y + fl->h > b) b = fl->y + fl->h;
    }
    return b;
}

static void line_bounds(L *c, Flow *f, float y, float h, float *l, float *r) {
    *l = f->left;
    *r = f->right;
    if (c->measure) return;
    Page *p = c->p;
    for (int i = 0; i < p->nfloats; i++) {
        Float *fl = &p->floats[i];
        if (fl->y >= y + h || fl->y + fl->h <= y) continue;
        if (fl->x >= f->right || fl->x + fl->w <= f->left) continue;
        if (fl->side == FL_LEFT) { if (fl->x + fl->w > *l) *l = fl->x + fl->w; }
        else if (fl->x < *r) *r = fl->x;
    }
}

static void start_line(L *c, Flow *f, float h) {
    if (f->pending_margin > 0) { f->y += f->pending_margin; f->pending_margin = 0; }
    float l, r;
    float need = fminf(60.0f, (f->right - f->left) * 0.5f);
    for (int tries = 0; tries < 12; tries++) {
        line_bounds(c, f, f->y, h, &l, &r);
        if (r - l >= need) break;
        /* not enough room beside floats: move below the nearest one */
        float next = 1e9f;
        for (int i = 0; i < c->p->nfloats; i++) {
            Float *fl = &c->p->floats[i];
            float bot = fl->y + fl->h;
            if (fl->y < f->y + h && bot > f->y && bot < next) next = bot;
        }
        if (next >= 1e9f) break;
        f->y = next;
    }
    f->line_l = l;
    f->line_r = r;
    f->x = l;
    f->line_h = 0;
    f->line_start = c->p->nboxes;
}

static void finish_line(L *c, Flow *f) {
    if (f->at_start) return;
    Page *p = c->p;
    if (!c->measure) {
        float free_w = f->line_r - f->x, dx = 0;
        if (f->align == TA_CENTER) dx = free_w * 0.5f;
        else if (f->align == TA_RIGHT) dx = free_w;
        if (dx < 0) dx = 0;
        for (int i = f->line_start; i < p->nboxes; i++) {
            Box *b = &p->boxes[i];
            if (b->type == BOX_BULLET && b->flags & 128) { b->flags &= ~128; b->y = f->y + (f->line_h - b->h) * 0.5f; continue; }
            b->x += dx;
            if (b->type == BOX_TEXT || b->type == BOX_IMAGE) b->y = f->y + f->line_h - b->h;
            else b->y = f->y + (f->line_h - b->h) * 0.5f;
        }
    }
    f->y += f->line_h;
    f->x = f->left;
    f->line_h = 0;
    f->at_start = true;
    f->space = false;
}

static void forced_break(L *c, Flow *f) {
    if (f->at_start) {
        if (f->pending_margin > 0) { f->y += f->pending_margin; f->pending_margin = 0; }
        f->y += f->empty_lh;
    } else {
        finish_line(c, f);
    }
}

static void emit_marker(L *c, Flow *f) {
    if (f->marker < 0) return;
    int type = f->marker;
    f->marker = -1;
    if (c->measure) return;
    Page *p = c->p;
    float size = f->marker_size;
    if (type == LS_DISC || type == LS_CIRCLE || type == LS_SQUARE) {
        Box *b = box_new(p);
        if (!b) return;
        b->type = BOX_BULLET;
        b->w = b->h = size * 0.32f;
        b->x = f->left - size * 0.75f;
        b->y = f->y;
        b->color = f->marker_color;
        b->flags = 128 | (type == LS_CIRCLE ? BF_CIRCLE : type == LS_SQUARE ? BF_SQUARE : 0);
    } else {
        char num[16];
        int n = f->marker_num;
        if (type == LS_LALPHA || type == LS_UALPHA) {
            char ch = (char)((type == LS_LALPHA ? 'a' : 'A') + (n - 1) % 26);
            snprintf(num, sizeof(num), "%c.", ch);
        } else {
            snprintf(num, sizeof(num), "%d.", n);
        }
        int len = (int)strlen(num);
        float w = gfx_text_width(FONT_BODY, size, num, len);
        Box *b = box_new(p);
        if (!b) return;
        b->type = BOX_TEXT;
        b->font = FONT_BODY;
        b->size = size;
        b->x = f->left - w - size * 0.35f;
        b->y = f->y;
        b->w = w;
        b->h = gfx_line_height(FONT_BODY, size);
        b->color = f->marker_color;
        b->text = text_add(p, num, len);
        b->len = len;
    }
}

/* Reserves space on the current line. Returns the x position. */
static float place(L *c, Flow *f, float w, float h, bool can_break) {
    if (f->at_start) {
        start_line(c, f, h);
    } else {
        float sp = f->space ? f->space_w : 0;
        if (can_break && f->x + sp + w > f->line_r + 0.5f) {
            finish_line(c, f);
            start_line(c, f, h);
        } else {
            f->x += sp;
        }
    }
    bool first = f->at_start;
    f->at_start = false;
    f->space = false;
    float x = f->x;
    f->x += w;
    if (h > f->line_h) f->line_h = h;
    if (first) emit_marker(c, f);
    if (c->measure) {
        if (f->x - c->origin > c->max_line) c->max_line = f->x - c->origin;
        float atom = w + (f->line_l - c->origin);
        if (atom > c->max_atom) c->max_atom = atom;
    }
    return x;
}

static FontId font_for(const Style *s) {
    if (s->mono) return FONT_MONO;
    if (s->bold) return FONT_UI;
    if (s->italic) return FONT_ITALIC;
    return FONT_BODY;
}

static void transform_word(char *w, int n, int tt, bool word_start) {
    for (int i = 0; i < n; i++) {
        if (tt == TT_UPPER || (tt == TT_CAP && i == 0 && word_start)) w[i] = (char)toupper((unsigned char)w[i]);
        else if (tt == TT_LOWER) w[i] = (char)tolower((unsigned char)w[i]);
    }
}

static __attribute__((noinline)) void add_word(L *c, Flow *f, const char *w, int n, const Style *s, bool can_break) {
    if (n <= 0) return;
    FontId font = font_for(s);
    float size = s->fsize * c->fscale;
    float h = gfx_line_height(font, size);
    char buf[WORD_MAX];
    if (n > WORD_MAX - 1) n = WORD_MAX - 1;
    memcpy(buf, w, (size_t)n);
    for (int i = 0; i < n; i++) if (buf[i] == 1) buf[i] = ' ';
    if (s->transform) transform_word(buf, n, s->transform, true);
    const char *t = buf;
    float width = gfx_text_width(font, size, t, n);
    float avail = f->right - f->left;
    while (n > 0) {
        int k = n;
        float wk = width;
        if (can_break && wk > avail && !c->measure) {
            /* a word wider than the column (e.g. a long URL): split it */
            float room = f->at_start ? avail : f->line_r - f->x - (f->space ? f->space_w : 0);
            if (room < size * 2) { if (!f->at_start) finish_line(c, f); room = avail; }
            k = gfx_text_fit(font, size, t, n, room);
            if (k <= 0) k = 1;
            wk = gfx_text_width(font, size, t, k);
        }
        float x = place(c, f, wk, h, can_break);
        if (!c->measure) {
            Box *b = box_new(c->p);
            if (b) {
                b->type = BOX_TEXT;
                b->font = (unsigned char)font;
                b->size = size;
                b->x = x; b->y = f->y; b->w = wk; b->h = h;
                b->link = c->link;
                b->color = pick_color(s->color, s->eff_bg, c->link >= 0);
                if (s->underline) b->flags |= BF_UNDERLINE;
                if (s->strike) b->flags |= BF_STRIKE;
                if (s->has_bg && s->display == D_INLINE) { b->flags |= BF_BG; b->bg = s->bg; }
                b->text = text_add(c->p, t, k);
                b->len = k;
            }
        }
        t += k;
        n -= k;
        width = n ? gfx_text_width(font, size, t, n) : 0;
    }
    f->space_w = gfx_text_width(font, size, " ", 1);
    f->empty_lh = h;
}

static __attribute__((noinline)) void emit_text(L *c, Flow *f, const Node *tn, const Style *s) {
    const char *t = tn->text;
    int n = tn->len;
    int ws = s->ws;
    bool wrap = (ws == WS_NORMAL || ws == WS_PREWRAP || ws == WS_PRELINE);
    if (ws == WS_PRE || ws == WS_PREWRAP) {
        /* keep spaces; break at newlines */
        int start = 0;
        for (int i = 0; i <= n; i++) {
            if (i == n || t[i] == '\n') {
                if (ws == WS_PRE) {
                    char line[WORD_MAX];
                    int o = 0;
                    for (int k = start; k < i && o < WORD_MAX - 5; k++) {
                        if (t[k] == '\t') { do line[o++] = ' '; while (o % 4); }
                        else if (t[k] != '\r') line[o++] = t[k];
                    }
                    if (o) add_word(c, f, line, o, s, false);
                } else {
                    int ws0 = start;
                    for (int k = start; k <= i; k++) {
                        if (k == i || t[k] == ' ' || t[k] == '\t') {
                            if (k > ws0) add_word(c, f, t + ws0, k - ws0, s, true);
                            if (k < i) { f->space = !f->at_start; f->space_w = gfx_text_width(font_for(s), s->fsize * c->fscale, " ", 1); }
                            ws0 = k + 1;
                        }
                    }
                }
                if (i < n) forced_break(c, f);
                start = i + 1;
            }
        }
        return;
    }
    int ws0 = -1;
    for (int i = 0; i <= n; i++) {
        char ch = i < n ? t[i] : ' ';
        bool space = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r' || ch == '\f';
        if (!space) { if (ws0 < 0) ws0 = i; continue; }
        if (ws0 >= 0) { add_word(c, f, t + ws0, i - ws0, s, wrap); ws0 = -1; }
        if (i < n) {
            if (ch == '\n' && ws == WS_PRELINE) { forced_break(c, f); continue; }
            if (!f->at_start) {
                f->space = true;
                f->space_w = gfx_text_width(font_for(s), s->fsize * c->fscale, " ", 1);
            }
        }
    }
}

/* ------------------------------------------------------------- boxes */

static int reserve_fill(L *c) {
    if (c->measure) return -1;
    Box *b = box_new(c->p);
    if (!b) return -1;
    b->type = BOX_FILL;
    b->bg = rgba(0, 0, 0, 0);
    return c->p->nboxes - 1;
}

static void set_fill(L *c, int idx, float x, float y, float w, float h, Color col) {
    if (idx < 0) return;
    Box *b = &c->p->boxes[idx];
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->bg = col;
}

typedef struct { float t, r, b, l; } Edges;

static Edges pad_of(const Style *s, float cw) {
    Edges e = { css_len(s->padding[0], cw, s->fsize, 0), css_len(s->padding[1], cw, s->fsize, 0),
                css_len(s->padding[2], cw, s->fsize, 0), css_len(s->padding[3], cw, s->fsize, 0) };
    return e;
}

static Edges border_of(const Style *s) {
    Edges e = { s->border[0], s->border[1], s->border[2], s->border[3] };
    return e;
}

static void flow_children(L *c, Flow *f, Node *n, const Style *s);
static float block_box(L *c, Node *n, const Style *s, float bx, float bw, float y, Flow *parent, float *escape_mb);

/* Measures min-content / max-content width of a node's content. */
static void measure(L *c, Node *n, const Style *s, float *minw, float *maxw) {
    /* Nested floats and tables measure their contents repeatedly; on some
     * pages that grows exponentially. After a fixed budget, estimate instead. */
    if (c->p->measure_left-- <= 0) { *minw = 40; *maxw = 300; return; }
    L m = *c;
    m.measure = true;
    m.origin = 0;
    m.max_line = 0;
    m.max_atom = 0;
    m.depth = c->depth + 1;
    Flow f;
    flow_init(&f, 0, 1e6f, 0, TA_LEFT);
    if (m.depth < 40) flow_children(&m, &f, n, s);
    *minw = m.max_atom;
    *maxw = m.max_line;
}

static __attribute__((noinline)) void emit_image(L *c, Flow *f, Node *n, const Style *s) {
    Page *p = c->p;
    PageImage *im = (n->index >= 0) ? &p->images[n->index] : NULL;
    /* skipped = blank placeholder or tracking pixel: nothing to show */
    if (g_set.load_images && im && im->state == IMG_SKIPPED) return;
    if (!g_set.load_images || !im || im->state == IMG_FAILED || im->state == IMG_SKIPPED) {
        const char *alt = dom_attr(n, "alt");
        char shown[200];
        if (alt && *alt && g_set.show_alt) snprintf(shown, sizeof(shown), "[%s]", alt);
        else if (c->link >= 0) snprintf(shown, sizeof(shown), "[image]");
        else return;
        Style alt_s = *s;
        alt_s.italic = 1;
        alt_s.bold = 0;
        alt_s.display = D_INLINE;
        alt_s.has_bg = 0;
        Node tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = NT_TEXT;
        tmp.text = shown;
        tmp.len = (int)strlen(shown);
        emit_text(c, f, &tmp, &alt_s);
        f->space = true;
        return;
    }
    float cw = f->right - f->left;
    float iw = 0, ih = 0;
    if (im->decl_w && im->decl_h) { iw = im->decl_w; ih = im->decl_h; }
    else if (im->state == IMG_READY) { iw = im->img.w; ih = im->img.h; }
    float ratio = (iw > 0 && ih > 0) ? ih / iw : (im->decl_w && im->decl_h ? (float)im->decl_h / im->decl_w : 0.66f);
    float w = -1, h = -1;
    if (s->width.u != U_UNSET && s->width.u != U_AUTO && !(c->measure && s->width.u == U_PCT))
        w = css_len(s->width, cw, s->fsize, -1);
    if (s->height.u != U_UNSET && s->height.u != U_AUTO && s->height.u != U_PCT) h = css_len(s->height, 0, s->fsize, -1);
    if (w > 0 && h <= 0) h = w * ratio;
    else if (h > 0 && w <= 0) w = ratio > 0 ? h / ratio : h;
    else if (w <= 0 && h <= 0) {
        if (iw > 0) { w = iw; h = ih; }
        else if (im->decl_w) { w = im->decl_w; h = im->decl_w * ratio; }
        else { w = 96; h = 72; }
    }
    if (s->maxw.u != U_UNSET && s->maxw.u != U_AUTO) {
        float mw = css_len(s->maxw, cw, s->fsize, w);
        if (w > mw && mw > 0) { h *= mw / w; w = mw; }
    }
    if (!c->measure && w > cw) { h *= cw / w; w = cw; }
    float maxh = settings_max_picture_height();
    if (h > maxh) { w *= maxh / h; h = maxh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (s->fl != FL_NONE && !c->measure) {
        if (!f->at_start) finish_line(c, f);
        if (f->pending_margin > 0) { f->y += f->pending_margin; f->pending_margin = 0; }
        float y = f->y, l, r;
        for (int tries = 0; tries < 12; tries++) {
            line_bounds(c, f, y, h, &l, &r);
            if (r - l >= w || r - l >= cw) break;
            float next = 1e9f;
            for (int i = 0; i < p->nfloats; i++) {
                float bot = p->floats[i].y + p->floats[i].h;
                if (p->floats[i].y < y + h && bot > y && bot < next) next = bot;
            }
            if (next >= 1e9f) break;
            y = next;
        }
        float x = s->fl == FL_LEFT ? l : r - w;
        Box *b = box_new(p);
        if (b) { b->type = BOX_IMAGE; b->x = x; b->y = y; b->w = w; b->h = h; b->link = c->link; b->text = n->index; }
        if (p->nfloats < PAGE_MAX_FLOATS) {
            Float *fl = &p->floats[p->nfloats++];
            fl->x = s->fl == FL_LEFT ? x : x - 8;
            fl->y = y;
            fl->w = w + 8;
            fl->h = h + 6;
            fl->side = s->fl;
        }
        return;
    }
    float x = place(c, f, w, h, true);
    if (!c->measure) {
        Box *b = box_new(p);
        if (b) { b->type = BOX_IMAGE; b->x = x; b->y = f->y; b->w = w; b->h = h; b->link = c->link; b->text = n->index; }
    }
    f->space = false;
}

static __attribute__((noinline)) void emit_widget(L *c, Flow *f, Node *n, const Style *s) {
    Page *p = c->p;
    if (n->index < 0) return;
    Field *fd = &p->fields[n->index];
    if (fd->type == FIELD_HIDDEN) return;
    float size = s->fsize * c->fscale;
    float lh = gfx_line_height(FONT_BODY, size);
    float cw = f->right - f->left;
    float w, h = lh + 10;
    int type = BOX_INPUT;
    const char *label = NULL;
    switch (fd->type) {
    case FIELD_SUBMIT:
        type = BOX_SUBMIT;
        label = fd->label;
        w = gfx_text_width(FONT_UI, size * 0.95f, label, -1) + 30;
        break;
    case FIELD_SELECT:
        type = BOX_SELECT;
        label = fd->label;
        w = gfx_text_width(FONT_BODY, size, label, -1) + 40;
        break;
    case FIELD_CHECK: case FIELD_RADIO:
        type = BOX_CHECK;
        w = h = lh * 0.8f;
        break;
    default: {
        const char *sz = dom_attr(n, "size");
        int chars = sz ? atoi(sz) : 20;
        if (chars < 4) chars = 4;
        if (chars > 60) chars = 60;
        w = chars * size * 0.55f;
        if (s->width.u == U_PX || s->width.u == U_EM || s->width.u == U_PCT || s->width.u == U_REM)
            w = css_len(s->width, cw, s->fsize, w);
        if (n->tag == TAG_TEXTAREA) { w = cw; h = lh * 3 + 10; }
        break;
    }
    }
    if (!c->measure && w > cw) w = cw;
    if (w < 16) w = 16;
    if (n->tag == TAG_TEXTAREA) { forced_break(c, f); if (f->at_start && f->y == 0) {} }
    float x = place(c, f, w, h, true);
    if (!c->measure) {
        Box *b = box_new(p);
        if (b) {
            b->type = (unsigned char)type;
            b->x = x; b->y = f->y; b->w = w; b->h = h;
            b->size = type == BOX_SUBMIT ? size * 0.95f : size;
            b->field = (short)n->index;
            if (fd->type == FIELD_RADIO) b->flags |= BF_RADIO;
            if (label) { b->text = text_add(p, label, (int)strlen(label)); b->len = (int)strlen(label); }
        }
    }
    if (n->tag == TAG_TEXTAREA) finish_line(c, f);
    f->space = true;
    f->space_w = gfx_text_width(FONT_BODY, size, " ", 1);
}

/* ------------------------------------------------------------- floats */

static __attribute__((noinline)) void layout_float(L *c, Flow *f, Node *n, const Style *s) {
    Page *p = c->p;
    float cw = f->right - f->left;
    Edges pd = pad_of(s, cw), bd = border_of(s);
    float ml = css_len(s->margin[3], cw, s->fsize, 0), mr = css_len(s->margin[1], cw, s->fsize, 0);
    float mt = css_len(s->margin[0], cw, s->fsize, 0), mb = css_len(s->margin[2], cw, s->fsize, 0);
    float extra = pd.l + pd.r + bd.l + bd.r;
    float bw, mn = 0, mx = 0;
    bool measured = false;
    if (s->width.u != U_UNSET && s->width.u != U_AUTO && !(c->measure && s->width.u == U_PCT)) {
        bw = css_len(s->width, cw, s->fsize, cw) + extra;
    } else {
        measure(c, n, s, &mn, &mx);
        measured = true;
        bw = mx + extra + 1;
    }
    if (s->maxw.u != U_UNSET && s->maxw.u != U_AUTO) {
        float mw = css_len(s->maxw, cw, s->fsize, bw) + extra;
        if (bw > mw) bw = mw;
    }
    if (bw > cw - ml - mr) bw = cw - ml - mr;
    if (bw < 8) bw = 8;
    if (c->measure) {
        if (!measured) measure(c, n, s, &mn, &mx);
        float atom = fminf(bw, mn + extra) + ml + mr + (f->left - c->origin);
        if (atom > c->max_atom) c->max_atom = atom;
        float line = bw + ml + mr + (f->left - c->origin);
        if (line > c->max_line) c->max_line = line;
        return;
    }
    if (!f->at_start) finish_line(c, f);
    if (f->pending_margin > 0) { f->y += f->pending_margin; f->pending_margin = 0; }
    float y = f->y, l = f->left, r = f->right;
    float total = bw + ml + mr;
    for (int tries = 0; tries < 12; tries++) {
        line_bounds(c, f, y, 20, &l, &r);
        if (r - l >= total || r - l >= cw) break;
        float next = 1e9f;
        for (int i = 0; i < p->nfloats; i++) {
            float bot = p->floats[i].y + p->floats[i].h;
            if (p->floats[i].y < y + 20 && bot > y && bot < next) next = bot;
        }
        if (next >= 1e9f) break;
        y = next;
    }
    float x = s->fl == FL_LEFT ? l + ml : r - mr - bw;
    int slot = -1;
    if (p->nfloats < PAGE_MAX_FLOATS) slot = p->nfloats++;   /* reserve so nested content sees it */
    if (slot >= 0) { Float *fl = &p->floats[slot]; fl->x = 0; fl->y = 0; fl->w = 0; fl->h = 0; fl->side = s->fl; }
    float esc = 0;
    float h = block_box(c, n, s, x, bw, y + mt, NULL, &esc);
    if (slot >= 0) {
        Float *fl = &p->floats[slot];
        fl->x = x - ml;
        fl->y = y;
        fl->w = total;
        fl->h = h + mt + mb;
        fl->side = s->fl;
    }
}

/* ------------------------------------------------------------- tables */

typedef struct { Node *n; int col, span; float minw, maxw, h; int start, end, bg; float x, w, y; } Cell;

static void collect_rows(Node *t, Node **rows, int *nrows, Node **caps, int *ncaps, int depth) {
    for (Node *ch = t->first; ch; ch = ch->next) {
        if (ch->type != NT_ELEM || !ch->style || ch->style->display == D_NONE) continue;
        int d = ch->style->display;
        if (d == D_ROW) { if (*nrows < MAX_TABLE_ROWS) rows[(*nrows)++] = ch; }
        else if (d == D_CAPTION) { if (*ncaps < 4) caps[(*ncaps)++] = ch; }
        else if ((d == D_ROWGROUP || ch->tag == TAG_FORM || ch->tag == TAG_DIV) && depth < 3)
            collect_rows(ch, rows, nrows, caps, ncaps, depth + 1);
    }
}

static float table_layout(L *c, Flow *pf, Node *t, const Style *ts, float tx, float avail, float y,
                          float table_w, float *out_w);

static __attribute__((noinline)) void layout_table(L *c, Flow *f, Node *t, const Style *ts) {
    finish_line(c, f);
    float cw = f->right - f->left;
    float ml = css_len(ts->margin[3], cw, ts->fsize, 0), mr = css_len(ts->margin[1], cw, ts->fsize, 0);
    float mt = css_len(ts->margin[0], cw, ts->fsize, 0), mb = css_len(ts->margin[2], cw, ts->fsize, 0);
    bool center = ts->margin[1].u == U_AUTO && ts->margin[3].u == U_AUTO;
    if (ts->margin[3].u == U_AUTO) ml = 0;
    if (ts->margin[1].u == U_AUTO) mr = 0;
    float avail = cw - ml - mr;
    float specified = -1;
    if (ts->width.u != U_UNSET && ts->width.u != U_AUTO && !(c->measure && ts->width.u == U_PCT))
        specified = css_len(ts->width, cw, ts->fsize, -1);
    if (f->pending_margin > mt) mt = f->pending_margin;
    f->pending_margin = 0;
    float y = f->y + mt;
    float w = 0;
    if (c->measure) {
        table_layout(c, f, t, ts, f->left + ml, avail, y, specified, &w);
        return;
    }
    /* first pass to learn the width (for centring), then lay out for real */
    float x = f->left + ml;
    if (center) {
        int save_boxes = c->p->nboxes, save_floats = c->p->nfloats, save_arena = c->p->arena_len;
        L m = *c;
        m.measure = true;
        m.origin = 0;
        m.max_line = m.max_atom = 0;
        float tw = 0;
        table_layout(&m, f, t, ts, 0, avail, 0, specified, &tw);
        c->p->nboxes = save_boxes;
        c->p->nfloats = save_floats;
        c->p->arena_len = save_arena;
        if (tw < avail) x += (avail - tw) * 0.5f;
    }
    float h = table_layout(c, f, t, ts, x, avail, y, specified, &w);
    f->y = y + h;
    f->pending_margin = mb;
    f->at_start = true;
    f->x = f->left;
}

/* Lays out a table at x with up to avail width. Returns its height; *out_w gets its width.
 * In measure mode only the content widths are reported. */
static float table_layout(L *c, Flow *pf, Node *t, const Style *ts, float tx, float avail, float y,
                          float specified, float *out_w) {
    Page *p = c->p;
    static Node *rows_buf[MAX_TABLE_DEPTH][MAX_TABLE_ROWS];
    int depth = c->depth;
    if (depth >= MAX_TABLE_DEPTH) {   /* too deeply nested: treat as a plain block */
        float esc = 0;
        if (c->measure) { Flow f; flow_init(&f, tx, tx + avail, y, ts->align); flow_children(c, &f, t, ts); *out_w = avail; return 0; }
        float h = block_box(c, t, ts, tx, avail, y, NULL, &esc);
        *out_w = avail;
        return h;
    }
    Node **rows = rows_buf[depth];
    Node *caps[4];
    int nrows = 0, ncaps = 0;
    collect_rows(t, rows, &nrows, caps, &ncaps, 0);

    Edges tp = pad_of(ts, avail), tb = border_of(ts);
    float spacing = ts->collapse ? 0 : ts->spacing;
    if (spacing < 0) spacing = 0;
    if (spacing > 30) spacing = 30;
    float extra = tp.l + tp.r + tb.l + tb.r;

    /* cells */
    int ncells = 0, cap = 64;
    Cell *cells = malloc(sizeof(Cell) * (size_t)cap);
    int *row_first = malloc(sizeof(int) * (size_t)(nrows + 1));
    if (!cells || !row_first) { free(cells); free(row_first); *out_w = 0; return 0; }
    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        row_first[r] = ncells;
        int col = 0;
        for (Node *ch = rows[r]->first; ch; ch = ch->next) {
            if (ch->type != NT_ELEM || !ch->style || ch->style->display == D_NONE) continue;
            if (ch->style->display != D_CELL && ch->tag != TAG_TD && ch->tag != TAG_TH) continue;
            if (col >= MAX_TABLE_COLS) break;
            if (ncells == cap) {
                Cell *nc = realloc(cells, sizeof(Cell) * (size_t)cap * 2);
                if (!nc) break;
                cells = nc;
                cap *= 2;
            }
            Cell *ce = &cells[ncells++];
            memset(ce, 0, sizeof(*ce));
            ce->n = ch;
            ce->col = col;
            const char *cs = dom_attr(ch, "colspan");
            ce->span = cs ? atoi(cs) : 1;
            if (ce->span < 1) ce->span = 1;
            if (col + ce->span > MAX_TABLE_COLS) ce->span = MAX_TABLE_COLS - col;
            col += ce->span;
        }
        if (col > ncols) ncols = col;
    }
    row_first[nrows] = ncells;
    if (ncols == 0) ncols = 1;

    /* measure */
    float colmin[MAX_TABLE_COLS], colmax[MAX_TABLE_COLS];
    for (int i = 0; i < ncols; i++) colmin[i] = colmax[i] = 0;
    L mc = *c;
    mc.depth = depth + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < ncells; i++) {
            Cell *ce = &cells[i];
            if ((pass == 0) != (ce->span == 1)) continue;
            const Style *s = ce->n->style;
            Edges pd = pad_of(s, avail), bd = border_of(s);
            float ext = pd.l + pd.r + bd.l + bd.r;
            if (pass == 0) {
                float mn, mx;
                measure(&mc, ce->n, s, &mn, &mx);
                ce->minw = mn + ext;
                ce->maxw = mx + ext;
                if (s->ws == WS_NOWRAP) ce->minw = ce->maxw;
                if (s->width.u == U_PX || s->width.u == U_EM) {
                    float w = css_len(s->width, 0, s->fsize, 0) + ext;
                    if (w > ce->minw) ce->minw = w;
                    ce->maxw = ce->minw;
                } else if (s->width.u == U_PCT && !c->measure) {
                    float w = s->width.v * avail / 100.0f;
                    if (w > ce->maxw) ce->maxw = w;
                }
                if (ce->minw > colmin[ce->col]) colmin[ce->col] = ce->minw;
                if (ce->maxw > colmax[ce->col]) colmax[ce->col] = ce->maxw;
            } else {
                float mn, mx;
                measure(&mc, ce->n, s, &mn, &mx);
                ce->minw = mn + ext;
                ce->maxw = mx + ext;
                float smin = spacing * (ce->span - 1), smax = smin;
                for (int k = 0; k < ce->span; k++) { smin += colmin[ce->col + k]; smax += colmax[ce->col + k]; }
                if (ce->minw > smin) for (int k = 0; k < ce->span; k++) colmin[ce->col + k] += (ce->minw - smin) / ce->span;
                if (ce->maxw > smax) for (int k = 0; k < ce->span; k++) colmax[ce->col + k] += (ce->maxw - smax) / ce->span;
            }
        }
    }
    for (int i = 0; i < ncols; i++) if (colmax[i] < colmin[i]) colmax[i] = colmin[i];
    float sum_min = 0, sum_max = 0;
    for (int i = 0; i < ncols; i++) { sum_min += colmin[i]; sum_max += colmax[i]; }
    float gaps = spacing * (ncols + 1);

    if (c->measure) {
        float base = tx - c->origin;
        float cap_w = specified > 0 ? fmaxf(specified, sum_min + gaps + extra) : sum_max + gaps + extra;
        if (base + cap_w > c->max_line) c->max_line = base + cap_w;
        float mn = (specified > 0 && specified < 2000) ? fmaxf(specified, sum_min + gaps + extra) : sum_min + gaps + extra;
        if (base + mn > c->max_atom) c->max_atom = base + mn;
        *out_w = cap_w;
        free(cells);
        free(row_first);
        return 0;
    }

    /* choose column widths */
    float target;
    if (specified > 0) target = fminf(specified, avail);
    else target = fminf(sum_max + gaps + extra, avail);
    float inner = target - gaps - extra;
    float widths[MAX_TABLE_COLS];
    if (inner >= sum_max) {
        float surplus = specified > 0 ? inner - sum_max : 0;
        for (int i = 0; i < ncols; i++)
            widths[i] = colmax[i] + (sum_max > 0 ? surplus * colmax[i] / sum_max : surplus / ncols);
    } else if (inner >= sum_min && sum_max > sum_min) {
        float k = (inner - sum_min) / (sum_max - sum_min);
        for (int i = 0; i < ncols; i++) widths[i] = colmin[i] + (colmax[i] - colmin[i]) * k;
    } else {
        for (int i = 0; i < ncols; i++) widths[i] = sum_min > 0 ? colmin[i] * fmaxf(inner, 0) / sum_min : inner / ncols;
    }
    float table_w = gaps + extra;
    for (int i = 0; i < ncols; i++) table_w += widths[i];
    *out_w = table_w;

    /* table background and border */
    int tbg = ts->has_bg ? reserve_fill(c) : -1, tbd[4];
    for (int i = 0; i < 4; i++) tbd[i] = tb.t || tb.r || tb.b || tb.l ? reserve_fill(c) : -1;
    float cy = y + tb.t + tp.t;

    /* captions */
    for (int i = 0; i < ncaps; i++) {
        float esc = 0;
        cy += block_box(c, caps[i], caps[i]->style, tx, table_w, cy, NULL, &esc);
    }
    cy += spacing;
    float colx[MAX_TABLE_COLS + 1];
    colx[0] = tx + tb.l + tp.l + spacing;
    for (int i = 0; i < ncols; i++) colx[i + 1] = colx[i] + widths[i] + spacing;

    for (int r = 0; r < nrows; r++) {
        const Style *rs = rows[r]->style;
        int rbg = (rs && rs->has_bg) ? reserve_fill(c) : -1;
        float row_h = 0;
        for (int i = row_first[r]; i < row_first[r + 1]; i++) {
            Cell *ce = &cells[i];
            ce->x = colx[ce->col];
            ce->w = colx[ce->col + ce->span] - spacing - ce->x;
            ce->start = p->nboxes;
            float esc = 0;
            L cc = *c;
            cc.depth = depth + 1;
            ce->h = block_box(&cc, ce->n, ce->n->style, ce->x, ce->w, cy, NULL, &esc);
            ce->end = p->nboxes;
            if (ce->n->style->height.u == U_PX && ce->n->style->height.v > ce->h) ce->h = ce->n->style->height.v;
            if (ce->h > row_h) row_h = ce->h;
        }
        if (rs && rs->height.u == U_PX && rs->height.v > row_h) row_h = rs->height.v;
        for (int i = row_first[r]; i < row_first[r + 1]; i++) {
            Cell *ce = &cells[i];
            float dy = 0;
            int va = ce->n->style->valign;
            if (va == VA_MIDDLE) dy = (row_h - ce->h) * 0.5f;
            else if (va == VA_BOTTOM) dy = row_h - ce->h;
            /* block_box reserved the cell's own decoration first: background,
             * then top/right/bottom/left borders. Stretch them to the row. */
            const Style *s = ce->n->style;
            int k = ce->start;
            if (s->has_bg && k < ce->end) p->boxes[k++].h = row_h;
            for (int side = 0; side < 4; side++) {
                if (s->border[side] <= 0 || k >= ce->end) continue;
                Box *b = &p->boxes[k++];
                if (side == 1 || side == 3) b->h = row_h;
                else if (side == 2) b->y = cy + row_h - s->border[2];
            }
            for (; k < ce->end; k++) p->boxes[k].y += dy;
        }
        if (rbg >= 0) set_fill(c, rbg, colx[0], cy, colx[ncols] - spacing - colx[0], row_h, rs->bg);
        cy += row_h + spacing;
    }
    float h = cy - y + tp.b + tb.b;
    if (ts->has_bg) set_fill(c, tbg, tx, y, table_w, h, ts->bg);
    if (tbd[0] >= 0) {
        set_fill(c, tbd[0], tx, y, table_w, tb.t, ts->bcolor);
        set_fill(c, tbd[1], tx + table_w - tb.r, y, tb.r, h, ts->bcolor);
        set_fill(c, tbd[2], tx, y + h - tb.b, table_w, tb.b, ts->bcolor);
        set_fill(c, tbd[3], tx, y, tb.l, h, ts->bcolor);
    }
    free(cells);
    free(row_first);
    (void)pf;
    return h;
}

/* ------------------------------------------------------------- blocks */

/* Lays out a block's content as a box with border-box left bx and width bw,
 * starting at top y. Returns the border-box height. */
static float block_box(L *c, Node *n, const Style *s, float bx, float bw, float y, Flow *parent, float *escape_mb) {
    Page *p = c->p;
    Edges pd = pad_of(s, bw), bd = border_of(s);
    int bg = -1, brd[4] = { -1, -1, -1, -1 };
    if (!c->measure) {
        if (s->has_bg) bg = reserve_fill(c);
        for (int i = 0; i < 4; i++) if (s->border[i] > 0) brd[i] = reserve_fill(c);
    }
    Flow f;
    flow_init(&f, bx + bd.l + pd.l, bx + bw - bd.r - pd.r, y + bd.t + pd.t, s->align);
    f.empty_lh = gfx_line_height(FONT_BODY, s->fsize * c->fscale);
    if (s->display == D_LIST_ITEM && s->list != LS_NONE) {
        f.marker = s->list;
        f.marker_size = s->fsize * c->fscale;
        f.marker_color = pick_color(s->color, s->eff_bg, false);
        int num = 1;
        const Node *list = n->parent;
        if (list && list->tag == TAG_OL && dom_attr(list, "start")) num = atoi(dom_attr(list, "start"));
        if (dom_attr(n, "value")) num = atoi(dom_attr(n, "value"));
        else for (const Node *q = n->prev; q; q = q->prev) if (q->type == NT_ELEM && q->tag == TAG_LI) num++;
        f.marker_num = num;
    } else if (parent && parent->marker >= 0 && parent->at_start) {
        /* <li><div>text</div></li>: the marker goes on the child's first line */
        f.marker = parent->marker;
        f.marker_num = parent->marker_num;
        f.marker_size = parent->marker_size;
        f.marker_color = parent->marker_color;
        parent->marker = -1;
    }
    int fstart = p->nfloats;
    c->depth++;
    flow_children(c, &f, n, s);
    c->depth--;
    finish_line(c, &f);
    float bottom = f.y;
    bool closed_bottom = pd.b > 0 || bd.b > 0;
    if (closed_bottom) bottom += f.pending_margin;
    else if (escape_mb) *escape_mb = f.pending_margin;
    if (s->bfc && !c->measure) {
        float fb = floats_bottom(p, fstart, f.left, f.right, CL_BOTH);
        if (fb > bottom) bottom = fb;
    }
    float height = bottom + pd.b + bd.b - y;
    if ((s->height.u == U_PX || s->height.u == U_EM) && n->tag != TAG_IMG && !s->abspos) {
        float hh = css_len(s->height, 0, s->fsize, 0) + pd.t + pd.b + bd.t + bd.b;
        if (hh > height && hh < 4000) height = hh;
    }
    if (!c->measure) {
        if (bg >= 0) set_fill(c, bg, bx, y, bw, height, s->bg);
        if (brd[0] >= 0) set_fill(c, brd[0], bx, y, bw, bd.t, s->bcolor);
        if (brd[1] >= 0) set_fill(c, brd[1], bx + bw - bd.r, y, bd.r, height, s->bcolor);
        if (brd[2] >= 0) set_fill(c, brd[2], bx, y + height - bd.b, bw, bd.b, s->bcolor);
        if (brd[3] >= 0) set_fill(c, brd[3], bx, y, bd.l, height, s->bcolor);
    }
    return height;
}

static void layout_block(L *c, Flow *pf, Node *n, const Style *s) {
    finish_line(c, pf);
    Page *p = c->p;
    float cw = pf->right - pf->left;
    if (s->clear && !c->measure) {
        float fb = floats_bottom(p, 0, pf->left, pf->right, s->clear);
        if (fb > pf->y + pf->pending_margin) { pf->y = fb; pf->pending_margin = 0; }
    }
    Edges pd = pad_of(s, cw), bd = border_of(s);
    float ml = css_len(s->margin[3], cw, s->fsize, 0), mr = css_len(s->margin[1], cw, s->fsize, 0);
    float mt = css_len(s->margin[0], cw, s->fsize, 0), mb = css_len(s->margin[2], cw, s->fsize, 0);
    bool auto_l = s->margin[3].u == U_AUTO, auto_r = s->margin[1].u == U_AUTO;
    if (auto_l) ml = 0;
    if (auto_r) mr = 0;
    float extra = pd.l + pd.r + bd.l + bd.r;
    float bw = cw - ml - mr;
    bool has_w = s->width.u != U_UNSET && s->width.u != U_AUTO && !(c->measure && s->width.u == U_PCT);
    if (has_w) bw = css_len(s->width, cw, s->fsize, bw) + extra;
    if (s->maxw.u != U_UNSET && s->maxw.u != U_AUTO && !(c->measure && s->maxw.u == U_PCT)) {
        float mw = css_len(s->maxw, cw, s->fsize, bw) + extra;
        if (bw > mw) bw = mw;
    }
    if (!c->measure && bw > cw - ml - mr) bw = cw - ml - mr;
    if (bw < extra + 8) bw = extra + 8;
    if (!c->measure && bw < cw - ml - mr) {
        if (auto_l && auto_r) ml += (cw - ml - mr - bw) * 0.5f;
        else if (auto_l) ml = cw - mr - bw;
    }
    if (c->measure && has_w) {
        float line = (pf->left - c->origin) + ml + bw + mr;
        if (line > c->max_line) c->max_line = line;
        if (line > c->max_atom) c->max_atom = line;
    }
    /* collapse with the previous sibling's bottom margin */
    float top = pf->y + (pf->pending_margin > mt ? pf->pending_margin : mt);
    pf->pending_margin = 0;
    float esc = 0;
    float h = block_box(c, n, s, pf->left + ml, bw, top, pf, &esc);
    pf->y = top + h;
    pf->pending_margin = mb > esc ? mb : esc;
    pf->at_start = true;
    pf->x = pf->left;
    pf->space = false;
}

/* Frames aren't supported, so each <frame> becomes a link to open it. */
static void emit_text_str(L *c, Flow *f, const char *str, const Style *s) {
    Node tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.type = NT_TEXT;
    tmp.text = str;
    tmp.len = (int)strlen(str);
    emit_text(c, f, &tmp, s);
}

static __attribute__((noinline)) void emit_frame_link(L *c, Flow *f, Node *n, const Style *s) {
    if (n->index < 0) return;
    finish_line(c, f);
    const char *src = dom_attr(n, "src"), *name = dom_attr(n, "name");
    const char *file = src ? src : "";
    const char *slash = strrchr(file, '/');
    if (slash && slash[1]) file = slash + 1;
    char label[200];
    snprintf(label, sizeof(label), "Open part %d: %s", n->sub, name && *name ? name : file);
    Style ls = *s;
    ls.underline = 1;
    ls.color = (Color){ DEFAULT_LINK_R, 0, 0, 0 };
    int saved = c->link;
    c->link = n->index;
    emit_text_str(c, f, label, &ls);
    c->link = saved;
    finish_line(c, f);
    f->pending_margin = 6;
}

/* Does this subtree contain a picture we can actually load? */
static bool contains_img(const Page *p, const Node *n, int depth) {
    if (n->tag == TAG_IMG)
        return n->index >= 0 && p->images[n->index].state != IMG_SKIPPED && p->images[n->index].state != IMG_FAILED;
    if (n->tag == TAG_NOSCRIPT || depth > 8) return false;
    for (const Node *c = n->first; c; c = c->next)
        if (c->type == NT_ELEM && contains_img(p, c, depth + 1)) return true;
    return false;
}

static bool has_text(const Node *n, int depth) {
    for (const Node *c = n->first; c; c = c->next) {
        if (c->type == NT_TEXT) {
            for (int i = 0; i < c->len; i++) if (!isspace((unsigned char)c->text[i]) && c->text[i] != 1) return true;
        } else if (depth < 8 && has_text(c, depth + 1)) {
            return true;
        }
    }
    return false;
}

/* <noscript><img></noscript> next to a lazy-loaded <img> is the same picture
 * again (meant for browsers without JavaScript): show only one of them. */
static bool duplicate_noscript(const Page *p, const Node *ns) {
    bool has_pic = false;
    for (const Node *c = ns->first; c && !has_pic; c = c->next)
        if (c->type == NT_ELEM && (c->tag == TAG_IMG || contains_img(p, c, 0))) has_pic = true;
    if (!has_pic || has_text(ns, 0)) return false;
    /* look for the loadable twin beside it, climbing through single-child wrappers */
    for (const Node *par = ns->parent, *from = ns; par && par->tag != TAG_BODY; from = par, par = par->parent) {
        for (const Node *sib = par->first; sib; sib = sib->next)
            if (sib != from && sib->type == NT_ELEM && contains_img(p, sib, 0)) return true;
        if (par->first != from || from->next) break;
    }
    return false;
}

/* Does this element contain anything visible (text, pictures, form controls)? */
static bool has_content(const Node *n, int depth) {
    if (depth > 64) return true;
    for (const Node *c = n->first; c; c = c->next) {
        if (c->type == NT_TEXT) {
            for (int i = 0; i < c->len; i++)
                if (!isspace((unsigned char)c->text[i]) && c->text[i] != 1) return true;
            continue;
        }
        if (!c->style || c->style->display == D_NONE) continue;
        if (c->tag == TAG_IMG || c->tag == TAG_INPUT || c->tag == TAG_SELECT || c->tag == TAG_BUTTON ||
            c->tag == TAG_TEXTAREA || c->tag == TAG_FRAME)
            return true;
        if (has_content(c, depth + 1)) return true;
    }
    return false;
}

static bool is_widget(int tag) {
    return tag == TAG_INPUT || tag == TAG_BUTTON || tag == TAG_SELECT || tag == TAG_TEXTAREA;
}

static void flow_children(L *c, Flow *f, Node *n, const Style *s) {
    for (Node *ch = n->first; ch; ch = ch->next) {
        if (ch->type == NT_TEXT) { emit_text(c, f, ch, s); continue; }
        const Style *cs = ch->style;
        if (!cs || cs->display == D_NONE) continue;
        /* empty positioned layers (decorative gradients, overlays) take no space */
        if (cs->abspos && ch->tag != TAG_IMG && !has_content(ch, 0)) continue;
        if (ch->tag == TAG_NOSCRIPT && duplicate_noscript(c->p, ch)) continue;
        if (ch->tag == TAG_BR) { forced_break(c, f); continue; }
        if (ch->tag == TAG_WBR) continue;
        if (ch->tag == TAG_IMG) { emit_image(c, f, ch, cs); continue; }
        if (ch->tag == TAG_FRAME) { emit_frame_link(c, f, ch, cs); continue; }
        if (ch->tag == TAG_FRAMESET && n->tag != TAG_FRAMESET && c->p->nframes) {
            finish_line(c, f);
            Style hs = *cs;
            hs.bold = 1;
            hs.fsize = cs->fsize * 1.2f;
            emit_text_str(c, f, "This page is split into frames, which are shown one at a time here.", &hs);
            finish_line(c, f);
            f->pending_margin = 10;
        }
        if (is_widget(ch->tag)) { emit_widget(c, f, ch, cs); continue; }
        if (cs->fl != FL_NONE) { layout_float(c, f, ch, cs); continue; }
        switch (cs->display) {
        case D_TABLE:
            layout_table(c, f, ch, cs);
            break;
        case D_INLINE: {
            if (c->depth > 100) { flow_children(c, f, ch, cs); break; }
            int saved = c->link;
            if (ch->tag == TAG_A && ch->index >= 0) c->link = ch->index;
            float pl = css_len(cs->padding[3], 0, cs->fsize, 0) + css_len(cs->margin[3], 0, cs->fsize, 0) + cs->border[3];
            float pr = css_len(cs->padding[1], 0, cs->fsize, 0) + css_len(cs->margin[1], 0, cs->fsize, 0) + cs->border[1];
            if (pl > 0 && pl < 40 && !f->at_start) f->x += pl;
            c->depth++;
            flow_children(c, f, ch, cs);
            c->depth--;
            if (pr > 0 && pr < 40 && !f->at_start) f->x += pr;
            c->link = saved;
            break;
        }
        default:
            if (c->depth > 100) {   /* absurdly deep nesting: flatten instead of recursing further */
                finish_line(c, f);
                flow_children(c, f, ch, cs);
                finish_line(c, f);
                break;
            }
            if (ch->tag == TAG_A && ch->index >= 0) {
                int saved = c->link;
                c->link = ch->index;
                layout_block(c, f, ch, cs);
                c->link = saved;
            } else {
                layout_block(c, f, ch, cs);
            }
            break;
        }
    }
}

/* ================================================================ public */

void page_init(Page *p) { memset(p, 0, sizeof(*p)); }

void page_free_images(Page *p) {
    for (int i = 0; i < p->nimages; i++) {
        free(p->images[i].url);
        gfx_image_free(&p->images[i].img);
    }
    p->nimages = 0;
    p->image_bytes = 0;
    p->relayout = false;
}

static void free_links(Page *p) {
    for (int i = 0; i < p->nlinks; i++) free(p->links[i]);
    p->nlinks = 0;
}

static void parse_job(void *arg) {
    Page *p = arg;
    p->dom = dom_parse(p->src ? p->src : "", p->src_len, p->plain);
    p->title[0] = 0;
    if (p->dom) {
        snprintf(p->title, sizeof(p->title), "%s", p->dom->title);
        int form = -1;
        prepass(p, p->dom->root, &form);
        sheets_init(p);
    }
}

void page_set_source(Page *p, char *src, size_t len, bool plain, const char *url) {
    page_free_images(p);
    free_links(p);
    sheets_free(p);
    dom_free(p->dom);
    arena_free(&p->style_arena);
    free(p->src);
    p->src = src;
    p->src_len = len;
    p->plain = plain;
    snprintf(p->url, sizeof(p->url), "%s", url);
    snprintf(p->base, sizeof(p->base), "%s", url);
    p->width = 0;
    p->nfields = 0;
    p->nforms = 0;
    p->nboxes = 0;
    p->has_author_styles = false;
    p->nframes = 0;
    p->scroll = p->scroll_target = 0;
    p->dom = NULL;
    run_with_big_stack(parse_job, p);
    p->restyle = true;
}

static void layout_job(void *arg);

void page_layout(Page *p, float width, float text_scale) {
    p->width = width;
    p->scale = text_scale;
    run_with_big_stack(layout_job, p);
}

static void layout_job(void *arg) {
    Page *p = arg;
    float width = p->width, text_scale = p->scale;
    p->nboxes = 0;
    p->arena_len = 0;
    p->nfloats = 0;
    p->width = width;
    p->scale = text_scale;
    p->relayout = false;
    p->content_h = 0;
    p->measure_left = 20000;
    if (!p->dom) return;
    if (p->restyle || p->styled_author != (g_set.page_styles != 0)) restyle(p);
    L c;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.fscale = 1.15f * text_scale;
    c.link = -1;
    Flow f;
    flow_init(&f, 0, width, 0, TA_LEFT);
    Node *root = p->dom->root;
    if (root->style) flow_children(&c, &f, root, root->style);
    finish_line(&c, &f);
    float bottom = f.y + f.pending_margin;
    float fb = floats_bottom(p, 0, 0, width, CL_BOTH);
    if (fb > bottom) bottom = fb;
    p->content_h = bottom + 12;
}

void page_free(Page *p) {
    page_free_images(p);
    free_links(p);
    sheets_free(p);
    dom_free(p->dom);
    arena_free(&p->style_arena);
    free(p->src);
    free(p->boxes);
    free(p->arena);
    free(p->links);
    memset(p, 0, sizeof(*p));
}

/* ================================================================ drawing */

static void clip_inter(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    float x0 = fmaxf(ax, bx), y0 = fmaxf(ay, by);
    float x1 = fminf(ax + aw, bx + bw), y1 = fminf(ay + ah, by + bh);
    gfx_set_clip(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

void page_draw(Page *p, float x, float y, float w, float h, int hover_link, int hover_field) {
    const Theme *t = theme();
    float top = p->scroll;
    gfx_set_clip(x, y, w, h);
    for (int i = 0; i < p->nboxes; i++) {
        Box *b = &p->boxes[i];
        if (!isfinite(b->x) || !isfinite(b->y) || !isfinite(b->w) || !isfinite(b->h) || b->w > 8192 || b->h > 8192 ||
            b->w < 0 || b->h < 0)
            continue;   /* never hand the GPU nonsense */
        if (b->y + b->h < top || b->y > top + h) continue;
        float bx = x + b->x, by = y + b->y - top;
        switch (b->type) {
        case BOX_FILL:
            if (b->bg.a) gfx_rect(bx, by, b->w, b->h, b->bg);
            break;
        case BOX_TEXT: {
            Color c = b->color;
            if (b->flags & BF_BG) gfx_rect(bx - 1, by, b->w + 2, b->h, b->bg);
            if (b->link >= 0 && b->link == hover_link) {
                gfx_rrect(bx - 3, by - 1, b->w + 6, b->h + 2, 5, color_alpha(t->accent, 0.3f));
                c = t->page_link_hover;
            }
            gfx_text((FontId)b->font, b->size, bx, by, c, p->arena + b->text, b->len);
            if (b->flags & BF_UNDERLINE) gfx_rect(bx, by + b->h - 3, b->w, b->link == hover_link ? 2 : 1, color_alpha(c, 0.8f));
            if (b->flags & BF_STRIKE) gfx_rect(bx, by + b->h * 0.55f, b->w, 1.5f, c);
            break;
        }
        case BOX_BULLET:
            if (b->flags & BF_SQUARE) gfx_rect(bx, by + b->h * 0.2f, b->w, b->w, b->color);
            else if (b->flags & BF_CIRCLE) gfx_ring(bx + b->w * 0.5f, by + b->h * 0.5f, b->w * 0.5f, 1.2f, 0, 6.2832f, b->color);
            else gfx_circle(bx + b->w * 0.5f, by + b->h * 0.5f, b->w * 0.5f, b->color);
            break;
        case BOX_IMAGE: {
            if (b->text < 0 || b->text >= p->nimages) break;
            PageImage *im = &p->images[b->text];
            if (im->state == IMG_READY) {
                gfx_image_draw(&im->img, bx, by, b->w, b->h, rgba(255, 255, 255, 255));
            } else {
                Color base = luminance(p->bg) > 0.5f ? rgba(0, 0, 0, 22) : rgba(255, 255, 255, 30);
                Color ink = luminance(p->bg) > 0.5f ? rgba(0, 0, 0, 70) : rgba(255, 255, 255, 90);
                gfx_rrect(bx, by, b->w, b->h, 6, base);
                float s = fminf(b->w, b->h) * 0.35f;
                if (s > 40) s = 40;
                float cx = bx + b->w * 0.5f, cy = by + b->h * 0.5f;
                if (s > 6) {
                    gfx_tri(cx - s, cy + s * 0.6f, cx - s * 0.3f, cy - s * 0.3f, cx + s * 0.3f, cy + s * 0.6f, ink);
                    gfx_tri(cx, cy + s * 0.6f, cx + s * 0.5f, cy, cx + s, cy + s * 0.6f, ink);
                    gfx_circle(cx + s * 0.55f, cy - s * 0.45f, s * 0.18f, ink);
                }
            }
            if (b->link >= 0 && b->link == hover_link)
                gfx_rrect_outline(bx - 3, by - 3, b->w + 6, b->h + 6, 6, 3, t->accent);
            break;
        }
        case BOX_INPUT: {
            Field *f = &p->fields[b->field];
            bool hov = (b->field == hover_field);
            gfx_rrect(bx, by, b->w, b->h, 6, rgba(255, 255, 255, 255));
            gfx_rrect_outline(bx, by, b->w, b->h, 6, hov ? 2.5f : 1.5f, hov ? t->accent : rgba(150, 150, 160, 255));
            char shown[256];
            int n = (int)strlen(f->value);
            const char *txt = shown;
            Color col = rgba(20, 20, 28, 255);
            for (int k = 0; k <= n; k++) shown[k] = (f->type == FIELD_PASSWORD && k < n) ? '*' : f->value[k];
            if (!n && f->label[0]) { txt = f->label; n = (int)strlen(f->label); col = rgba(140, 140, 150, 255); }
            clip_inter(x, y, w, h, bx + 4, by, b->w - 8, b->h);
            gfx_text(FONT_BODY, b->size, bx + 7, by + 5, col, txt, n);
            gfx_set_clip(x, y, w, h);
            break;
        }
        case BOX_SUBMIT:
        case BOX_SELECT: {
            bool hov = (b->field == hover_field);
            if (hov) gfx_rrect(bx - 3, by - 3, b->w + 6, b->h + 6, 10, color_alpha(t->accent, 0.5f));
            gfx_rrect_vgrad(bx, by, b->w, b->h, 8, rgba(250, 250, 252, 255), rgba(216, 218, 226, 255));
            gfx_rrect_outline(bx, by, b->w, b->h, 8, 1.2f, rgba(140, 142, 156, 255));
            if (b->type == BOX_SUBMIT) {
                gfx_text(FONT_UI, b->size, bx + 15, by + 5, rgba(30, 30, 40, 255), p->arena + b->text, b->len);
            } else {
                gfx_text(FONT_BODY, b->size, bx + 10, by + 5, rgba(30, 30, 40, 255), p->arena + b->text, b->len);
                float ax = bx + b->w - 18, ay = by + b->h * 0.5f;
                gfx_tri(ax, ay - 3, ax + 10, ay - 3, ax + 5, ay + 4, rgba(80, 80, 96, 255));
            }
            break;
        }
        case BOX_CHECK: {
            Field *f = &p->fields[b->field];
            bool hov = (b->field == hover_field);
            Color edge = hov ? t->accent : rgba(120, 120, 135, 255);
            if (b->flags & BF_RADIO) {
                gfx_circle(bx + b->w * 0.5f, by + b->h * 0.5f, b->w * 0.5f, rgba(255, 255, 255, 255));
                gfx_ring(bx + b->w * 0.5f, by + b->h * 0.5f, b->w * 0.5f, 1.8f, 0, 6.2832f, edge);
                if (f->checked) gfx_circle(bx + b->w * 0.5f, by + b->h * 0.5f, b->w * 0.25f, t->accent);
            } else {
                gfx_rrect(bx, by, b->w, b->h, 3, rgba(255, 255, 255, 255));
                gfx_rrect_outline(bx, by, b->w, b->h, 3, 1.8f, edge);
                if (f->checked) {
                    gfx_line(bx + b->w * 0.2f, by + b->h * 0.55f, bx + b->w * 0.42f, by + b->h * 0.78f, 2.5f, t->accent);
                    gfx_line(bx + b->w * 0.42f, by + b->h * 0.78f, bx + b->w * 0.82f, by + b->h * 0.25f, 2.5f, t->accent);
                }
            }
            break;
        }
        }
    }
    gfx_clear_clip();
}

int page_hit(Page *p, float px, float py, int *field_out) {
    *field_out = -1;
    for (int i = p->nboxes - 1; i >= 0; i--) {
        Box *b = &p->boxes[i];
        if (b->link < 0 && b->field < 0) continue;
        if (px >= b->x - 2 && px < b->x + b->w + 2 && py >= b->y - 2 && py < b->y + b->h + 2) {
            if (b->field >= 0) { *field_out = b->field; return -1; }
            return b->link;
        }
    }
    return -1;
}

void page_toggle_field(Page *p, int f) {
    if (f < 0 || f >= p->nfields) return;
    Field *fd = &p->fields[f];
    if (fd->type == FIELD_CHECK) {
        fd->checked = !fd->checked;
    } else if (fd->type == FIELD_RADIO) {
        for (int i = 0; i < p->nfields; i++)
            if (p->fields[i].type == FIELD_RADIO && p->fields[i].form == fd->form && !strcmp(p->fields[i].name, fd->name))
                p->fields[i].checked = 0;
        fd->checked = 1;
    }
}

bool page_build_submit(Page *p, int field_idx, int submit_field, char *url_out, size_t url_len, char **post_out) {
    *post_out = NULL;
    if (field_idx < 0 || field_idx >= p->nfields) return false;
    int form = p->fields[field_idx].form;
    if (form < 0 || form >= p->nforms) return false;
    size_t cap = 4096, n = 0;
    char *q = malloc(cap);
    if (!q) return false;
    q[0] = 0;
    for (int i = 0; i < p->nfields; i++) {
        Field *f = &p->fields[i];
        if (f->form != form || !f->name[0]) continue;
        if (f->type == FIELD_SUBMIT && i != submit_field) continue;
        if ((f->type == FIELD_CHECK || f->type == FIELD_RADIO) && !f->checked) continue;
        char en[200], ev[800];
        url_encode_component(f->name, en, sizeof(en));
        url_encode_component(f->value, ev, sizeof(ev));
        n += snprintf(q + n, cap - n, "%s%s=%s", n ? "&" : "", en, ev);
        if (n >= cap - 1) break;
    }
    Form *fm = &p->forms[form];
    if (fm->post) {
        snprintf(url_out, url_len, "%s", fm->action);
        *post_out = q;
    } else {
        char action[URL_MAX];
        snprintf(action, sizeof(action), "%s", fm->action);
        char *qm = strchr(action, '?');
        if (qm) *qm = 0;
        snprintf(url_out, url_len, "%s?%s", action, q);
        free(q);
    }
    return true;
}
