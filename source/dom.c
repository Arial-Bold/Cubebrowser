/* dom.c - forgiving HTML tree builder. */
#include "dom.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_NODES 30000
#define MAX_DEPTH 256

/* ----------------------------------------------------------------- arena */

struct ArenaBlock { struct ArenaBlock *next; size_t used, cap; char data[]; };

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    ArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 65536 ? n : 65536;
        b = malloc(sizeof(ArenaBlock) + cap);
        if (!b) return NULL;
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *p = b->data + b->used;
    b->used += n;
    a->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *d = arena_alloc(a, n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) { ArenaBlock *n = b->next; free(b); b = n; }
    a->head = NULL;
    a->used = 0;
}

static bool strcasestr_simple(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

/* ------------------------------------------------------------------ tags */

static const char *const tag_names[TAG_COUNT] = {
    "", "#root", "html", "head", "body", "title", "style", "script", "link", "meta", "base", "a", "img", "br",
    "hr", "p", "div", "span", "ul", "ol", "li", "dl", "dt", "dd", "table", "thead", "tbody", "tfoot", "tr", "td",
    "th", "caption", "col", "colgroup", "form", "input", "textarea", "button", "select", "option", "optgroup",
    "label", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "blockquote", "center", "font", "noscript", "svg",
    "math", "template", "iframe", "object", "embed", "wbr", "area", "source", "param", "track", "xmp",
    "listing", "plaintext", "section", "article", "header", "footer", "nav", "main", "aside", "figure",
    "address", "fieldset", "details", "menu", "picture", "video", "audio", "canvas", "dialog",
};

int dom_tag_id(const char *n) {
    for (int i = 2; i < TAG_COUNT; i++)
        if (n[0] == tag_names[i][0] && !strcmp(n, tag_names[i])) return i;
    return TAG_UNKNOWN;
}

static bool is_void(int t) {
    switch (t) {
    case TAG_IMG: case TAG_BR: case TAG_HR: case TAG_INPUT: case TAG_META: case TAG_LINK: case TAG_BASE:
    case TAG_COL: case TAG_WBR: case TAG_AREA: case TAG_SOURCE: case TAG_PARAM: case TAG_TRACK: case TAG_EMBED:
        return true;
    default: return false;
    }
}

/* Opening one of these closes an open <p>. */
static bool closes_p(int t) {
    switch (t) {
    case TAG_P: case TAG_DIV: case TAG_UL: case TAG_OL: case TAG_DL: case TAG_TABLE: case TAG_H1: case TAG_H2:
    case TAG_H3: case TAG_H4: case TAG_H5: case TAG_H6: case TAG_PRE: case TAG_BLOCKQUOTE: case TAG_FORM:
    case TAG_HR: case TAG_SECTION: case TAG_ARTICLE: case TAG_HEADER: case TAG_FOOTER: case TAG_NAV:
    case TAG_MAIN: case TAG_ASIDE: case TAG_FIGURE: case TAG_ADDRESS: case TAG_FIELDSET: case TAG_DETAILS:
    case TAG_MENU: case TAG_CENTER: case TAG_XMP: case TAG_LISTING:
        return true;
    default: return false;
    }
}

/* ------------------------------------------------------- text conversion */

static const char *ascii_for(unsigned cp, char *one) {
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == ' ') { one[0] = (char)cp; one[1] = 0; return one; }
    if (cp < 32 || cp == 127) return "";
    if (cp < 127) { one[0] = (char)cp; one[1] = 0; return one; }
    if (cp >= 0xC0 && cp <= 0xFF) { one[0] = latin1[cp - 0xC0]; one[1] = 0; return one; }
    switch (cp) {
    case 0xA0: return "\x01";   /* non-breaking space marker, drawn as a space */
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return "";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0xAB: return "<<";
    case 0xBB: return ">>";
    case 0xB7: case 0x2022: case 0x25CF: case 0x2023: case 0x2219: return "*";
    case 0xB0: return "deg";
    case 0xB1: return "+/-";
    case 0xBD: return "1/2";
    case 0xBC: return "1/4";
    case 0xBE: return "3/4";
    case 0xA3: return "GBP";
    case 0xA2: return "c";
    case 0xA5: return "JPY";
    case 0xA7: return "S";
    case 0xB6: return "P";
    case 0xBF: return "?";
    case 0xA1: return "!";
    case 0xD7: return "x";
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
    case 0x2713: case 0x2714: return "v";
    case 0x2715: case 0x2716: case 0x2717: return "x";
    case 0x2009: case 0x2002: case 0x2003: case 0x202F: case 0x2007: case 0x2008: case 0x200A: return " ";
    default: return "?";
    }
}

static unsigned utf8_next(const unsigned char *s, size_t len, size_t *i) {
    unsigned c = s[*i];
    if (c < 0x80) { (*i)++; return c; }
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0 || *i + (size_t)n >= len) { (*i)++; return c; }   /* Latin-1 fallback */
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
    { "sect", 0xA7 }, { "para", 0xB6 }, { "frac12", 0xBD }, { "frac14", 0xBC }, { "frac34", 0xBE },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "shy", 0xAD },
    { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "zwnj", 0x200C }, { "zwj", 0x200D },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "ecirc", 0xEA }, { "aacute", 0xE1 }, { "agrave", 0xE0 },
    { "acirc", 0xE2 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "auml", 0xE4 }, { "ccedil", 0xE7 },
    { "ntilde", 0xF1 }, { "szlig", 0xDF }, { "oacute", 0xF3 }, { "iacute", 0xED }, { "uacute", 0xFA },
    { "Eacute", 0xC9 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "Auml", 0xC4 }, { "iexcl", 0xA1 },
    { "iquest", 0xBF }, { "minus", 0x2212 }, { "prime", 0x2032 }, { "plusmn", 0xB1 }, { "check", 0x2713 },
};

static size_t parse_entity(const char *s, size_t len, unsigned *cp) {
    if (len < 3) return 0;
    if (s[1] == '#') {
        size_t i = 2;
        unsigned v = 0;
        bool hex = (i < len && (s[i] == 'x' || s[i] == 'X'));
        if (hex) i++;
        size_t start = i;
        while (i < len && i < 12 && (hex ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i]))) {
            int d = isdigit((unsigned char)s[i]) ? s[i] - '0' : tolower((unsigned char)s[i]) - 'a' + 10;
            v = v * (hex ? 16u : 10u) + (unsigned)d;
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

/* Converts src[0..len) (HTML text with entities, UTF-8) to ASCII into out. */
static size_t decode_text(const char *src, size_t len, char *out, size_t cap, bool entities_on) {
    size_t o = 0, i = 0;
    while (i < len && o + 4 < cap) {
        unsigned cp;
        size_t n;
        if (entities_on && src[i] == '&' && (n = parse_entity(src + i, len - i, &cp)) > 0) i += n;
        else cp = utf8_next((const unsigned char *)src, len, &i);
        char one[2];
        const char *a = ascii_for(cp, one);
        while (*a && o + 1 < cap) out[o++] = *a++;
    }
    out[o] = 0;
    return o;
}

void dom_decode_inplace(char *s, size_t cap) {
    size_t len = strlen(s);
    char *tmp = malloc(len * 3 + 8);
    if (!tmp) return;
    decode_text(s, len, tmp, len * 3 + 8, true);
    for (char *c = tmp; *c; c++) if (*c == 1) *c = ' ';
    snprintf(s, cap, "%s", tmp);
    free(tmp);
}

/* ---------------------------------------------------------------- parser */

typedef struct {
    Dom *d;
    Node *stack[MAX_DEPTH];
    int depth;
    bool full;
} P;

static Node *new_node(P *ps, int type) {
    if (ps->d->nodes >= MAX_NODES) { ps->full = true; return NULL; }
    Node *n = arena_alloc(&ps->d->arena, sizeof(Node));
    if (!n) { ps->full = true; return NULL; }
    n->type = (unsigned char)type;
    n->index = -1;
    n->sub = -1;
    ps->d->nodes++;
    return n;
}

static void append(Node *parent, Node *n) {
    n->parent = parent;
    n->prev = parent->last;
    if (parent->last) parent->last->next = n;
    else parent->first = n;
    parent->last = n;
}

static Node *top(P *ps) { return ps->stack[ps->depth - 1]; }

/* Pops up to and including the nearest open element with this tag, unless a
 * stop tag is found first. Returns true if popped. */
static bool pop_to(P *ps, int tag, const int *stops) {
    for (int i = ps->depth - 1; i > 0; i--) {
        int t = ps->stack[i]->tag;
        if (t == tag) { ps->depth = i; return true; }
        for (const int *s = stops; s && *s; s++)
            if (t == *s) return false;
    }
    return false;
}

static void add_text(P *ps, const char *src, size_t len, bool decode) {
    if (!len || ps->full) return;
    Node *parent = top(ps);
    /* text directly inside table structure (usually whitespace) is dropped */
    if (parent->tag == TAG_TABLE || parent->tag == TAG_TR || parent->tag == TAG_TBODY ||
        parent->tag == TAG_THEAD || parent->tag == TAG_TFOOT || parent->tag == TAG_SELECT) {
        bool blank = true;
        for (size_t i = 0; i < len && blank; i++) if (!isspace((unsigned char)src[i])) blank = false;
        if (blank) return;
    }
    /* decode into a scratch buffer, then keep only what's needed */
    static char *scratch;
    static size_t scratch_cap;
    size_t cap = len * 3 + 8;
    if (cap > scratch_cap) {
        char *nb = realloc(scratch, cap);
        if (!nb) { ps->full = true; return; }
        scratch = nb;
        scratch_cap = cap;
    }
    int n = (int)decode_text(src, len, scratch, cap, decode);
    if (!n) return;
    char *buf = arena_strndup(&ps->d->arena, scratch, (size_t)n);
    if (!buf) { ps->full = true; return; }
    Node *last = parent->last;
    Node *t = new_node(ps, NT_TEXT);
    if (!t) return;
    (void)last;
    t->text = buf;
    t->len = n;
    append(parent, t);
}

static void set_classes(Arena *a, Node *n) {
    const char *c = dom_attr(n, "class");
    n->id = dom_attr(n, "id");
    if (!c) return;
    const char *list[32];
    int k = 0;
    const char *p = c;
    while (*p && k < 32) {
        while (*p && isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (p > s) list[k++] = arena_strndup(a, s, (size_t)(p - s));
    }
    if (!k) return;
    n->classes = arena_alloc(a, sizeof(char *) * (size_t)k);
    if (!n->classes) return;
    memcpy(n->classes, list, sizeof(char *) * (size_t)k);
    n->nclasses = k;
}

static Attr *parse_attrs(Arena *a, const char *s, int len) {
    Attr *head = NULL, *tail = NULL;
    int i = 0;
    while (i < len) {
        while (i < len && (isspace((unsigned char)s[i]) || s[i] == '/')) i++;
        int ks = i;
        while (i < len && !isspace((unsigned char)s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') i++;
        int klen = i - ks;
        while (i < len && isspace((unsigned char)s[i])) i++;
        const char *v = "";
        int vlen = 0;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && isspace((unsigned char)s[i])) i++;
            if (i < len && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                v = s + i;
                while (i < len && s[i] != q) i++;
                vlen = (int)(s + i - v);
                if (i < len) i++;
            } else {
                v = s + i;
                while (i < len && !isspace((unsigned char)s[i]) && s[i] != '>') i++;
                vlen = (int)(s + i - v);
            }
        }
        if (klen <= 0) { if (i < len) i++; continue; }
        Attr *at = arena_alloc(a, sizeof(Attr));
        if (!at) break;
        char *k = arena_strndup(a, s + ks, (size_t)klen);
        for (char *c = k; c && *c; c++) *c = (char)tolower((unsigned char)*c);
        char small[512];
        size_t cap = (size_t)vlen * 3 + 8;
        char *tmp = cap <= sizeof(small) ? small : malloc(cap);
        if (!k || !tmp) break;
        size_t vl = decode_text(v, (size_t)vlen, tmp, cap, true);
        for (char *c = tmp; *c; c++) if (*c == 1) *c = ' ';
        char *val = arena_strndup(a, tmp, vl);
        if (tmp != small) free(tmp);
        if (!val) break;
        at->name = k;
        at->value = val;
        if (tail) tail->next = at; else head = at;
        tail = at;
    }
    return head;
}

const char *dom_attr(const Node *n, const char *name) {
    for (Attr *a = n->attrs; a; a = a->next)
        if (!strcmp(a->name, name)) return a->value;
    return NULL;
}

static size_t find_close(const char *src, size_t len, size_t pos, const char *name) {
    size_t nl = strlen(name);
    for (size_t i = pos; i + nl + 2 <= len; i++)
        if (src[i] == '<' && src[i + 1] == '/' && strncasecmp(src + i + 2, name, nl) == 0) return i;
    return len;
}

static size_t skip_tag_end(const char *src, size_t len, size_t pos) {
    while (pos < len && src[pos] != '>') pos++;
    return pos < len ? pos + 1 : len;
}

static void open_elem(P *ps, int tag, const char *name, const char *attrs, int alen, bool selfclose) {
    static const int stop_list[] = { TAG_UL, TAG_OL, TAG_TABLE, TAG_MENU, 0 };
    static const int stop_dl[] = { TAG_DL, TAG_TABLE, 0 };
    static const int stop_row[] = { TAG_TABLE, 0 };
    static const int stop_cell[] = { TAG_TR, TAG_TABLE, 0 };
    static const int stop_p[] = { TAG_TABLE, TAG_TD, TAG_TH, TAG_BUTTON, TAG_LI, TAG_DD, 0 };
    static const int stop_sel[] = { TAG_SELECT, 0 };

    if (closes_p(tag)) pop_to(ps, TAG_P, stop_p);
    switch (tag) {
    case TAG_LI: pop_to(ps, TAG_LI, stop_list); break;
    case TAG_DT: case TAG_DD:
        if (!pop_to(ps, TAG_DD, stop_dl)) pop_to(ps, TAG_DT, stop_dl);
        break;
    case TAG_TR:
        pop_to(ps, TAG_TR, stop_row);
        break;
    case TAG_TD: case TAG_TH:
        if (!pop_to(ps, TAG_TD, stop_cell)) pop_to(ps, TAG_TH, stop_cell);
        break;
    case TAG_THEAD: case TAG_TBODY: case TAG_TFOOT:
        pop_to(ps, TAG_TR, stop_row);
        if (!pop_to(ps, TAG_TBODY, stop_row) && !pop_to(ps, TAG_THEAD, stop_row)) pop_to(ps, TAG_TFOOT, stop_row);
        break;
    case TAG_OPTION: pop_to(ps, TAG_OPTION, stop_sel); break;
    case TAG_A: pop_to(ps, TAG_A, stop_row); break;
    case TAG_BODY: case TAG_HTML: case TAG_HEAD: {
        /* merge attributes into an existing element instead of nesting */
        for (int i = 1; i < ps->depth; i++)
            if (ps->stack[i]->tag == tag) return;
        break;
    }
    }
    Node *n = new_node(ps, NT_ELEM);
    if (!n) return;
    n->tag = (unsigned char)tag;
    n->name = tag ? tag_names[tag] : arena_strndup(&ps->d->arena, name, strlen(name));
    n->attrs = parse_attrs(&ps->d->arena, attrs, alen);
    set_classes(&ps->d->arena, n);
    append(top(ps), n);
    if (!is_void(tag) && !selfclose && ps->depth < MAX_DEPTH) ps->stack[ps->depth++] = n;
}

static void close_elem(P *ps, int tag, const char *name) {
    if (tag == TAG_BODY || tag == TAG_HTML) return;   /* stray </body> before content is common */
    static const int stop_cell[] = { TAG_TD, TAG_TH, TAG_TABLE, 0 };
    static const int stop_none[] = { 0 };
    if (tag == TAG_P) {
        if (!pop_to(ps, TAG_P, stop_cell)) { /* </p> without <p>: ignore */ }
        return;
    }
    if (tag == TAG_UNKNOWN) {
        for (int i = ps->depth - 1; i > 0; i--) {
            Node *n = ps->stack[i];
            if (n->tag == TAG_UNKNOWN && !strcmp(n->name, name)) { ps->depth = i; return; }
            if (n->tag == TAG_TD || n->tag == TAG_TH || n->tag == TAG_TABLE) return;
        }
        return;
    }
    /* table structure closes only within its own table */
    bool inline_like = !(tag == TAG_TABLE || tag == TAG_TR || tag == TAG_TD || tag == TAG_TH ||
                         tag == TAG_TBODY || tag == TAG_THEAD || tag == TAG_TFOOT || tag == TAG_DIV ||
                         tag == TAG_UL || tag == TAG_OL || tag == TAG_LI || tag == TAG_FORM);
    pop_to(ps, tag, inline_like ? stop_cell : stop_none);
}

Dom *dom_parse(const char *src, size_t len, bool plain) {
    Dom *d = calloc(1, sizeof(Dom));
    if (!d) return NULL;
    P ps;
    memset(&ps, 0, sizeof(ps));
    ps.d = d;
    Node *root = new_node(&ps, NT_ELEM);
    if (!root) { free(d); return NULL; }
    root->tag = TAG_ROOT;
    root->name = tag_names[TAG_ROOT];
    d->root = root;
    ps.stack[ps.depth++] = root;

    if (plain) {
        open_elem(&ps, TAG_PRE, "pre", "", 0, false);
        add_text(&ps, src, len, false);
        return d;
    }

    size_t i = 0, text_start = 0;
    while (i < len && !ps.full) {
        if (src[i] != '<') { i++; continue; }
        /* flush text before the tag */
        if (i > text_start) add_text(&ps, src + text_start, i - text_start, true);
        if (i + 3 < len && src[i + 1] == '!' && src[i + 2] == '-' && src[i + 3] == '-') {
            const char *end = NULL;
            for (size_t k = i + 4; k + 2 < len; k++)
                if (src[k] == '-' && src[k + 1] == '-' && src[k + 2] == '>') { end = src + k; break; }
            i = end ? (size_t)(end - src) + 3 : len;
            text_start = i;
            continue;
        }
        if (i + 1 < len && (src[i + 1] == '!' || src[i + 1] == '?')) {
            i = skip_tag_end(src, len, i);
            text_start = i;
            continue;
        }
        size_t j = i + 1;
        bool closing = false;
        if (j < len && src[j] == '/') { closing = true; j++; }
        if (j >= len || !isalpha((unsigned char)src[j])) {   /* a lone '<' is text */
            text_start = i;
            i++;
            continue;
        }
        char name[32];
        int nl = 0;
        while (j < len && (isalnum((unsigned char)src[j]) || src[j] == '-' || src[j] == ':' || src[j] == '_')) {
            if (nl < 31) name[nl++] = (char)tolower((unsigned char)src[j]);
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
        bool selfclose = alen > 0 && src[as + alen - 1] == '/';
        size_t after = j < len ? j + 1 : len;
        int tag = dom_tag_id(name);
        i = after;
        text_start = after;

        if (closing) { close_elem(&ps, tag, name); continue; }

        /* elements whose content is not parsed as HTML */
        if (tag == TAG_SCRIPT || tag == TAG_TEMPLATE || tag == TAG_SVG || tag == TAG_MATH ||
            tag == TAG_STYLE || tag == TAG_TEXTAREA || tag == TAG_TITLE || tag == TAG_XMP ||
            tag == TAG_IFRAME || tag == TAG_OBJECT || tag == TAG_VIDEO || tag == TAG_AUDIO || tag == TAG_CANVAS) {
            size_t c = selfclose ? after : find_close(src, len, after, name);
            if (tag == TAG_STYLE && d->nsheets < DOM_MAX_SHEETS) {
                Node tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.attrs = parse_attrs(&d->arena, src + as, alen);
                DomSheet *sh = &d->sheets[d->nsheets++];
                sh->text = arena_strndup(&d->arena, src + after, c - after);
                sh->len = (int)(c - after);
                sh->media = dom_attr(&tmp, "media");
            } else if (tag == TAG_TITLE) {
                char buf[512];
                size_t n = c - after < 400 ? c - after : 400;
                decode_text(src + after, n, buf, sizeof(buf), true);
                /* collapse whitespace */
                size_t o = 0;
                bool sp = false;
                for (char *p = buf; *p && o + 1 < sizeof(d->title); p++) {
                    if (isspace((unsigned char)*p) || *p == 1) { sp = o > 0; continue; }
                    if (sp) { d->title[o++] = ' '; sp = false; }
                    d->title[o++] = *p;
                }
                d->title[o] = 0;
            } else if (tag == TAG_TEXTAREA || tag == TAG_XMP) {
                open_elem(&ps, tag, name, src + as, alen, false);
                add_text(&ps, src + after, c - after, tag == TAG_TEXTAREA);
                if (top(&ps)->tag == tag) ps.depth--;
            } else if (tag == TAG_IFRAME || tag == TAG_VIDEO || tag == TAG_AUDIO || tag == TAG_OBJECT) {
                /* keep a placeholder element (fallback content isn't shown) */
                open_elem(&ps, tag, name, src + as, alen, true);
            }
            i = skip_tag_end(src, len, c);
            text_start = i;
            continue;
        }
        if (tag == TAG_LINK && d->nsheets < DOM_MAX_SHEETS) {
            Node tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.attrs = parse_attrs(&d->arena, src + as, alen);
            const char *rel = dom_attr(&tmp, "rel"), *href = dom_attr(&tmp, "href");
            if (rel && href && href[0] && strcasestr_simple(rel, "stylesheet") && !strcasestr_simple(rel, "alternate")) {
                DomSheet *sh = &d->sheets[d->nsheets++];
                sh->href = href;
                sh->media = dom_attr(&tmp, "media");
            }
            continue;
        }
        if (tag == TAG_PLAINTEXT) {
            open_elem(&ps, TAG_PRE, "pre", "", 0, false);
            add_text(&ps, src + after, len - after, false);
            break;
        }
        open_elem(&ps, tag, name, src + as, alen, selfclose);
    }
    if (!ps.full && text_start < len && i >= len) add_text(&ps, src + text_start, len - text_start, true);
    return d;
}

void dom_free(Dom *d) {
    if (!d) return;
    arena_free(&d->arena);
    free(d);
}
