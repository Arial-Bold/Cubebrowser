/* http.c - HTTP/1.1 GET/POST with redirects, chunked decoding, size limits
 * and keep-alive: the last connection stays open so the next request to the
 * same site (e.g. a run of images) skips the slow TLS handshake. */
#include "http.h"
#include "conn.h"
#include "netio.h"
#include "settings.h"
#include "cookies.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEFAULT_ACCEPT "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5"

typedef struct {
    Conn *c;
    char buf[16384];
    size_t pos, len;
    bool eof, err;
} Reader;

/* one cached connection */
static Conn kconn;
static bool kopen;
static char kkey[300];

void http_close_idle(void) {
    if (kopen) conn_close(&kconn);
    kopen = false;
}

static bool rd_fill(Reader *r) {
    if (r->eof || r->err) return false;
    int n = conn_read(r->c, r->buf, sizeof(r->buf));
    if (n < 0) { r->err = true; return false; }
    if (n == 0) { r->eof = true; return false; }
    r->pos = 0;
    r->len = (size_t)n;
    return true;
}

/* Reads one line (without CR/LF). Returns length, or -1 at EOF/error. */
static int rd_line(Reader *r, char *out, size_t cap) {
    size_t n = 0;
    bool any = false;
    for (;;) {
        if (r->pos >= r->len && !rd_fill(r)) return any ? (int)n : -1;
        char ch = r->buf[r->pos++];
        any = true;
        if (ch == '\n') break;
        if (ch != '\r' && n + 1 < cap) out[n++] = ch;
    }
    out[n] = 0;
    return (int)n;
}

static int rd_some(Reader *r, char *out, size_t max) {
    if (r->pos >= r->len && max >= sizeof(r->buf) && !r->eof && !r->err) {
        /* big reads go straight into the caller's buffer (no extra copy) */
        int n = conn_read(r->c, out, max);
        if (n < 0) { r->err = true; return -1; }
        if (n == 0) { r->eof = true; return 0; }
        return n;
    }
    if (r->pos >= r->len && !rd_fill(r)) return r->err ? -1 : 0;
    size_t n = r->len - r->pos;
    if (n > max) n = max;
    memcpy(out, r->buf + r->pos, n);
    r->pos += n;
    return (int)n;
}

typedef struct {
    char *data;
    size_t len, cap, limit;
    bool truncated;
} Body;

static bool body_append(Body *b, const char *src, size_t n) {
    if (b->len + n > b->limit) {
        n = b->limit - b->len;
        b->truncated = true;
    }
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 16384;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = realloc(b->data, nc);
        if (!nd) { b->truncated = true; return false; }
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = 0;
    return !b->truncated;
}

static bool read_exact(Reader *r, Body *b, size_t n, const HttpHooks *h) {
    char tmp[4096];
    while (n > 0) {
        int got = rd_some(r, tmp, n < sizeof(tmp) ? n : sizeof(tmp));
        if (got <= 0) return false;
        if (!body_append(b, tmp, (size_t)got)) return false;
        n -= (size_t)got;
        if (h && h->progress) *h->progress = (int)b->len;
        if (h && h->cancel && *h->cancel) return false;
    }
    return true;
}

static void stage(const HttpHooks *h, const char *msg) {
    if (h && h->stage) h->stage(msg);
}

enum { FO_FAIL, FO_OK, FO_RETRY };

/* Extra request headers: Cookie, and Referer/Origin (many login forms
 * check where a submission came from). */
static int extra_headers(char *out, size_t cap, const Url *u, bool tls, const char *post, const HttpHooks *hooks) {
    int n = 0;
    out[0] = 0;
    if (!hooks || !hooks->no_cookies) {
        char ck[4096];   /* local: several download connections build headers at once */
        cookies_for(u->host, u->path, tls, netio_now(), ck, sizeof(ck));
        if (ck[0]) n += snprintf(out + n, cap - (size_t)n, "Cookie: %s\r\n", ck);
    }
    const char *ref = hooks ? hooks->referer : NULL;
    /* never reveal an https page to an http site */
    if (ref && (!strncasecmp(ref, "http://", 7) || (!strncasecmp(ref, "https://", 8) && tls)) && (size_t)n < cap) {
        char clean[URL_MAX];
        snprintf(clean, sizeof(clean), "%s", ref);
        char *h = strchr(clean, '#');
        if (h) *h = 0;
        n += snprintf(out + n, cap - (size_t)n, "Referer: %s\r\n", clean);
        if (post && (size_t)n < cap) {
            Url r;
            if (url_parse(clean, &r)) {
                bool def = (!strcmp(r.scheme, "https") && r.port == 443) || (!strcmp(r.scheme, "http") && r.port == 80);
                if (def) n += snprintf(out + n, cap - (size_t)n, "Origin: %s://%s\r\n", r.scheme, r.host);
                else n += snprintf(out + n, cap - (size_t)n, "Origin: %s://%s:%u\r\n", r.scheme, r.host, r.port);
            }
        }
    }
    if ((size_t)n >= cap) { out[0] = 0; n = 0; }
    return n;
}

static bool contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return true;
    return false;
}

static int fetch_once(const char *url, const char *post, const char *accept, HttpResult *res,
                      const HttpHooks *hooks, char *location, size_t loclen) {
    Url u;
    if (!url_parse(url, &u) || strcmp(u.scheme, "about") == 0) {
        snprintf(res->error, sizeof(res->error), "That address isn't a web page I can open.");
        return FO_FAIL;
    }
    bool tls = strcmp(u.scheme, "https") == 0;
    char key[300];
    snprintf(key, sizeof(key), "%s:%u:%d", u.host, u.port, tls ? 1 : 0);
    bool reused = false;
    if (kopen && strcmp(key, kkey) == 0) {
        reused = true;
        kconn.cancel = hooks ? hooks->cancel : NULL;
    } else {
        http_close_idle();
        if (!conn_open(&kconn, u.host, u.port, tls, hooks ? hooks->cancel : NULL, hooks ? hooks->stage : NULL,
                       res->error, sizeof(res->error)))
            return FO_FAIL;
        kopen = true;
        snprintf(kkey, sizeof(kkey), "%s", key);
    }
    res->secure = tls;
    res->verified = kconn.verified;

    static char req[URL_MAX + 8192];
    bool default_port = (tls && u.port == 443) || (!tls && u.port == 80);
    char hosthdr[300];
    if (default_port) snprintf(hosthdr, sizeof(hosthdr), "%s", u.host);
    else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
    static char extra[6144];
    extra_headers(extra, sizeof(extra), &u, tls, post, hooks);
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: %s\r\n"
                     "Accept-Language: en\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n%s",
                     post ? "POST" : "GET", u.path, hosthdr, user_agent_strings[g_set.user_agent],
                     accept ? accept : DEFAULT_ACCEPT, extra);
    if (post && n < (int)sizeof(req))
        n += snprintf(req + n, sizeof(req) - n,
                      "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n",
                      (unsigned)strlen(post));
    if (n < (int)sizeof(req)) n += snprintf(req + n, sizeof(req) - n, "\r\n");
    if (n >= (int)sizeof(req)) {
        snprintf(res->error, sizeof(res->error), "That address is too long.");
        return FO_FAIL;
    }
    if (!reused) stage(hooks, "Requesting page...");
    if (!conn_write_all(&kconn, req, (size_t)n) || (post && !conn_write_all(&kconn, post, strlen(post)))) {
        http_close_idle();
        if (reused) return FO_RETRY;   /* the server had closed the idle connection */
        snprintf(res->error, sizeof(res->error), "Couldn't send the request.");
        return FO_FAIL;
    }

    Reader *r = calloc(1, sizeof(Reader));
    if (!r) { http_close_idle(); snprintf(res->error, sizeof(res->error), "Out of memory."); return FO_FAIL; }
    r->c = &kconn;
    char line[1024];
    long content_length = -1;
    bool chunked = false, server_close = false, complete = false;
    Body b = { 0 };

    /* status line, skipping any 1xx interim responses */
    for (;;) {
        int l = rd_line(r, line, sizeof(line));
        if (l < 0 && reused) { free(r); http_close_idle(); return FO_RETRY; }
        if (l < 0 || strncmp(line, "HTTP/", 5) != 0) {
            snprintf(res->error, sizeof(res->error), "The site sent a response I couldn't understand.");
            goto fail;
        }
        const char *sp = strchr(line, ' ');
        res->status = sp ? atoi(sp + 1) : 0;
        if (res->status >= 200 || res->status < 100) break;
        while (rd_line(r, line, sizeof(line)) > 0) {}
    }
    if (!strncmp(line, "HTTP/1.0", 8)) server_close = true;
    stage(hooks, "Receiving...");
    for (int i = 0; i < 200; i++) {
        int l = rd_line(r, line, sizeof(line));
        if (l < 0) { snprintf(res->error, sizeof(res->error), "Connection closed early."); goto fail; }
        if (l == 0) break;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (!strcasecmp(line, "content-length")) content_length = atol(val);
        else if (!strcasecmp(line, "transfer-encoding") && strstr(val, "chunked")) chunked = true;
        else if (!strcasecmp(line, "content-type")) snprintf(res->content_type, sizeof(res->content_type), "%s", val);
        else if (!strcasecmp(line, "location")) snprintf(location, loclen, "%s", val);
        else if (!strcasecmp(line, "connection") && contains_nocase(val, "close")) server_close = true;
        else if (!strcasecmp(line, "content-disposition")) http_disposition_filename(val, res->filename, sizeof(res->filename));
        else if (!strcasecmp(line, "set-cookie") && !(hooks && hooks->no_cookies)) cookies_receive(u.host, u.path, tls, val, netio_now());
    }
    res->length = content_length;
    if (hooks && hooks->want_body && res->status >= 200 && res->status < 300 &&
        !hooks->want_body(url, res->content_type)) {
        /* a file to save, not a page: stop here and let the browser ask */
        res->download = true;
        free(r);
        http_close_idle();
        return FO_OK;
    }

    b.limit = (size_t)g_set.max_page_kb * 1024;
    bool no_body = (res->status == 204 || res->status == 304);
    if (no_body) {
        complete = true;
    } else if (chunked) {
        for (;;) {
            if (rd_line(r, line, sizeof(line)) < 0) break;
            long sz = strtol(line, NULL, 16);
            if (sz <= 0) {
                while (rd_line(r, line, sizeof(line)) > 0) {}   /* trailers */
                complete = true;
                break;
            }
            if (!read_exact(r, &b, (size_t)sz, hooks)) break;
            rd_line(r, line, sizeof(line));
        }
    } else if (content_length >= 0) {
        complete = read_exact(r, &b, (size_t)content_length, hooks);
    } else {
        char tmp[4096];
        for (;;) {
            int got = rd_some(r, tmp, sizeof(tmp));
            if (got <= 0 || !body_append(&b, tmp, (size_t)got)) break;
            if (hooks && hooks->progress) *hooks->progress = (int)b.len;
            if (hooks && hooks->cancel && *hooks->cancel) break;
        }
        server_close = true;   /* body ended by closing the connection */
    }
    if (hooks && hooks->cancel && *hooks->cancel) {
        free(b.data);
        snprintf(res->error, sizeof(res->error), "Stopped.");
        goto fail;
    }
    if (!b.data) { b.data = calloc(1, 1); b.len = 0; }
    res->body = b.data;
    res->body_len = b.len;
    res->truncated = b.truncated;
    free(r);
    /* keep the connection only if the response ended cleanly */
    if (server_close || !complete || b.truncated) http_close_idle();
    return FO_OK;

fail:
    free(r);
    http_close_idle();
    return FO_FAIL;
}

bool http_fetch(const char *url, const char *post_body, const char *accept, HttpResult *res,
                const HttpHooks *hooks) {
    memset(res, 0, sizeof(*res));
    if (netio_state() != NET_UP) {
        snprintf(res->error, sizeof(res->error), "%s",
                 netio_state() == NET_STARTING ? "Still connecting to the network. Try again in a moment."
                                               : "The network isn't connected. Check the network adapter.");
        return false;
    }
    static char cur[URL_MAX], next[URL_MAX], location[URL_MAX];
    snprintf(cur, sizeof(cur), "%s", url);
    const char *post = post_body;
    for (int hop = 0; hop < 8; hop++) {
        location[0] = 0;
        http_result_free(res);
        memset(res, 0, sizeof(*res));
        snprintf(res->url, sizeof(res->url), "%s", cur);
        int r = fetch_once(cur, post, accept, res, hooks, location, sizeof(location));
        if (r == FO_RETRY) {
            memset(res, 0, sizeof(*res));
            snprintf(res->url, sizeof(res->url), "%s", cur);
            r = fetch_once(cur, post, accept, res, hooks, location, sizeof(location));
        }
        if (r != FO_OK) return false;
        if (res->download) return true;
        bool redirect = res->status >= 300 && res->status < 400 && location[0];
        if (!redirect) return true;
        url_resolve(cur, location, next, sizeof(next));
        snprintf(cur, sizeof(cur), "%s", next);
        if (res->status == 303 || ((res->status == 301 || res->status == 302) && post)) post = NULL;
        stage(hooks, "Following redirect...");
    }
    http_result_free(res);
    snprintf(res->error, sizeof(res->error), "Too many redirects.");
    return false;
}

void http_result_free(HttpResult *res) {
    free(res->body);
    res->body = NULL;
    res->body_len = 0;
}

/* ================================================================ streams */

void http_disposition_filename(const char *v, char *out, size_t outlen) {
    out[0] = 0;
    const char *p = NULL;
    for (const char *q = v; *q; q++)
        if (!strncasecmp(q, "filename*=", 10)) { p = q + 10; break; }
    if (p) {   /* filename*=UTF-8''name%20here */
        const char *t = strstr(p, "''");
        p = t ? t + 2 : p;
    } else {
        for (const char *q = v; *q; q++)
            if (!strncasecmp(q, "filename=", 9)) { p = q + 9; break; }
    }
    if (!p) return;
    while (*p == ' ') p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') quote = *p++;
    size_t o = 0;
    while (*p && o + 1 < outlen) {
        if (quote ? *p == quote : (*p == ';' || *p == ' ')) break;
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            p += 3;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

struct HttpStream {
    Conn conn;
    Reader rd;
    bool chunked, done;
    long long chunk_left;     /* bytes left in the current chunk */
    long long body_left;      /* for Content-Length bodies, -1 otherwise */
};

HttpStream *http_stream_open(const char *url, long long range_from, const HttpHooks *hooks, HttpStreamInfo *info,
                             char *err, size_t errlen) {
    memset(info, 0, sizeof(*info));
    err[0] = 0;
    if (netio_state() != NET_UP) { snprintf(err, errlen, "The network isn't connected."); return NULL; }
    HttpStream *st = calloc(1, sizeof(HttpStream));
    if (!st) { snprintf(err, errlen, "Out of memory."); return NULL; }
    snprintf(info->url, sizeof(info->url), "%s", url);
    for (int hop = 0; hop < 8; hop++) {
        Url u;
        if (!url_parse(info->url, &u) || !strcmp(u.scheme, "about")) { snprintf(err, errlen, "That address can't be downloaded."); break; }
        bool tls = !strcmp(u.scheme, "https");
        if (!conn_open(&st->conn, u.host, u.port, tls, hooks ? hooks->cancel : NULL, hooks ? hooks->stage : NULL, err, errlen))
            break;
        static char req[URL_MAX + 8192], extra[6144];
        extra_headers(extra, sizeof(extra), &u, tls, NULL, hooks);
        bool default_port = (tls && u.port == 443) || (!tls && u.port == 80);
        char hosthdr[300], range[64] = "";
        if (default_port) snprintf(hosthdr, sizeof(hosthdr), "%s", u.host);
        else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
        if (range_from >= 0) snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", range_from);   /* also shows if parts are supported */
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
                         "Accept-Encoding: identity\r\n%s%sConnection: close\r\n\r\n",
                         u.path, hosthdr, user_agent_strings[g_set.user_agent], range, extra);
        if (n >= (int)sizeof(req) || !conn_write_all(&st->conn, req, (size_t)n)) {
            snprintf(err, errlen, "Couldn't send the request.");
            conn_close(&st->conn);
            break;
        }
        memset(&st->rd, 0, sizeof(st->rd));
        st->rd.c = &st->conn;
        char line[1024], location[URL_MAX] = "";
        long long clen = -1, rtotal = -1;
        st->chunked = false;
        int status = 0;
        for (;;) {
            if (rd_line(&st->rd, line, sizeof(line)) < 0 || strncmp(line, "HTTP/", 5)) { status = -1; break; }
            const char *sp = strchr(line, ' ');
            status = sp ? atoi(sp + 1) : 0;
            if (status >= 200 || status < 100) break;
            while (rd_line(&st->rd, line, sizeof(line)) > 0) {}
        }
        if (status < 0) { snprintf(err, errlen, "The server sent a response I couldn't understand."); conn_close(&st->conn); break; }
        for (int i = 0; i < 200; i++) {
            int l = rd_line(&st->rd, line, sizeof(line));
            if (l <= 0) break;
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;
            if (!strcasecmp(line, "content-length")) clen = atoll(val);
            else if (!strcasecmp(line, "transfer-encoding") && strstr(val, "chunked")) st->chunked = true;
            else if (!strcasecmp(line, "content-type")) snprintf(info->content_type, sizeof(info->content_type), "%s", val);
            else if (!strcasecmp(line, "location")) snprintf(location, sizeof(location), "%s", val);
            else if (!strcasecmp(line, "content-disposition")) http_disposition_filename(val, info->filename, sizeof(info->filename));
            else if (!strcasecmp(line, "set-cookie") && !(hooks && hooks->no_cookies)) cookies_receive(u.host, u.path, tls, val, netio_now());
            else if (!strcasecmp(line, "content-range")) {   /* bytes 100-999/1000 */
                const char *slash = strrchr(val, '/');
                if (slash && slash[1] != '*') rtotal = atoll(slash + 1);
            }
        }
        if (status >= 300 && status < 400 && location[0]) {
            char next[URL_MAX];
            url_resolve(info->url, location, next, sizeof(next));
            snprintf(info->url, sizeof(info->url), "%s", next);
            conn_close(&st->conn);
            continue;
        }
        info->status = status;
        if (status == 416) {   /* the partial file is already complete */
            info->ranged = true;
            info->length = 0;
            info->total = range_from;
            st->done = true;
            return st;
        }
        if (status >= 400) {
            snprintf(err, errlen, "The server refused the download (error %d).", status);
            conn_close(&st->conn);
            break;
        }
        info->ranged = (status == 206);
        info->length = st->chunked ? -1 : clen;
        if (info->ranged) info->total = rtotal >= 0 ? rtotal : (clen >= 0 ? range_from + clen : -1);
        else info->total = st->chunked ? -1 : clen;
        st->body_left = st->chunked ? -1 : clen;
        return st;
    }
    if (!err[0]) snprintf(err, errlen, "Too many redirects.");
    free(st);
    return NULL;
}

int http_stream_read(HttpStream *st, void *buf, size_t len) {
    if (st->done) return 0;
    if (st->chunked) {
        if (st->chunk_left == 0) {
            char line[64];
            if (rd_line(&st->rd, line, sizeof(line)) < 0) return -1;
            if (!line[0] && rd_line(&st->rd, line, sizeof(line)) < 0) return -1;   /* CRLF after a chunk */
            st->chunk_left = strtoll(line, NULL, 16);
            if (st->chunk_left <= 0) { st->done = true; return 0; }
        }
        if ((long long)len > st->chunk_left) len = (size_t)st->chunk_left;
        int n = rd_some(&st->rd, buf, len);
        if (n <= 0) return -1;   /* the connection ended mid-chunk */
        st->chunk_left -= n;
        return n;
    }
    if (st->body_left == 0) { st->done = true; return 0; }
    if (st->body_left > 0 && (long long)len > st->body_left) len = (size_t)st->body_left;
    int n = rd_some(&st->rd, buf, len);
    if (n < 0) return -1;
    if (n == 0) { st->done = true; return st->body_left > 0 ? -1 : 0; }   /* closed early? */
    if (st->body_left > 0) st->body_left -= n;
    return n;
}

void http_stream_close(HttpStream *st) {
    if (!st) return;
    conn_close(&st->conn);
    free(st);
}

/* ============================================================ block fetch */

struct HttpKeep {
    Conn conn;
    Reader rd;
    bool open;
    char key[300];
};

HttpKeep *http_keep_new(void) { return calloc(1, sizeof(HttpKeep)); }

void http_keep_free(HttpKeep *k) {
    if (!k) return;
    if (k->open) conn_close(&k->conn);
    free(k);
}

static void keep_close(HttpKeep *k) {
    if (k->open) conn_close(&k->conn);
    k->open = false;
}

long long http_range_get(HttpKeep *k, const char *url, long long first, long long last, HttpSink sink, void *ctx,
                         const HttpHooks *hooks, char *err, size_t errlen) {
    err[0] = 0;
    Url u;
    if (!url_parse(url, &u) || strcmp(u.scheme, "about") == 0) { snprintf(err, errlen, "Bad address."); return -1; }
    bool tls = strcmp(u.scheme, "https") == 0;
    char key[300];
    snprintf(key, sizeof(key), "%s:%u:%d", u.host, u.port, tls ? 1 : 0);
    if (k->open && strcmp(key, k->key)) keep_close(k);
    long long want = last - first + 1;
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = k->open;
        if (!k->open) {
            if (!conn_open(&k->conn, u.host, u.port, tls, hooks ? hooks->cancel : NULL, NULL, err, errlen)) return -1;
            k->open = true;
            snprintf(k->key, sizeof(k->key), "%s", key);
        }
        k->conn.cancel = hooks ? hooks->cancel : NULL;
        char req[URL_MAX + 8192], extra[6144];   /* on this thread's stack (download threads have 128 KB) */
        extra_headers(extra, sizeof(extra), &u, tls, NULL, hooks);
        bool def = (tls && u.port == 443) || (!tls && u.port == 80);
        char hosthdr[300];
        if (def) snprintf(hosthdr, sizeof(hosthdr), "%s", u.host);
        else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
                         "Range: bytes=%lld-%lld\r\n%sConnection: keep-alive\r\n\r\n",
                         u.path, hosthdr, user_agent_strings[g_set.user_agent], first, last, extra);
        if (n >= (int)sizeof(req)) { snprintf(err, errlen, "Address too long."); return -1; }
        memset(&k->rd, 0, sizeof(k->rd));
        k->rd.c = &k->conn;
        if (!conn_write_all(&k->conn, req, (size_t)n)) {
            keep_close(k);
            if (reused) continue;   /* the server had closed the idle connection: reconnect once */
            snprintf(err, errlen, "Couldn't send the request.");
            return -1;
        }
        char line[1024];
        int status = 0;
        if (rd_line(&k->rd, line, sizeof(line)) < 0 || strncmp(line, "HTTP/", 5)) {
            keep_close(k);
            if (reused) continue;
            snprintf(err, errlen, "No response from the server.");
            return -1;
        }
        const char *sp = strchr(line, ' ');
        status = sp ? atoi(sp + 1) : 0;
        long long clen = -1;
        bool close_after = !strncmp(line, "HTTP/1.0", 8), chunked = false;
        for (int i = 0; i < 200; i++) {
            int l = rd_line(&k->rd, line, sizeof(line));
            if (l < 0) { keep_close(k); snprintf(err, errlen, "Connection closed early."); return -1; }
            if (l == 0) break;
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;
            if (!strcasecmp(line, "content-length")) clen = atoll(val);
            else if (!strcasecmp(line, "connection") && contains_nocase(val, "close")) close_after = true;
            else if (!strcasecmp(line, "transfer-encoding") && strstr(val, "chunked")) chunked = true;
            else if (!strcasecmp(line, "set-cookie") && !(hooks && hooks->no_cookies)) cookies_receive(u.host, u.path, tls, val, netio_now());
        }
        if (status != 206 || chunked || clen != want) {
            keep_close(k);
            snprintf(err, errlen, status == 206 ? "Unexpected response size." : "The server stopped allowing partial downloads (error %d).", status);
            /* busy / too many connections: worth trying again later */
            bool retry = status == 206 || status == 408 || status == 429 || status >= 500;
            return retry ? -1 : -2;
        }
        long long have = 0;
        unsigned char piece[8192];   /* on this thread's stack */
        while (have < want) {
            if (hooks && hooks->cancel && *hooks->cancel) { keep_close(k); snprintf(err, errlen, "Stopped."); return -1; }
            long long chunk = want - have;
            if (chunk > (long long)sizeof(piece)) chunk = (long long)sizeof(piece);
            int r = rd_some(&k->rd, (char *)piece, (size_t)chunk);
            if (r <= 0) { keep_close(k); snprintf(err, errlen, "Connection lost."); return -1; }
            if (!sink(ctx, piece, (size_t)r)) { keep_close(k); snprintf(err, errlen, "Stopped."); return -1; }
            have += r;
        }
        if (close_after || k->rd.pos < k->rd.len) keep_close(k);   /* leftover bytes would be out of step */
        return want;
    }
    snprintf(err, errlen, "Couldn't reconnect.");
    return -1;
}
