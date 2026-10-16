/* cookies.h - cookie storage so sites can remember you (logins, settings).
 * Follows the usual browser rules: domain and path matching, expiry,
 * Secure cookies only over HTTPS, and no cookies for whole public suffixes
 * like ".com" or ".co.uk". Optionally saved to sd:/gcbrowser/cookies.txt. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

void cookies_init(void);                 /* loads saved cookies if "keep logins" is on */
void cookies_save_if_needed(void);       /* writes the file if anything changed */
void cookies_clear(void);                /* forgets everything (and deletes the file) */
int  cookies_count(void);

/* Stores one Set-Cookie header received from host/path (secure = HTTPS). */
void cookies_receive(const char *host, const char *path, bool secure, const char *header, time_t now);

/* Builds the Cookie header value for a request (empty string if none). */
void cookies_for(const char *host, const char *path, bool secure, time_t now, char *out, size_t cap);

/* Parses an HTTP date (several historic formats). Returns 0 if unreadable. */
time_t cookies_parse_date(const char *s);
