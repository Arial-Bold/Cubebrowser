/* url.c - URL handling. */
#include "url.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_lower(char *dst, const char *src, size_t n, size_t cap) {
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[n] = 0;
}

static void strip_fragment(char *s) {
    char *h = strchr(s, '#');
    if (h) *h = 0;
}

bool url_parse(const char *s, Url *u) {
    memset(u, 0, sizeof(*u));
    while (isspace((unsigned char)*s)) s++;
    if (strncasecmp(s, "about:", 6) == 0) {
        strcpy(u->scheme, "about");
        snprintf(u->path, sizeof(u->path), "%s", s + 6);
        return true;
    }
    const char *sep = strstr(s, "://");
    if (!sep) return false;
    copy_lower(u->scheme, s, (size_t)(sep - s), sizeof(u->scheme));
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https")) return false;
    u->port = strcmp(u->scheme, "https") == 0 ? 443 : 80;
    const char *h = sep + 3;
    const char *end = h + strcspn(h, "/?#");
    const char *at = memchr(h, '@', (size_t)(end - h));
    if (at) h = at + 1;                       /* drop user:pass@ */
    const char *colon = memchr(h, ':', (size_t)(end - h));
    const char *host_end = colon ? colon : end;
    if (host_end == h) return false;
    copy_lower(u->host, h, (size_t)(host_end - h), sizeof(u->host));
    if (colon) {
        int p = atoi(colon + 1);
        if (p <= 0 || p > 65535) return false;
        u->port = (unsigned short)p;
    }
    if (*end == '/' || *end == '?') {
        if (*end == '?') snprintf(u->path, sizeof(u->path), "/%s", end);
        else snprintf(u->path, sizeof(u->path), "%s", end);
    } else {
        strcpy(u->path, "/");
    }
    strip_fragment(u->path);
    for (char *p = u->path; *p; p++)
        if (*p == ' ') *p = '+';   /* never send raw spaces */
    return true;
}

/* Remove "." and ".." segments from the path part (before any '?'). */
static void normalize_path(char *path) {
    char *q = strchr(path, '?');
    char query[URL_MAX] = "";
    if (q) { snprintf(query, sizeof(query), "%s", q); *q = 0; }
    char out[URL_MAX];
    size_t n = 0;
    const char *p = path;
    bool trailing = false;
    while (*p) {
        while (*p == '/') p++;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        if (len == 0) continue;
        trailing = (*p == '/');
        if (len == 1 && seg[0] == '.') { trailing = true; continue; }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 0) n--;
            trailing = true;
            continue;
        }
        if (n + len + 2 >= sizeof(out)) break;
        out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
    }
    if (n == 0 || trailing) out[n++] = '/';
    out[n] = 0;
    snprintf(path, URL_MAX, "%s%s", out, query);
}

static bool has_scheme(const char *s) {
    if (!isalpha((unsigned char)*s)) return false;
    for (const char *p = s; *p; p++) {
        if (*p == ':') return true;
        if (!(isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) return false;
    }
    return false;
}

void url_resolve(const char *base, const char *rel, char *out, size_t outlen) {
    while (isspace((unsigned char)*rel)) rel++;
    char r[URL_MAX];
    snprintf(r, sizeof(r), "%s", rel);
    size_t rl = strlen(r);
    while (rl && isspace((unsigned char)r[rl - 1])) r[--rl] = 0;

    if (has_scheme(r)) { snprintf(out, outlen, "%s", r); strip_fragment(out); return; }
    Url b;
    if (!url_parse(base, &b) || !strcmp(b.scheme, "about")) { snprintf(out, outlen, "%s", r); return; }
    if (r[0] == 0 || r[0] == '#') {
        snprintf(out, outlen, "%s", base);
        strip_fragment(out);
        return;
    }
    if (r[0] == '/' && r[1] == '/') { snprintf(out, outlen, "%s:%s", b.scheme, r); strip_fragment(out); return; }

    char prefix[300];
    bool default_port = (b.port == 443 && !strcmp(b.scheme, "https")) || (b.port == 80 && !strcmp(b.scheme, "http"));
    if (default_port) snprintf(prefix, sizeof(prefix), "%s://%s", b.scheme, b.host);
    else snprintf(prefix, sizeof(prefix), "%s://%s:%u", b.scheme, b.host, b.port);

    char path[URL_MAX];
    if (r[0] == '/') {
        snprintf(path, sizeof(path), "%s", r);
    } else if (r[0] == '?') {
        char *q = strchr(b.path, '?');
        if (q) *q = 0;
        snprintf(path, sizeof(path), "%s%s", b.path, r);
    } else {
        char *q = strchr(b.path, '?');
        if (q) *q = 0;
        char *slash = strrchr(b.path, '/');
        if (slash) slash[1] = 0;
        snprintf(path, sizeof(path), "%s%s", b.path, r);
    }
    strip_fragment(path);
    normalize_path(path);
    snprintf(out, outlen, "%s%s", prefix, path);
}

void url_encode_component(const char *in, char *out, size_t outlen) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else if (c == ' ') out[n++] = '+';
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
}

void url_from_input(const char *input, char *out, size_t outlen) {
    while (isspace((unsigned char)*input)) input++;
    char t[URL_MAX];
    snprintf(t, sizeof(t), "%s", input);
    size_t l = strlen(t);
    while (l && isspace((unsigned char)t[l - 1])) t[--l] = 0;
    if (!l) { out[0] = 0; return; }
    if (!strncasecmp(t, "about:", 6) || strstr(t, "://")) { snprintf(out, outlen, "%s", t); return; }
    bool looks_like_address = !strchr(t, ' ') && (strchr(t, '.') || !strncasecmp(t, "localhost", 9));
    if (looks_like_address) {
        snprintf(out, outlen, "https://%s", t);
        return;
    }
    char enc[URL_MAX];
    url_encode_component(t, enc, sizeof(enc));
    snprintf(out, outlen, "%s%s", settings_search_prefix(), enc);
}
