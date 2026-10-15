/* http.c - HTTP/1.1 GET/POST with redirects, chunked decoding, size limits
 * and keep-alive: the last connection stays open so the next request to the
 * same site (e.g. a run of images) skips the slow TLS handshake. */
#include "http.h"
#include "conn.h"
#include "netio.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEFAULT_ACCEPT "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5"

typedef struct {
    Conn *c;
    char buf[4096];
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

    static char req[URL_MAX + 1024];
    bool default_port = (tls && u.port == 443) || (!tls && u.port == 80);
    char hosthdr[300];
    if (default_port) snprintf(hosthdr, sizeof(hosthdr), "%s", u.host);
    else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: %s\r\n"
                     "Accept-Language: en\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n",
                     post ? "POST" : "GET", u.path, hosthdr, user_agent_strings[g_set.user_agent],
                     accept ? accept : DEFAULT_ACCEPT);
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
