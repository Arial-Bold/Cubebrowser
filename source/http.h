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
    char error[256];           /* set when the fetch failed */
} HttpResult;

typedef struct {
    volatile int *cancel;
    volatile int *progress;            /* bytes received so far */
    void (*stage)(const char *msg);
} HttpHooks;

/* post_body may be NULL (GET); accept may be NULL for the default page types.
 * Connections are kept open and reused for the next request to the same site.
 * Returns true if a response was received. */
bool http_fetch(const char *url, const char *post_body, const char *accept, HttpResult *res,
                const HttpHooks *hooks);
void http_close_idle(void);
void http_result_free(HttpResult *res);
