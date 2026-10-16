/* http.h - HTTP/1.1 client over conn (plain or TLS), with redirects and
 * chunked transfer decoding. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "url.h"

typedef struct {
    char url[URL_MAX];         /* final URL after redirects */
    int status;
    char content_type[96];
    char *body;                /* malloc'd, NUL-terminated */
    size_t body_len;
    bool secure, verified, truncated;
    bool download;             /* not a web page: offer to save it instead */
    long long length;          /* Content-Length, or -1 */
    char filename[128];        /* from Content-Disposition, if given */
    char error[256];           /* set when the fetch failed */
} HttpResult;

typedef struct {
    volatile int *cancel;
    volatile int *progress;            /* bytes received so far */
    void (*stage)(const char *msg);
    /* optional: return false to stop before the body (e.g. it's a file download) */
    bool (*want_body)(const char *url, const char *content_type);
    bool no_cookies;            /* e.g. pictures sent through the image helper */
    const char *referer;        /* page this request comes from, or NULL */
} HttpHooks;

/* post_body may be NULL (GET); accept may be NULL for the default page types.
 * Connections are kept open and reused for the next request to the same site.
 * Returns true if a response was received. */
bool http_fetch(const char *url, const char *post_body, const char *accept, HttpResult *res,
                const HttpHooks *hooks);
void http_close_idle(void);

/* ---- streaming (downloads): the body is read piece by piece, never held whole ---- */

typedef struct {
    int status;
    long long length;        /* bytes in this response body, -1 if unknown */
    long long total;         /* size of the whole file if known (Content-Range or Content-Length) */
    bool ranged;             /* server honoured the Range request (206) */
    char content_type[96];
    char filename[128];
    char url[URL_MAX];       /* final URL after redirects */
} HttpStreamInfo;

typedef struct HttpStream HttpStream;

/* range_from > 0 asks the server to resume from that byte. */
HttpStream *http_stream_open(const char *url, long long range_from, const HttpHooks *hooks, HttpStreamInfo *info,
                             char *err, size_t errlen);
int  http_stream_read(HttpStream *st, void *buf, size_t len);   /* >0 bytes, 0 end, <0 error */
void http_stream_close(HttpStream *st);

/* ---- block downloads: many byte ranges over one kept-open connection ---- */

typedef struct HttpKeep HttpKeep;
HttpKeep *http_keep_new(void);
void http_keep_free(HttpKeep *k);
/* Receives one piece of a range response. Return false to refuse it (the
 * block was handed to another connection): the fetch then stops. */
typedef bool (*HttpSink)(void *ctx, const unsigned char *data, size_t len);

/* Fetches bytes first..last (inclusive) of url, passing them to sink as they
 * arrive. Reuses the connection between calls. Returns the number of bytes
 * (last-first+1), or -1 with err filled (worth retrying), or -2 when the
 * server won't serve the range at all (e.g. 404). */
long long http_range_get(HttpKeep *k, const char *url, long long first, long long last, HttpSink sink, void *ctx,
                         const HttpHooks *hooks, char *err, size_t errlen);

/* Filename from a Content-Disposition header value (empty if none). */
void http_disposition_filename(const char *value, char *out, size_t outlen);
void http_result_free(HttpResult *res);
