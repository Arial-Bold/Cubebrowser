/* adblock.c - see adblock.h. */
#include "adblock.h"
#include "settings.h"
#include "storage.h"
#include "url.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Well-known ad networks, trackers and domain-parking routers (the pages that
 * sell a visit to an advertiser). Kept short: the list on the card adds more. */
static const char *const builtin[] = {
    /* ad networks */
    "doubleclick.net", "googlesyndication.com", "googleadservices.com", "googletagservices.com", "adservice.google.com",
    "adnxs.com", "adsrvr.org", "amazon-adsystem.com", "criteo.com", "criteo.net", "taboola.com", "outbrain.com",
    "pubmatic.com", "rubiconproject.com", "openx.net", "casalemedia.com", "indexww.com", "smartadserver.com",
    "adform.net", "yieldmo.com", "sharethrough.com", "teads.tv", "media.net", "contextweb.com", "lijit.com",
    "33across.com", "sovrn.com", "gumgum.com", "bidswitch.net", "onetag-sys.com", "3lift.com", "adroll.com",
    "advertising.com", "serving-sys.com", "flashtalking.com", "innovid.com", "spotxchange.com", "springserve.com",
    "undertone.com", "admixer.net", "mgid.com", "revcontent.com", "adskeeper.com", "zedo.com", "adcash.com",
    "popads.net", "popcash.net", "propellerads.com", "propellerclick.com", "onclickads.net", "exoclick.com",
    "exosrv.com", "juicyads.com", "trafficjunky.com", "trafficstars.com", "adsterra.com", "hilltopads.net",
    "clickadu.com", "zeropark.com", "a-ads.com", "coinzilla.com", "bitmedia.io", "adsafeprotected.com",
    "doubleverify.com", "moatads.com", "ads.yahoo.com", "ads.linkedin.com", "ads-twitter.com", "ads.pinterest.com",
    "adsystem.com", "adtech.com", "adition.com", "smartclip.net", "yieldlab.net", "rtbhouse.com", "ad.gt",
    /* trackers */
    "google-analytics.com", "googletagmanager.com", "scorecardresearch.com", "quantserve.com", "quantcount.com",
    "chartbeat.com", "chartbeat.net", "hotjar.com", "clarity.ms", "bat.bing.com", "connect.facebook.net",
    "analytics.tiktok.com", "cdn.segment.com", "api.segment.io", "statcounter.com", "histats.com",
    "cloudflareinsights.com", "nr-data.net", "mouseflow.com", "crazyegg.com", "fullstory.com", "newrelic.com",
    /* domain parking and traffic routers */
    "parklogic.com", "bodis.com", "sedoparking.com", "parkingcrew.net", "above.com", "dsnextgen.com",
    "voluumtrk.com", "trackingclick.net", "parked.com", "parkingpage.namecheap.com", "domainmarket.com",
    "playfunpartner.com",
};

static char **list;      /* sorted, lower case */
static int nlist;
#define ALLOW_MAX 16
static char allowed[ALLOW_MAX][128];
static volatile int nallowed;

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add(char ***arr, int *n, int *cap, const char *s, size_t len) {
    while (len && (s[len - 1] == '.' || isspace((unsigned char)s[len - 1]))) len--;
    while (len && *s == '.') { s++; len--; }
    if (!len || len > 120 || memchr(s, '/', len)) return;
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 256;
        char **na = realloc(*arr, sizeof(char *) * (size_t)nc);
        if (!na) return;
        *arr = na;
        *cap = nc;
    }
    char *c = malloc(len + 1);
    if (!c) return;
    for (size_t i = 0; i < len; i++) c[i] = (char)tolower((unsigned char)s[i]);
    c[len] = 0;
    (*arr)[(*n)++] = c;
}

void adblock_init(void) {
    char **arr = NULL;
    int n = 0, cap = 0;
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) add(&arr, &n, &cap, builtin[i], strlen(builtin[i]));
    char path[400];
    storage_path(path, sizeof(path), "blocklist.txt");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *h = strchr(line, '#');
            if (h) *h = 0;
            char *p = line;
            while (isspace((unsigned char)*p)) p++;
            /* a hosts file: "0.0.0.0 name" or "127.0.0.1 name" */
            if (!strncmp(p, "0.0.0.0", 7) || !strncmp(p, "127.0.0.1", 9) || !strncmp(p, "::", 2)) {
                while (*p && !isspace((unsigned char)*p)) p++;
                while (isspace((unsigned char)*p)) p++;
            }
            /* ||ads.example.com^ (the usual ad-block filter form for a whole site) */
            if (p[0] == '|' && p[1] == '|') p += 2;
            size_t len = 0;
            while (p[len] && !isspace((unsigned char)p[len]) && p[len] != '^') len++;
            if (len && strcmp(p, "localhost") != 0) add(&arr, &n, &cap, p, len);
        }
        fclose(f);
    }
    if (n) qsort(arr, (size_t)n, sizeof(char *), cmp);
    /* drop repeats */
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k && !strcmp(arr[k - 1], arr[i])) { free(arr[i]); continue; }
        arr[k++] = arr[i];
    }
    list = arr;
    nlist = k;
}

int adblock_count(void) { return nlist; }

static bool listed(const char *host) {
    for (const char *h = host; h && *h;) {
        const char *key = h;
        if (bsearch(&key, list, (size_t)nlist, sizeof(char *), cmp)) return true;
        h = strchr(h, '.');
        if (h) h++;
    }
    return false;
}

static bool host_of(const char *url, char *out, size_t cap) {
    Url u;
    if (!url_parse(url, &u)) return false;
    snprintf(out, cap, "%s", u.host);
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
    return out[0] != 0;
}

bool adblock_blocked(const char *url) {
    if (!g_set.block_ads || !nlist || !url) return false;
    char host[256];
    if (!host_of(url, host, sizeof(host))) return false;
    char pic[URL_MAX];
    if (!strcmp(host, "images.weserv.nl")) {   /* the image helper: judge the picture's own address */
        const char *q = strstr(url, "url=");
        if (!q) return false;
        char dec[URL_MAX];
        size_t n = 0;
        for (const char *s = q + 4; *s && *s != '&' && n + 1 < sizeof(dec); s++) {
            if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
                char hx[3] = { s[1], s[2], 0 };
                dec[n++] = (char)strtol(hx, NULL, 16);
                s += 2;
            } else dec[n++] = *s;
        }
        dec[n] = 0;
        if (!strncasecmp(dec, "http://", 7) || !strncasecmp(dec, "https://", 8)) snprintf(pic, sizeof(pic), "%s", dec);
        else snprintf(pic, sizeof(pic), "https://%s", dec[0] == '/' && dec[1] == '/' ? dec + 2 : dec);
        url = pic;
        if (!host_of(url, host, sizeof(host))) return false;
    }
    char site[256];
    adblock_site_of(url, site, sizeof(site));
    for (int i = 0; i < nallowed; i++)
        if (!strcmp(allowed[i], host) || !strcmp(allowed[i], site)) return false;
    return listed(host);
}

void adblock_allow(const char *url) {
    char host[256];
    if (!host_of(url, host, sizeof(host))) return;
    for (int i = 0; i < nallowed; i++) if (!strcmp(allowed[i], host)) return;
    int i = nallowed < ALLOW_MAX ? nallowed : ALLOW_MAX - 1;
    snprintf(allowed[i], sizeof(allowed[i]), "%s", host);
    if (nallowed < ALLOW_MAX) nallowed++;
}

void adblock_site_of(const char *url, char *out, size_t cap) {
    char host[256];
    out[0] = 0;
    if (!host_of(url, host, sizeof(host))) return;
    /* the last two parts of the name, or three for names like example.co.uk */
    int dots = 0;
    const char *p = host + strlen(host);
    const char *cut = host;
    int want = 2;
    const char *last = strrchr(host, '.');
    if (last && strlen(last + 1) == 2) {   /* a country ending: is the part before it a co/com/org...? */
        const char *prev = last;
        while (prev > host && prev[-1] != '.') prev--;
        size_t pl = (size_t)(last - prev);
        if (pl <= 3 && prev > host &&
            (!strncmp(prev, "co", pl) || !strncmp(prev, "com", pl) || !strncmp(prev, "net", pl) || !strncmp(prev, "org", pl) ||
             !strncmp(prev, "gov", pl) || !strncmp(prev, "ac", pl) || !strncmp(prev, "edu", pl) || !strncmp(prev, "ne", pl) ||
             !strncmp(prev, "or", pl)))
            want = 3;
    }
    while (p > host) {
        p--;
        if (*p == '.' && ++dots == want) { cut = p + 1; break; }
    }
    snprintf(out, cap, "%s", cut);
}
