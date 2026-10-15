/* css.c - CSS parsing, matching and cascade. */
#include "css.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_RULES 6000
#define MAX_DECLS 40000
#define MAX_VARS 512

/* ------------------------------------------------------------ properties */

enum {
    P_DISPLAY, P_COLOR, P_BG, P_FWEIGHT, P_FSTYLE, P_FSIZE, P_FFAMILY, P_TALIGN, P_TDECO, P_TTRANSFORM, P_WS,
    P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL, P_BWT, P_BWR, P_BWB, P_BWL, P_BST, P_BSR, P_BSB, P_BSL,
    P_BC, P_WIDTH, P_MAXW, P_HEIGHT, P_FLOAT, P_CLEAR, P_LIST, P_VIS, P_POS, P_OVERFLOW, P_CLIP, P_VALIGN,
    P_SPACING, P_COLLAPSE, P_LEFT, P_TEXTINDENT, P_COUNT
};

static const struct { const char *name; int prop; } longhands[] = {
    { "display", P_DISPLAY }, { "color", P_COLOR }, { "background-color", P_BG }, { "font-weight", P_FWEIGHT },
    { "font-style", P_FSTYLE }, { "font-size", P_FSIZE }, { "font-family", P_FFAMILY },
    { "text-align", P_TALIGN }, { "text-decoration-line", P_TDECO }, { "text-transform", P_TTRANSFORM },
    { "white-space", P_WS }, { "margin-top", P_MT }, { "margin-right", P_MR }, { "margin-bottom", P_MB },
    { "margin-left", P_ML }, { "margin-block-start", P_MT }, { "margin-block-end", P_MB },
    { "margin-inline-start", P_ML }, { "margin-inline-end", P_MR }, { "padding-top", P_PT },
    { "padding-right", P_PR }, { "padding-bottom", P_PB }, { "padding-left", P_PL },
    { "padding-block-start", P_PT }, { "padding-block-end", P_PB }, { "padding-inline-start", P_PL },
    { "padding-inline-end", P_PR }, { "border-top-width", P_BWT }, { "border-right-width", P_BWR },
    { "border-bottom-width", P_BWB }, { "border-left-width", P_BWL }, { "border-top-style", P_BST },
    { "border-right-style", P_BSR }, { "border-bottom-style", P_BSB }, { "border-left-style", P_BSL },
    { "border-top-color", P_BC }, { "border-right-color", P_BC }, { "border-bottom-color", P_BC },
    { "border-left-color", P_BC }, { "width", P_WIDTH }, { "max-width", P_MAXW }, { "height", P_HEIGHT },
    { "float", P_FLOAT }, { "clear", P_CLEAR }, { "list-style-type", P_LIST }, { "visibility", P_VIS },
    { "position", P_POS }, { "overflow", P_OVERFLOW }, { "overflow-x", P_OVERFLOW }, { "clip", P_CLIP },
    { "clip-path", P_CLIP }, { "vertical-align", P_VALIGN }, { "border-spacing", P_SPACING },
    { "border-collapse", P_COLLAPSE }, { "left", P_LEFT }, { "text-indent", P_TEXTINDENT },
};

/* ---------------------------------------------------------------- sheets */

typedef struct Compound {
    const char *tag, *id, *attr, *aval;
    const char *cls[6];
    unsigned char ncls, aop, pseudo;
    struct Compound *not_;
} Compound;

enum { PS_LINK = 1, PS_FIRST = 2, PS_LAST = 4, PS_ROOT = 8, PS_ONLY = 16 };

typedef struct {
    Compound *c;           /* c[0] is the rightmost compound */
    unsigned char comb[8]; /* comb[i] joins c[i] and c[i+1] */
    unsigned char n;
    unsigned spec;
} Selector;

typedef struct {
    unsigned char prop, important, kind, key;
    const char *raw;
    Color col;
    Len len;
} Decl;

enum { K_BAD, K_KEY, K_LEN, K_COLOR, K_CURCOLOR };

typedef struct {
    Selector sel;
    int first, count, order;
} Rule;

struct CssSheet {
    Arena arena;
    Rule *rules;
    int nrules, caprules;
    Decl *decls;
    int ndecls, capdecls;
    const char *var_name[MAX_VARS], *var_val[MAX_VARS];
    int nvars;
};

int css_rule_count(const CssSheet *s) { return s ? s->nrules : 0; }

/* -------------------------------------------------------------- helpers */

static bool ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

static bool ci_contains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

static const char *skip_ws(const char *p, const char *e) {
    for (;;) {
        while (p < e && isspace((unsigned char)*p)) p++;
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            const char *c = p + 2;
            while (c + 1 < e && !(c[0] == '*' && c[1] == '/')) c++;
            p = c + 1 < e ? c + 2 : e;
            continue;
        }
        return p;
    }
}

/* Finds the end of a {...} block starting just after '{'; returns pointer to matching '}' or e. */
static const char *block_end(const char *p, const char *e) {
    int depth = 1;
    char q = 0;
    while (p < e) {
        char c = *p;
        if (q) { if (c == '\\') p++; else if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == '/' && p + 1 < e && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            p++;
        } else if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return p; }
        p++;
    }
    return e;
}

/* ---------------------------------------------------------------- colours */

static const struct { const char *n; unsigned rgb; } named_colors[] = {
    { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 },
    { "blue", 0x0000ff }, { "yellow", 0xffff00 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xc0c0c0 }, { "maroon", 0x800000 }, { "purple", 0x800080 }, { "fuchsia", 0xff00ff },
    { "lime", 0x00ff00 }, { "olive", 0x808000 }, { "navy", 0x000080 }, { "teal", 0x008080 },
    { "aqua", 0x00ffff }, { "orange", 0xffa500 }, { "darkgray", 0xa9a9a9 }, { "darkgrey", 0xa9a9a9 },
    { "lightgray", 0xd3d3d3 }, { "lightgrey", 0xd3d3d3 }, { "dimgray", 0x696969 }, { "gainsboro", 0xdcdcdc },
    { "whitesmoke", 0xf5f5f5 }, { "darkblue", 0x00008b }, { "darkred", 0x8b0000 }, { "darkgreen", 0x006400 },
    { "lightblue", 0xadd8e6 }, { "lightgreen", 0x90ee90 }, { "lightyellow", 0xffffe0 }, { "pink", 0xffc0cb },
    { "brown", 0xa52a2a }, { "gold", 0xffd700 }, { "beige", 0xf5f5dc }, { "ivory", 0xfffff0 },
    { "lavender", 0xe6e6fa }, { "crimson", 0xdc143c }, { "coral", 0xff7f50 }, { "tomato", 0xff6347 },
    { "indigo", 0x4b0082 }, { "violet", 0xee82ee }, { "orchid", 0xda70d6 }, { "salmon", 0xfa8072 },
    { "khaki", 0xf0e68c }, { "tan", 0xd2b48c }, { "chocolate", 0xd2691e }, { "skyblue", 0x87ceeb },
    { "steelblue", 0x4682b4 }, { "royalblue", 0x4169e1 }, { "dodgerblue", 0x1e90ff }, { "slategray", 0x708090 },
    { "darkslategray", 0x2f4f4f }, { "midnightblue", 0x191970 }, { "cornflowerblue", 0x6495ed },
    { "firebrick", 0xb22222 }, { "forestgreen", 0x228b22 }, { "seagreen", 0x2e8b57 }, { "aliceblue", 0xf0f8ff },
    { "ghostwhite", 0xf8f8ff }, { "honeydew", 0xf0fff0 }, { "linen", 0xfaf0e6 }, { "snow", 0xfffafa },
    { "azure", 0xf0ffff }, { "mintcream", 0xf5fffa }, { "lightcyan", 0xe0ffff }, { "cyan", 0x00ffff },
    { "magenta", 0xff00ff }, { "darkorange", 0xff8c00 }, { "goldenrod", 0xdaa520 }, { "wheat", 0xf5deb3 },
    { "lightgoldenrodyellow", 0xfafad2 }, { "papayawhip", 0xffefd5 }, { "cornsilk", 0xfff8dc },
    { "lightsteelblue", 0xb0c4de }, { "powderblue", 0xb0e0e6 }, { "darkcyan", 0x008b8b }, { "sienna", 0xa0522d },
    { "rebeccapurple", 0x663399 }, { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 }, { "hotpink", 0xff69b4 },
    { "lightpink", 0xffb6c1 }, { "mediumblue", 0x0000cd }, { "darkolivegreen", 0x556b2f }, { "olivedrab", 0x6b8e23 },
};

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static float num_or_pct(const char *s, float scale_pct) {
    char *end;
    float v = strtof(s, &end);
    if (*end == '%') return v * scale_pct / 100.0f;
    return v;
}

static Color hsl_to_rgb(float h, float s, float l, float a) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360;
    float c = (1 - fabsf(2 * l - 1)) * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = l - c / 2;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    Color o = { (u8)((r + m) * 255), (u8)((g + m) * 255), (u8)((b + m) * 255), (u8)(clamp01(a) * 255) };
    return o;
}

/* Returns K_COLOR, K_CURCOLOR or K_BAD. Alpha 0 colours become "transparent" (a=0, r=2). */
static int parse_color(const char *s, Color *out) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '#') {
        s++;
        size_t n = 0;
        while (isxdigit((unsigned char)s[n])) n++;
        unsigned v = (unsigned)strtoul(s, NULL, 16);
        unsigned r, g, b, a = 255;
        if (n == 3 || n == 4) {
            if (n == 4) { a = (v & 0xF) * 17; v >>= 4; }
            r = ((v >> 8) & 0xF) * 17; g = ((v >> 4) & 0xF) * 17; b = (v & 0xF) * 17;
        } else if (n == 6 || n == 8) {
            if (n == 8) { a = v & 0xFF; v >>= 8; }
            r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
        } else {
            return K_BAD;
        }
        *out = (Color){ (u8)r, (u8)g, (u8)b, (u8)a };
        if (a == 0) *out = (Color){ 2, 0, 0, 0 };
        return K_COLOR;
    }
    if (!strncasecmp(s, "rgb", 3) || !strncasecmp(s, "hsl", 3)) {
        bool hsl = tolower((unsigned char)s[0]) == 'h';
        const char *p = strchr(s, '(');
        if (!p) return K_BAD;
        p++;
        float v[4] = { 0, 0, 0, 1 };
        int n = 0;
        while (*p && *p != ')' && n < 4) {
            while (*p == ' ' || *p == ',' || *p == '/') p++;
            if (*p == ')' || !*p) break;
            if (hsl) v[n] = (n == 0) ? strtof(p, NULL) : (n == 3 ? num_or_pct(p, 1.0f) : num_or_pct(p, 1.0f));
            else v[n] = (n == 3) ? num_or_pct(p, 1.0f) : num_or_pct(p, 255.0f);
            if (n == 3) { char *e; float t = strtof(p, &e); if (*e != '%') v[3] = t; }
            n++;
            while (*p && *p != ' ' && *p != ',' && *p != '/' && *p != ')') p++;
        }
        if (n < 3) return K_BAD;
        if (hsl) {
            float s2 = v[1] > 1 ? v[1] / 100.0f : v[1], l = v[2] > 1 ? v[2] / 100.0f : v[2];
            *out = hsl_to_rgb(v[0], s2, l, v[3]);
        } else {
            *out = (Color){ (u8)fminf(255, fmaxf(0, v[0])), (u8)fminf(255, fmaxf(0, v[1])),
                            (u8)fminf(255, fmaxf(0, v[2])), (u8)(clamp01(v[3]) * 255) };
        }
        if (out->a == 0) *out = (Color){ 2, 0, 0, 0 };
        return K_COLOR;
    }
    char word[32];
    int n = 0;
    while (isalpha((unsigned char)s[n]) && n < 31) { word[n] = (char)tolower((unsigned char)s[n]); n++; }
    word[n] = 0;
    if (!strcmp(word, "transparent")) { *out = (Color){ 2, 0, 0, 0 }; return K_COLOR; }
    if (!strcmp(word, "currentcolor")) return K_CURCOLOR;
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++)
        if (!strcmp(word, named_colors[i].n)) {
            unsigned v = named_colors[i].rgb;
            *out = (Color){ (u8)(v >> 16), (u8)(v >> 8), (u8)v, 255 };
            return K_COLOR;
        }
    return K_BAD;
}

/* --------------------------------------------------------------- lengths */

static bool parse_len(const char *s, Len *out) {
    while (isspace((unsigned char)*s)) s++;
    if (!strncasecmp(s, "auto", 4)) { out->u = U_AUTO; out->v = 0; return true; }
    if (!strncasecmp(s, "calc(", 5) || !strncasecmp(s, "min(", 4) || !strncasecmp(s, "max(", 4) ||
        !strncasecmp(s, "clamp(", 6)) {
        /* take the first plain length inside as an approximation */
        const char *p = strchr(s, '(') + 1;
        while (*p && !(isdigit((unsigned char)*p) || *p == '.' || *p == '-')) p++;
        if (!*p) return false;
        return parse_len(p, out);
    }
    char *end;
    float v = strtof(s, &end);
    if (end == s) return false;
    char unit[8];
    int n = 0;
    while ((isalpha((unsigned char)*end) || *end == '%') && n < 7) unit[n++] = (char)tolower((unsigned char)*end++);
    unit[n] = 0;
    out->v = v;
    if (!n || !strcmp(unit, "px")) out->u = U_PX;
    else if (!strcmp(unit, "em")) out->u = U_EM;
    else if (!strcmp(unit, "rem")) out->u = U_REM;
    else if (!strcmp(unit, "%")) out->u = U_PCT;
    else if (!strcmp(unit, "vw") || !strcmp(unit, "vmin")) out->u = U_VW;
    else if (!strcmp(unit, "vh") || !strcmp(unit, "vmax")) out->u = U_VH;
    else if (!strcmp(unit, "pt")) { out->u = U_PX; out->v = v * 4.0f / 3.0f; }
    else if (!strcmp(unit, "pc")) { out->u = U_PX; out->v = v * 16.0f; }
    else if (!strcmp(unit, "in")) { out->u = U_PX; out->v = v * 96.0f; }
    else if (!strcmp(unit, "cm")) { out->u = U_PX; out->v = v * 37.8f; }
    else if (!strcmp(unit, "mm")) { out->u = U_PX; out->v = v * 3.78f; }
    else if (!strcmp(unit, "ch") || !strcmp(unit, "ex")) { out->u = U_EM; out->v = v * 0.5f; }
    else return false;
    if (n == 0 && v != 0) out->u = U_PX;   /* unitless (quirks) */
    return true;
}

float css_len(Len l, float pct_base, float fsize, float auto_val) {
    switch (l.u) {
    case U_PX: return l.v;
    case U_EM: return l.v * fsize;
    case U_REM: return l.v * 16.0f;
    case U_PCT: return l.v * pct_base / 100.0f;
    case U_VW: return l.v * CSS_VIEWPORT_W / 100.0f;
    case U_VH: return l.v * CSS_VIEWPORT_H / 100.0f;
    default: return auto_val;
    }
}

/* ---------------------------------------------------------- media queries */

static bool media_one(const char *q) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", q);
    for (char *c = buf; *c; c++) *c = (char)tolower((unsigned char)*c);
    char *p = buf;
    while (isspace((unsigned char)*p)) p++;
    bool negate = false;
    if (!strncmp(p, "not ", 4)) { negate = true; p += 4; }
    if (!strncmp(p, "only ", 5)) p += 5;
    bool ok = true;
    while (*p && ok) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (!strncmp(p, "and", 3) && (isspace((unsigned char)p[3]) || p[3] == '(')) { p += 3; continue; }
        if (*p == '(') {
            char *e = strchr(p, ')');
            if (!e) break;
            *e = 0;
            char *feat = p + 1, *colon = strchr(feat, ':');
            char *val = colon ? colon + 1 : NULL;
            if (colon) *colon = 0;
            while (isspace((unsigned char)*feat)) feat++;
            char *fe = feat + strlen(feat);
            while (fe > feat && isspace((unsigned char)fe[-1])) *--fe = 0;
            Len l = { 0, 0 };
            float px = 0;
            if (val && parse_len(val, &l)) px = css_len(l, 0, 16, 0);
            if (!strcmp(feat, "max-width") || !strcmp(feat, "max-device-width")) ok = CSS_VIEWPORT_W <= px;
            else if (!strcmp(feat, "min-width") || !strcmp(feat, "min-device-width")) ok = CSS_VIEWPORT_W >= px;
            else if (!strcmp(feat, "max-height")) ok = CSS_VIEWPORT_H <= px;
            else if (!strcmp(feat, "min-height")) ok = CSS_VIEWPORT_H >= px;
            else if (!strcmp(feat, "prefers-color-scheme")) ok = val && strstr(val, "light");
            else if (!strcmp(feat, "prefers-reduced-motion")) ok = val && strstr(val, "reduce");
            else if (!strcmp(feat, "orientation")) ok = val && strstr(val, "portrait");
            else if (!strcmp(feat, "hover") || !strcmp(feat, "any-hover")) ok = val && strstr(val, "none");
            else if (!strcmp(feat, "pointer") || !strcmp(feat, "any-pointer")) ok = val && strstr(val, "coarse");
            else if (!strcmp(feat, "color")) ok = true;
            else ok = false;
            p = e + 1;
            continue;
        }
        char word[32];
        int n = 0;
        while (*p && !isspace((unsigned char)*p) && *p != '(' && n < 31) word[n++] = *p++;
        word[n] = 0;
        if (!strcmp(word, "screen") || !strcmp(word, "all")) continue;
        ok = false;
    }
    return negate ? !ok : ok;
}

bool css_media_matches(const char *query) {
    if (!query) return true;
    while (isspace((unsigned char)*query)) query++;
    if (!*query) return true;
    char part[256];
    const char *p = query;
    for (;;) {
        const char *c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n >= sizeof(part)) n = sizeof(part) - 1;
        memcpy(part, p, n);
        part[n] = 0;
        if (media_one(part)) return true;
        if (!c) return false;
        p = c + 1;
    }
}

/* ------------------------------------------------------------- selectors */

static const char *ident(Arena *a, const char **pp, const char *e, bool lower) {
    const char *p = *pp, *s = p;
    while (p < e && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || (unsigned char)*p >= 0x80 || *p == '\\')) {
        if (*p == '\\' && p + 1 < e) p++;
        p++;
    }
    *pp = p;
    if (p == s) return NULL;
    char *r = arena_strndup(a, s, (size_t)(p - s));
    if (r) {
        /* drop escape backslashes */
        char *w = r;
        for (char *c = r; *c; c++) if (*c != '\\') *w++ = lower ? (char)tolower((unsigned char)*c) : *c;
        *w = 0;
    }
    return r;
}

static bool parse_compound(Arena *a, const char **pp, const char *e, Compound *c, unsigned *spec) {
    const char *p = *pp;
    memset(c, 0, sizeof(*c));
    bool any = false;
    if (p < e && *p == '*') { p++; any = true; }
    else if (p < e && (isalpha((unsigned char)*p) || *p == '-' || *p == '_')) {
        c->tag = ident(a, &p, e, true);
        *spec += 1;
        any = true;
    }
    while (p < e) {
        if (*p == '#') {
            p++;
            c->id = ident(a, &p, e, false);
            if (!c->id) return false;
            *spec += 0x10000;
        } else if (*p == '.') {
            p++;
            const char *cl = ident(a, &p, e, false);
            if (!cl) return false;
            if (c->ncls < 6) c->cls[c->ncls++] = cl;
            *spec += 0x100;
        } else if (*p == '[') {
            const char *end = memchr(p, ']', (size_t)(e - p));
            if (!end) return false;
            p++;
            while (p < end && isspace((unsigned char)*p)) p++;
            const char *name = ident(a, &p, end, true);
            if (!name) return false;
            c->attr = name;
            c->aop = 1;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p < end) {
                char op = *p;
                if (op == '=') { c->aop = 2; p++; }
                else if (p + 1 < end && p[1] == '=') {
                    c->aop = op == '~' ? 3 : op == '^' ? 4 : op == '$' ? 5 : op == '*' ? 6 : op == '|' ? 7 : 0;
                    p += 2;
                }
                if (!c->aop) return false;
                while (p < end && isspace((unsigned char)*p)) p++;
                const char *vs = p, *ve;
                if (p < end && (*p == '"' || *p == '\'')) {
                    char q = *p++;
                    vs = p;
                    while (p < end && *p != q) p++;
                    ve = p;
                } else {
                    while (p < end && !isspace((unsigned char)*p)) p++;
                    ve = p;
                }
                c->aval = arena_strndup(a, vs, (size_t)(ve - vs));
            }
            p = end + 1;
            *spec += 0x100;
        } else if (*p == ':') {
            p++;
            if (p < e && *p == ':') return false;   /* pseudo-elements are not rendered */
            const char *name = ident(a, &p, e, true);
            if (!name) return false;
            const char *arg = NULL, *argend = NULL;
            if (p < e && *p == '(') {
                int depth = 1;
                arg = ++p;
                while (p < e && depth) { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
                argend = p - 1;
            }
            *spec += 0x100;
            if (!strcmp(name, "link") || !strcmp(name, "any-link")) c->pseudo |= PS_LINK;
            else if (!strcmp(name, "first-child")) c->pseudo |= PS_FIRST;
            else if (!strcmp(name, "last-child")) c->pseudo |= PS_LAST;
            else if (!strcmp(name, "only-child")) c->pseudo |= PS_ONLY;
            else if (!strcmp(name, "root")) c->pseudo |= PS_ROOT;
            else if (!strcmp(name, "not") && arg && !c->not_) {
                Compound *n = arena_alloc(a, sizeof(Compound));
                unsigned s2 = 0;
                const char *ap = skip_ws(arg, argend);
                if (!n || !parse_compound(a, &ap, argend, n, &s2)) return false;
                if (skip_ws(ap, argend) != argend) return false;   /* only simple :not() */
                c->not_ = n;
            } else if (!strcmp(name, "is") || !strcmp(name, "where")) {
                /* :is(x) with a single simple argument */
                if (!arg) return false;
                const char *ap = skip_ws(arg, argend);
                Compound tmp;
                unsigned s2 = 0;
                if (!parse_compound(a, &ap, argend, &tmp, &s2) || skip_ws(ap, argend) != argend) return false;
                if (tmp.tag && !c->tag) c->tag = tmp.tag;
                for (int i = 0; i < tmp.ncls && c->ncls < 6; i++) c->cls[c->ncls++] = tmp.cls[i];
                if (tmp.id) c->id = tmp.id;
            } else {
                return false;   /* :hover, :nth-child(), etc. never match here */
            }
        } else {
            break;
        }
        any = true;
    }
    *pp = p;
    return any;
}

static bool parse_selector(Arena *a, const char *s, const char *e, Selector *out) {
    Compound tmp[8];
    unsigned char comb[8];
    int n = 0;
    unsigned spec = 0;
    const char *p = skip_ws(s, e);
    while (p < e) {
        if (n == 8) return false;
        if (!parse_compound(a, &p, e, &tmp[n], &spec)) return false;
        n++;
        const char *q = p;
        p = skip_ws(p, e);
        if (p >= e) break;
        char c = ' ';
        if (*p == '>' || *p == '+' || *p == '~') { c = *p; p = skip_ws(p + 1, e); }
        else if (q == p) return false;
        comb[n - 1] = (unsigned char)c;
    }
    if (!n) return false;
    out->c = arena_alloc(a, sizeof(Compound) * (size_t)n);
    if (!out->c) return false;
    for (int i = 0; i < n; i++) out->c[i] = tmp[n - 1 - i];
    for (int i = 0; i < n - 1; i++) out->comb[i] = comb[n - 2 - i];
    out->n = (unsigned char)n;
    out->spec = spec;
    return true;
}

/* ----------------------------------------------------------- declarations */

static int find_prop(const char *name) {
    for (size_t i = 0; i < sizeof(longhands) / sizeof(longhands[0]); i++)
        if (ieq(name, longhands[i].name)) return longhands[i].prop;
    return -1;
}

static void add_decl(CssSheet *sh, int prop, const char *raw, size_t len, bool important) {
    if (sh->ndecls >= MAX_DECLS) return;
    if (sh->ndecls == sh->capdecls) {
        int nc = sh->capdecls ? sh->capdecls * 2 : 512;
        Decl *nd = realloc(sh->decls, sizeof(Decl) * (size_t)nc);
        if (!nd) return;
        sh->decls = nd;
        sh->capdecls = nc;
    }
    Decl *d = &sh->decls[sh->ndecls++];
    memset(d, 0, sizeof(*d));
    d->prop = (unsigned char)prop;
    d->important = important;
    d->raw = arena_strndup(&sh->arena, raw, len);
}

/* Splits a value into whitespace-separated tokens (keeping parentheses together). */
static int tokens(const char *v, const char **tok, size_t *tlen, int max) {
    int n = 0;
    const char *p = v;
    while (*p && n < max) {
        while (isspace((unsigned char)*p) || *p == ',') { if (*p == ',' && n) break; p++; }
        if (*p == ',') { p++; continue; }
        if (!*p) break;
        const char *s = p;
        int depth = 0;
        while (*p && (depth || !isspace((unsigned char)*p))) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        tok[n] = s;
        tlen[n] = (size_t)(p - s);
        n++;
    }
    return n;
}

static bool is_border_style(const char *t, size_t n) {
    static const char *const st[] = { "none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge",
                                      "inset", "outset", NULL };
    for (int i = 0; st[i]; i++) if (strlen(st[i]) == n && !strncasecmp(t, st[i], n)) return true;
    return false;
}

static bool looks_like_len(const char *t, size_t n) {
    if (!n) return false;
    if (isdigit((unsigned char)t[0]) || t[0] == '.' || (t[0] == '-' && n > 1)) return true;
    if (!strncasecmp(t, "thin", n) || !strncasecmp(t, "medium", n) || !strncasecmp(t, "thick", n)) return true;
    if (!strncasecmp(t, "calc(", 5)) return true;
    return false;
}

static bool font_kw_prefix(const char *t) {
    return !strncasecmp(t, "small", 5) || !strncasecmp(t, "large", 5) || !strncasecmp(t, "x-", 2) ||
           !strncasecmp(t, "xx-", 3) || !strncasecmp(t, "medium", 6) || !strncasecmp(t, "smaller", 7) ||
           !strncasecmp(t, "larger", 6);
}

static void four_sides(CssSheet *sh, const char *v, int base, bool imp) {
    const char *tok[4];
    size_t tl[4];
    int n = tokens(v, tok, tl, 4);
    if (!n) return;
    int map[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
    for (int side = 0; side < 4; side++) {
        int t = map[n - 1][side];
        add_decl(sh, base + side, tok[t], tl[t], imp);
    }
}

static void border_short(CssSheet *sh, const char *v, int side, bool imp) {
    const char *tok[6];
    size_t tl[6];
    int n = tokens(v, tok, tl, 6);
    const char *w = "medium", *s = "none", *c = NULL;
    size_t wl = 6, sl = 4, cl = 0;
    for (int i = 0; i < n; i++) {
        if (is_border_style(tok[i], tl[i])) { s = tok[i]; sl = tl[i]; }
        else if (looks_like_len(tok[i], tl[i])) { w = tok[i]; wl = tl[i]; }
        else { c = tok[i]; cl = tl[i]; }
    }
    for (int k = 0; k < 4; k++) {
        if (side >= 0 && k != side) continue;
        add_decl(sh, P_BWT + k, w, wl, imp);
        add_decl(sh, P_BST + k, s, sl, imp);
    }
    if (c) add_decl(sh, P_BC, c, cl, imp);
}

static void parse_decls(CssSheet *sh, const char *p, const char *e, bool vars_global) {
    while (p < e) {
        p = skip_ws(p, e);
        const char *ns = p;
        while (p < e && *p != ':' && *p != ';') p++;
        if (p >= e || *p == ';') { p++; continue; }
        const char *ne = p;
        while (ne > ns && isspace((unsigned char)ne[-1])) ne--;
        p++;
        const char *vs = skip_ws(p, e);
        int depth = 0;
        char q = 0;
        while (p < e && (q || depth || *p != ';')) {
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        const char *ve = p;
        while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        if (p < e) p++;
        bool imp = false;
        if (ve - vs >= 10) {
            const char *bang = ve;
            while (bang > vs && *bang != '!') bang--;
            if (*bang == '!' && !strncasecmp(skip_ws(bang + 1, ve), "important", 9)) { imp = true; ve = bang; }
            while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        }
        size_t nl = (size_t)(ne - ns);
        if (nl == 0 || nl > 40) continue;
        char name[48];
        memcpy(name, ns, nl);
        name[nl] = 0;
        char *val = arena_strndup(&sh->arena, vs, (size_t)(ve - vs));
        if (!val) return;

        if (name[0] == '-' && name[1] == '-') {
            if (vars_global && sh->nvars < MAX_VARS) {
                sh->var_name[sh->nvars] = arena_strndup(&sh->arena, name, nl);
                sh->var_val[sh->nvars++] = val;
            }
            continue;
        }
        for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        int prop = find_prop(name);
        if (prop >= 0) { add_decl(sh, prop, val, strlen(val), imp); continue; }
        if (!strcmp(name, "margin")) four_sides(sh, val, P_MT, imp);
        else if (!strcmp(name, "padding")) four_sides(sh, val, P_PT, imp);
        else if (!strcmp(name, "border-width")) four_sides(sh, val, P_BWT, imp);
        else if (!strcmp(name, "border-style")) four_sides(sh, val, P_BST, imp);
        else if (!strcmp(name, "border-color")) add_decl(sh, P_BC, val, strlen(val), imp);
        else if (!strcmp(name, "border")) border_short(sh, val, -1, imp);
        else if (!strcmp(name, "border-top")) border_short(sh, val, 0, imp);
        else if (!strcmp(name, "border-right")) border_short(sh, val, 1, imp);
        else if (!strcmp(name, "border-bottom")) border_short(sh, val, 2, imp);
        else if (!strcmp(name, "border-left")) border_short(sh, val, 3, imp);
        else if (!strcmp(name, "margin-inline") || !strcmp(name, "padding-inline")) {
            const char *tok[2]; size_t tl[2];
            int n = tokens(val, tok, tl, 2);
            int base = name[0] == 'm' ? P_ML : P_PL, other = name[0] == 'm' ? P_MR : P_PR;
            if (n) { add_decl(sh, base, tok[0], tl[0], imp); add_decl(sh, other, tok[n - 1], tl[n - 1], imp); }
        } else if (!strcmp(name, "margin-block") || !strcmp(name, "padding-block")) {
            const char *tok[2]; size_t tl[2];
            int n = tokens(val, tok, tl, 2);
            int base = name[0] == 'm' ? P_MT : P_PT, other = name[0] == 'm' ? P_MB : P_PB;
            if (n) { add_decl(sh, base, tok[0], tl[0], imp); add_decl(sh, other, tok[n - 1], tl[n - 1], imp); }
        } else if (!strcmp(name, "background")) {
            const char *tok[12]; size_t tl[12];
            int n = tokens(val, tok, tl, 12);
            const char *col = "transparent";
            size_t cl = 11;
            for (int i = 0; i < n; i++) {
                Color tmpc;
                char t[64];
                size_t l = tl[i] < 63 ? tl[i] : 63;
                memcpy(t, tok[i], l);
                t[l] = 0;
                if (!strncasecmp(t, "var(", 4) || parse_color(t, &tmpc) != K_BAD) { col = tok[i]; cl = tl[i]; }
            }
            add_decl(sh, P_BG, col, cl, imp);
        } else if (!strcmp(name, "font")) {
            const char *tok[10]; size_t tl[10];
            int n = tokens(val, tok, tl, 10);
            bool italic = false, bold = false;
            for (int i = 0; i < n; i++) {
                bool digits = true;
                for (size_t k = 0; k < tl[i]; k++) if (!isdigit((unsigned char)tok[i][k])) digits = false;
                if (!strncasecmp(tok[i], "italic", 6) || !strncasecmp(tok[i], "oblique", 7)) italic = true;
                else if (!strncasecmp(tok[i], "bold", 4)) bold = true;
                else if (digits) bold = atoi(tok[i]) >= 600;
                else if (isdigit((unsigned char)tok[i][0]) || tok[i][0] == '.' || !strncasecmp(tok[i], "var(", 4) ||
                         font_kw_prefix(tok[i])) {
                    size_t l = tl[i];
                    const char *slash = memchr(tok[i], '/', l);
                    if (slash) l = (size_t)(slash - tok[i]);
                    add_decl(sh, P_FSIZE, tok[i], l, imp);
                    const char *fam = tok[i] + tl[i];
                    add_decl(sh, P_FFAMILY, fam, strlen(fam), imp);
                    break;
                }
            }
            add_decl(sh, P_FSTYLE, italic ? "italic" : "normal", 6, imp);
            add_decl(sh, P_FWEIGHT, bold ? "bold" : "normal", bold ? 4 : 6, imp);
        } else if (!strcmp(name, "list-style")) {
            const char *tok[4]; size_t tl[4];
            int n = tokens(val, tok, tl, 4);
            for (int i = 0; i < n; i++)
                if (strncasecmp(tok[i], "url(", 4) && strncasecmp(tok[i], "inside", 6) &&
                    strncasecmp(tok[i], "outside", 7))
                    add_decl(sh, P_LIST, tok[i], tl[i], imp);
        } else if (!strcmp(name, "text-decoration")) {
            add_decl(sh, P_TDECO, val, strlen(val), imp);
        } else if (!strcmp(name, "overflow-y") || !strcmp(name, "inset")) {
            /* ignored */
        }
    }
}

/* ---------------------------------------------------------------- parser */

static void add_rules(CssSheet *sh, const char *sel_s, const char *sel_e, const char *body_s, const char *body_e) {
    /* is this a global-variable scope like :root or html? */
    char selbuf[128];
    size_t sl = (size_t)(sel_e - sel_s) < sizeof(selbuf) - 1 ? (size_t)(sel_e - sel_s) : sizeof(selbuf) - 1;
    memcpy(selbuf, sel_s, sl);
    selbuf[sl] = 0;
    bool vars_global = strstr(selbuf, ":root") || strstr(selbuf, "html") || !strncmp(selbuf, "body", 4) ||
                       !strcmp(selbuf, "*") || strstr(selbuf, ":host");
    int first = sh->ndecls;
    parse_decls(sh, body_s, body_e, vars_global);
    int count = sh->ndecls - first;
    if (!count) return;
    /* one rule per comma-separated selector, sharing the declarations */
    const char *p = sel_s;
    while (p < sel_e) {
        const char *c = p;
        int depth = 0;
        while (c < sel_e && (depth || *c != ',')) {
            if (*c == '(' || *c == '[') depth++;
            else if (*c == ')' || *c == ']') depth--;
            c++;
        }
        Selector s;
        if (sh->nrules < MAX_RULES && parse_selector(&sh->arena, p, c, &s)) {
            if (sh->nrules == sh->caprules) {
                int nc = sh->caprules ? sh->caprules * 2 : 256;
                Rule *nr = realloc(sh->rules, sizeof(Rule) * (size_t)nc);
                if (!nr) return;
                sh->rules = nr;
                sh->caprules = nc;
            }
            Rule *r = &sh->rules[sh->nrules];
            r->sel = s;
            r->first = first;
            r->count = count;
            r->order = sh->nrules++;
        }
        p = c + 1;
    }
}

static void parse_block(CssSheet *sh, const char *p, const char *e, int depth) {
    while (p < e) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *s = p;
            while (p < e && *p != '{' && *p != ';') p++;
            if (p >= e) break;
            if (*p == ';') { p++; continue; }   /* @import, @charset ... */
            const char *body = p + 1, *end = block_end(body, e);
            size_t hl = (size_t)(p - s);
            char head[256];
            if (hl >= sizeof(head)) hl = sizeof(head) - 1;
            memcpy(head, s, hl);
            head[hl] = 0;
            if (depth < 4) {
                if (!strncasecmp(head, "@media", 6)) {
                    if (css_media_matches(head + 6)) parse_block(sh, body, end, depth + 1);
                } else if (!strncasecmp(head, "@supports", 9) || !strncasecmp(head, "@layer", 6) ||
                           !strncasecmp(head, "@container", 10) || !strncasecmp(head, "@document", 9)) {
                    if (strncasecmp(head, "@container", 10) != 0) parse_block(sh, body, end, depth + 1);
                }
            }
            p = end < e ? end + 1 : e;
            continue;
        }
        const char *sel_s = p;
        while (p < e && *p != '{') p++;
        if (p >= e) break;
        const char *sel_e = p;
        while (sel_e > sel_s && isspace((unsigned char)sel_e[-1])) sel_e--;
        const char *body = p + 1, *end = block_end(body, e);
        add_rules(sh, sel_s, sel_e, body, end);
        p = end < e ? end + 1 : e;
    }
}

CssSheet *css_parse(const char *text, int len) {
    CssSheet *sh = calloc(1, sizeof(CssSheet));
    if (!sh) return NULL;
    if (text && len > 0) parse_block(sh, text, text + len, 0);
    return sh;
}

void css_free(CssSheet *s) {
    if (!s) return;
    free(s->rules);
    free(s->decls);
    arena_free(&s->arena);
    free(s);
}

/* ------------------------------------------------- default stylesheet (UA) */

static const char ua_css[] =
    "html,body,div,p,ul,ol,dl,dt,dd,h1,h2,h3,h4,h5,h6,pre,blockquote,form,center,section,article,header,"
    "footer,nav,main,aside,figure,figcaption,address,fieldset,details,summary,hr,legend,menu,dir,listing,"
    "xmp,plaintext,hgroup,search,dialog,frameset,frame,optgroup{display:block}"
    "li{display:list-item}"
    "head,script,style,title,meta,link,base,template,option,datalist,param,source,track,area,map,noembed,"
    "[hidden],input[type=hidden],dialog:not([open]){display:none}"
    "noframes,frameset,frame{display:block}noframes{margin-top:1em}"
    "table{display:table;border-spacing:2px}"
    "tr{display:table-row}td,th{display:table-cell;padding:1px;vertical-align:middle}"
    "th{font-weight:bold;text-align:center}"
    "thead,tbody,tfoot{display:table-row-group}caption{display:table-caption;text-align:center}"
    "body{margin:8px}"
    "p,dl,blockquote,figure,ul,ol,menu{margin-top:1em;margin-bottom:1em}"
    "ul ul,ol ol,ul ol,ol ul,li ul,li ol{margin-top:0;margin-bottom:0}"
    "ul,ol,menu{padding-left:40px}ul,menu{list-style-type:disc}ol{list-style-type:decimal}"
    "ul ul,ol ul{list-style-type:circle}ul ul ul,ol ul ul,ul ol ul{list-style-type:square}"
    "blockquote,figure{margin-left:40px;margin-right:40px}dd{margin-left:40px}"
    "h1{font-size:2em;margin-top:.67em;margin-bottom:.67em;font-weight:bold}"
    "h2{font-size:1.5em;margin-top:.83em;margin-bottom:.83em;font-weight:bold}"
    "h3{font-size:1.17em;margin-top:1em;margin-bottom:1em;font-weight:bold}"
    "h4{margin-top:1.33em;margin-bottom:1.33em;font-weight:bold}"
    "h5{font-size:.83em;margin-top:1.67em;margin-bottom:1.67em;font-weight:bold}"
    "h6{font-size:.67em;margin-top:2.33em;margin-bottom:2.33em;font-weight:bold}"
    "b,strong,dt,summary,legend{font-weight:bold}i,em,cite,var,dfn,address{font-style:italic}"
    "pre,code,tt,kbd,samp,listing,xmp,plaintext{font-family:monospace}"
    "pre,listing,xmp,plaintext{white-space:pre;margin-top:1em;margin-bottom:1em}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a:link{text-decoration:underline}center{text-align:center}"
    "small,sub,sup{font-size:smaller}big{font-size:larger}"
    "hr{border-top:1px solid #a0a0a0;margin-top:.5em;margin-bottom:.5em}"
    "mark{background-color:#ff0;color:#000}fieldset{border:1px solid #c0c0c0;padding:4px 8px;margin:8px 0}"
    "iframe,video,audio,object,canvas,embed{display:none}"
    "textarea{display:block}";

static CssSheet *ua_sheet;

/* ------------------------------------------------------------- matching */

static bool has_class(const Node *n, const char *c) {
    for (int i = 0; i < n->nclasses; i++) if (!strcmp(n->classes[i], c)) return true;
    return false;
}

static const Node *prev_elem(const Node *n) {
    for (const Node *p = n->prev; p; p = p->prev) if (p->type == NT_ELEM) return p;
    return NULL;
}

static const Node *next_elem(const Node *n) {
    for (const Node *p = n->next; p; p = p->next) if (p->type == NT_ELEM) return p;
    return NULL;
}

static bool match_compound(const Compound *c, const Node *n) {
    if (n->type != NT_ELEM || n->tag == TAG_ROOT) return false;
    if (c->tag && strcmp(c->tag, n->name)) return false;
    if (c->id && (!n->id || strcmp(c->id, n->id))) return false;
    for (int i = 0; i < c->ncls; i++) if (!has_class(n, c->cls[i])) return false;
    if (c->attr) {
        const char *v = dom_attr(n, c->attr);
        if (!v) return false;
        const char *a = c->aval ? c->aval : "";
        size_t al = strlen(a), vl = strlen(v);
        switch (c->aop) {
        case 2: if (strcmp(v, a)) return false; break;
        case 3: {
            const char *f = strstr(v, a);
            bool ok = false;
            while (f && al) {
                if ((f == v || isspace((unsigned char)f[-1])) && (!f[al] || isspace((unsigned char)f[al]))) { ok = true; break; }
                f = strstr(f + 1, a);
            }
            if (!ok) return false;
            break;
        }
        case 4: if (strncmp(v, a, al)) return false; break;
        case 5: if (vl < al || strcmp(v + vl - al, a)) return false; break;
        case 6: if (!strstr(v, a)) return false; break;
        case 7: if (strncmp(v, a, al) || (v[al] && v[al] != '-')) return false; break;
        }
    }
    if (c->pseudo) {
        if ((c->pseudo & PS_LINK) && !(n->tag == TAG_A && dom_attr(n, "href"))) return false;
        if ((c->pseudo & (PS_FIRST | PS_ONLY)) && prev_elem(n)) return false;
        if ((c->pseudo & (PS_LAST | PS_ONLY)) && next_elem(n)) return false;
        if ((c->pseudo & PS_ROOT) && n->tag != TAG_HTML) return false;
    }
    if (c->not_ && match_compound(c->not_, n)) return false;
    return true;
}

static bool match_from(const Selector *s, int i, const Node *n, int budget) {
    if (!match_compound(&s->c[i], n)) return false;
    if (i + 1 == s->n) return true;
    if (budget <= 0) return false;
    switch (s->comb[i]) {
    case '>': return n->parent && match_from(s, i + 1, n->parent, budget - 1);
    case '+': { const Node *p = prev_elem(n); return p && match_from(s, i + 1, p, budget - 1); }
    case '~':
        for (const Node *p = prev_elem(n); p; p = prev_elem(p))
            if (match_from(s, i + 1, p, budget - 1)) return true;
        return false;
    default:
        for (const Node *p = n->parent; p; p = p->parent)
            if (match_from(s, i + 1, p, budget - 1)) return true;
        return false;
    }
}

/* -------------------------------------------------------------- indexing */

typedef struct { const Rule *rule; const CssSheet *sheet; int origin, sheet_no; int next; } Entry;

typedef struct {
    Entry *e;
    int n, cap;
    int buckets[2048];
    int universal;
} Index;

static unsigned hkey(char kind, const char *s) {
    unsigned h = 2166136261u ^ (unsigned char)kind;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void index_add(Index *ix, const Rule *r, const CssSheet *sh, int origin, int sheet_no) {
    if (ix->n == ix->cap) {
        int nc = ix->cap ? ix->cap * 2 : 1024;
        Entry *ne = realloc(ix->e, sizeof(Entry) * (size_t)nc);
        if (!ne) return;
        ix->e = ne;
        ix->cap = nc;
    }
    Entry *en = &ix->e[ix->n];
    en->rule = r;
    en->sheet = sh;
    en->origin = origin;
    en->sheet_no = sheet_no;
    const Compound *c = &r->sel.c[0];
    int *head;
    if (c->id) head = &ix->buckets[hkey('#', c->id) & 2047];
    else if (c->ncls) head = &ix->buckets[hkey('.', c->cls[0]) & 2047];
    else if (c->tag) head = &ix->buckets[hkey('t', c->tag) & 2047];
    else head = &ix->universal;
    en->next = *head;
    *head = ix->n++;
}

/* ------------------------------------------------------------ cascading */

typedef struct {
    Style *s;
    const Style *parent;
    float bw[4];
    bool bs[4];
    bool color_set, pos_abs, overflow_hidden, clipped, visible, far_left, far_indent;
    const CssSheet *const *var_sheets;
    int nvar_sheets;
} Ctx;

static const char *lookup_var(Ctx *c, const char *name, size_t nl) {
    for (int s = c->nvar_sheets - 1; s >= 0; s--) {
        const CssSheet *sh = c->var_sheets[s];
        for (int i = sh->nvars - 1; i >= 0; i--)
            if (strlen(sh->var_name[i]) == nl && !strncmp(sh->var_name[i], name, nl)) return sh->var_val[i];
    }
    return NULL;
}

/* Replaces var(--x, fallback) with values. out must hold 256 bytes. */
static void subst_vars(Ctx *c, const char *in, char *out, size_t cap, int depth) {
    size_t o = 0;
    const char *p = in;
    while (*p && o + 1 < cap) {
        if (!strncasecmp(p, "var(", 4) && depth < 4) {
            const char *q = p + 4;
            while (isspace((unsigned char)*q)) q++;
            const char *ns = q;
            while (*q && *q != ',' && *q != ')' && !isspace((unsigned char)*q)) q++;
            size_t nl = (size_t)(q - ns);
            while (isspace((unsigned char)*q)) q++;
            const char *fb = NULL, *fe = NULL;
            int d = 1;
            if (*q == ',') {
                fb = ++q;
                while (*q && d) { if (*q == '(') d++; else if (*q == ')') d--; if (d) q++; }
                fe = q;
            } else {
                while (*q && *q != ')') q++;
            }
            if (*q == ')') q++;
            const char *val = lookup_var(c, ns, nl);
            char tmp[256];
            if (val) subst_vars(c, val, tmp, sizeof(tmp), depth + 1);
            else if (fb) {
                char fbuf[256];
                size_t fl = (size_t)(fe - fb) < 255 ? (size_t)(fe - fb) : 255;
                memcpy(fbuf, fb, fl);
                fbuf[fl] = 0;
                subst_vars(c, fbuf, tmp, sizeof(tmp), depth + 1);
            } else tmp[0] = 0;
            for (char *t = tmp; *t && o + 1 < cap; t++) out[o++] = *t;
            p = q;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

static Color blend(Color fg, Color bg) {
    if (fg.a == 255 || color_is_default(fg)) return fg;
    float a = fg.a / 255.0f;
    Color o = { (u8)(fg.r * a + bg.r * (1 - a)), (u8)(fg.g * a + bg.g * (1 - a)), (u8)(fg.b * a + bg.b * (1 - a)), 255 };
    return o;
}

static float font_kw(const char *v, float parent) {
    static const struct { const char *k; float px; } kw[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) if (ieq(v, kw[i].k)) return kw[i].px;
    if (ieq(v, "smaller")) return parent * 0.83f;
    if (ieq(v, "larger")) return parent * 1.2f;
    return -1;
}

static int keyword(const char *v, const char *const *list) {
    char w[32];
    int n = 0;
    while (*v && !isspace((unsigned char)*v) && n < 31) w[n++] = (char)tolower((unsigned char)*v++);
    w[n] = 0;
    for (int i = 0; list[i]; i++) if (!strcmp(w, list[i])) return i;
    return -1;
}

static void apply_value(Ctx *c, int prop, const char *raw) {
    Style *s = c->s;
    char buf[256];
    if (strstr(raw, "var(")) { subst_vars(c, raw, buf, sizeof(buf), 0); raw = buf; }
    while (isspace((unsigned char)*raw)) raw++;
    if (!*raw) return;
    bool inherit = ieq(raw, "inherit");
    const Style *ps = c->parent;
    Len l;
    Color col;
    int k;
    switch (prop) {
    case P_DISPLAY: {
        static const char *const d[] = { "none", "inline", "block", "list-item", "table", "table-row", "table-cell",
            "table-row-group", "table-header-group", "table-footer-group", "table-caption", "inline-block",
            "flex", "inline-flex", "grid", "inline-grid", "contents", "flow-root", "inline-table", NULL };
        static const unsigned char map[] = { D_NONE, D_INLINE, D_BLOCK, D_LIST_ITEM, D_TABLE, D_ROW, D_CELL,
            D_ROWGROUP, D_ROWGROUP, D_ROWGROUP, D_CAPTION, D_INLINE, D_BLOCK, D_INLINE, D_BLOCK, D_INLINE,
            D_INLINE, D_BLOCK, D_TABLE };
        if ((k = keyword(raw, d)) >= 0) { s->display = map[k]; s->bfc = (k == 17 || k == 12 || k == 14); }
        break;
    }
    case P_COLOR:
        if (inherit) { s->color = ps->color; c->color_set = true; }
        else if ((k = parse_color(raw, &col)) == K_COLOR) {
            s->color = col.a == 0 ? ps->color : blend(col, ps->eff_bg);
            c->color_set = true;
        }
        break;
    case P_BG:
        if ((k = parse_color(raw, &col)) == K_COLOR) {
            if (col.a < 26) s->has_bg = 0;
            else { s->bg = blend(col, ps->eff_bg); s->has_bg = 1; }
        } else if (k == K_CURCOLOR && !color_is_default(s->color)) { s->bg = s->color; s->has_bg = 1; }
        break;
    case P_FWEIGHT:
        if (inherit) s->bold = ps->bold;
        else if (ieq(raw, "bold") || ieq(raw, "bolder")) s->bold = 1;
        else if (ieq(raw, "normal") || ieq(raw, "lighter")) s->bold = 0;
        else if (isdigit((unsigned char)raw[0])) s->bold = atoi(raw) >= 600;
        break;
    case P_FSTYLE:
        if (inherit) s->italic = ps->italic;
        else s->italic = !strncasecmp(raw, "italic", 6) || !strncasecmp(raw, "oblique", 7);
        break;
    case P_FSIZE: {
        float kw = font_kw(raw, ps->fsize);
        if (inherit) s->fsize = ps->fsize;
        else if (kw > 0) s->fsize = kw;
        else if (parse_len(raw, &l) && l.u != U_AUTO) {
            float v = l.u == U_EM ? l.v * ps->fsize : l.u == U_PCT ? l.v * ps->fsize / 100.0f : css_len(l, 0, ps->fsize, 16);
            if (v > 0) s->fsize = v;
        }
        if (s->fsize < 7) s->fsize = 7;
        if (s->fsize > 72) s->fsize = 72;
        break;
    }
    case P_FFAMILY: {
        char low[128];
        snprintf(low, sizeof(low), "%s", raw);
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        s->mono = strstr(low, "mono") || strstr(low, "courier") || strstr(low, "consolas") || strstr(low, "menlo");
        break;
    }
    case P_TALIGN: {
        static const char *const a[] = { "left", "start", "justify", "center", "right", "end", "-webkit-center",
                                         "-moz-center", "inherit", NULL };
        static const unsigned char map[] = { TA_LEFT, TA_LEFT, TA_LEFT, TA_CENTER, TA_RIGHT, TA_RIGHT, TA_CENTER,
                                             TA_CENTER, 255 };
        if ((k = keyword(raw, a)) >= 0) s->align = map[k] == 255 ? ps->align : map[k];
        break;
    }
    case P_TDECO: {
        char low[64];
        snprintf(low, sizeof(low), "%s", raw);
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(low, "none")) { s->underline = 0; s->strike = 0; }
        if (strstr(low, "underline")) s->underline = 1;
        if (strstr(low, "line-through")) s->strike = 1;
        break;
    }
    case P_TTRANSFORM: {
        static const char *const t[] = { "none", "uppercase", "lowercase", "capitalize", NULL };
        if ((k = keyword(raw, t)) >= 0) s->transform = (unsigned char)k;
        break;
    }
    case P_WS: {
        static const char *const w[] = { "normal", "pre", "nowrap", "pre-wrap", "pre-line", "break-spaces", NULL };
        static const unsigned char map[] = { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PREWRAP, WS_PRELINE, WS_PREWRAP };
        if ((k = keyword(raw, w)) >= 0) s->ws = map[k];
        break;
    }
    case P_MT: case P_MR: case P_MB: case P_ML:
        if (parse_len(raw, &l)) s->margin[prop - P_MT] = l;
        break;
    case P_PT: case P_PR: case P_PB: case P_PL:
        if (parse_len(raw, &l) && l.u != U_AUTO && l.v >= 0) s->padding[prop - P_PT] = l;
        break;
    case P_BWT: case P_BWR: case P_BWB: case P_BWL: {
        float w = -1;
        if (ieq(raw, "thin")) w = 1;
        else if (ieq(raw, "medium")) w = 3;
        else if (ieq(raw, "thick")) w = 5;
        else if (parse_len(raw, &l) && l.u != U_AUTO) w = css_len(l, 0, s->fsize, 0);
        if (w >= 0) c->bw[prop - P_BWT] = w > 20 ? 20 : w;
        break;
    }
    case P_BST: case P_BSR: case P_BSB: case P_BSL:
        c->bs[prop - P_BST] = !(ieq(raw, "none") || ieq(raw, "hidden"));
        break;
    case P_BC:
        if ((k = parse_color(raw, &col)) == K_COLOR) s->bcolor = col.a ? blend(col, ps->eff_bg) : (Color){ 0, 0, 0, 0 };
        else if (k == K_CURCOLOR) s->bcolor = s->color;
        break;
    case P_WIDTH: if (parse_len(raw, &l) && l.v >= 0) s->width = l; break;
    case P_MAXW: if (!ieq(raw, "none") && parse_len(raw, &l) && l.v >= 0) s->maxw = l; break;
    case P_HEIGHT: if (parse_len(raw, &l) && l.v >= 0) s->height = l; break;
    case P_FLOAT: {
        static const char *const f[] = { "none", "left", "right", "inline-start", "inline-end", NULL };
        static const unsigned char map[] = { FL_NONE, FL_LEFT, FL_RIGHT, FL_LEFT, FL_RIGHT };
        if ((k = keyword(raw, f)) >= 0) s->fl = map[k];
        break;
    }
    case P_CLEAR: {
        static const char *const cl[] = { "none", "left", "right", "both", NULL };
        if ((k = keyword(raw, cl)) >= 0) s->clear = (unsigned char)k;
        break;
    }
    case P_LIST: {
        static const char *const ls[] = { "disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha",
                                          "none", "lower-latin", "upper-latin", "decimal-leading-zero",
                                          "lower-roman", "upper-roman", NULL };
        static const unsigned char map[] = { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LALPHA, LS_UALPHA,
                                             LS_NONE, LS_LALPHA, LS_UALPHA, LS_DECIMAL, LS_DECIMAL, LS_DECIMAL };
        if ((k = keyword(raw, ls)) >= 0) s->list = map[k];
        break;
    }
    case P_VIS: c->visible = !(ieq(raw, "hidden") || ieq(raw, "collapse")); break;
    case P_POS: c->pos_abs = ieq(raw, "absolute") || ieq(raw, "fixed"); break;
    case P_OVERFLOW:
        c->overflow_hidden = ieq(raw, "hidden") || ieq(raw, "clip");
        if (!ieq(raw, "visible")) s->bfc = 1;
        break;
    case P_CLIP: {
        char low[64];
        snprintf(low, sizeof(low), "%s", raw);
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(low, "rect(0") || strstr(low, "rect(1px") || strstr(low, "inset(50%") || strstr(low, "inset(100%"))
            c->clipped = true;
        break;
    }
    case P_VALIGN: {
        static const char *const va[] = { "middle", "top", "bottom", "baseline", "text-top", "text-bottom", NULL };
        static const unsigned char map[] = { VA_MIDDLE, VA_TOP, VA_BOTTOM, VA_TOP, VA_TOP, VA_BOTTOM };
        if ((k = keyword(raw, va)) >= 0) s->valign = map[k];
        break;
    }
    case P_SPACING:
        if (parse_len(raw, &l) && l.u != U_AUTO) s->spacing = css_len(l, 0, s->fsize, 0);
        break;
    case P_COLLAPSE: s->collapse = ieq(raw, "collapse"); break;
    case P_LEFT:
        /* "left: -9999px" is a common way to hide things off-screen */
        if (parse_len(raw, &l) && l.u != U_AUTO) c->far_left = css_len(l, 1000, s->fsize, 0) <= -900;
        break;
    case P_TEXTINDENT:
        /* "text-indent: -9999px" hides text, usually replaced by a background image */
        if (parse_len(raw, &l) && l.u != U_AUTO) c->far_indent = css_len(l, 1000, s->fsize, 0) <= -900;
        break;
    }
}

/* ------------------------------------------------- presentational hints */

static bool attr_len(const char *v, Len *l) {
    if (!v || !*v) return false;
    char *end;
    float f = strtof(v, &end);
    if (end == v || f < 0) return false;
    l->v = f;
    l->u = (*end == '%') ? U_PCT : U_PX;
    return true;
}

static const Node *ancestor(const Node *n, int tag, int stop) {
    for (const Node *p = n->parent; p; p = p->parent) {
        if (p->tag == tag) return p;
        if (p->tag == stop) return NULL;
    }
    return NULL;
}

static void apply_hints(Ctx *c, const Node *n) {
    Style *s = c->s;
    const char *v;
    Color col;
    Len l;
    int t = n->tag;
    if ((t == TAG_BODY || t == TAG_TABLE || t == TAG_TR || t == TAG_TD || t == TAG_TH) &&
        (v = dom_attr(n, "bgcolor")) && parse_color(v, &col) == K_COLOR && col.a) {
        s->bg = col; s->has_bg = 1;
    }
    if (t == TAG_BODY && (v = dom_attr(n, "text")) && parse_color(v, &col) == K_COLOR && col.a) {
        s->color = col; c->color_set = true;
    }
    if (t == TAG_FONT) {
        if ((v = dom_attr(n, "color")) && parse_color(v, &col) == K_COLOR && col.a) { s->color = col; c->color_set = true; }
        if ((v = dom_attr(n, "size"))) {
            static const float sz[] = { 10, 13, 16, 18, 24, 32, 48 };
            int n2 = atoi(v);
            if (v[0] == '+' || v[0] == '-') n2 += 3;
            if (n2 < 1) n2 = 1;
            if (n2 > 7) n2 = 7;
            s->fsize = sz[n2 - 1];
        }
        if ((v = dom_attr(n, "face")) && (ci_contains(v, "courier") || ci_contains(v, "mono"))) s->mono = 1;
    }
    if ((v = dom_attr(n, "align"))) {
        if (t == TAG_TABLE) {
            if (!strcasecmp(v, "center")) { s->margin[1].u = U_AUTO; s->margin[3].u = U_AUTO; }
            else if (!strcasecmp(v, "left")) s->fl = FL_LEFT;
            else if (!strcasecmp(v, "right")) s->fl = FL_RIGHT;
        } else if (t == TAG_IMG) {
            if (!strcasecmp(v, "left")) s->fl = FL_LEFT;
            else if (!strcasecmp(v, "right")) s->fl = FL_RIGHT;
        } else if (t == TAG_P || t == TAG_DIV || (t >= TAG_H1 && t <= TAG_H6) || t == TAG_TD || t == TAG_TH ||
                   t == TAG_TR || t == TAG_CAPTION || t == TAG_THEAD || t == TAG_TBODY) {
            if (!strcasecmp(v, "center") || !strcasecmp(v, "middle")) s->align = TA_CENTER;
            else if (!strcasecmp(v, "right")) s->align = TA_RIGHT;
            else if (!strcasecmp(v, "left")) s->align = TA_LEFT;
        }
    }
    if ((t == TAG_TABLE || t == TAG_TD || t == TAG_TH || t == TAG_IMG || t == TAG_HR || t == TAG_INPUT) &&
        attr_len(dom_attr(n, "width"), &l))
        s->width = l;
    if ((t == TAG_IMG) && attr_len(dom_attr(n, "height"), &l)) s->height = l;
    if (t == TAG_TABLE) {
        if ((v = dom_attr(n, "cellspacing"))) s->spacing = (float)atof(v);
        if ((v = dom_attr(n, "border")) && atoi(v) > 0) {
            for (int i = 0; i < 4; i++) { c->bw[i] = (float)atoi(v); c->bs[i] = true; }
            s->bcolor = (Color){ 128, 128, 128, 255 };
        }
    }
    if (t == TAG_TD || t == TAG_TH) {
        const Node *tbl = ancestor(n, TAG_TABLE, TAG_ROOT);
        if (tbl) {
            if ((v = dom_attr(tbl, "cellpadding"))) {
                float p = (float)atof(v);
                for (int i = 0; i < 4; i++) s->padding[i] = (Len){ p, U_PX };
            }
            if ((v = dom_attr(tbl, "border")) && atoi(v) > 0) {
                for (int i = 0; i < 4; i++) { c->bw[i] = 1; c->bs[i] = true; }
                s->bcolor = (Color){ 128, 128, 128, 255 };
            }
        }
        const char *va = dom_attr(n, "valign");
        if (!va && n->parent && n->parent->tag == TAG_TR) va = dom_attr(n->parent, "valign");
        if (va) s->valign = !strcasecmp(va, "top") ? VA_TOP : !strcasecmp(va, "bottom") ? VA_BOTTOM : VA_MIDDLE;
        if (dom_attr(n, "nowrap")) s->ws = WS_NOWRAP;
        if (!dom_attr(n, "align") && n->parent && n->parent->tag == TAG_TR && (v = dom_attr(n->parent, "align"))) {
            if (!strcasecmp(v, "center")) s->align = TA_CENTER;
            else if (!strcasecmp(v, "right")) s->align = TA_RIGHT;
        }
        if (!s->has_bg && n->parent && n->parent->tag == TAG_TR && (v = dom_attr(n->parent, "bgcolor")) &&
            parse_color(v, &col) == K_COLOR && col.a) { s->bg = col; s->has_bg = 1; }
    }
    if (t == TAG_IMG && (v = dom_attr(n, "border")) && atoi(v) > 0) {
        for (int i = 0; i < 4; i++) { c->bw[i] = (float)atoi(v); c->bs[i] = true; }
    }
}

/* ---------------------------------------------------------------- cascade */

typedef struct { const Decl *d; int origin; unsigned spec; int sheet_no, order; } Match;

static int match_cmp(const Match *a, const Match *b) {
    if (a->origin != b->origin) return a->origin - b->origin;
    if (a->spec != b->spec) return a->spec < b->spec ? -1 : 1;
    if (a->sheet_no != b->sheet_no) return a->sheet_no - b->sheet_no;
    return a->order - b->order;
}

static void init_style(Style *s, const Style *p) {
    memset(s, 0, sizeof(*s));
    s->display = D_INLINE;
    s->color = p->color;
    s->fsize = p->fsize;
    s->bold = p->bold;
    s->italic = p->italic;
    s->mono = p->mono;
    s->underline = p->underline;
    s->strike = p->strike;
    s->align = p->align;
    s->ws = p->ws;
    s->transform = p->transform;
    s->list = p->list;
    s->spacing = p->spacing;
    s->collapse = p->collapse;
    s->eff_bg = p->eff_bg;
    s->bcolor = (Color){ 0, 0, 0, 0 };
}

typedef struct {
    Index ix;
    const CssSheet *const *sheets;
    int nsheets;
    bool author;
    Arena *arena;
    Match *matches;
    int maxm;
} Run;

static void collect(Run *r, int head, const Node *n, int *nm) {
    for (int i = head; i >= 0; i = r->ix.e[i].next) {
        const Entry *en = &r->ix.e[i];
        if (!match_from(&en->rule->sel, 0, n, 32)) continue;
        for (int k = 0; k < en->rule->count && *nm < r->maxm; k++) {
            Match *m = &r->matches[(*nm)++];
            m->d = &en->sheet->decls[en->rule->first + k];
            m->origin = en->origin;
            m->spec = en->rule->sel.spec;
            m->sheet_no = en->sheet_no;
            m->order = en->rule->order * 64 + k;
        }
    }
}

static void style_node(Run *r, Node *n, const Style *parent) {
    Style *s = arena_alloc(r->arena, sizeof(Style));
    if (!s) return;
    n->style = s;
    init_style(s, parent);
    Ctx c;
    memset(&c, 0, sizeof(c));
    c.s = s;
    c.parent = parent;
    c.visible = true;
    for (int i = 0; i < 4; i++) c.bw[i] = 3;
    c.var_sheets = r->sheets;
    c.nvar_sheets = r->author ? r->nsheets : 0;

    int nm = 0;
    collect(r, r->ix.universal, n, &nm);
    if (n->id) collect(r, r->ix.buckets[hkey('#', n->id) & 2047], n, &nm);
    for (int i = 0; i < n->nclasses; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (((hkey('.', n->classes[j]) ^ hkey('.', n->classes[i])) & 2047) == 0) dup = true;
        if (!dup) collect(r, r->ix.buckets[hkey('.', n->classes[i]) & 2047], n, &nm);
    }
    collect(r, r->ix.buckets[hkey('t', n->name) & 2047], n, &nm);
    /* insertion sort by cascade order */
    for (int i = 1; i < nm; i++) {
        Match m = r->matches[i];
        int j = i - 1;
        while (j >= 0 && match_cmp(&r->matches[j], &m) > 0) { r->matches[j + 1] = r->matches[j]; j--; }
        r->matches[j + 1] = m;
    }
    /* UA normal */
    for (int i = 0; i < nm; i++)
        if (r->matches[i].origin == 0 && !r->matches[i].d->important) apply_value(&c, r->matches[i].d->prop, r->matches[i].d->raw);
    if (r->author) apply_hints(&c, n);
    for (int i = 0; i < nm; i++)
        if (r->matches[i].origin == 1 && !r->matches[i].d->important) apply_value(&c, r->matches[i].d->prop, r->matches[i].d->raw);
    /* inline style="" */
    CssSheet *inl = NULL;
    const char *st = r->author ? dom_attr(n, "style") : NULL;
    if (st && *st) {
        inl = calloc(1, sizeof(CssSheet));
        if (inl) {
            parse_decls(inl, st, st + strlen(st), false);
            for (int i = 0; i < inl->ndecls; i++) if (!inl->decls[i].important) apply_value(&c, inl->decls[i].prop, inl->decls[i].raw);
        }
    }
    for (int i = 0; i < nm; i++)
        if (r->matches[i].origin == 1 && r->matches[i].d->important) apply_value(&c, r->matches[i].d->prop, r->matches[i].d->raw);
    if (inl) {
        for (int i = 0; i < inl->ndecls; i++) if (inl->decls[i].important) apply_value(&c, inl->decls[i].prop, inl->decls[i].raw);
        css_free(inl);
    }

    /* finish up */
    for (int i = 0; i < 4; i++) s->border[i] = c.bs[i] ? c.bw[i] : 0;
    if (color_is_default(s->bcolor) && (s->border[0] || s->border[1] || s->border[2] || s->border[3]))
        s->bcolor = color_is_default(s->color) ? (Color){ 128, 128, 128, 255 } : s->color;
    if (n->tag == TAG_A && dom_attr(n, "href") && !c.color_set) s->color = (Color){ DEFAULT_LINK_R, 0, 0, 0 };
    if (!c.visible) s->display = D_NONE;
    if (c.clipped) s->display = D_NONE;
    if (c.pos_abs && c.overflow_hidden) {
        float w = css_len(s->width, 100, s->fsize, 100), h = css_len(s->height, 100, s->fsize, 100);
        if (w <= 2 || h <= 2) s->display = D_NONE;   /* "screen reader only" text */
    }
    if (c.pos_abs && c.far_left) s->display = D_NONE;
    if (c.far_indent) s->display = D_NONE;
    if (c.pos_abs) {
        /* positioned boxes sit outside the normal flow in real browsers;
         * we keep their content in place but never let them reserve space */
        s->abspos = 1;
        s->fl = FL_NONE;
        if (s->display == D_INLINE && n->tag != TAG_IMG && n->tag != TAG_A && n->tag != TAG_SPAN) s->display = D_BLOCK;
    }
    if (s->fl != FL_NONE && s->display == D_INLINE && n->tag != TAG_IMG) s->display = D_BLOCK;
    if (n->tag == TAG_HTML || n->tag == TAG_BODY) {
        if (s->display == D_INLINE) s->display = D_BLOCK;
        s->fl = FL_NONE;
    }
    if (s->display == D_CELL || s->fl != FL_NONE) s->bfc = 1;
    s->eff_bg = s->has_bg ? s->bg : parent->eff_bg;
}

static void style_tree(Run *r, Node *n, const Style *parent) {
    for (Node *ch = n->first; ch; ch = ch->next) {
        if (ch->type != NT_ELEM) continue;
        style_node(r, ch, parent);
        if (!ch->style) continue;
        if (ch->style->display == D_NONE) continue;   /* children never shown */
        style_tree(r, ch, ch->style);
    }
}

void css_style_tree(Dom *dom, CssSheet *const *sheets, int nsheets, bool author, Color default_bg, Arena *styles) {
    if (!ua_sheet) ua_sheet = css_parse(ua_css, (int)strlen(ua_css));
    Run r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 2048; i++) r.ix.buckets[i] = -1;
    r.ix.universal = -1;
    r.sheets = (const CssSheet *const *)sheets;
    r.nsheets = nsheets;
    r.author = author;
    r.arena = styles;
    r.maxm = 2048;
    r.matches = malloc(sizeof(Match) * (size_t)r.maxm);
    if (!r.matches) return;
    /* add in reverse so bucket lists come out in source order (not required, but tidy) */
    if (ua_sheet)
        for (int i = ua_sheet->nrules - 1; i >= 0; i--) index_add(&r.ix, &ua_sheet->rules[i], ua_sheet, 0, 0);
    if (author)
        for (int s = 0; s < nsheets; s++)
            if (sheets[s])
                for (int i = sheets[s]->nrules - 1; i >= 0; i--) index_add(&r.ix, &sheets[s]->rules[i], sheets[s], 1, s + 1);

    Style root;
    memset(&root, 0, sizeof(root));
    root.display = D_BLOCK;
    root.fsize = 16;
    root.color = (Color){ DEFAULT_TEXT_R, 0, 0, 0 };
    root.eff_bg = default_bg;
    root.spacing = 2;
    Style *rs = arena_alloc(styles, sizeof(Style));
    if (rs) { *rs = root; dom->root->style = rs; }
    style_tree(&r, dom->root, &root);
    free(r.matches);
    free(r.ix.e);
}
