/* cookies.c - see cookies.h */
#include "cookies.h"
#include "settings.h"
#include "storage.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_COOKIES 300
#define COOKIE_FILE STORAGE_DIR "/cookies.txt"
#define COOKIE_TMP  STORAGE_DIR "/cookies.tmp"

typedef struct {
    char name[64];
    char value[1024];
    char domain[128];     /* lowercase, no leading dot */
    char path[128];
    time_t expires;       /* 0 = until the browser closes */
    unsigned last_used;
    bool host_only, secure, used;
} Cookie;

static Cookie *jar;
static unsigned use_counter;
static bool dirty;

#ifdef GEKKO
#include <gccore.h>
#include <ogc/mutex.h>
static mutex_t lock;
static bool lock_ready;
static void jar_lock(void) { if (lock_ready) LWP_MutexLock(lock); }
static void jar_unlock(void) { if (lock_ready) LWP_MutexUnlock(lock); }
static void lock_init(void) { if (LWP_MutexInit(&lock, false) >= 0) lock_ready = true; }
#else
static void jar_lock(void) {}
static void jar_unlock(void) {}
static void lock_init(void) {}
#endif

static bool ensure_jar(void) {
    if (!jar) jar = calloc(MAX_COOKIES, sizeof(Cookie));
    return jar != NULL;
}

int cookies_count(void) {
    int n = 0;
    if (jar) for (int i = 0; i < MAX_COOKIES; i++) if (jar[i].used) n++;
    return n;
}

/* ------------------------------------------------------------- dates */

static time_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)era * 146097 + doe - 719468;
}

/* Accepts "Wed, 21 Oct 2015 07:28:00 GMT", "Wednesday, 21-Oct-15 07:28:00 GMT",
 * "Wed Oct 21 07:28:00 2015" and similar. */
time_t cookies_parse_date(const char *s) {
    static const char *const months[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    int day = -1, mon = -1, year = -1, hh = -1, mm = 0, ss = 0;
    char tok[32];
    while (*s) {
        while (*s && !isalnum((unsigned char)*s)) s++;
        int n = 0;
        while (*s && (isalnum((unsigned char)*s) || *s == ':') && n < 31) tok[n++] = *s++;
        tok[n] = 0;
        if (!n) break;
        if (strchr(tok, ':') && hh < 0) { sscanf(tok, "%d:%d:%d", &hh, &mm, &ss); continue; }
        if (isalpha((unsigned char)tok[0])) {
            for (int m = 0; m < 12; m++) if (!strncasecmp(tok, months[m], 3)) mon = m + 1;
            continue;
        }
        int v = atoi(tok);
        if (n >= 3 && year < 0) year = v;
        else if (day < 0 && v >= 1 && v <= 31) day = v;
        else if (year < 0) year = v;
    }
    if (year >= 0 && year < 70) year += 2000;
    else if (year >= 70 && year < 100) year += 1900;
    if (day < 1 || mon < 1 || year < 1970 || hh < 0 || hh > 23 || mm > 59 || ss > 60) return 0;
    return days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

/* ------------------------------------------------------------- rules */

/* Shared suffixes under which anyone can register a site; a cookie for one
 * of these would leak to unrelated sites, so it's refused. */
static bool public_suffix(const char *d) {
    static const char *const list[] = {
        "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "net.uk", "com.au", "net.au", "org.au", "edu.au",
        "co.jp", "ne.jp", "or.jp", "co.nz", "org.nz", "com.br", "com.cn", "com.tw", "co.in", "co.za",
        "com.mx", "co.kr", "com.sg", "com.tr", "com.ar", "co.il", "github.io", "blogspot.com",
        "herokuapp.com", "netlify.app", "vercel.app", "pages.dev", "neocities.org", "wordpress.com",
        "appspot.com", "cloudfront.net", "azurewebsites.net", NULL
    };
    if (!strchr(d, '.')) return true;   /* a bare top-level domain like "com" */
    for (int i = 0; list[i]; i++) if (!strcasecmp(d, list[i])) return true;
    return false;
}

static bool domain_match(const char *host, const char *domain) {
    size_t h = strlen(host), d = strlen(domain);
    if (h == d) return !strcasecmp(host, domain);
    return h > d && host[h - d - 1] == '.' && !strcasecmp(host + h - d, domain);
}

static bool path_match(const char *req, const char *cookie) {
    size_t n = strlen(cookie);
    if (strncmp(req, cookie, n)) return false;
    return req[n] == 0 || req[n] == '/' || req[n] == '?' || (n && cookie[n - 1] == '/');
}

static void default_path(const char *req, char *out, size_t cap) {
    char p[256];
    snprintf(p, sizeof(p), "%s", req);
    char *q = strchr(p, '?');
    if (q) *q = 0;
    char *slash = strrchr(p, '/');
    if (!slash || slash == p) { snprintf(out, cap, "/"); return; }
    *slash = 0;
    snprintf(out, cap, "%s", p);
}

static void trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t') a++;
    memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
}

static Cookie *find(const char *name, const char *domain, const char *path) {
    for (int i = 0; i < MAX_COOKIES; i++)
        if (jar[i].used && !strcmp(jar[i].name, name) && !strcasecmp(jar[i].domain, domain) && !strcmp(jar[i].path, path))
            return &jar[i];
    return NULL;
}

static Cookie *free_slot(void) {
    int oldest = 0;
    for (int i = 0; i < MAX_COOKIES; i++) {
        if (!jar[i].used) return &jar[i];
        if (jar[i].last_used < jar[oldest].last_used) oldest = i;
    }
    return &jar[oldest];   /* full: replace the least recently used */
}

void cookies_receive(const char *host, const char *req_path, bool secure, const char *header, time_t now) {
    if (!g_set.cookies || !ensure_jar()) return;
    char buf[1400];
    snprintf(buf, sizeof(buf), "%s", header);
    char *semi = strchr(buf, ';');
    char *attrs = NULL;
    if (semi) { *semi = 0; attrs = semi + 1; }
    char *eq = strchr(buf, '=');
    if (!eq) return;
    *eq = 0;
    char name[64], value[1024];
    snprintf(name, sizeof(name), "%s", buf);
    snprintf(value, sizeof(value), "%s", eq + 1);
    trim(name);
    trim(value);
    if (!name[0] || strlen(eq + 1) >= sizeof(value)) return;

    char domain[128] = "", path[128] = "";
    bool is_secure = false, have_maxage = false;
    time_t expires = 0;
    long maxage = 0;
    while (attrs && *attrs) {
        char *next = strchr(attrs, ';');
        if (next) *next++ = 0;
        char *k = attrs, *v = strchr(attrs, '=');
        if (v) *v++ = 0; else v = "";
        trim(k);
        trim(v);
        if (!strcasecmp(k, "domain")) { while (*v == '.') v++; snprintf(domain, sizeof(domain), "%s", v); }
        else if (!strcasecmp(k, "path") && v[0] == '/') snprintf(path, sizeof(path), "%s", v);
        else if (!strcasecmp(k, "secure")) is_secure = true;
        else if (!strcasecmp(k, "max-age")) { have_maxage = true; maxage = atol(v); }
        else if (!strcasecmp(k, "expires")) expires = cookies_parse_date(v);
        attrs = next;
    }
    for (char *c = domain; *c; c++) *c = (char)tolower((unsigned char)*c);
    bool host_only = !domain[0];
    if (host_only) snprintf(domain, sizeof(domain), "%s", host);
    else if (!domain_match(host, domain) || public_suffix(domain)) return;   /* not this site's to set */
    if (is_secure && !secure) return;   /* secure cookies only come over HTTPS */
    if (!path[0]) default_path(req_path, path, sizeof(path));
    if (have_maxage) expires = maxage > 0 ? now + maxage : 1;   /* 1 = already expired */

    jar_lock();
    Cookie *c = find(name, domain, path);
    if (expires && expires <= now) {   /* the site is deleting it */
        if (c) { c->used = false; dirty = true; }
        jar_unlock();
        return;
    }
    if (!c) c = free_slot();
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->value, sizeof(c->value), "%s", value);
    snprintf(c->domain, sizeof(c->domain), "%s", domain);
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->expires = expires;
    c->secure = is_secure;
    c->host_only = host_only;
    c->used = true;
    c->last_used = ++use_counter;
    dirty = true;
    jar_unlock();
}

void cookies_for(const char *host, const char *path, bool secure, time_t now, char *out, size_t cap) {
    out[0] = 0;
    if (!g_set.cookies || !jar) return;
    size_t o = 0;
    jar_lock();
    /* longer paths first, as browsers do */
    for (int pass = 127; pass >= 0; pass--) {
        for (int i = 0; i < MAX_COOKIES; i++) {
            Cookie *c = &jar[i];
            if (!c->used || (int)strlen(c->path) != pass) continue;
            if (c->expires && c->expires <= now) { c->used = false; dirty = true; continue; }
            if (c->secure && !secure) continue;
            if (c->host_only ? strcasecmp(host, c->domain) : !domain_match(host, c->domain)) continue;
            if (!path_match(path, c->path)) continue;
            size_t need = strlen(c->name) + strlen(c->value) + 3;
            if (o + need >= cap) continue;
            o += (size_t)snprintf(out + o, cap - o, "%s%s=%s", o ? "; " : "", c->name, c->value);
            c->last_used = ++use_counter;
        }
    }
    jar_unlock();
}

/* --------------------------------------------------------- saving */

void cookies_clear(void) {
    if (!ensure_jar()) return;
    jar_lock();
    memset(jar, 0, sizeof(Cookie) * MAX_COOKIES);
    dirty = false;
    jar_unlock();
    remove(COOKIE_FILE);
}

void cookies_save_if_needed(void) {
    if (!dirty || !jar) return;
    if (!g_set.keep_logins || !g_set.cookies) { dirty = false; remove(COOKIE_FILE); return; }
    size_t cap = 64 * 1024, n = 0;
    char *buf = malloc(cap);
    if (!buf) return;
    jar_lock();
    n += (size_t)snprintf(buf, cap, "# CubeBrowser cookies. Delete this file to sign out of every site.\n");
    for (int i = 0; i < MAX_COOKIES && n < cap - 1400; i++) {
        Cookie *c = &jar[i];
        if (!c->used) continue;
        n += (size_t)snprintf(buf + n, cap - n, "%s\t%d\t%s\t%d\t%lld\t%s\t%s\n", c->domain, c->host_only ? 1 : 0, c->path,
                              c->secure ? 1 : 0, (long long)c->expires, c->name, c->value);
    }
    dirty = false;
    jar_unlock();
    storage_write_atomic(COOKIE_FILE, COOKIE_TMP, buf, n);
    free(buf);
}

void cookies_init(void) {
    lock_init();
    if (!ensure_jar()) return;
    if (!g_set.keep_logins) { remove(COOKIE_FILE); return; }
    size_t len;
    char *data = storage_read(COOKIE_FILE, &len);
    if (!data) return;
    char *save = NULL;
    int k = 0;
    for (char *line = strtok_r(data, "\n", &save); line && k < MAX_COOKIES; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#') continue;
        char *f[7];
        int nf = 0;
        char *p = line;
        while (nf < 7) {
            f[nf++] = p;
            char *t = strchr(p, '\t');
            if (!t) break;
            *t = 0;
            p = t + 1;
        }
        if (nf < 7) continue;
        Cookie *c = &jar[k++];
        snprintf(c->domain, sizeof(c->domain), "%s", f[0]);
        c->host_only = atoi(f[1]) != 0;
        snprintf(c->path, sizeof(c->path), "%s", f[2]);
        c->secure = atoi(f[3]) != 0;
        c->expires = (time_t)atoll(f[4]);
        snprintf(c->name, sizeof(c->name), "%s", f[5]);
        snprintf(c->value, sizeof(c->value), "%s", f[6]);
        c->used = c->name[0] && c->domain[0] && c->path[0] == '/';
        c->last_used = ++use_counter;
    }
    free(data);
}
