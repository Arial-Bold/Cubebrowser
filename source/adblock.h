/* adblock.h - "Block ads": addresses of ad, tracking and domain-parking
 * services that CubeBrowser doesn't download from. A list is built in, and
 * gcbrowser/blocklist.txt on the SD card adds to it (one site per line, or a
 * hosts file: "0.0.0.0 ads.example.com"; # starts a comment). A site on the
 * list covers its subdomains too. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

void adblock_init(void);                  /* once at startup, after storage is up */
/* Whether to refuse this address (Block ads on, and not allowed this session).
 * Pictures sent through the image helper are judged by the picture's own
 * address. Safe to call from the loading thread. */
bool adblock_blocked(const char *url);
void adblock_allow(const char *url);      /* "Open it anyway": its site, until CubeBrowser restarts */
int  adblock_count(void);                 /* sites on the list */

/* The site an address belongs to, roughly (example.com, example.co.uk), for
 * telling whether a page sends you somewhere else. */
void adblock_site_of(const char *url, char *out, size_t cap);
