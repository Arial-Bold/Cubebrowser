/* jsengine.c - see jsengine.h.
 *
 * The page's scripts run in order once their files have downloaded (as if
 * the page had loaded in one go), then the page gets DOMContentLoaded and
 * load. document.write puts its HTML right after the script that wrote it
 * (and scripts written that way run next). After that, scripts run from
 * timers and from the player's clicks and changes.
 *
 * Scripts see the page through the functions in N (registered as __n, then
 * hidden by jsprelude.js): elements are passed as numbers (their address),
 * checked against the ones handed out before being used. */
#include "jsengine.h"
#include "quickjs.h"
#include "bigstack.h"
#include "memstat.h"
#include "settings.h"
#include "cookies.h"
#include "diag.h"
#include "url.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef GEKKO
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
static double now_ms(void) { return ticks_to_microsecs(gettime()) / 1000.0; }
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

extern const char js_prelude[];
extern const size_t js_prelude_len;

/* how deep scripts may go before QuickJS stops them (the big stack they run
 * on is 1 MB: this leaves room for the browser's own calls) */
#define JS_STACK_LIMIT (640 * 1024)
#define SCRIPT_MAX (512 * 1024)      /* one script file bigger than this isn't run */
#define SCRIPTS_TOTAL (1536 * 1024)  /* nor the ones after this much script */
#define LOAD_TIME_LIMIT 6000.0       /* ms a script may run while the page loads */
#define EVENT_TIME_LIMIT 2000.0      /* ...and later (timers, clicks) */
#define TIMER_RELAYOUT_MS 400.0      /* timers lay the page out again at most this often */
#define DOM_GROWTH_MAX (4u * 1024 * 1024)   /* scripts adding more than this to the page are stopped */
#define MEM_FLOOR (1536u * 1024)     /* ...or when the console gets this low on memory */

enum { SQ_NEED, SQ_LOADING, SQ_READY, SQ_DONE };

typedef struct {
    Node *node;
    char *url;           /* a script file's address (NULL: the code is in the page) */
    char *code;          /* a downloaded file's code (owned) */
    size_t len;
    unsigned char state;
    unsigned char dynamic;   /* added by a script (it gets a load or error event) */
    unsigned char failed;    /* its file couldn't be downloaded */
    unsigned char module;    /* type="module": run as an ES module, once the files it imports are here */
    unsigned char scanned;   /* ...and its imports have been looked for */
} Script;

/* The files module scripts import, downloaded before the module runs (QuickJS
 * asks for them by address as it links the module). */
#define MOD_BASE 0x100       /* (their numbers for the downloader start here) */
#define MOD_MAX 200
typedef struct {
    char *url;
    char *code;              /* downloaded, not yet handed to QuickJS */
    size_t len;
    unsigned char state;     /* SQ_NEED, SQ_LOADING, SQ_READY, SQ_DONE */
    unsigned char failed;
} ModFile;

/* Data a script asked for (XMLHttpRequest, fetch), waiting for the
 * browser's downloader or being downloaded. */
#define NET_MAX 12
#define NET_BODY_MAX (1536u * 1024)   /* bigger answers are cut off here */
enum { NET_FREE, NET_QUEUED, NET_LOADING };
typedef struct {
    unsigned char state;
    bool cookies;
    char *url, *body;
    char ctype[96], accept[96];
} NetReq;

static struct {
    Page *page;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue fn_timers, fn_dispatch, fn_loaded, fn_net;
    NetReq net[NET_MAX];
    ModFile *mods;
    int nmods, capmods;
    int blocked;         /* downloads the ad block list stopped (scripts, data) */
    bool started;        /* the context exists */
    bool failed;         /* it couldn't start, or scripts keep getting stopped */
    Script *q;
    int nq, capq, next;
    bool loaded;         /* DOMContentLoaded and load sent */
    size_t total;        /* script bytes run so far */
    Node *cur_script, *write_after;
    bool changed;        /* the page changed: lay it out again */
    double last_relayout;
    bool timers_on;
    double timer_at;     /* when the next one is due */
    size_t arena_start;  /* the page's memory before its scripts changed it */
    double deadline;
    int stops;           /* scripts stopped for running too long */
    bool stopped_now;
    int nav;
    char nav_url[URL_MAX];
    int nav_field;
    char message[200];
    void **reg;          /* elements handed to scripts (an open-addressing set) */
    size_t regcap, regn;
} J;

static void queue_add_at(Node *n, int at);
static char *mod_normalize(JSContext *ctx, const char *base, const char *name, void *op);
static JSModuleDef *mod_load(JSContext *ctx, const char *name, void *op);

/* -------------------------------------------------------------- the log
 * What happened with the last page's scripts (files, errors, console
 * messages), for about:console: kept when you go there to read it. */
#define LOG_LINES 40
static char log_lines[LOG_LINES][180];
static int log_count;
static char log_page[256];

static void jslog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void jslog(const char *fmt, ...) {
    char *l = log_lines[log_count % LOG_LINES];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l, sizeof(log_lines[0]), fmt, ap);
    va_end(ap);
    log_count++;
}

/* ------------------------------------------------- elements handed out */

static size_t ptr_hash(const void *p) { uintptr_t x = (uintptr_t)p; x ^= x >> 13; x *= 0x9E3779B1u; return (size_t)(x ^ (x >> 16)); }

static bool reg_has(const void *p) {
    if (!p || !J.regcap) return false;
    for (size_t i = ptr_hash(p) & (J.regcap - 1);; i = (i + 1) & (J.regcap - 1)) {
        if (!J.reg[i]) return false;
        if (J.reg[i] == p) return true;
    }
}

static void reg_add(void *p) {
    if (!p || reg_has(p)) return;
    if ((J.regn + 1) * 10 >= J.regcap * 7) {
        size_t nc = J.regcap ? J.regcap * 2 : 1024;
        void **nr = calloc(nc, sizeof(void *));
        if (!nr) return;
        for (size_t k = 0; k < J.regcap; k++) {
            void *q = J.reg[k];
            if (!q) continue;
            size_t i = ptr_hash(q) & (nc - 1);
            while (nr[i]) i = (i + 1) & (nc - 1);
            nr[i] = q;
        }
        free(J.reg);
        J.reg = nr;
        J.regcap = nc;
    }
    size_t i = ptr_hash(p) & (J.regcap - 1);
    while (J.reg[i]) i = (i + 1) & (J.regcap - 1);
    J.reg[i] = p;
    J.regn++;
}

static JSValue H(JSContext *c, Node *n) {
    if (!n) return JS_NULL;
    reg_add(n);
    return JS_NewInt64(c, (int64_t)(uintptr_t)n);
}

static Node *NODE(JSContext *c, JSValueConst v) {
    int64_t x;
    if (JS_IsNull(v) || JS_IsUndefined(v) || JS_ToInt64(c, &x, v)) return NULL;
    Node *n = (Node *)(uintptr_t)x;
    return reg_has(n) ? n : NULL;
}

/* ------------------------------------------------------------- helpers */

static Dom *DOM(void) { return J.page ? J.page->dom : NULL; }

static void changed(void) { J.changed = true; }

static void button_caption(Node *n);

static bool is_connected(const Node *n) {
    Dom *d = DOM();
    if (!d) return false;
    while (n->parent) n = n->parent;
    return n == d->root;
}

typedef struct Buf { char *s; size_t n, cap; bool full; } Buf;

static void buf_put(Buf *b, const char *s, size_t n) {
    if (b->full) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + n + 1) nc *= 2;
        if (nc > 2 * 1024 * 1024) { b->full = true; return; }
        char *ns = realloc(b->s, nc);
        if (!ns) { b->full = true; return; }
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static JSValue buf_value(JSContext *c, Buf *b) {
    JSValue v = JS_NewStringLen(c, b->s ? b->s : "", b->n);
    free(b->s);
    return v;
}

static void text_into(const Node *n, Buf *b, int depth) {
    if (n->type == NT_TEXT) {
        for (int i = 0; i < n->len; i++) {
            char ch = n->text[i] == 1 ? ' ' : n->text[i];
            buf_put(b, &ch, 1);
        }
        return;
    }
    if (n->tag == TAG_SCRIPT || n->tag == TAG_STYLE || depth > 200) return;
    for (const Node *k = n->first; k; k = k->next) text_into(k, b, depth + 1);
}

/* a button's caption follows its text (the browser draws it from the field) */
static void button_caption(Node *n) {
    Page *p = J.page;
    for (Node *a = n; a && p; a = a->parent) {
        if (a->tag != TAG_BUTTON) continue;
        if (a->index >= 0 && a->index < p->nfields && p->fields[a->index].node == a) {
            Buf b = { 0 };
            text_into(a, &b, 0);
            const char *t = b.s ? b.s : "";
            while (*t == ' ' || *t == '\n' || *t == '\t') t++;
            snprintf(p->fields[a->index].label, sizeof(p->fields[a->index].label), "%s", *t ? t : "Submit");
            size_t l = strlen(p->fields[a->index].label);
            while (l && (p->fields[a->index].label[l - 1] == ' ' || p->fields[a->index].label[l - 1] == '\n')) p->fields[a->index].label[--l] = 0;
            free(b.s);
        }
        return;
    }
}

static bool is_void_tag(int t) {
    switch (t) {
    case TAG_IMG: case TAG_BR: case TAG_HR: case TAG_INPUT: case TAG_META: case TAG_LINK: case TAG_BASE:
    case TAG_COL: case TAG_WBR: case TAG_AREA: case TAG_SOURCE: case TAG_PARAM: case TAG_TRACK: case TAG_EMBED:
        return true;
    }
    return false;
}

static void escape_into(Buf *b, const char *s, size_t n, bool attr) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i] == 1 ? ' ' : s[i];
        if (c == '&') buf_str(b, "&amp;");
        else if (c == '<') buf_str(b, "&lt;");
        else if (c == '>') buf_str(b, "&gt;");
        else if (c == '"' && attr) buf_str(b, "&quot;");
        else buf_put(b, &c, 1);
    }
}

static void html_into(const Node *n, Buf *b, int depth) {
    if (depth > 200 || b->full) return;
    if (n->type == NT_TEXT) { escape_into(b, n->text, (size_t)n->len, false); return; }
    if (n->tag == TAG_ROOT) {
        for (const Node *k = n->first; k; k = k->next) html_into(k, b, depth + 1);
        return;
    }
    buf_str(b, "<");
    buf_str(b, n->name);
    for (const Attr *a = n->attrs; a; a = a->next) {
        buf_str(b, " ");
        buf_str(b, a->name);
        buf_str(b, "=\"");
        escape_into(b, a->value, strlen(a->value), true);
        buf_str(b, "\"");
    }
    buf_str(b, ">");
    if (is_void_tag(n->tag)) return;
    if (n->tag == TAG_SCRIPT && n->text) buf_put(b, n->text, (size_t)n->len);
    for (const Node *k = n->first; k; k = k->next) html_into(k, b, depth + 1);
    buf_str(b, "</");
    buf_str(b, n->name);
    buf_str(b, ">");
}

static const char *arg_str(JSContext *c, JSValueConst v, size_t *len) {
    return JS_ToCStringLen(c, len, v);
}

/* the form field an element is, or -1 */
static int field_of(const Node *n) {
    Page *p = J.page;
    if (!p || n->index < 0 || n->index >= p->nfields) return -1;
    if (n->tag != TAG_INPUT && n->tag != TAG_TEXTAREA && n->tag != TAG_SELECT && n->tag != TAG_BUTTON) return -1;
    return p->fields[n->index].node == n ? n->index : -1;
}

/* ------------------------------------------------------------ selectors
 * A practical subset: tag, #id, .class, [attr], [attr=v] (~= ^= $= *= |=),
 * :first-child, :last-child, :not(simple), * and the combinators (space,
 * >, +, ~), in comma-separated groups. */

typedef struct {
    char tag[32];
    char id[64];
    char cls[8][48];
    int ncls;
    struct { char name[32], val[96]; char op; } at[6];
    int nat;
    unsigned char first_child, last_child, empty;
    char not_sel[96];
    bool bad;
} Simple;

static const char *parse_ident(const char *s, char *out, size_t cap) {
    size_t k = 0;
    while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || (unsigned char)*s >= 0x80 || *s == '\\')) {
        if (*s == '\\' && s[1]) s++;
        if (k + 1 < cap) out[k++] = *s;
        s++;
    }
    out[k] = 0;
    return s;
}

static const char *parse_simple(const char *s, Simple *ss) {
    memset(ss, 0, sizeof(*ss));
    if (*s == '*') s++;
    else if (isalpha((unsigned char)*s)) {
        s = parse_ident(s, ss->tag, sizeof(ss->tag));
        for (char *c = ss->tag; *c; c++) *c = (char)tolower((unsigned char)*c);
    }
    for (;;) {
        if (*s == '#') s = parse_ident(s + 1, ss->id, sizeof(ss->id));
        else if (*s == '.') {
            char tmp[48];
            s = parse_ident(s + 1, tmp, sizeof(tmp));
            if (ss->ncls < 8) snprintf(ss->cls[ss->ncls++], 48, "%s", tmp);
        } else if (*s == '[') {
            s++;
            while (*s == ' ') s++;
            char name[32];
            s = parse_ident(s, name, sizeof(name));
            for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
            while (*s == ' ') s++;
            char op = 0;
            if (*s == '=') { op = '='; s++; }
            else if (strchr("~^$*|", *s) && *s && s[1] == '=') { op = *s; s += 2; }
            char val[96] = "";
            if (op) {
                while (*s == ' ') s++;
                size_t k = 0;
                if (*s == '"' || *s == '\'') {
                    char q = *s++;
                    while (*s && *s != q) { if (k + 1 < sizeof(val)) val[k++] = *s; s++; }
                    if (*s) s++;
                } else {
                    while (*s && *s != ']' && *s != ' ') { if (k + 1 < sizeof(val)) val[k++] = *s; s++; }
                }
                val[k] = 0;
                while (*s == ' ' || *s == 'i' || *s == 's') s++;   /* (case flags) */
            }
            if (*s == ']') s++;
            if (ss->nat < 6) {
                snprintf(ss->at[ss->nat].name, 32, "%s", name);
                snprintf(ss->at[ss->nat].val, 96, "%s", val);
                ss->at[ss->nat].op = op;
                ss->nat++;
            }
        } else if (*s == ':') {
            s++;
            if (*s == ':') s++;
            char name[32];
            s = parse_ident(s, name, sizeof(name));
            if (!strcasecmp(name, "first-child")) ss->first_child = 1;
            else if (!strcasecmp(name, "last-child")) ss->last_child = 1;
            else if (!strcasecmp(name, "empty")) ss->empty = 1;
            else if (!strcasecmp(name, "not") && *s == '(') {
                s++;
                size_t k = 0;
                int depth = 1;
                while (*s && depth) {
                    if (*s == '(') depth++;
                    if (*s == ')' && --depth == 0) break;
                    if (k + 1 < sizeof(ss->not_sel)) ss->not_sel[k++] = *s;
                    s++;
                }
                ss->not_sel[k] = 0;
                if (*s == ')') s++;
            } else if (*s == '(') {   /* other pseudo-classes with arguments: skipped (no match) */
                while (*s && *s != ')') s++;
                if (*s) s++;
                ss->bad = true;
            } else if (strcasecmp(name, "link") && strcasecmp(name, "visited") && strcasecmp(name, "root") &&
                       strcasecmp(name, "enabled")) {
                ss->bad = true;   /* :hover, :checked... (a state we don't have): no match */
            }
        } else break;
    }
    return s;
}

static bool word_in(const char *list, const char *w) {
    size_t n = strlen(w);
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == n && !strncmp(p, w, n)) return true;
        p = e;
    }
    return false;
}

static bool match_selector_one(const Node *n, const char *sel);

static bool match_simple(const Node *n, const Simple *ss) {
    if (n->type != NT_ELEM || n->tag == TAG_ROOT || ss->bad) return false;
    if (ss->tag[0] && strcmp(ss->tag, n->name)) return false;
    if (ss->id[0] && (!n->id || strcmp(ss->id, n->id))) return false;
    for (int i = 0; i < ss->ncls; i++) {
        bool found = false;
        for (int k = 0; k < n->nclasses && !found; k++) if (!strcmp(n->classes[k], ss->cls[i])) found = true;
        if (!found) return false;
    }
    for (int i = 0; i < ss->nat; i++) {
        const char *v = dom_attr(n, ss->at[i].name);
        if (!v) return false;
        const char *w = ss->at[i].val;
        size_t vl = strlen(v), wl = strlen(w);
        switch (ss->at[i].op) {
        case '=': if (strcmp(v, w)) return false; break;
        case '~': if (!word_in(v, w)) return false; break;
        case '^': if (!wl || strncmp(v, w, wl)) return false; break;
        case '$': if (!wl || vl < wl || strcmp(v + vl - wl, w)) return false; break;
        case '*': if (!wl || !strstr(v, w)) return false; break;
        case '|': if (strncmp(v, w, wl) || (v[wl] && v[wl] != '-')) return false; break;
        }
    }
    if (ss->first_child) { const Node *k = n->prev; while (k && k->type != NT_ELEM) k = k->prev; if (k) return false; }
    if (ss->last_child) { const Node *k = n->next; while (k && k->type != NT_ELEM) k = k->next; if (k) return false; }
    if (ss->empty && n->first) return false;
    if (ss->not_sel[0] && match_selector_one(n, ss->not_sel)) return false;
    return true;
}

/* One compound selector chain (no commas), matched right to left. */
#define MAX_PARTS 12
typedef struct { Simple s[MAX_PARTS]; char comb[MAX_PARTS]; int n; } Chain;

static bool parse_chain(const char *sel, Chain *ch) {
    ch->n = 0;
    const char *s = sel;
    while (*s == ' ') s++;
    char comb = 0;
    while (*s && ch->n < MAX_PARTS) {
        const char *before = s;
        s = parse_simple(s, &ch->s[ch->n]);
        if (s == before) return false;   /* something we can't read */
        ch->comb[ch->n] = comb;
        ch->n++;
        bool space = false;
        while (*s == ' ' || *s == '\t' || *s == '\n') { s++; space = true; }
        if (*s == '>' || *s == '+' || *s == '~') { comb = *s++; while (*s == ' ') s++; }
        else if (space && *s) comb = ' ';
        else if (*s) return false;
    }
    return ch->n > 0;
}

static bool match_chain_at(const Node *n, const Chain *ch, int i) {
    if (!match_simple(n, &ch->s[i])) return false;
    if (i == 0) return true;
    char c = ch->comb[i];
    if (c == '>') return n->parent && match_chain_at(n->parent, ch, i - 1);
    if (c == '+') { const Node *k = n->prev; while (k && k->type != NT_ELEM) k = k->prev; return k && match_chain_at(k, ch, i - 1); }
    if (c == '~') { for (const Node *k = n->prev; k; k = k->prev) if (k->type == NT_ELEM && match_chain_at(k, ch, i - 1)) return true; return false; }
    for (const Node *a = n->parent; a; a = a->parent) if (match_chain_at(a, ch, i - 1)) return true;
    return false;
}

static bool match_selector_one(const Node *n, const char *sel) {
    Chain *ch = malloc(sizeof(Chain));
    if (!ch) return false;
    bool ok = parse_chain(sel, ch) && match_chain_at(n, ch, ch->n - 1);
    free(ch);
    return ok;
}

/* the groups of a selector list (split at top-level commas) */
static int split_groups(const char *sel, char groups[][256], int max) {
    int n = 0, depth = 0;
    size_t k = 0;
    char q = 0;
    for (const char *s = sel;; s++) {
        if (q) { if (*s == q) q = 0; }
        else if (*s == '"' || *s == '\'') q = *s;
        else if (*s == '(' || *s == '[') depth++;
        else if (*s == ')' || *s == ']') depth--;
        if ((!*s || (*s == ',' && !depth && !q))) {
            groups[n][k] = 0;
            while (k && groups[n][k - 1] == ' ') groups[n][--k] = 0;
            if (k && n < max - 1) n++;
            k = 0;
            if (!*s) break;
            continue;
        }
        if (k || *s != ' ') { if (k < 255) groups[n][k++] = *s; }
    }
    if (k) n++;
    return n;
}

typedef struct { Chain *chains; int n; } SelList;

/* false: a selector this can't read or doesn't support (scripts get an
 * exception, as browsers give for bad selectors: libraries like jQuery then
 * use their own selector code instead) */
static bool sel_compile(const char *sel, SelList *sl) {
    static char groups[16][256];
    int ng = split_groups(sel, groups, 16);
    sl->chains = calloc((size_t)(ng ? ng : 1), sizeof(Chain));
    sl->n = 0;
    if (!sl->chains || !ng) return false;
    for (int i = 0; i < ng; i++) {
        Chain *ch = &sl->chains[sl->n];
        if (!parse_chain(groups[i], ch)) return false;
        for (int k = 0; k < ch->n; k++) {
            if (ch->s[k].bad) return false;
            if (ch->s[k].not_sel[0]) {   /* (what's inside :not() must be readable too) */
                Chain tmp;
                if (!parse_chain(ch->s[k].not_sel, &tmp)) return false;
                for (int j = 0; j < tmp.n; j++) if (tmp.s[j].bad) return false;
            }
        }
        sl->n++;
    }
    return true;
}

static bool sel_match(const Node *n, const SelList *sl) {
    for (int i = 0; i < sl->n; i++)
        if (match_chain_at(n, &sl->chains[i], sl->chains[i].n - 1)) return true;
    return false;
}

/* ------------------------------------------------- the functions in N */

#define FN(name) static JSValue name(JSContext *c, JSValueConst this_val, int argc, JSValueConst *argv)
#define UNUSED (void)this_val; (void)argc

FN(n_type) { UNUSED; Node *n = NODE(c, argv[0]); if (!n) return JS_NewInt32(c, 0);
    return JS_NewInt32(c, n->type == NT_TEXT ? 3 : n->tag == TAG_ROOT ? 9 : 1); }
FN(n_tag) { UNUSED; Node *n = NODE(c, argv[0]); return JS_NewString(c, n && n->type == NT_ELEM ? n->name : ""); }
FN(n_parent) { UNUSED; Node *n = NODE(c, argv[0]); return H(c, n ? n->parent : NULL); }
FN(n_first) { UNUSED; Node *n = NODE(c, argv[0]); return H(c, n ? n->first : NULL); }
FN(n_last) { UNUSED; Node *n = NODE(c, argv[0]); return H(c, n ? n->last : NULL); }
FN(n_next) { UNUSED; Node *n = NODE(c, argv[0]); return H(c, n ? n->next : NULL); }
FN(n_prev) { UNUSED; Node *n = NODE(c, argv[0]); return H(c, n ? n->prev : NULL); }
FN(n_doc) { UNUSED; (void)argv; Dom *d = DOM(); return H(c, d ? d->root : NULL); }
FN(n_connected) { UNUSED; Node *n = NODE(c, argv[0]); return JS_NewBool(c, n && is_connected(n)); }

FN(n_kids) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    bool elems = JS_ToBool(c, argv[1]);
    JSValue a = JS_NewArray(c);
    uint32_t i = 0;
    if (n)
        for (Node *k = n->first; k; k = k->next)
            if (!elems || k->type == NT_ELEM) JS_SetPropertyUint32(c, a, i++, H(c, k));
    return a;
}

FN(n_attr) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *name = JS_ToCString(c, argv[1]);
    const char *v = n && name && n->type == NT_ELEM ? dom_attr(n, name) : NULL;
    if (name) JS_FreeCString(c, name);
    return v ? JS_NewString(c, v) : JS_NULL;
}

FN(n_attrs) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    JSValue a = JS_NewArray(c);
    uint32_t i = 0;
    if (n)
        for (const Attr *at = n->attrs; at; at = at->next) {
            JSValue pair = JS_NewArray(c);
            JS_SetPropertyUint32(c, pair, 0, JS_NewString(c, at->name));
            JS_SetPropertyUint32(c, pair, 1, JS_NewString(c, at->value));
            JS_SetPropertyUint32(c, a, i++, pair);
        }
    return a;
}

static void attr_changed(Node *n, const char *name) {
    Page *p = J.page;
    if (!strcmp(name, "href") && n->tag == TAG_A) page_set_link_url(p, n);
    else if ((!strcmp(name, "src") || !strcmp(name, "data-src")) && n->tag == TAG_IMG) page_image_changed(p, n);
    else if (!strcmp(name, "value")) { int f = field_of(n); if (f >= 0 && n->tag != TAG_SELECT) {
        const char *v = dom_attr(n, "value"); snprintf(p->fields[f].value, sizeof(p->fields[f].value), "%s", v ? v : "");
        if (p->fields[f].type == FIELD_SUBMIT && n->tag == TAG_INPUT && v && *v) snprintf(p->fields[f].label, sizeof(p->fields[f].label), "%s", v); } }
    else if (!strcmp(name, "checked")) { int f = field_of(n); if (f >= 0) p->fields[f].checked = dom_attr(n, "checked") != NULL; }
    else if (!strncmp(name, "on", 2) && !strcmp(name, "onclick")) page_make_clickable(p, n);
    changed();
}

FN(n_set_attr) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *name = JS_ToCString(c, argv[1]);
    const char *v = JS_ToCString(c, argv[2]);
    if (n && name && v && n->type == NT_ELEM && name[0]) {
        const char *old = dom_attr(n, name);
        if (!old || strcmp(old, v)) {
            dom_set_attr(DOM(), n, name, v);
            attr_changed(n, name);
        }
    }
    if (name) JS_FreeCString(c, name);
    if (v) JS_FreeCString(c, v);
    return JS_UNDEFINED;
}

FN(n_rm_attr) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *name = JS_ToCString(c, argv[1]);
    if (n && name && n->type == NT_ELEM && dom_set_attr(DOM(), n, name, NULL)) attr_changed(n, name);
    if (name) JS_FreeCString(c, name);
    return JS_UNDEFINED;
}

FN(n_text) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    if (n && n->tag == TAG_SCRIPT) return JS_NewStringLen(c, n->text ? n->text : "", n->text ? (size_t)n->len : 0);
    Buf b = { 0 };
    if (n) text_into(n, &b, 0);
    return buf_value(c, &b);
}

FN(n_set_text) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    size_t len;
    const char *s = arg_str(c, argv[1], &len);
    Dom *d = DOM();
    if (n && s && d && n->tag == TAG_SCRIPT) {   /* a script's code: kept as it is */
        char *code = arena_strndup(&d->arena, s, len);
        if (code) { n->text = code; n->len = (int)len; }
    } else if (n && s && d) {
        if (n->type == NT_TEXT) {
            Node *t = dom_create_text(d, s, len);
            if (t) { n->text = t->text; n->len = t->len; }
        } else {
            while (n->first) dom_detach(n->first);
            if (len) { Node *t = dom_create_text(d, s, len); if (t) dom_append(n, t); }
            int f = field_of(n);
            if (f >= 0 && n->tag == TAG_TEXTAREA) snprintf(J.page->fields[f].value, sizeof(J.page->fields[f].value), "%s", s);
        }
        button_caption(n);
        changed();
    }
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

FN(n_inner) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    Buf b = { 0 };
    if (n && n->type == NT_ELEM) {
        if (n->tag == TAG_SCRIPT && n->text) buf_put(&b, n->text, (size_t)n->len);
        for (const Node *k = n->first; k; k = k->next) html_into(k, &b, 0);
    } else if (n) text_into(n, &b, 0);
    return buf_value(c, &b);
}

FN(n_outer) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    Buf b = { 0 };
    if (n) html_into(n, &b, 0);
    return buf_value(c, &b);
}

FN(n_set_inner) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    size_t len;
    const char *s = arg_str(c, argv[1], &len);
    Dom *d = DOM();
    if (n && s && d && n->type == NT_ELEM) {
        while (n->first) dom_detach(n->first);
        if (n->tag == TAG_SCRIPT) {
            char *code = arena_strndup(&d->arena, s, len);
            if (code) { n->text = code; n->len = (int)len; }
        } else if (n->tag == TAG_STYLE) {
            Node *t = dom_create_text(d, s, len);
            if (t) dom_append(n, t);
        } else {
            dom_parse_fragment(d, n, s, len);
        }
        if (is_connected(n)) page_dom_added(J.page, n);
        button_caption(n);
        changed();
    }
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

FN(n_create) {
    UNUSED;
    const char *t = JS_ToCString(c, argv[0]);
    Dom *d = DOM();
    Node *n = t && d ? dom_create_elem(d, t) : NULL;
    if (t) JS_FreeCString(c, t);
    return H(c, n);
}

FN(n_create_text) {
    UNUSED;
    size_t len;
    const char *s = arg_str(c, argv[0], &len);
    Dom *d = DOM();
    Node *n = s && d ? dom_create_text(d, s, len) : NULL;
    if (s) JS_FreeCString(c, s);
    return H(c, n);
}

/* <script> elements a script put into the page run too (loaders do this) */
static void queue_added_scripts(Node *n, int depth) {
    if (n->type != NT_ELEM || depth > 50) return;
    if (n->tag == TAG_SCRIPT && n->sub != 1) {
        int before = J.nq;
        queue_add_at(n, -1);
        if (J.nq > before) J.q[J.nq - 1].dynamic = 1;
        return;
    }
    for (Node *k = n->first; k; k = k->next) queue_added_scripts(k, depth + 1);
}

static void inserted(Node *child) {
    if (is_connected(child)) { page_dom_added(J.page, child); queue_added_scripts(child, 0); }
    if (child->parent) button_caption(child->parent);
    changed();
}

static bool may_insert(Node *parent, Node *child) {
    if (!parent || !child || parent->type != NT_ELEM || child->tag == TAG_ROOT) return false;
    for (Node *a = parent; a; a = a->parent) if (a == child) return false;   /* (into itself) */
    return true;
}

FN(n_append) {
    UNUSED;
    Node *p = NODE(c, argv[0]), *k = NODE(c, argv[1]);
    if (may_insert(p, k)) { dom_append(p, k); inserted(k); }
    return JS_UNDEFINED;
}

FN(n_insert) {
    UNUSED;
    Node *p = NODE(c, argv[0]), *k = NODE(c, argv[1]), *ref = NODE(c, argv[2]);
    if (may_insert(p, k)) { dom_insert_before(p, k, ref); inserted(k); }
    return JS_UNDEFINED;
}

FN(n_detach) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    if (n && n->parent) { Node *p = n->parent; dom_detach(n); button_caption(p); changed(); }
    return JS_UNDEFINED;
}

FN(n_clone) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    Dom *d = DOM();
    return H(c, n && d ? dom_clone(d, n, JS_ToBool(c, argv[1])) : NULL);
}

static void collect(Node *n, JSContext *c, JSValue a, uint32_t *i, int mode, const char *key, const SelList *sl, bool one, int depth) {
    if (depth > 300 || *i >= 5000) return;
    for (Node *k = n->first; k; k = k->next) {
        if (k->type != NT_ELEM) continue;
        bool m = false;
        if (mode == 0) m = !strcmp(key, "*") || !strcmp(k->name, key);
        else if (mode == 1) {   /* every class in key */
            m = true;
            const char *p = key;
            while (*p && m) {
                while (*p == ' ') p++;
                const char *e = p;
                while (*e && *e != ' ') e++;
                if (e > p) {
                    bool found = false;
                    for (int j = 0; j < k->nclasses && !found; j++)
                        if (strlen(k->classes[j]) == (size_t)(e - p) && !strncmp(k->classes[j], p, (size_t)(e - p))) found = true;
                    if (!found) m = false;
                }
                p = e;
            }
        } else m = sel_match(k, sl);
        if (m) {
            JS_SetPropertyUint32(c, a, (*i)++, H(c, k));
            if (one) return;
        }
        collect(k, c, a, i, mode, key, sl, one, depth + 1);
        if (one && *i) return;
    }
}

FN(n_by_tag) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *t = JS_ToCString(c, argv[1]);
    JSValue a = JS_NewArray(c);
    uint32_t i = 0;
    if (n && t) collect(n, c, a, &i, 0, t, NULL, false, 0);
    if (t) JS_FreeCString(c, t);
    return a;
}

FN(n_by_class) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *t = JS_ToCString(c, argv[1]);
    JSValue a = JS_NewArray(c);
    uint32_t i = 0;
    if (n && t) collect(n, c, a, &i, 1, t, NULL, false, 0);
    if (t) JS_FreeCString(c, t);
    return a;
}

static Node *find_id(Node *n, const char *id, int depth) {
    if (depth > 300) return NULL;
    for (Node *k = n->first; k; k = k->next) {
        if (k->type != NT_ELEM) continue;
        if (k->id && !strcmp(k->id, id)) return k;
        Node *r = find_id(k, id, depth + 1);
        if (r) return r;
    }
    return NULL;
}

FN(n_by_id) {
    UNUSED;
    const char *id = JS_ToCString(c, argv[0]);
    Dom *d = DOM();
    Node *n = id && *id && d ? find_id(d->root, id, 0) : NULL;
    if (id) JS_FreeCString(c, id);
    return H(c, n);
}

FN(n_query) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *s = JS_ToCString(c, argv[1]);
    bool one = JS_ToBool(c, argv[2]);
    JSValue a = JS_NewArray(c);
    uint32_t i = 0;
    SelList sl = { 0 };
    bool ok = s && sel_compile(s, &sl);
    if (n && ok) collect(n, c, a, &i, 2, NULL, &sl, one, 0);
    free(sl.chains);
    if (!ok) {
        JS_FreeValue(c, a);
        JSValue e = JS_ThrowSyntaxError(c, "'%s' is not a selector this browser supports", s ? s : "");
        if (s) JS_FreeCString(c, s);
        return e;
    }
    JS_FreeCString(c, s);
    return a;
}

FN(n_matches) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *s = JS_ToCString(c, argv[1]);
    SelList sl = { 0 };
    bool ok = s && sel_compile(s, &sl);
    bool m = n && ok && sel_match(n, &sl);
    free(sl.chains);
    if (!ok) {
        JSValue e = JS_ThrowSyntaxError(c, "'%s' is not a selector this browser supports", s ? s : "");
        if (s) JS_FreeCString(c, s);
        return e;
    }
    JS_FreeCString(c, s);
    return JS_NewBool(c, m);
}

FN(n_value) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    if (!n) return JS_NewString(c, "");
    int f = field_of(n);
    if (f >= 0) return JS_NewString(c, J.page->fields[f].value);
    if (n->tag == TAG_TEXTAREA) { Buf b = { 0 }; text_into(n, &b, 0); return buf_value(c, &b); }
    if (n->tag == TAG_SELECT) {   /* (not a field: the first chosen option) */
        for (Node *o = n->first; o; o = o->next)
            if (o->tag == TAG_OPTION) { const char *v = dom_attr(o, "value"); if (v) return JS_NewString(c, v);
                Buf b = { 0 }; text_into(o, &b, 0); return buf_value(c, &b); }
        return JS_NewString(c, "");
    }
    const char *v = dom_attr(n, "value");
    return JS_NewString(c, v ? v : "");
}

FN(n_set_value) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    const char *v = JS_ToCString(c, argv[1]);
    if (n && v) {
        int f = field_of(n);
        if (f >= 0) {
            Field *fd = &J.page->fields[f];
            snprintf(fd->value, sizeof(fd->value), "%s", v);
            if (fd->type == FIELD_SUBMIT && n->tag == TAG_INPUT && *v) snprintf(fd->label, sizeof(fd->label), "%s", v);
            if (fd->type == FIELD_SELECT) {   /* its caption: the matching option's text */
                SelectOption opts[1];
                (void)opts;
                for (Node *o = n->first; o; o = o->next) {
                    Node *list = o->tag == TAG_OPTGROUP ? o->first : o;
                    for (Node *q = list; q; q = o->tag == TAG_OPTGROUP ? q->next : NULL) {
                        if (q->tag != TAG_OPTION) continue;
                        Buf b = { 0 };
                        text_into(q, &b, 0);
                        const char *ov = dom_attr(q, "value");
                        if (!strcmp(ov ? ov : (b.s ? b.s : ""), v)) snprintf(fd->label, sizeof(fd->label), "%s", b.s ? b.s : "");
                        free(b.s);
                    }
                }
            }
        } else if (n->tag == TAG_TEXTAREA) {
            while (n->first) dom_detach(n->first);
            Node *t = dom_create_text(DOM(), v, strlen(v));
            if (t) dom_append(n, t);
        } else if (n->type == NT_ELEM) {
            dom_set_attr(DOM(), n, "value", v);
        }
        changed();
    }
    if (v) JS_FreeCString(c, v);
    return JS_UNDEFINED;
}

FN(n_checked) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    if (!n) return JS_FALSE;
    int f = field_of(n);
    if (f >= 0) return JS_NewBool(c, J.page->fields[f].checked);
    return JS_NewBool(c, dom_attr(n, "checked") != NULL);
}

FN(n_set_checked) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    bool on = JS_ToBool(c, argv[1]);
    if (n) {
        int f = field_of(n);
        if (f >= 0) {
            if (on && J.page->fields[f].type == FIELD_RADIO) page_toggle_field(J.page, f);
            else J.page->fields[f].checked = on;
        } else dom_set_attr(DOM(), n, "checked", on ? "" : NULL);
        changed();
    }
    return JS_UNDEFINED;
}

/* is it showing? (not taken out of the page, and not display: none) */
static bool shown(const Node *n) {
    if (!is_connected(n)) return false;
    for (const Node *a = n; a && a->tag != TAG_ROOT; a = a->parent) {
        if (a->type != NT_ELEM) continue;
        if (a->style && a->style->display == D_NONE) return false;
        const char *st = dom_attr(a, "style");
        if (st && strstr(st, "display") && (strstr(st, "display:none") || strstr(st, "display: none"))) return false;
        if (dom_attr(a, "hidden")) return false;
    }
    return true;
}
FN(n_shown) { UNUSED; Node *n = NODE(c, argv[0]); return JS_NewBool(c, n && shown(n)); }

FN(n_listen) { UNUSED; Node *n = NODE(c, argv[0]); if (n) page_make_clickable(J.page, n); return JS_UNDEFINED; }

FN(n_write) {
    UNUSED;
    size_t len;
    const char *s = arg_str(c, argv[0], &len);
    Dom *d = DOM();
    if (s && d && len) {
        Node *tmp = dom_create_elem(d, "div");
        if (tmp) {
            dom_parse_fragment(d, tmp, s, len);
            Node *after = J.write_after;
            Node *parent = after && after->parent ? after->parent : NULL;
            if (!parent) {   /* after the page has loaded: at the end of the body */
                parent = d->root;
                for (Node *k = d->root->first; k; k = k->next)
                    if (k->tag == TAG_HTML) { parent = k; for (Node *b = k->first; b; b = b->next) if (b->tag == TAG_BODY) parent = b; }
                    else if (k->tag == TAG_BODY) parent = k;
                after = parent->last;
            }
            int at = J.next + 1;   /* scripts it wrote run next, in order */
            while (tmp->first) {
                Node *k = tmp->first;
                dom_detach(k);
                if (after && after->parent == parent) dom_insert_before(parent, k, after->next);
                else dom_append(parent, k);
                after = k;
                page_dom_added(J.page, k);
                /* <script> elements it wrote: queued */
                Node *stack[64];
                int sp = 0;
                stack[sp++] = k;
                while (sp) {
                    Node *x = stack[--sp];
                    if (x->type != NT_ELEM) continue;
                    if (x->tag == TAG_SCRIPT && J.write_after) {
                        int before = J.nq;
                        queue_add_at(x, at);
                        if (J.nq > before) at++;
                    }
                    for (Node *y = x->last; y && sp < 64; y = y->prev) stack[sp++] = y;
                }
            }
            if (J.write_after) J.write_after = after;
            changed();
        }
    }
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

FN(n_current_script) { UNUSED; (void)argv; return H(c, J.cur_script); }

FN(n_title) { UNUSED; (void)argv; return JS_NewString(c, J.page ? J.page->title : ""); }
FN(n_set_title) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    if (s && J.page) { snprintf(J.page->title, sizeof(J.page->title), "%s", s); dom_decode_inplace(J.page->title, sizeof(J.page->title)); }
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}
FN(n_url) { UNUSED; (void)argv; return JS_NewString(c, J.page ? J.page->url : ""); }

FN(n_resolve) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    char out[URL_MAX] = "";
    if (s && J.page) {
        const char *b = argc > 1 && JS_IsString(argv[1]) ? JS_ToCString(c, argv[1]) : NULL;
        if (!strncasecmp(s, "javascript:", 11) || (!*s && !b)) snprintf(out, sizeof(out), "%s", *s ? s : J.page->url);
        else url_resolve(b && *b ? b : J.page->base, s, out, sizeof(out));
        if (b) JS_FreeCString(c, b);
    }
    if (s) JS_FreeCString(c, s);
    return JS_NewString(c, out);
}

static bool page_url(Url *u);

/* N.net(method, url, body, contentType, accept, withCredentials): queues a
 * download for the browser; its number, or -1 if it can't be done. */
FN(n_net) {
    UNUSED;
    if (argc < 6 || !J.page) return JS_NewInt32(c, -1);
    const char *m = JS_ToCString(c, argv[0]), *s = JS_ToCString(c, argv[1]);
    const char *b = JS_IsNull(argv[2]) || JS_IsUndefined(argv[2]) ? NULL : JS_ToCString(c, argv[2]);
    const char *ct = JS_ToCString(c, argv[3]), *ac = JS_ToCString(c, argv[4]);
    bool creds = JS_ToBool(c, argv[5]);
    int slot = -1;
    char abs[URL_MAX] = "";
    if (m && s) url_resolve(J.page->base, s, abs, sizeof(abs));
    Url u, pu;
    bool web = abs[0] && url_parse(abs, &u) && (!strcmp(u.scheme, "http") || !strcmp(u.scheme, "https"));
    bool post = m && !strcasecmp(m, "POST");
    if (!m || !web) {
        jslog("A script asked for %.120s, which CubeBrowser can't download", s ? s : "?");
    } else if (strcasecmp(m, "GET") && strcasecmp(m, "POST") && strcasecmp(m, "HEAD")) {
        jslog("A script asked to %.10s %.110s (only GET and POST are supported)", m, abs);
    } else {
        for (int i = 0; i < NET_MAX; i++) if (J.net[i].state == NET_FREE) { slot = i; break; }
        if (slot < 0) jslog("Too many downloads at once from scripts: %.120s skipped", abs);
    }
    if (slot >= 0) {
        NetReq *r = &J.net[slot];
        r->url = strdup(abs);
        r->body = post ? strdup(b ? b : "") : NULL;
        if (!r->url || (post && !r->body)) { free(r->url); free(r->body); r->url = r->body = NULL; slot = -1; }
        else {
            snprintf(r->ctype, sizeof(r->ctype), "%s", ct && *ct ? ct : "text/plain;charset=UTF-8");
            snprintf(r->accept, sizeof(r->accept), "%s", ac && *ac ? ac : "*/*");
            /* the page's own site gets its cookies; others only when asked */
            r->cookies = creds || (page_url(&pu) && !strcasecmp(pu.host, u.host) && !strcmp(pu.scheme, u.scheme) && pu.port == u.port);
            r->state = NET_QUEUED;
            jslog("A script asked for %.140s", abs);
        }
    }
    if (m) JS_FreeCString(c, m);
    if (s) JS_FreeCString(c, s);
    if (b) JS_FreeCString(c, b);
    if (ct) JS_FreeCString(c, ct);
    if (ac) JS_FreeCString(c, ac);
    return JS_NewInt32(c, slot);
}

/* the time zone set in Settings, in minutes east of UTC (with daylight saving) */
FN(n_tz) { UNUSED; (void)argv; return JS_NewInt32(c, g_set.tz_minutes + (g_set.dst ? 60 : 0)); }

static bool page_url(Url *u) { return J.page && url_parse(J.page->url, u) && (!strcmp(u->scheme, "http") || !strcmp(u->scheme, "https")); }

FN(n_cookie) {
    UNUSED;
    (void)argv;
    Url u;
    char out[2048] = "";
    if (page_url(&u)) cookies_for_script(u.host, u.path, !strcmp(u.scheme, "https"), time(NULL), out, sizeof(out));
    return JS_NewString(c, out);
}

FN(n_set_cookie) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    Url u;
    if (s && page_url(&u)) {
        cookies_set_by_script(u.host, u.path, !strcmp(u.scheme, "https"), s, time(NULL));
        const char *eq = strchr(s, '=');
        jslog("A script set the cookie \"%.*s\"", eq ? (int)(eq - s > 40 ? 40 : eq - s) : 40, s);
    }
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}

FN(n_ua) { UNUSED; (void)argv; return JS_NewString(c, user_agent_strings[g_set.user_agent >= 0 && g_set.user_agent <= 2 ? g_set.user_agent : 0]); }

/* a note from the browser's own script part (no "console:" in front) */
FN(n_note) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    if (s) { jslog("%.170s", s); JS_FreeCString(c, s); }
    return JS_UNDEFINED;
}

FN(n_log) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    if (s) {
        diag_note(DIAG_PAGE, "script: %.150s", s);
        jslog("console: %.160s", s);
#ifndef GEKKO
        fprintf(stderr, "[console] %s\n", s);
#endif
        JS_FreeCString(c, s);
    }
    return JS_UNDEFINED;
}

FN(n_alert) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    if (s) {
        snprintf(J.message, sizeof(J.message), "The page says: %s", s);
        dom_decode_inplace(J.message, sizeof(J.message));
        JS_FreeCString(c, s);
    }
    return JS_UNDEFINED;
}

FN(n_now) { UNUSED; (void)argv; return JS_NewFloat64(c, now_ms()); }
FN(n_timers) {
    UNUSED;
    J.timers_on = JS_ToBool(c, argv[0]);
    double at = 0;
    if (argc > 1) JS_ToFloat64(c, &at, argv[1]);
    J.timer_at = at;
    return JS_UNDEFINED;
}

FN(n_go) {
    UNUSED;
    const char *s = JS_ToCString(c, argv[0]);
    if (s && *s && strncasecmp(s, "javascript:", 11)) {
        snprintf(J.nav_url, sizeof(J.nav_url), "%s", s);
        J.nav = JS_ToBool(c, argv[1]) ? JS_NAV_REPLACE : JS_NAV_GO;
        jslog("A script opened %.140s", s);
    }
    if (s) JS_FreeCString(c, s);
    return JS_UNDEFINED;
}
FN(n_back) { UNUSED; (void)argv; J.nav = JS_NAV_BACK; return JS_UNDEFINED; }

/* form.submit(): the browser sends the form (from its first field) */
FN(n_submit) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    if (n && n->tag == TAG_FORM && n->index >= 0 && J.page) {
        int pick = -1;
        for (int i = 0; i < J.page->nfields; i++)
            if (J.page->fields[i].form == n->index) {
                if (pick < 0) pick = i;
                if (J.page->fields[i].type == FIELD_SUBMIT && !J.page->fields[i].button) { pick = i; break; }
            }
        if (pick >= 0) { J.nav = JS_NAV_SUBMIT; J.nav_field = pick; }
    }
    return JS_UNDEFINED;
}

/* element.click(): what a click would do when no script stops it */
FN(n_activate) {
    UNUSED;
    Node *n = NODE(c, argv[0]);
    if (!n || !J.page) return JS_UNDEFINED;
    int f = field_of(n);
    if (n->tag == TAG_A && n->index >= 0) {
        const char *u = page_link_url(J.page, n->index);
        if (u && *u && strncasecmp(u, "javascript:", 11)) { snprintf(J.nav_url, sizeof(J.nav_url), "%s", u); J.nav = JS_NAV_GO; }
    } else if (n->tag == TAG_A && dom_attr(n, "href")) {   /* a link a script made */
        const char *h = dom_attr(n, "href");
        if (*h && strncasecmp(h, "javascript:", 11) && h[0] != '#') {
            url_resolve(J.page->base, h, J.nav_url, sizeof(J.nav_url));
            J.nav = JS_NAV_GO;
        }
    } else if (f >= 0) {
        Field *fd = &J.page->fields[f];
        if (fd->type == FIELD_SUBMIT && !fd->button) { J.nav = JS_NAV_SUBMIT; J.nav_field = f; }
        else if (fd->type == FIELD_CHECK || fd->type == FIELD_RADIO) { page_toggle_field(J.page, f); changed(); }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry n_funcs[] = {
    JS_CFUNC_DEF("type", 1, n_type), JS_CFUNC_DEF("tag", 1, n_tag), JS_CFUNC_DEF("parent", 1, n_parent),
    JS_CFUNC_DEF("first", 1, n_first), JS_CFUNC_DEF("last", 1, n_last), JS_CFUNC_DEF("next", 1, n_next),
    JS_CFUNC_DEF("prev", 1, n_prev), JS_CFUNC_DEF("doc", 0, n_doc), JS_CFUNC_DEF("connected", 1, n_connected),
    JS_CFUNC_DEF("kids", 2, n_kids), JS_CFUNC_DEF("attr", 2, n_attr), JS_CFUNC_DEF("attrs", 1, n_attrs),
    JS_CFUNC_DEF("setAttr", 3, n_set_attr), JS_CFUNC_DEF("rmAttr", 2, n_rm_attr), JS_CFUNC_DEF("text", 1, n_text),
    JS_CFUNC_DEF("setText", 2, n_set_text), JS_CFUNC_DEF("inner", 1, n_inner), JS_CFUNC_DEF("outer", 1, n_outer),
    JS_CFUNC_DEF("setInner", 2, n_set_inner), JS_CFUNC_DEF("create", 1, n_create),
    JS_CFUNC_DEF("createText", 1, n_create_text), JS_CFUNC_DEF("append", 2, n_append),
    JS_CFUNC_DEF("insert", 3, n_insert), JS_CFUNC_DEF("detach", 1, n_detach), JS_CFUNC_DEF("clone", 2, n_clone),
    JS_CFUNC_DEF("byTag", 2, n_by_tag), JS_CFUNC_DEF("byClass", 2, n_by_class), JS_CFUNC_DEF("byId", 1, n_by_id),
    JS_CFUNC_DEF("query", 3, n_query), JS_CFUNC_DEF("matches", 2, n_matches), JS_CFUNC_DEF("value", 1, n_value),
    JS_CFUNC_DEF("setValue", 2, n_set_value), JS_CFUNC_DEF("checked", 1, n_checked),
    JS_CFUNC_DEF("setChecked", 2, n_set_checked), JS_CFUNC_DEF("shown", 1, n_shown),
    JS_CFUNC_DEF("listen", 1, n_listen), JS_CFUNC_DEF("write", 1, n_write),
    JS_CFUNC_DEF("currentScript", 0, n_current_script), JS_CFUNC_DEF("title", 0, n_title),
    JS_CFUNC_DEF("setTitle", 1, n_set_title), JS_CFUNC_DEF("url", 0, n_url), JS_CFUNC_DEF("resolve", 2, n_resolve),
    JS_CFUNC_DEF("cookie", 0, n_cookie), JS_CFUNC_DEF("setCookie", 1, n_set_cookie), JS_CFUNC_DEF("ua", 0, n_ua),
    JS_CFUNC_DEF("log", 1, n_log), JS_CFUNC_DEF("note", 1, n_note), JS_CFUNC_DEF("net", 6, n_net), JS_CFUNC_DEF("tz", 0, n_tz), JS_CFUNC_DEF("alert", 1, n_alert), JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("timers", 1, n_timers), JS_CFUNC_DEF("go", 2, n_go), JS_CFUNC_DEF("back", 0, n_back),
    JS_CFUNC_DEF("submit", 1, n_submit), JS_CFUNC_DEF("activate", 1, n_activate),
};

/* ------------------------------------------------------------- running */

static int interrupt(JSRuntime *rt, void *opaque) {
    (void)rt; (void)opaque;
    if (now_ms() > J.deadline) { J.stopped_now = true; return 1; }
    return 0;
}

static void report_exception(void) {
    JSValue e = JS_GetException(J.ctx);
    if (J.stopped_now) {
        J.stopped_now = false;
        J.stops++;
        snprintf(J.message, sizeof(J.message), "A script on this page took too long and was stopped.");
        jslog("STOPPED: a script ran too long");
        if (J.stops >= 3) { J.failed = true; jslog("Scripts turned off for this page (stopped 3 times)"); }
    } else {
        const char *s = JS_ToCString(J.ctx, e);
        JSValue stack = JS_GetPropertyStr(J.ctx, e, "stack");
        const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(J.ctx, stack);
        diag_note(DIAG_PAGE, "script error: %.100s %.80s", s ? s : "?", st ? st : "");
        {   /* the log: the error, and where (the first line of the stack) */
            const char *at = st ? strstr(st, "at ") : NULL;
            char where[100] = "";
            if (at) { size_t k = 0; at += 3; while (at[k] && at[k] != '\n' && k < sizeof(where) - 1) { where[k] = at[k]; k++; } where[k] = 0; }
            jslog("ERROR: %.90s%s%.80s", s ? s : "?", where[0] ? " | " : "", where);
        }
#ifndef GEKKO
        fprintf(stderr, "[js] %s\n%s\n", s ? s : "?", st ? st : "");   /* (PC tests) */
#endif
        if (st) JS_FreeCString(J.ctx, st);
        JS_FreeValue(J.ctx, stack);
        if (s) JS_FreeCString(J.ctx, s);
    }
    JS_FreeValue(J.ctx, e);
}

/* promise callbacks and the like, after each piece of script */
static void run_jobs(void) {
    JSContext *c;
    for (int i = 0; i < 2000; i++) {
        int r = JS_ExecutePendingJob(J.rt, &c);
        if (r <= 0) { if (r < 0) report_exception(); break; }
    }
}

/* QuickJS's memory, counted here (the GameCube's C library can't say how
 * big a block is): each block has its size in front of it. */
#define JM_HEAD 8
static void *jm_malloc(JSMallocState *s, size_t size) {
    if (s->malloc_size + size + JM_HEAD > s->malloc_limit) return NULL;
    char *p = malloc(size + JM_HEAD);
    if (!p) return NULL;
    *(size_t *)p = size;
    s->malloc_count++;
    s->malloc_size += size + JM_HEAD;
    return p + JM_HEAD;
}
static void jm_free(JSMallocState *s, void *ptr) {
    if (!ptr) return;
    char *p = (char *)ptr - JM_HEAD;
    s->malloc_count--;
    s->malloc_size -= *(size_t *)p + JM_HEAD;
    free(p);
}
static void *jm_realloc(JSMallocState *s, void *ptr, size_t size) {
    if (!ptr) return size ? jm_malloc(s, size) : NULL;
    char *p = (char *)ptr - JM_HEAD;
    size_t old = *(size_t *)p;
    if (!size) { jm_free(s, ptr); return NULL; }
    if (size > old && s->malloc_size + (size - old) > s->malloc_limit) return NULL;
    char *np = realloc(p, size + JM_HEAD);
    if (!np) return NULL;
    *(size_t *)np = size;
    s->malloc_size = s->malloc_size - old + size;
    return np + JM_HEAD;
}
static size_t jm_size(const void *ptr) { return ptr ? *(const size_t *)((const char *)ptr - JM_HEAD) : 0; }
/* A promise failed and nothing was waiting to catch it (in a browser this shows
 * in the console too). Our own "can't download yet" failures are logged
 * already, when the script asks. */
static void promise_failed(JSContext *ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void *op) {
    (void)promise; (void)op;
    if (handled) return;
    const char *s = JS_ToCString(ctx, reason);
    if (s && !strstr(s, "not supported yet")) {
        char where[100] = "";
        JSValue st = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED;
        const char *sts = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
        const char *at = sts ? strstr(sts, "at ") : NULL;
        if (at) {
            size_t n = strcspn(at + 3, "\n");
            snprintf(where, sizeof(where), " | %.*s", (int)(n < 90 ? n : 90), at + 3);
        }
        if (sts) JS_FreeCString(ctx, sts);
        JS_FreeValue(ctx, st);
        jslog("Error (in a promise): %.120s%s", s, where);
    }
    if (s) JS_FreeCString(ctx, s);
}

static const JSMallocFunctions jm_funcs = { jm_malloc, jm_free, jm_realloc, jm_size };

static void start_context(void) {
    if (J.started || J.failed) return;
    J.started = true;
    J.rt = JS_NewRuntime2(&jm_funcs, NULL);
    if (!J.rt) { J.failed = true; jslog("Not enough memory to start JavaScript"); return; }
    size_t free_now = cb_mem_free();
    size_t limit = free_now / 3;
    if (limit > 6 * 1024 * 1024) limit = 6 * 1024 * 1024;
    if (limit < 1536 * 1024) limit = 1536 * 1024;
    JS_SetMemoryLimit(J.rt, limit);
    JS_SetMaxStackSize(J.rt, JS_STACK_LIMIT);
    JS_SetInterruptHandler(J.rt, interrupt, NULL);
    JS_SetHostPromiseRejectionTracker(J.rt, promise_failed, NULL);
    JS_SetModuleLoaderFunc(J.rt, mod_normalize, mod_load, NULL);
    J.ctx = JS_NewContext(J.rt);
    if (!J.ctx) { J.failed = true; return; }
    JSValue global = JS_GetGlobalObject(J.ctx);
    JSValue n = JS_NewObject(J.ctx);
    JS_SetPropertyFunctionList(J.ctx, n, n_funcs, (int)(sizeof(n_funcs) / sizeof(n_funcs[0])));
    JS_SetPropertyStr(J.ctx, global, "__n", n);
    J.deadline = now_ms() + LOAD_TIME_LIMIT;
    JSValue api = JS_Eval(J.ctx, js_prelude, js_prelude_len, "<browser>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(api)) { report_exception(); J.failed = true; jslog("The browser's own script part failed to start"); JS_FreeValue(J.ctx, global); return; }
    J.fn_timers = JS_GetPropertyStr(J.ctx, api, "timers");
    J.fn_dispatch = JS_GetPropertyStr(J.ctx, api, "dispatch");
    J.fn_loaded = JS_GetPropertyStr(J.ctx, api, "loaded");
    J.fn_net = JS_GetPropertyStr(J.ctx, api, "net");
    JS_FreeValue(J.ctx, api);
    JS_FreeValue(J.ctx, global);
}

static bool ready(void) { return J.page && !J.failed && g_set.javascript; }

/* Runs one script of the page's (on the big stack). */
/* ------------------------------------------------------------ modules */

static bool is_path_spec(const char *s, size_t n) {
    return (n >= 2 && s[0] == '.' && (s[1] == '/' || (n >= 3 && s[1] == '.' && s[2] == '/'))) || (n >= 1 && s[0] == '/') ||
           (n >= 7 && !strncmp(s, "http://", 7)) || (n >= 8 && !strncmp(s, "https://", 8));
}

static ModFile *mod_find(const char *url) {
    for (int i = 0; i < J.nmods; i++) if (!strcmp(J.mods[i].url, url)) return &J.mods[i];
    return NULL;
}

static void mod_add(const char *base, const char *spec, size_t n) {
    char rel[URL_MAX], abs[URL_MAX];
    if (n >= sizeof(rel)) return;
    memcpy(rel, spec, n);
    rel[n] = 0;
    if (!is_path_spec(rel, n)) { jslog("A module imports \"%.60s\" (a bare name: not supported)", rel); return; }
    url_resolve(base, rel, abs, sizeof(abs));
    if (mod_find(abs)) return;
    if (J.nmods >= MOD_MAX) return;
    if (J.nmods == J.capmods) {
        int nc = J.capmods ? J.capmods * 2 : 16;
        ModFile *nm = realloc(J.mods, sizeof(ModFile) * (size_t)nc);
        if (!nm) return;
        J.mods = nm;
        J.capmods = nc;
    }
    ModFile *m = &J.mods[J.nmods];
    memset(m, 0, sizeof(*m));
    m->url = strdup(abs);
    if (!m->url) return;
    m->state = SQ_NEED;
    J.nmods++;
}

static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_' || c == '$'; }

/* The files a module's code imports: import ... from "x", import "x",
 * export ... from "x" and import("x") with a written-out address. (A look
 * through the text, not a full parse: a stray match only costs a download.) */
static void scan_imports(const char *code, size_t len, const char *base) {
    for (size_t i = 0; i + 6 < len; i++) {
        bool imp = !memcmp(code + i, "import", 6), exp = !memcmp(code + i, "export", 6);
        if (!imp && !exp) continue;
        if (i > 0 && (ident_char(code[i - 1]) || code[i - 1] == '.')) continue;
        size_t j = i + 6;
        if (j < len && ident_char(code[j])) continue;
        while (j < len && isspace((unsigned char)code[j])) j++;
        if (j >= len) break;
        if (imp && code[j] == '.') continue;                 /* import.meta */
        if (imp && code[j] == '(') {                         /* import("x") */
            j++;
            while (j < len && isspace((unsigned char)code[j])) j++;
        } else if (!(imp && (code[j] == '"' || code[j] == '\''))) {
            /* ... from "x", before the statement ends */
            size_t k = j, stop = j + 4000 < len ? j + 4000 : len;
            bool found = false;
            int depth = 0;
            for (; k + 4 < stop; k++) {
                char c = code[k];
                if (c == '{') depth++;
                else if (c == '}') depth--;
                else if (depth <= 0 && (c == ';' || c == '(' || c == '=')) break;
                if (!memcmp(code + k, "from", 4) && (k == 0 || !ident_char(code[k - 1])) && !ident_char(code[k + 4])) {
                    k += 4;
                    while (k < len && isspace((unsigned char)code[k])) k++;
                    if (k < len && (code[k] == '"' || code[k] == '\'')) { found = true; break; }
                }
            }
            if (!found) continue;
            j = k;
        }
        if (j >= len || (code[j] != '"' && code[j] != '\'' && code[j] != '`')) continue;
        char q = code[j++];
        size_t a = j;
        while (j < len && code[j] != q && code[j] != '\n' && j - a < URL_MAX) j++;
        if (j >= len || code[j] != q) continue;
        if (q == '`' && memchr(code + a, '$', j - a)) continue;   /* a template with ${}: not a fixed address */
        mod_add(base, code + a, j - a);
        i = j;
    }
}

static bool mods_pending(void) {
    for (int i = 0; i < J.nmods; i++) if (J.mods[i].state == SQ_NEED || J.mods[i].state == SQ_LOADING) return true;
    return false;
}

static void set_import_meta(JSContext *ctx, JSModuleDef *d, const char *url) {
    JSValue meta = JS_GetImportMeta(ctx, d);
    if (JS_IsException(meta)) return;
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta);
}

/* QuickJS: a module's import, made into an address (relative to the module) */
static char *mod_normalize(JSContext *ctx, const char *base, const char *name, void *op) {
    (void)op;
    char out[URL_MAX];
    if (is_path_spec(name, strlen(name))) url_resolve(base, name, out, sizeof(out));
    else snprintf(out, sizeof(out), "%s", name);
    size_t n = strlen(out);
    char *r = js_malloc(ctx, n + 1);
    if (r) memcpy(r, out, n + 1);
    return r;
}

/* QuickJS: the module at this address (downloaded beforehand) */
static JSModuleDef *mod_load(JSContext *ctx, const char *name, void *op) {
    (void)op;
    ModFile *m = mod_find(name);
    if (!m || !m->code) {
        JS_ThrowReferenceError(ctx, "could not load module %s%s", name,
                               !m ? "" : m->failed ? " (its download failed)" : " (not downloaded)");
        return NULL;
    }
    JSValue f = JS_Eval(ctx, m->code, m->len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(m->code);
    m->code = NULL;
    m->state = SQ_DONE;
    if (JS_IsException(f)) return NULL;
    JSModuleDef *d = JS_VALUE_GET_PTR(f);
    set_import_meta(ctx, d, name);
    JS_FreeValue(ctx, f);
    return d;
}

static void run_module(Script *s, const char *code, size_t len) {
    const char *name = s->url ? s->url : J.page->base;
    JSValue f = JS_Eval(J.ctx, code, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(f)) { report_exception(); return; }
    set_import_meta(J.ctx, JS_VALUE_GET_PTR(f), name);
    JSValue r = JS_EvalFunction(J.ctx, f);   /* (takes f) */
    if (JS_IsException(r)) report_exception();
    JS_FreeValue(J.ctx, r);
}

static void run_script_job(void *arg) {
    Script *s = arg;
    start_context();
    if (J.failed) return;
    const char *code = s->code ? s->code : s->node->text;
    size_t len = s->code ? s->len : (size_t)s->node->len;
    if (!code || !len) return;
    J.cur_script = s->node;
    J.write_after = s->node;
    J.deadline = now_ms() + LOAD_TIME_LIMIT;
    char name[128];
    snprintf(name, sizeof(name), "%.120s", s->url ? s->url : "<page>");
    double t0 = now_ms();
    if (s->module) {
        run_module(s, code, len);
    } else {
        JSValue r = JS_Eval(J.ctx, code, len, name, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) report_exception();
        JS_FreeValue(J.ctx, r);
    }
    run_jobs();
    const char *what = s->module ? "module" : "script";
    if (s->url) jslog("Ran %s%.120s (%.0f ms)", s->module ? "module " : "", s->url, now_ms() - t0);
    else jslog("Ran a %s in the page (%u bytes, %.0f ms)", what, (unsigned)len, now_ms() - t0);
    J.cur_script = J.write_after = NULL;
}

static void loaded_job(void *arg) {
    (void)arg;
    if (!J.started || J.failed) return;
    J.deadline = now_ms() + LOAD_TIME_LIMIT;
    JSValue r = JS_Call(J.ctx, J.fn_loaded, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) report_exception();
    JS_FreeValue(J.ctx, r);
    run_jobs();
}

static void timers_job(void *arg) {
    (void)arg;
    if (!J.started || J.failed) return;
    J.deadline = now_ms() + EVENT_TIME_LIMIT;
    JSValue now = JS_NewFloat64(J.ctx, now_ms());
    JSValue r = JS_Call(J.ctx, J.fn_timers, JS_UNDEFINED, 1, &now);
    if (JS_IsException(r)) report_exception();
    JS_FreeValue(J.ctx, r);
    run_jobs();
}

typedef struct { Node *n; const char *type; bool result; } EventJob;

static void event_job(void *arg) {
    EventJob *e = arg;
    e->result = false;
    start_context();
    if (J.failed) return;
    J.deadline = now_ms() + EVENT_TIME_LIMIT;
    JSValue args[2] = { H(J.ctx, e->n), JS_NewString(J.ctx, e->type) };
    JSValue r = JS_Call(J.ctx, J.fn_dispatch, JS_UNDEFINED, 2, args);
    JS_FreeValue(J.ctx, args[1]);
    if (JS_IsException(r)) report_exception();
    else e->result = JS_ToBool(J.ctx, r);
    JS_FreeValue(J.ctx, r);
    run_jobs();
}

/* The page changed: it's laid out again (from timers, not too often). */
static void apply_changes(bool from_timer) {
    if (!J.changed || !J.page) return;
    Dom *d = DOM();
    if (d && (d->arena.used > J.arena_start + DOM_GROWTH_MAX || cb_mem_free() < MEM_FLOOR) && !J.failed) {
        J.failed = true;   /* (the page itself stays as it is) */
        snprintf(J.message, sizeof(J.message), "This page's scripts were stopped: they were using too much memory.");
        jslog("STOPPED: scripts were using too much memory");
    }
    double t = now_ms();
    if (from_timer && t - J.last_relayout < TIMER_RELAYOUT_MS) return;
    J.changed = false;
    J.last_relayout = t;
    J.page->restyle = true;
}

/* -------------------------------------------------------------- the queue */

static bool runnable_type(const Node *n) {
    const char *t = dom_attr(n, "type");
    if (!t || !*t) return true;
    char low[48];
    snprintf(low, sizeof(low), "%s", t);
    for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
    return strstr(low, "javascript") || strstr(low, "ecmascript") || !strcmp(low, "text/jscript");
}

static void queue_add_at(Node *n, int at) {
    if (n->sub == 1) return;   /* queued already */
    n->sub = 1;
    const char *ty = dom_attr(n, "type");
    bool module = ty && !strcasecmp(ty, "module");
    if (!module && !runnable_type(n)) return;
    if (!module && dom_attr(n, "nomodule")) return;   /* (for browsers without modules: this one runs those) */
    if (J.nq >= DOM_MAX_SCRIPTS * 2) { jslog("Too many scripts: the rest are skipped"); return; }
    if (J.nq == J.capq) {
        int nc = J.capq ? J.capq * 2 : 32;
        Script *ns = realloc(J.q, sizeof(Script) * (size_t)nc);
        if (!ns) return;
        J.q = ns;
        J.capq = nc;
    }
    if (at < 0 || at > J.nq) at = J.nq;
    memmove(&J.q[at + 1], &J.q[at], sizeof(Script) * (size_t)(J.nq - at));
    Script *s = &J.q[at];
    memset(s, 0, sizeof(*s));
    s->node = n;
    s->module = module;
    const char *src = dom_attr(n, "src");
    if (src && *src) {
        char abs[URL_MAX];
        url_resolve(J.page->base, src, abs, sizeof(abs));
        s->url = strdup(abs);
        s->state = s->url && (!strncmp(abs, "http://", 7) || !strncmp(abs, "https://", 8)) ? SQ_NEED : SQ_DONE;
    } else {
        s->state = n->text && n->len ? SQ_READY : SQ_DONE;
    }
    J.nq++;
}

static void free_all(void) {
    if (J.ctx) {
        JS_FreeValue(J.ctx, J.fn_timers);
        JS_FreeValue(J.ctx, J.fn_dispatch);
        JS_FreeValue(J.ctx, J.fn_loaded);
        JS_FreeValue(J.ctx, J.fn_net);
        JS_FreeContext(J.ctx);
    }
    if (J.rt) JS_FreeRuntime(J.rt);
    for (int i = 0; i < J.nq; i++) { free(J.q[i].url); free(J.q[i].code); }
    for (int i = 0; i < NET_MAX; i++) { free(J.net[i].url); free(J.net[i].body); }
    for (int i = 0; i < J.nmods; i++) { free(J.mods[i].url); free(J.mods[i].code); }
    free(J.mods);
    free(J.q);
    free(J.reg);
    memset(&J, 0, sizeof(J));
}

static void end_job(void *arg) { (void)arg; free_all(); }

/* (QuickJS lets go on the thread it ran on: the big stack's) */
void js_page_end(void) { if (J.page || J.rt) run_with_big_stack(end_job, NULL); }

int js_next_request(JsRequest *out) {
    if (!J.page) return -1;
    for (int i = 0; i < NET_MAX; i++) {
        NetReq *r = &J.net[i];
        if (r->state != NET_QUEUED) continue;
        r->state = NET_LOADING;
        out->url = r->url;
        out->post = r->body;
        out->content_type = r->ctype;
        out->accept = r->accept;
        out->cookies = r->cookies;
        return i;
    }
    return -1;
}

typedef struct { int slot; HttpResult *r; } NetJob;

static void net_job(void *arg) {
    NetJob *j = arg;
    NetReq *q = &J.net[j->slot];
    HttpResult *r = j->r;
    bool ok = r && !r->error[0] && r->status > 0;
    if (r && r->blocked) J.blocked++;
    if (ok) {
        jslog("Got %.120s (%d, %u bytes)", q->url, r->status, (unsigned)r->body_len);
    } else {
        jslog("Couldn't download %.100s: %.60s", q->url, r && r->error[0] ? r->error : "no answer");
    }
    free(q->url); free(q->body);
    q->url = q->body = NULL;
    q->state = NET_FREE;
    if (!J.started || J.failed) return;
    J.deadline = now_ms() + EVENT_TIME_LIMIT;
    size_t len = ok && r->body ? r->body_len : 0;
    if (len > NET_BODY_MAX) len = NET_BODY_MAX;
    JSValue args[5] = {
        JS_NewInt32(J.ctx, j->slot),
        JS_NewInt32(J.ctx, ok ? r->status : 0),
        JS_NewString(J.ctx, ok ? r->content_type : ""),
        JS_NewString(J.ctx, ok ? r->url : ""),
        JS_NewStringLen(J.ctx, ok && r->body ? r->body : "", len),
    };
    JSValue res = JS_Call(J.ctx, J.fn_net, JS_UNDEFINED, 5, args);
    if (JS_IsException(res)) report_exception();
    JS_FreeValue(J.ctx, res);
    for (int i = 0; i < 5; i++) JS_FreeValue(J.ctx, args[i]);
    run_jobs();
}

void js_request_done(int slot, HttpResult *r) {
    if (slot < 0 || slot >= NET_MAX || J.net[slot].state != NET_LOADING) return;
    NetJob j = { slot, r };
    run_with_big_stack(net_job, &j);
    apply_changes(true);
}

void js_page_begin(Page *p) {
    js_page_end();
    if (p && strncasecmp(p->url, "about:", 6)) {   /* (about: pages, like the log itself, keep the last one) */
        log_count = 0;
        snprintf(log_page, sizeof(log_page), "%s", p->url);
        if (!g_set.javascript) jslog("JavaScript is turned off (Settings > Browsing)");
    }
    if (!g_set.javascript || !p || !p->dom || !dom_keep_scripts) return;
    J.page = p;
    J.arena_start = p->dom->arena.used;
    if (strncasecmp(p->url, "about:", 6))
        jslog("The page has %d script%s", p->dom->nscript_nodes, p->dom->nscript_nodes == 1 ? "" : "s");
    /* module scripts wait for the page (like defer): after the others */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < p->dom->nscript_nodes; i++) {
            const char *ty = dom_attr(p->dom->scripts[i], "type");
            if ((ty && !strcasecmp(ty, "module")) == (pass == 1)) queue_add_at(p->dom->scripts[i], -1);
        }
}

int js_next_script(char *url, size_t cap) {
    if (!ready()) return -1;
    for (int i = J.next; i < J.nq && i < MOD_BASE; i++)
        if (J.q[i].state == SQ_NEED) {
            J.q[i].state = SQ_LOADING;
            snprintf(url, cap, "%s", J.q[i].url);
            return i;
        }
    for (int i = 0; i < J.nmods; i++)
        if (J.mods[i].state == SQ_NEED) {
            J.mods[i].state = SQ_LOADING;
            snprintf(url, cap, "%s", J.mods[i].url);
            return MOD_BASE + i;
        }
    return -1;
}

void js_script_loaded(int idx, char *body, size_t len) {
    if (idx >= MOD_BASE) {   /* a file a module imports */
        int mi = idx - MOD_BASE;
        if (mi >= J.nmods || J.mods[mi].state != SQ_LOADING) { free(body); return; }
        ModFile *m = &J.mods[mi];
        if (body && len && len <= SCRIPT_MAX && J.total + len <= SCRIPTS_TOTAL) {
            m->code = body;
            m->len = len;
            m->state = SQ_READY;
            J.total += len;
            jslog("Downloaded %u KB (module): %.130s", (unsigned)((len + 1023) / 1024), m->url);
            char base[URL_MAX];
            snprintf(base, sizeof(base), "%s", m->url);   /* (adding may move J.mods) */
            scan_imports(body, len, base);
        } else {
            if (!body) jslog("FAILED to download module: %.130s", m->url);
            else if (len > SCRIPT_MAX) jslog("Skipped module (too big, %u KB): %.120s", (unsigned)(len / 1024), m->url);
            else if (len) jslog("Skipped module (too much script on this page): %.110s", m->url);
            free(body);
            m->state = SQ_DONE;
            m->failed = 1;
        }
        return;
    }
    if (idx < 0 || idx >= J.nq || J.q[idx].state != SQ_LOADING) { free(body); return; }
    Script *s = &J.q[idx];
    if (body && len && len <= SCRIPT_MAX && J.total + len <= SCRIPTS_TOTAL) {
        s->code = body;
        s->len = len;
        s->state = SQ_READY;
        J.total += len;
        jslog("Downloaded %u KB: %.140s", (unsigned)((len + 1023) / 1024), s->url);
    } else {
        if (!body) jslog("FAILED to download: %.140s", s->url);
        else if (len > SCRIPT_MAX) jslog("Skipped (too big, %u KB): %.120s", (unsigned)(len / 1024), s->url);
        else if (len) jslog("Skipped (too much script on this page): %.120s", s->url);
        free(body);
        s->state = SQ_DONE;
        s->failed = 1;
    }
}

void js_script_blocked(int idx) {
    char *u = NULL;
    if (idx >= MOD_BASE && idx - MOD_BASE < J.nmods && J.mods[idx - MOD_BASE].state == SQ_LOADING) {
        ModFile *m = &J.mods[idx - MOD_BASE];
        m->state = SQ_DONE;
        m->failed = 1;
        u = m->url;
    } else if (idx >= 0 && idx < J.nq && J.q[idx].state == SQ_LOADING) {
        J.q[idx].state = SQ_DONE;
        J.q[idx].failed = 1;
        u = J.q[idx].url;
    }
    if (u) { jslog("Blocked (ad list): %.140s", u); J.blocked++; }
}

bool js_scripts_pending(void) {
    if (!ready()) return false;
    for (int i = J.next; i < J.nq; i++)
        if (J.q[i].state == SQ_NEED || J.q[i].state == SQ_LOADING) return true;
    return mods_pending();
}

/* a script element's load or error event (loaders wait for it) */
static void script_event(Node *n, const char *type) {
    if (!J.started || J.failed || !n) return;
    EventJob e = { n, type, false };
    run_with_big_stack(event_job, &e);
}

int js_blocked_count(void) { return J.page ? J.blocked : 0; }

bool js_net_pending(void) {
    if (!ready()) return false;
    for (int i = 0; i < NET_MAX; i++) if (J.net[i].state != NET_FREE) return true;
    return false;
}

bool js_busy(void) { return ready() && (J.next < J.nq || (!J.loaded && J.nq)); }

void js_frame(void) {
    if (!ready()) return;
    double start = now_ms();
    /* the page's scripts, in order (a file still downloading holds the rest) */
    while (J.next < J.nq && !J.failed) {
        Script *s = &J.q[J.next];
        if (s->state == SQ_NEED || s->state == SQ_LOADING) break;
        if (s->state == SQ_READY && s->module) {   /* first the files it imports */
            if (!s->scanned) {
                s->scanned = 1;
                const char *code = s->code ? s->code : s->node->text;
                size_t clen = s->code ? s->len : (size_t)s->node->len;
                if (code) scan_imports(code, clen, s->url ? s->url : J.page->base);
            }
            if (mods_pending()) break;
        }
        if (s->state == SQ_READY) {
            s->state = SQ_DONE;
            if (s->code || (s->node->text && J.total + (size_t)s->node->len <= SCRIPTS_TOTAL)) {
                if (!s->code) J.total += (size_t)s->node->len;
                Script copy = *s;   /* (document.write can add to the queue, moving it) */
                run_with_big_stack(run_script_job, &copy);
                s = &J.q[J.next];
            }
            free(s->code);
            s->code = NULL;
            if (s->url || s->dynamic) script_event(s->node, "load");
        } else if (s->failed && (s->url || s->dynamic)) {
            s->failed = 0;
            script_event(s->node, "error");
        }
        J.next++;
        if (now_ms() - start > 400) break;   /* (let the screen update now and then) */
    }
    if (J.next >= J.nq && !J.loaded && !J.failed) {
        J.loaded = true;
        if (J.started) run_with_big_stack(loaded_job, NULL);
        apply_changes(false);
    }
    if (J.loaded) apply_changes(false);
    if (J.timers_on && J.started && !J.failed && now_ms() >= J.timer_at) {
        run_with_big_stack(timers_job, NULL);
        apply_changes(true);
    }
    if (J.changed) apply_changes(true);   /* (a change held back for a moment) */
}

/* Does anything here want to hear about this event? (A page with no scripts
 * of its own doesn't get its JavaScript started just for a click.) */
static bool wanted(const Node *n, const char *type) {
    if (J.started) return true;
    char attr[24];
    snprintf(attr, sizeof(attr), "on%s", type);
    for (const Node *a = n; a; a = a->parent) {
        if (a->type != NT_ELEM) continue;
        if (dom_attr(a, attr)) return true;
        if (a->tag == TAG_A) { const char *h = dom_attr(a, "href"); if (h && !strncasecmp(h, "javascript:", 11)) return true; }
    }
    return false;
}

bool js_click(Node *n) {
    if (!ready() || !n || !wanted(n, "click")) return false;
    EventJob e = { n, "click", false };
    run_with_big_stack(event_job, &e);
    apply_changes(false);
    return e.result;
}

bool js_event(Node *n, const char *type) {
    if (!ready() || !n || !wanted(n, type)) return false;
    EventJob e = { n, type, false };
    run_with_big_stack(event_job, &e);
    apply_changes(false);
    return e.result;
}

int js_take_navigation(char *url, size_t cap, int *field) {
    int nav = J.nav;
    if (!nav) return JS_NAV_NONE;
    J.nav = JS_NAV_NONE;
    snprintf(url, cap, "%s", J.nav_url);
    if (field) *field = J.nav_field;
    return nav;
}

const char *js_take_message(void) {
    static char out[200];
    if (!J.message[0]) return NULL;
    snprintf(out, sizeof(out), "%s", J.message);
    J.message[0] = 0;
    return out;
}

/* ---------------------------------------------------------- about:console */

static void html_escape_into(char *out, size_t cap, size_t *k, const char *s) {
    for (; *s && *k + 8 < cap; s++) {
        if (*s == '<') *k += (size_t)snprintf(out + *k, cap - *k, "&lt;");
        else if (*s == '>') *k += (size_t)snprintf(out + *k, cap - *k, "&gt;");
        else if (*s == '&') *k += (size_t)snprintf(out + *k, cap - *k, "&amp;");
        else out[(*k)++] = *s;
    }
    out[*k] = 0;
}

void js_console_html(char *out, size_t cap) {
    size_t k = 0;
    if (!cap) return;
    out[0] = 0;
    k += (size_t)snprintf(out + k, cap - k, "<title>Script log</title><h2>Script log</h2><p>What happened with the scripts on ");
    if (log_page[0]) html_escape_into(out, cap, &k, log_page);
    else k += (size_t)snprintf(out + k, cap - k, "(no page yet)");
    k += (size_t)snprintf(out + k, cap - k, ":</p>");
    if (!log_count) k += (size_t)snprintf(out + k, cap - k, "<p>Nothing to report: no script files, errors or messages.</p>");
    int first = log_count > LOG_LINES ? log_count - LOG_LINES : 0;
    if (first) k += (size_t)snprintf(out + k, cap - k, "<p>(the first %d lines are left out)</p>", first);
    k += (size_t)snprintf(out + k, cap - k, "<ol>");
    for (int i = first; i < log_count && k + 400 < cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, "<li>");
        html_escape_into(out, cap, &k, log_lines[i % LOG_LINES]);
        k += (size_t)snprintf(out + k, cap - k, "</li>");
    }
    if (k + 120 < cap) k += (size_t)snprintf(out + k, cap - k, "</ol><p><a href=\"about:home\">Start page</a></p>");
}

/* ================================================================ speed test */

typedef struct { const char *name, *what, *code, *expect; } JsTest;

static const JsTest tests[] = {
    { "Counting loop", "200,000 additions",
      "let s = 0; for (let i = 0; i < 200000; i++) s += i; s", "19999900000" },
    { "Function calls", "fib(20): about 22,000 calls",
      "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } fib(20)", "6765" },
    { "Text", "5,000 pieces of text joined, searched and split",
      "let a = []; for (let i = 0; i < 5000; i++) a.push('item' + i);"
      "let s = a.join(','); s.indexOf('item4999') + ',' + s.split(',').length + ',' + s.toUpperCase().length",
      "43881,5000,43889" },
    { "Objects and sorting", "10,000 objects made and sorted",
      "let o = []; for (let i = 0; i < 10000; i++) o.push({ id: i, v: (i * 7919) % 10007 });"
      "o.sort((a, b) => a.v - b.v); o[0].id + ',' + o[9999].id",
      "0,1040" },
    { "JSON", "2,000 records turned into JSON and read back",
      "let d = { items: [] }; for (let i = 0; i < 2000; i++) d.items.push({ n: i, name: 'Thing ' + i, tags: ['a', 'b'], ok: i % 2 == 0 });"
      "let t = JSON.stringify(d); JSON.parse(t).items.length + ',' + t.length",
      "2000,114791" },
    { "Regular expressions", "links and addresses found and rewritten in 30 KB of text",
      "let t = ''; for (let i = 0; i < 500; i++) t += 'Visit https://example.com/page' + i + ' or mail bob' + i + '@mail.com. ';"
      "(t.match(/https?:\\/\\/[^\\s]+/g) || []).length + ',' + t.replace(/\\b(\\w+)@(\\w+)\\.com/g, '$2 at $1').length",
      "500,28780" },
    { "Maths", "50,000 sines and square roots",
      "let x = 0; for (let i = 1; i < 50000; i++) x += Math.sin(i) * Math.sqrt(i); Math.round(x)",
      NULL },   /* (its answer depends a little on the maths library: not checked) */
};
#define NTESTS (int)(sizeof(tests) / sizeof(tests[0]))

/* a big made-up script, like a site's: functions, objects, strings */
static char *big_script(size_t *len) {
    const int n = 1500;
    size_t cap = (size_t)n * 160 + 64;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t k = 0;
    for (int i = 0; i < n; i++)
        k += (size_t)snprintf(s + k, cap - k,
            "function f%d(a, b) { var c = a + b * %d; if (c > 10) { return { v: c, s: 'item %d' }; } "
            "return [a, b, c].map(function (x) { return x * 2; }).join(','); }\n", i, i, i);
    *len = k;
    return s;
}

typedef struct { char *out; size_t cap; } Job;

static void put(Job *j, size_t *k, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void put(Job *j, size_t *k, const char *fmt, ...) {
    if (*k >= j->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->out + *k, j->cap - *k, fmt, ap);
    va_end(ap);
    if (n > 0) *k += (size_t)n;
    if (*k >= j->cap) *k = j->cap - 1;
}

static void run(void *arg) {
    Job *j = arg;
    size_t k = 0;
    put(j, &k, "<title>JavaScript speed test</title><h2>JavaScript speed test</h2>"
               "<p>CubeBrowser's new JavaScript engine (QuickJS), timed on this console. "
               "Each test is a small script doing one kind of work web pages do.</p>");
    size_t free0 = cb_mem_free();
    double t0 = now_ms();
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    double t_start = now_ms() - t0;
    if (!ctx) {
        put(j, &k, "<p><b>The engine couldn't start: not enough memory.</b></p>");
        if (rt) JS_FreeRuntime(rt);
        return;
    }
    JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
    size_t used_start = free0 > cb_mem_free() ? free0 - cb_mem_free() : 0;

    put(j, &k, "<table border=1 cellpadding=4><tr><th>Test</th><th>What it does</th><th>Time</th><th>Answer</th></tr>");
    double total = 0;
    int wrong = 0;
    for (int i = 0; i < NTESTS; i++) {
        const JsTest *t = &tests[i];
        double a = now_ms();
        JSValue v = JS_Eval(ctx, t->code, strlen(t->code), "<test>", JS_EVAL_TYPE_GLOBAL);
        double ms = now_ms() - a;
        total += ms;
        char ans[64] = "";
        bool ok;
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *es = JS_ToCString(ctx, e);
            snprintf(ans, sizeof(ans), "error: %.48s", es ? es : "?");
            if (es) JS_FreeCString(ctx, es);
            JS_FreeValue(ctx, e);
            ok = false;
        } else {
            const char *s = JS_ToCString(ctx, v);
            snprintf(ans, sizeof(ans), "%s", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            ok = !t->expect || !strcmp(ans, t->expect);
        }
        JS_FreeValue(ctx, v);
        if (!ok) wrong++;
        put(j, &k, "<tr><td>%s</td><td>%s</td><td>%.0f ms</td><td>%s%s</td></tr>",
            t->name, t->what, ms, ok ? "" : "<b>WRONG</b> ", ans);
        /* each test in a fresh scope: let/const names would clash otherwise */
        JS_FreeContext(ctx);
        ctx = JS_NewContext(rt);
        if (!ctx) break;
    }
    /* reading in a big script (only reading: nothing in it runs) */
    size_t blen = 0;
    char *big = ctx ? big_script(&blen) : NULL;
    if (big) {
        double a = now_ms();
        JSValue fn = JS_Eval(ctx, big, blen, "<big>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        double ms = now_ms() - a;
        bool ok = !JS_IsException(fn);
        JS_FreeValue(ctx, fn);
        free(big);
        put(j, &k, "<tr><td>Reading a big script</td><td>%u KB of code, read but not run</td><td>%.0f ms</td><td>%s%.0f KB a second</td></tr>",
            (unsigned)(blen / 1024), ms, ok ? "" : "<b>WRONG</b> ", ms > 0 ? blen / 1024.0 / (ms / 1000.0) : 0);
        if (!ok) wrong++;
    }
    size_t used_peak = free0 > cb_mem_free() ? free0 - cb_mem_free() : 0;
    put(j, &k, "</table>");
    put(j, &k, "<p><b>All the small tests:</b> %.0f ms. %s</p>", total,
        wrong ? "<b>Some answers were wrong: please report this.</b>" : "Every answer was right.");
    put(j, &k, "<p><b>Starting the engine:</b> %.0f ms, %u KB of memory. "
               "<b>With the tests' leftovers:</b> %u KB. "
               "<b>Memory still free:</b> %u KB.</p>",
        t_start, (unsigned)(used_start / 1024), (unsigned)(used_peak / 1024), (unsigned)(cb_mem_free() / 1024));
    if (ctx) JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    put(j, &k, "<p><a href=\"about:js\">Run it again</a> &nbsp; <a href=\"about:home\">Start page</a></p>");
}

void js_speed_test_html(char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    Job j = { out, cap };
    run_with_big_stack(run, &j);   /* (scripts can go deep: the 1 MB stack) */
}
