/* http.c - HTTP/1.1 GET/POST with redirects, chunked decoding and size limits. */
#include "http.h"
#include "conn.h"
#include "netio.h"
#include "settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    Conn *c;
    char buf[4096];
    size_t pos, len;
    bool eof, err;
} Reader;

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

static bool fetch_once(const char *url, const char *post, HttpResult *res, const HttpHooks *hooks,
                       char *location, size_t loclen) {
    Url u;
    if (!url_parse(url, &u) || strcmp(u.scheme, "about") == 0) {
        snprintf(res->error, sizeof(res->error), "That address isn't a web page I can open.");
        return false;
    }
    bool tls = strcmp(u.scheme, "https") == 0;
    Conn c;
    if (!conn_open(&c, u.host, u.port, tls, hooks ? hooks->cancel : NULL, hooks ? hooks->stage : NULL,
                   res->error, sizeof(res->error)))
        return false;
    res->secure = tls;
    res->verified = c.verified;

    char req[2048];
    bool default_port = (tls && u.port == 443) || (!tls && u.port == 80);
    char hosthdr[300];
    if (default_port) snprintf(hosthdr, sizeof(hosthdr), "%s", u.host);
    else snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
                     "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.5\r\n"
                     "Accept-Language: en\r\nAccept-Encoding: identity\r\nConnection: close\r\n",
                     post ? "POST" : "GET", u.path, hosthdr, user_agent_strings[g_set.user_agent]);
    if (post)
        n += snprintf(req + n, sizeof(req) - n,
                      "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n",
                      (unsigned)strlen(post));
    n += snprintf(req + n, sizeof(req) - n, "\r\n");
    stage(hooks, "Requesting page...");
    if (n >= (int)sizeof(req) || !conn_write_all(&c, req, (size_t)n) || (post && !conn_write_all(&c, post, strlen(post)))) {
        snprintf(res->error, sizeof(res->error), "Couldn't send the request.");
        conn_close(&c);
        return false;
    }

    Reader *r = calloc(1, sizeof(Reader));
    if (!r) { conn_close(&c); snprintf(res->error, sizeof(res->error), "Out of memory."); return false; }
    r->c = &c;
    char line[1024];
    long content_length = -1;
    bool chunked = false;

    /* status line, skipping any 1xx interim responses */
    for (;;) {
        if (rd_line(r, line, sizeof(line)) < 0 || strncmp(line, "HTTP/", 5) != 0) {
            snprintf(res->error, sizeof(res->error), "The site sent a response I couldn't understand.");
            goto fail;
        }
        const char *sp = strchr(line, ' ');
        res->status = sp ? atoi(sp + 1) : 0;
        if (res->status >= 200 || res->status < 100) break;
        while (rd_line(r, line, sizeof(line)) > 0) {}
    }
    stage(hooks, "Receiving page...");
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
    }

    Body b = { 0 };
    b.limit = (size_t)g_set.max_page_kb * 1024;
    bool no_body = (res->status == 204 || res->status == 304 || (res->status >= 300 && res->status < 400 && location[0]));
    if (!no_body) {
        if (chunked) {
            for (;;) {
                if (rd_line(r, line, sizeof(line)) < 0) break;
                long sz = strtol(line, NULL, 16);
                if (sz <= 0) break;
                if (!read_exact(r, &b, (size_t)sz, hooks)) break;
                rd_line(r, line, sizeof(line));
            }
        } else if (content_length >= 0) {
            read_exact(r, &b, (size_t)content_length, hooks);
        } else {
            char tmp[4096];
            for (;;) {
                int got = rd_some(r, tmp, sizeof(tmp));
                if (got <= 0 || !body_append(&b, tmp, (size_t)got)) break;
                if (hooks && hooks->progress) *hooks->progress = (int)b.len;
                if (hooks && hooks->cancel && *hooks->cancel) break;
            }
        }
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
    conn_close(&c);
    return true;

fail:
    free(r);
    conn_close(&c);
    return false;
}

bool http_fetch(const char *url, const char *post_body, HttpResult *res, const HttpHooks *hooks) {
    memset(res, 0, sizeof(*res));
    if (netio_state() != NET_UP) {
        snprintf(res->error, sizeof(res->error), "%s",
                 netio_state() == NET_STARTING ? "Still connecting to the network. Try again in a moment."
                                               : "The network isn't connected. Check the Broadband Adapter.");
        return false;
    }
    char cur[URL_MAX];
    snprintf(cur, sizeof(cur), "%s", url);
    const char *post = post_body;
    for (int hop = 0; hop < 8; hop++) {
        char location[URL_MAX] = "";
        http_result_free(res);
        memset(res, 0, sizeof(*res));
        snprintf(res->url, sizeof(res->url), "%s", cur);
        if (!fetch_once(cur, post, res, hooks, location, sizeof(location))) return false;
        bool redirect = res->status >= 300 && res->status < 400 && location[0];
        if (!redirect) return true;
        char next[URL_MAX];
        url_resolve(cur, location, next, sizeof(next));
        snprintf(cur, sizeof(cur), "%s", next);
        if (res->status == 303 || ((res->status == 301 || res->status == 302) && post)) post = NULL;
        stage(hooks, "Following redirect...");
    }
    snprintf(res->error, sizeof(res->error), "Too many redirects.");
    return false;
}

void http_result_free(HttpResult *res) {
    free(res->body);
    res->body = NULL;
    res->body_len = 0;
}
