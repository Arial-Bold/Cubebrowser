/* page.c - a forgiving single-pass HTML renderer.
 *
 * Supported: text, headings, paragraphs, line breaks, bold, links, lists,
 * blockquotes, preformatted text, horizontal rules, simple tables (cells flow
 * inline, rows break), image alt text, and forms (text/password/hidden inputs,
 * textareas, selects (first/selected option), submit buttons; GET and POST).
 * Not supported: CSS layout, JavaScript, images, frames.
 */
#include "page.h"
#include "theme.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PAD 16.0f
#define WORD_MAX 400

/* ------------------------------------------------------- text conversion */

/* Approximates a Unicode code point with ASCII the bitmap font can draw. */
static const char *ascii_for(unsigned cp, char *one) {
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == ' ') { one[0] = (char)cp; one[1] = 0; return one; }
    if (cp < 32 || cp == 127) return "";
    if (cp < 127) { one[0] = (char)cp; one[1] = 0; return one; }
    if (cp >= 0xC0 && cp <= 0xFF) { one[0] = latin1[cp - 0xC0]; one[1] = 0; return one; }
    switch (cp) {
    case 0xA0: return " ";
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return "";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0xAB: return "<<";
    case 0xBB: return ">>";
    case 0xB7: case 0x2022: case 0x25CF: case 0x2023: return "*";
    case 0xB0: return "deg";
    case 0xB1: return "+/-";
    case 0xBD: return "1/2";
    case 0xBC: return "1/4";
    case 0xA3: return "GBP";
    case 0xA2: return "c";
    case 0xA5: return "JPY";
    case 0xA7: return "S";
    case 0xBF: return "?";
    case 0xA1: return "!";
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x2039: case 0x203A: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return "-";
    case 0x2014: case 0x2015: return "--";
    case 0x2026: return "...";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0x2191: return "^";
    case 0x2193: return "v";
    case 0x2009: case 0x2002: case 0x2003: case 0x202F: return " ";
    default: return "?";
    }
}

/* Decodes one UTF-8 sequence (falling back to Latin-1 for invalid bytes). */
static unsigned utf8_next(const unsigned char *s, size_t len, size_t *i) {
    unsigned c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0 || *i + (size_t)n >= len) { (*i)++; return c; }
    unsigned cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        unsigned cc = s[*i + k];
        if ((cc & 0xC0) != 0x80) { (*i)++; return c; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += (size_t)n + 1;
    return cp;
}

static const struct { const char *name; unsigned cp; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 },
    { "middot", 0xB7 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "trade", 0x2122 }, { "euro", 0x20AC },
    { "times", 0xD7 }, { "deg", 0xB0 }, { "pound", 0xA3 }, { "cent", 0xA2 }, { "yen", 0xA5 },
    { "sect", 0xA7 }, { "frac12", 0xBD }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 },
    { "darr", 0x2193 }, { "shy", 0xAD }, { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "ouml", 0xF6 },
    { "uuml", 0xFC }, { "auml", 0xE4 }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 }, { "szlig", 0xDF },
    { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "minus", 0x2212 }, { "prime", 0x2032 },
};

/* Parses an entity starting at s[0] == '&'. Returns chars consumed (0 if not an entity). */
static size_t parse_entity(const char *s, size_t len, unsigned *cp) {
    if (len < 3) return 0;
    if (s[1] == '#') {
        size_t i = 2;
        unsigned v = 0;
        bool hex = (i < len && (s[i] == 'x' || s[i] == 'X'));
        if (hex) i++;
        size_t start = i;
        while (i < len && i < 12 && (hex ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i]))) {
            v = v * (hex ? 16 : 10) + (unsigned)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10));
            i++;
        }
        if (i == start) return 0;
        if (i < len && s[i] == ';') i++;
        *cp = v;
        return i;
    }
    size_t i = 1;
    char name[12];
    while (i < len && i < 11 && isalnum((unsigned char)s[i])) { name[i - 1] = s[i]; i++; }
    name[i - 1] = 0;
    for (size_t k = 0; k < sizeof(entities) / sizeof(entities[0]); k++) {
        if (strcmp(name, entities[k].name) == 0) {
            *cp = entities[k].cp;
            if (i < len && s[i] == ';') i++;
            return i;
        }
    }
    return 0;
}

/* Decodes entities and UTF-8 in place to plain ASCII. */
static void decode_inplace(char *s) {
    size_t len = strlen(s), o = 0, i = 0;
    char out[URL_MAX];
    while (i < len && o + 4 < sizeof(out)) {
        unsigned cp;
        size_t n;
        if (s[i] == '&' && (n = parse_entity(s + i, len - i, &cp)) > 0) i += n;
        else cp = utf8_next((const unsigned char *)s, len, &i);
        char one[2];
        const char *a = (cp < 128) ? (one[0] = (char)cp, one[1] = 0, one) : ascii_for(cp, one);
        while (*a && o + 1 < sizeof(out)) out[o++] = *a++;
    }
    out[o] = 0;
    memcpy(s, out, o + 1);
}

/* --------------------------------------------------------------- arena */

static int arena_add(Page *p, const char *s, int len) {
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
        int nc = p->capboxes ? p->capboxes * 2 : 1024;
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
        int *nl = realloc(p->links, sizeof(int) * (size_t)nc);
        if (!nl) return -1;
        p->links = nl;
        p->caplinks = nc;
    }
    p->links[p->nlinks] = arena_add(p, url, (int)strlen(url));
    return p->nlinks++;
}

const char *page_link_url(Page *p, int link) {
    if (link < 0 || link >= p->nlinks) return "";
    return p->arena + p->links[link];
}

/* --------------------------------------------------------- layout state */

typedef struct {
    Page *p;
    float left_base, right;
    float indent, x, y, line_h, pending_gap;
    int line_start;
    bool at_line_start, space_pending;
    float base;
    int bold, heading, pre, dim, link, form, in_title, in_head_skip;
    int list_depth;
    int list_num[16];
    bool list_ordered[16];
    char word[WORD_MAX];
    int wlen;
    bool show_alt;
} LS;

static float left(LS *s) { return s->left_base + s->indent; }

static FontId cur_font(LS *s) { return (s->bold || s->heading) ? FONT_UI : FONT_BODY; }

static float cur_size(LS *s) {
    static const float mult[7] = { 1.0f, 1.7f, 1.45f, 1.25f, 1.1f, 1.0f, 0.9f };
    return s->base * mult[s->heading];
}

static void newline(LS *s) {
    Page *p = s->p;
    float lh = s->line_h > 0 ? s->line_h : gfx_line_height(FONT_BODY, s->base);
    for (int i = s->line_start; i < p->nboxes; i++) {
        Box *b = &p->boxes[i];
        if (b->type == BOX_TEXT || b->type == BOX_BULLET) b->y = s->y + lh - b->h;
        else b->y = s->y + (lh - b->h) * 0.5f;
    }
    s->y += lh;
    s->x = left(s);
    s->line_h = 0;
    s->at_line_start = true;
    s->space_pending = false;
    s->line_start = p->nboxes;
}

static void block_break(LS *s, float gap) {
    if (!s->at_line_start) newline(s);
    if (gap > s->pending_gap) s->pending_gap = gap;
}

/* Prepares to place an inline box of width w and height h; returns its x. */
static float place(LS *s, float w, float h) {
    if (s->at_line_start) {
        if (s->pending_gap > 0 && s->y > 0) s->y += s->pending_gap;
        s->pending_gap = 0;
        s->x = left(s);
    } else {
        if (s->space_pending) s->x += gfx_text_width(cur_font(s), cur_size(s), " ", 1);
        if (s->x + w > s->right && !s->pre) {
            newline(s);
            s->pending_gap = 0;
            s->x = left(s);
        }
    }
    float x = s->x;
    s->x += w;
    if (h > s->line_h) s->line_h = h;
    s->at_line_start = false;
    s->space_pending = false;
    return x;
}

static void add_text_box(LS *s, const char *t, int n, float w) {
    Page *p = s->p;
    FontId f = cur_font(s);
    float size = cur_size(s);
    float h = gfx_line_height(f, size);
    float x = place(s, w, h);
    Box *b = box_new(p);
    if (!b) return;
    b->type = BOX_TEXT;
    b->x = x; b->y = s->y; b->w = w; b->h = h;
    b->size = size;
    b->flags = (unsigned char)((s->bold ? BF_BOLD : 0) | (s->heading ? BF_HEADING | BF_BOLD : 0) |
                               (s->dim ? BF_DIM : 0) | (s->pre ? BF_PRE : 0));
    b->link = s->link;
    b->text = arena_add(p, t, n);
    b->len = n;
}

static void emit_text(LS *s, const char *t, int n) {
    if (n <= 0) return;
    Page *p = s->p;
    if (s->in_title) {
        size_t tl = strlen(p->title);
        if (tl && tl + 1 < sizeof(p->title)) p->title[tl++] = ' ';
        snprintf(p->title + tl, sizeof(p->title) - tl, "%.*s", n, t);
        return;
    }
    FontId f = cur_font(s);
    float size = cur_size(s);
    float w = gfx_text_width(f, size, t, n);
    float avail = s->right - left(s);
    if (w <= avail || s->pre) { add_text_box(s, t, n, w); return; }
    /* a single word wider than the column (e.g. a long URL): split it */
    while (n > 0) {
        float room = s->at_line_start ? avail : s->right - s->x;
        int k = gfx_text_fit(f, size, t, n, room);
        if (k <= 0) {
            if (!s->at_line_start) { newline(s); continue; }
            k = 1;
        }
        add_text_box(s, t, k, gfx_text_width(f, size, t, k));
        t += k;
        n -= k;
        if (n > 0) newline(s);
    }
}

static void flush_word(LS *s) {
    if (s->wlen) emit_text(s, s->word, s->wlen);
    s->wlen = 0;
}

static void put_char(LS *s, char c) {
    if (s->wlen >= WORD_MAX - 1) flush_word(s);
    s->word[s->wlen++] = c;
}

static void put_text_cp(LS *s, unsigned cp) {
    char one[2];
    const char *a = ascii_for(cp, one);
    for (; *a; a++) {
        char c = *a;
        if (c == '\r') continue;
        if (s->pre && !s->in_title) {
            if (c == '\n') { flush_word(s); if (s->at_line_start) s->y += gfx_line_height(FONT_BODY, s->base); else newline(s); }
            else if (c == '\t') { for (int k = 0; k < 4; k++) put_char(s, ' '); }
            else put_char(s, c);
        } else if (c == ' ' || c == '\n' || c == '\t') {
            flush_word(s);
            if (!s->at_line_start || s->in_title) s->space_pending = true;
        } else {
            put_char(s, c);
        }
    }
}

/* ------------------------------------------------------------ attributes */

static bool get_attr(const char *a, int alen, const char *name, char *out, size_t outlen) {
    int nlen = (int)strlen(name);
    int i = 0;
    while (i < alen) {
        while (i < alen && (isspace((unsigned char)a[i]) || a[i] == '/')) i++;
        int ks = i;
        while (i < alen && !isspace((unsigned char)a[i]) && a[i] != '=' && a[i] != '>' && a[i] != '/') i++;
        int klen = i - ks;
        while (i < alen && isspace((unsigned char)a[i])) i++;
        const char *v = "";
        int vlen = 0;
        if (i < alen && a[i] == '=') {
            i++;
            while (i < alen && isspace((unsigned char)a[i])) i++;
            if (i < alen && (a[i] == '"' || a[i] == '\'')) {
                char q = a[i++];
                v = a + i;
                while (i < alen && a[i] != q) i++;
                vlen = (int)(a + i - v);
                if (i < alen) i++;
            } else {
                v = a + i;
                while (i < alen && !isspace((unsigned char)a[i]) && a[i] != '>') i++;
                vlen = (int)(a + i - v);
            }
        }
        if (klen == nlen && strncasecmp(a + ks, name, (size_t)nlen) == 0) {
            if (out) {
                snprintf(out, outlen, "%.*s", vlen, v);
                decode_inplace(out);
            }
            return true;
        }
        if (klen == 0) i++;
    }
    return false;
}

/* Finds "</name" (case-insensitive) from pos; returns index of '<' or len. */
static size_t find_close(const char *src, size_t len, size_t pos, const char *name) {
    size_t nl = strlen(name);
    for (size_t i = pos; i + nl + 2 <= len; i++)
        if (src[i] == '<' && src[i + 1] == '/' && strncasecmp(src + i + 2, name, nl) == 0) return i;
    return len;
}

static size_t skip_past_tag_end(const char *src, size_t len, size_t pos) {
    while (pos < len && src[pos] != '>') pos++;
    return pos < len ? pos + 1 : len;
}

/* Strips tags and decodes entities from src[a..b) into out. */
static void inner_text(const char *src, size_t a, size_t b, char *out, size_t outlen) {
    size_t o = 0;
    bool in_tag = false, space = false;
    for (size_t i = a; i < b && o + 2 < outlen; i++) {
        char c = src[i];
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (isspace((unsigned char)c)) { space = (o > 0); continue; }
        if (space) { out[o++] = ' '; space = false; }
        out[o++] = c;
    }
    out[o] = 0;
    decode_inplace(out);
}

/* --------------------------------------------------------------- fields */

static int field_add(LS *s, int type, const char *name, const char *value) {
    Page *p = s->p;
    if (p->nfields >= PAGE_MAX_FIELDS) return -1;
    Field *f = &p->fields[p->nfields];
    f->form = s->form;
    f->type = (unsigned char)type;
    snprintf(f->name, sizeof(f->name), "%s", name);
    snprintf(f->value, sizeof(f->value), "%s", value);
    return p->nfields++;
}

static void add_input_box(LS *s, int field, float w) {
    float h = gfx_line_height(FONT_BODY, s->base) + 10;
    float x = place(s, w, h);
    Box *b = box_new(s->p);
    if (!b) return;
    b->type = BOX_INPUT;
    b->x = x; b->y = s->y; b->w = w; b->h = h;
    b->size = s->base;
    b->field = (short)field;
    s->space_pending = true;
}

static void add_submit_box(LS *s, int field, const char *label) {
    float ts = s->base * 0.95f;
    int n = (int)strlen(label);
    float w = gfx_text_width(FONT_UI, ts, label, n) + 30;
    float h = gfx_line_height(FONT_UI, ts) + 10;
    float x = place(s, w, h);
    Box *b = box_new(s->p);
    if (!b) return;
    b->type = BOX_SUBMIT;
    b->x = x; b->y = s->y; b->w = w; b->h = h;
    b->size = ts;
    b->field = (short)field;
    b->text = arena_add(s->p, label, n);
    b->len = n;
    s->space_pending = true;
}

/* ----------------------------------------------------------------- tags */

static bool is_block(const char *n) {
    static const char *const blocks[] = {
        "p", "div", "section", "article", "header", "footer", "nav", "main", "aside", "form", "table",
        "dl", "dt", "dd", "center", "figure", "figcaption", "address", "fieldset", "details", "summary",
        "caption", "tbody", "thead", "tfoot", "noscript", "body", "html", "legend", NULL
    };
    for (int i = 0; blocks[i]; i++)
        if (!strcmp(n, blocks[i])) return true;
    return false;
}

static size_t handle_tag(LS *s, const char *src, size_t len, size_t after, const char *name, bool closing,
                         const char *attrs, int alen) {
    Page *p = s->p;
    float lh = gfx_line_height(FONT_BODY, s->base);
    char val[URL_MAX];

    /* elements whose contents are never shown */
    if (!closing && (!strcmp(name, "script") || !strcmp(name, "style") || !strcmp(name, "template") ||
                     !strcmp(name, "svg") || !strcmp(name, "math") || !strcmp(name, "object"))) {
        size_t c = find_close(src, len, after, name);
        return skip_past_tag_end(src, len, c);
    }
    if (!strcmp(name, "title")) {
        flush_word(s);
        s->in_title = closing ? 0 : 1;
        s->space_pending = false;
        return after;
    }
    if (!strcmp(name, "base") && !closing) {
        if (get_attr(attrs, alen, "href", val, sizeof(val))) url_resolve(p->url, val, p->base, sizeof(p->base));
        return after;
    }
    if (s->in_title) return after;

    flush_word(s);

    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block_break(s, lh * 0.6f);
        s->heading = closing ? 0 : name[1] - '0';
        if (closing) block_break(s, lh * 0.4f);
        return after;
    }
    if (!strcmp(name, "br")) {
        if (s->at_line_start) s->y += lh;
        else newline(s);
        return after;
    }
    if (!strcmp(name, "p")) { block_break(s, lh * 0.5f); return after; }
    if (!strcmp(name, "hr") && !closing) {
        block_break(s, lh * 0.3f);
        float x = place(s, s->right - left(s), 10);
        Box *b = box_new(p);
        if (b) { b->type = BOX_RULE; b->x = x; b->y = s->y; b->w = s->right - x; b->h = 10; }
        newline(s);
        block_break(s, lh * 0.3f);
        return after;
    }
    if (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "th") || !strcmp(name, "dt")) {
        if (!strcmp(name, "th") && !closing && !s->at_line_start) { s->x += 16; s->space_pending = true; }
        s->bold += closing ? -1 : 1;
        if (s->bold < 0) s->bold = 0;
        if (!strcmp(name, "dt")) block_break(s, 0);
        return after;
    }
    if (!strcmp(name, "td")) {
        if (!closing && !s->at_line_start) { s->x += 16; s->space_pending = true; }
        return after;
    }
    if (!strcmp(name, "tr")) { block_break(s, 2); return after; }
    if (!strcmp(name, "a")) {
        if (closing) { s->link = -1; return after; }
        s->link = -1;
        if (get_attr(attrs, alen, "href", val, sizeof(val)) && val[0] && strncasecmp(val, "javascript:", 11) &&
            strncasecmp(val, "mailto:", 7)) {
            char abs[URL_MAX];
            url_resolve(p->base, val, abs, sizeof(abs));
            s->link = link_add(p, abs);
        }
        return after;
    }
    if (!strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "menu")) {
        block_break(s, s->list_depth ? 0 : lh * 0.3f);
        if (!closing && s->list_depth < 15) {
            s->list_depth++;
            s->list_ordered[s->list_depth] = !strcmp(name, "ol");
            s->list_num[s->list_depth] = 0;
            s->indent += 28;
        } else if (closing && s->list_depth > 0) {
            s->list_depth--;
            s->indent -= 28;
            if (!s->list_depth) block_break(s, lh * 0.3f);
        }
        return after;
    }
    if (!strcmp(name, "li") && !closing) {
        block_break(s, 3);
        if (s->pending_gap > 0 && s->y > 0) s->y += s->pending_gap;
        s->pending_gap = 0;
        int d = s->list_depth;
        Box *b = box_new(p);
        if (!b) return after;
        if (d > 0 && s->list_ordered[d]) {
            char num[12];
            int n = snprintf(num, sizeof(num), "%d.", ++s->list_num[d]);
            float w = gfx_text_width(FONT_BODY, s->base, num, n);
            b->type = BOX_TEXT;
            b->x = left(s) - w - 6; b->y = s->y; b->w = w; b->h = lh; b->size = s->base;
            b->text = arena_add(p, num, n);
            b->len = n;
        } else {
            b->type = BOX_BULLET;
            b->x = left(s) - 15; b->w = 6; b->h = lh * 0.62f; b->y = s->y;
        }
        return after;
    }
    if (!strcmp(name, "blockquote")) {
        block_break(s, lh * 0.4f);
        s->indent += closing ? -24 : 24;
        if (s->indent < 0) s->indent = 0;
        s->dim += closing ? -1 : 1;
        if (s->dim < 0) s->dim = 0;
        return after;
    }
    if (!strcmp(name, "pre") || !strcmp(name, "listing") || !strcmp(name, "xmp")) {
        block_break(s, lh * 0.4f);
        s->pre += closing ? -1 : 1;
        if (s->pre < 0) s->pre = 0;
        return after;
    }
    if (!strcmp(name, "img") && !closing) {
        char alt[200] = "";
        get_attr(attrs, alen, "alt", alt, sizeof(alt));
        char shown[220];
        if (alt[0] && s->show_alt) snprintf(shown, sizeof(shown), "[%s]", alt);
        else if (s->link >= 0) snprintf(shown, sizeof(shown), "[image]");
        else return after;
        s->dim++;
        char *w = shown;
        while (*w) {           /* emit word by word so it wraps */
            char *sp = strchr(w, ' ');
            int n = sp ? (int)(sp - w) : (int)strlen(w);
            emit_text(s, w, n);
            if (!sp) break;
            s->space_pending = true;
            w = sp + 1;
        }
        s->dim--;
        s->space_pending = true;
        return after;
    }
    if (!strcmp(name, "form")) {
        block_break(s, lh * 0.3f);
        if (closing) { s->form = -1; return after; }
        if (p->nforms < PAGE_MAX_FORMS) {
            Form *f = &p->forms[p->nforms];
            char act[URL_MAX] = "";
            get_attr(attrs, alen, "action", act, sizeof(act));
            url_resolve(p->base, act, f->action, sizeof(f->action));
            char method[16] = "";
            get_attr(attrs, alen, "method", method, sizeof(method));
            f->post = strcasecmp(method, "post") == 0;
            s->form = p->nforms++;
        }
        return after;
    }
    if (!strcmp(name, "input") && !closing) {
        char type[24] = "text", nm[64] = "";
        get_attr(attrs, alen, "type", type, sizeof(type));
        get_attr(attrs, alen, "name", nm, sizeof(nm));
        val[0] = 0;
        get_attr(attrs, alen, "value", val, sizeof(val));
        for (char *c = type; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(type, "hidden")) { field_add(s, FIELD_HIDDEN, nm, val); return after; }
        if (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "image")) {
            int f = field_add(s, FIELD_SUBMIT, nm, val);
            if (f >= 0) add_submit_box(s, f, val[0] ? val : (!strcmp(type, "image") ? "Go" : "Submit"));
            return after;
        }
        if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) {
            if (get_attr(attrs, alen, "checked", NULL, 0)) field_add(s, FIELD_HIDDEN, nm, val[0] ? val : "on");
            return after;
        }
        if (!strcmp(type, "reset") || !strcmp(type, "file")) return after;
        int f = field_add(s, !strcmp(type, "password") ? FIELD_PASSWORD : FIELD_TEXT, nm, val);
        if (f >= 0) {
            float w = s->base * 14;
            if (w > s->right - left(s)) w = s->right - left(s);
            add_input_box(s, f, w);
        }
        return after;
    }
    if (!strcmp(name, "textarea") && !closing) {
        char nm[64] = "";
        get_attr(attrs, alen, "name", nm, sizeof(nm));
        size_t c = find_close(src, len, after, "textarea");
        char content[256];
        inner_text(src, after, c, content, sizeof(content));
        int f = field_add(s, FIELD_TEXT, nm, content);
        if (f >= 0) {
            block_break(s, 4);
            add_input_box(s, f, s->right - left(s));
        }
        return skip_past_tag_end(src, len, c);
    }
    if (!strcmp(name, "button") && !closing) {
        char nm[64] = "", type[16] = "submit";
        get_attr(attrs, alen, "name", nm, sizeof(nm));
        get_attr(attrs, alen, "type", type, sizeof(type));
        val[0] = 0;
        get_attr(attrs, alen, "value", val, sizeof(val));
        size_t c = find_close(src, len, after, "button");
        char label[64];
        inner_text(src, after, c, label, sizeof(label));
        if (strcasecmp(type, "submit") == 0 && s->form >= 0) {
            int f = field_add(s, FIELD_SUBMIT, nm, val);
            if (f >= 0) add_submit_box(s, f, label[0] ? label : "Submit");
        } else if (label[0]) {
            emit_text(s, label, (int)strlen(label));
        }
        return skip_past_tag_end(src, len, c);
    }
    if (!strcmp(name, "select") && !closing) {
        char nm[64] = "";
        get_attr(attrs, alen, "name", nm, sizeof(nm));
        size_t c = find_close(src, len, after, "select");
        /* use the selected option, else the first */
        char chosen[256] = "";
        bool have = false;
        for (size_t i = after; i < c; i++) {
            if (src[i] != '<' || strncasecmp(src + i + 1, "option", 6) != 0) continue;
            size_t e = skip_past_tag_end(src, c, i);
            const char *oa = src + i + 7;
            int oalen = (int)(e - (size_t)(oa - src)) - 1;
            char v[256];
            if (!get_attr(oa, oalen, "value", v, sizeof(v))) {
                size_t end = e;
                while (end < c && src[end] != '<') end++;
                inner_text(src, e, end, v, sizeof(v));
            }
            if (!have || get_attr(oa, oalen, "selected", NULL, 0)) {
                snprintf(chosen, sizeof(chosen), "%s", v);
                if (have) break;
                have = true;
            }
        }
        if (have) field_add(s, FIELD_HIDDEN, nm, chosen);
        return skip_past_tag_end(src, len, c);
    }
    if (!strcmp(name, "li") || !strcmp(name, "option")) return after;
    if (is_block(name)) { block_break(s, !strcmp(name, "div") ? 0 : lh * 0.25f); return after; }
    return after;
}

/* --------------------------------------------------------------- driver */

void page_init(Page *p) {
    memset(p, 0, sizeof(*p));
}

void page_set_source(Page *p, char *src, size_t len, bool plain, const char *url) {
    free(p->src);
    p->src = src;
    p->src_len = len;
    p->plain = plain;
    snprintf(p->url, sizeof(p->url), "%s", url);
    p->width = 0;
    p->nfields = 0;
    p->scroll = p->scroll_target = 0;
}

void page_layout(Page *p, float width, float text_scale) {
    /* keep anything the user typed into form fields */
    Field saved[PAGE_MAX_FIELDS];
    int nsaved = p->nfields;
    memcpy(saved, p->fields, sizeof(Field) * (size_t)nsaved);

    p->nboxes = 0;
    p->arena_len = 0;
    p->nlinks = 0;
    p->nfields = 0;
    p->nforms = 0;
    p->title[0] = 0;
    p->width = width;
    p->scale = text_scale;
    snprintf(p->base, sizeof(p->base), "%s", p->url);

    LS s;
    memset(&s, 0, sizeof(s));
    s.p = p;
    s.left_base = PAD;
    s.right = width - PAD;
    s.base = 19.0f * text_scale;
    s.x = PAD;
    s.y = PAD;
    s.at_line_start = true;
    s.link = -1;
    s.form = -1;
    s.pre = p->plain ? 1 : 0;
    s.show_alt = g_set.show_alt != 0;

    const char *src = p->src ? p->src : "";
    size_t len = p->src_len, i = 0;
    while (i < len) {
        char c = src[i];
        if (c == '<' && !p->plain) {
            if (i + 3 < len && src[i + 1] == '!' && src[i + 2] == '-' && src[i + 3] == '-') {
                const char *end = strstr(src + i + 4, "-->");
                i = end ? (size_t)(end - src) + 3 : len;
                continue;
            }
            if (i + 1 < len && (src[i + 1] == '!' || src[i + 1] == '?')) {
                i = skip_past_tag_end(src, len, i);
                continue;
            }
            size_t j = i + 1;
            bool closing = false;
            if (j < len && src[j] == '/') { closing = true; j++; }
            if (j >= len || !isalpha((unsigned char)src[j])) {  /* a lone '<' is text */
                put_text_cp(&s, '<');
                i++;
                continue;
            }
            char name[16];
            int nl = 0;
            while (j < len && (isalnum((unsigned char)src[j]) || src[j] == '-' || src[j] == ':')) {
                if (nl < 15) name[nl++] = (char)tolower((unsigned char)src[j]);
                j++;
            }
            name[nl] = 0;
            size_t as = j;
            char q = 0;
            while (j < len && (q || src[j] != '>')) {
                if (q) { if (src[j] == q) q = 0; }
                else if (src[j] == '"' || src[j] == '\'') q = src[j];
                j++;
            }
            int alen = (int)(j - as);
            size_t after = j < len ? j + 1 : len;
            i = handle_tag(&s, src, len, after, name, closing, src + as, alen);
            continue;
        }
        unsigned cp;
        size_t n;
        if (c == '&' && !p->plain && (n = parse_entity(src + i, len - i, &cp)) > 0) {
            i += n;
        } else {
            cp = utf8_next((const unsigned char *)src, len, &i);
        }
        put_text_cp(&s, cp);
    }
    flush_word(&s);
    if (!s.at_line_start) newline(&s);
    p->content_h = s.y + PAD * 2;

    /* trim title */
    char *t = p->title;
    while (*t == ' ') t++;
    memmove(p->title, t, strlen(t) + 1);

    if (nsaved == p->nfields) {
        for (int k = 0; k < nsaved; k++)
            if (!strcmp(saved[k].name, p->fields[k].name) && p->fields[k].type != FIELD_HIDDEN)
                memcpy(p->fields[k].value, saved[k].value, sizeof(saved[k].value));
    }
}

void page_free(Page *p) {
    free(p->src);
    free(p->boxes);
    free(p->arena);
    free(p->links);
    memset(p, 0, sizeof(*p));
}

/* ----------------------------------------------------------------- draw */

static void clip_inter(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    float x0 = ax > bx ? ax : bx, y0 = ay > by ? ay : by;
    float x1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    float y1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    gfx_set_clip(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

void page_draw(Page *p, float x, float y, float w, float h, int hover_link, int hover_field) {
    const Theme *t = theme();
    float top = p->scroll;
    gfx_set_clip(x, y, w, h);
    for (int i = 0; i < p->nboxes; i++) {
        Box *b = &p->boxes[i];
        if (b->y + b->h < top || b->y > top + h) continue;
        float bx = x + b->x, by = y + b->y - top;
        switch (b->type) {
        case BOX_TEXT: {
            Color c = t->page_text;
            if (b->flags & BF_HEADING) c = t->page_heading;
            if (b->flags & BF_DIM) c = t->page_dim;
            if (b->link >= 0) {
                bool hov = (b->link == hover_link);
                c = hov ? t->page_link_hover : t->page_link;
                if (hov) gfx_rrect(bx - 3, by - 1, b->w + 6, b->h + 2, 5, color_alpha(t->accent, 0.22f));
                gfx_rect(bx, by + b->h - 3, b->w, hov ? 2 : 1, color_alpha(c, 0.7f));
            }
            gfx_text((b->flags & BF_BOLD) ? FONT_UI : FONT_BODY, b->size, bx, by, c, p->arena + b->text, b->len);
            break;
        }
        case BOX_RULE:
            gfx_rect(bx, by + 4, b->w, 2, t->page_rule);
            break;
        case BOX_BULLET:
            gfx_circle(bx + 3, by + b->h * 0.55f, 3, t->page_dim);
            break;
        case BOX_INPUT: {
            Field *f = &p->fields[b->field];
            bool hov = (b->field == hover_field);
            gfx_rrect(bx, by, b->w, b->h, 7, t->field_bg);
            gfx_rrect_outline(bx, by, b->w, b->h, 7, hov ? 2.5f : 1.5f, hov ? t->accent : t->page_rule);
            char shown[256];
            int n = (int)strlen(f->value);
            for (int k = 0; k <= n; k++) shown[k] = (f->type == FIELD_PASSWORD && k < n) ? '*' : f->value[k];
            clip_inter(x, y, w, h, bx + 4, by, b->w - 8, b->h);
            gfx_text(FONT_BODY, b->size, bx + 8, by + 5, t->field_text, shown, n);
            gfx_set_clip(x, y, w, h);
            break;
        }
        case BOX_SUBMIT: {
            bool hov = (b->field == hover_field);
            if (hov) gfx_rrect(bx - 3, by - 3, b->w + 6, b->h + 6, 12, color_alpha(t->accent, 0.5f));
            gfx_rrect_vgrad(bx, by, b->w, b->h, 10, t->button_top, t->button_bottom);
            gfx_rrect_outline(bx, by, b->w, b->h, 10, 1.5f, t->panel_edge);
            gfx_text(FONT_UI, b->size, bx + 15, by + 5, t->text, p->arena + b->text, b->len);
            break;
        }
        }
    }
    gfx_clear_clip();
}

int page_hit(Page *p, float px, float py, int *field_out) {
    *field_out = -1;
    for (int i = 0; i < p->nboxes; i++) {
        Box *b = &p->boxes[i];
        if (b->link < 0 && b->field < 0) continue;
        if (px >= b->x - 2 && px < b->x + b->w + 2 && py >= b->y - 2 && py < b->y + b->h + 2) {
            if (b->field >= 0) { *field_out = b->field; return -1; }
            return b->link;
        }
    }
    return -1;
}

bool page_build_submit(Page *p, int field_idx, int submit_field, char *url_out, size_t url_len, char **post_out) {
    *post_out = NULL;
    if (field_idx < 0 || field_idx >= p->nfields) return false;
    int form = p->fields[field_idx].form;
    if (form < 0) return false;
    size_t cap = 4096, n = 0;
    char *q = malloc(cap);
    if (!q) return false;
    q[0] = 0;
    for (int i = 0; i < p->nfields; i++) {
        Field *f = &p->fields[i];
        if (f->form != form || !f->name[0]) continue;
        if (f->type == FIELD_SUBMIT && i != submit_field) continue;
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
