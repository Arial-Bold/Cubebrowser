/* url.h - URL parsing, relative resolution and address-bar input handling. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define URL_MAX 2048

typedef struct {
    char scheme[8];      /* "http", "https", "about" */
    char host[256];
    unsigned short port;
    char path[URL_MAX];  /* path + query, always starts with '/' for http(s) */
} Url;

bool url_parse(const char *s, Url *u);
void url_resolve(const char *base, const char *rel, char *out, size_t outlen);
void url_encode_component(const char *in, char *out, size_t outlen);
void url_from_input(const char *input, char *out, size_t outlen);  /* address or search */
void url_escape_path(char *path, size_t cap);
/* Search engines send result links through a tracking page that forwards
 * with script (e.g. duckduckgo.com/l/?uddg=<address>). If url is one, the
 * real address is written to out and true returned. */
bool url_unwrap_redirect(const char *url, char *out, size_t outlen);
