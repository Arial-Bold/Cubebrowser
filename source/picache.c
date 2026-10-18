/* picache.c - see picache.h.
 *
 * Layout: <data folder>/webcache/<0-f>/<16 hex digits>.pic, the name being a
 * hash of the picture's address (16 sub-folders keep each folder small, which
 * keeps FAT lookups quick). A file holds:
 *     "CBPC" | address length (4 bytes, big-endian) | address | picture
 * The address is checked on reading, so two addresses with the same hash
 * can't mix up pictures.
 *
 * <data folder>/webcache/size holds the running total, so the cache doesn't
 * have to be measured at every start. Only when the total passes the limit is
 * the whole cache listed and the oldest files removed (down to 80%). */
#include "picache.h"
#include "settings.h"
#include "storage.h"
#include "thr.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAGIC "CBPC"
#define MAX_PICTURE (1024 * 1024)        /* bigger downloads aren't kept */
#define MAX_AGE (7L * 24 * 60 * 60)      /* a week: then fetched again */
#define CLOCK_SANE 1600000000L           /* the clock has been set (after 2020) */
#define MAX_LIST 16384

static const unsigned long long limits_mb[] = { 0, 25, 50, 100, 250 };

static thr_mutex lock;
static bool lock_ok, lock_tried;
static bool total_known;
static unsigned long long total;       /* bytes in the cache */

static void take(void) {
    if (!lock_tried) { lock_tried = true; lock_ok = thr_mutex_init(&lock); }
    if (lock_ok) thr_lock(&lock);
}
static void give(void) { if (lock_ok) thr_unlock(&lock); }

static unsigned long long limit_bytes(void) {
    int i = g_set.pic_cache;
    if (i < 0 || i > 4) i = 3;
    return limits_mb[i] * 1024ULL * 1024ULL;
}

bool picache_enabled(void) { return storage_ok() && limit_bytes() > 0; }

static uint64_t hash64(const char *s) {   /* FNV-1a */
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static void cache_dir(char *out, size_t cap) { storage_path(out, cap, PICACHE_DIR); }

static void file_for(const char *url, char *out, size_t cap, char *sub, size_t subcap) {
    char dir[400];
    cache_dir(dir, sizeof(dir));
    uint64_t h = hash64(url);
    char hex[17];
    snprintf(hex, sizeof(hex), "%08lx%08lx", (unsigned long)(h >> 32), (unsigned long)(h & 0xffffffffUL));
    snprintf(sub, subcap, "%s/%c", dir, hex[0]);
    snprintf(out, cap, "%s/%s.pic", sub, hex);
}

/* ------------------------------------------------------------ the total */

static void save_total(void) {
    char dir[400], path[420];
    cache_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/size", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%llu\n", total);
    fclose(f);
}

static void load_total(void) {
    if (total_known) return;
    total_known = true;
    total = 0;
    char dir[400], path[420];
    cache_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/size", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned long long v;
    if (fscanf(f, "%llu", &v) == 1) total = v;
    fclose(f);
}

/* ------------------------------------------------------------ making room */

typedef struct { time_t when; unsigned long size; char name[24]; char sub; } Entry;

static int by_age(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    return (x->when > y->when) - (x->when < y->when);
}

/* Lists every saved picture, sets the true total, and removes the oldest
 * until it's under `target` bytes. Returns the size it measured first. */
static unsigned long long trim_to(unsigned long long target) {
    Entry *list = malloc(sizeof(Entry) * MAX_LIST);
    if (!list) return 0;
    int n = 0;
    unsigned long long sum = 0;
    char dir[400];
    cache_dir(dir, sizeof(dir));
    for (int s = 0; s < 16; s++) {
        char sub[420];
        snprintf(sub, sizeof(sub), "%s/%x", dir, s);
        DIR *d = opendir(sub);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            if (l < 5 || l >= sizeof(list[0].name) || strcmp(e->d_name + l - 4, ".pic")) continue;
            char path[460];
            snprintf(path, sizeof(path), "%s/%s", sub, e->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            sum += (unsigned long long)st.st_size;
            if (n < MAX_LIST) {
                list[n].when = st.st_mtime;
                list[n].size = (unsigned long)st.st_size;
                list[n].sub = (char)s;
                snprintf(list[n].name, sizeof(list[n].name), "%s", e->d_name);
                n++;
            }
        }
        closedir(d);
    }
    unsigned long long measured = sum;
    qsort(list, (size_t)n, sizeof(Entry), by_age);
    for (int i = 0; i < n && sum > target; i++) {
        char path[460];
        snprintf(path, sizeof(path), "%s/%x/%s", dir, list[i].sub, list[i].name);
        if (remove(path) == 0) sum -= list[i].size;
    }
    free(list);
    total = sum;
    total_known = true;
    save_total();
    return measured;
}

/* ------------------------------------------------------------ get / put */

void *picache_get(const char *url, size_t *len) {
    if (!picache_enabled() || !url || !url[0]) return NULL;
    char path[460], sub[420];
    file_for(url, path, sizeof(path), sub, sizeof(sub));
    take();
    void *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { give(); return NULL; }
    struct stat st;
    long now = (long)time(NULL);
    bool stale = fstat(fileno(f), &st) == 0 && now > CLOCK_SANE && (long)st.st_mtime > CLOCK_SANE &&
                 now - (long)st.st_mtime > MAX_AGE;
    unsigned char head[8];
    size_t ul = strlen(url);
    if (!stale && fread(head, 1, 8, f) == 8 && !memcmp(head, MAGIC, 4)) {
        size_t sl = ((size_t)head[4] << 24) | ((size_t)head[5] << 16) | ((size_t)head[6] << 8) | head[7];
        char *saved = sl == ul ? malloc(sl + 1) : NULL;
        if (saved && fread(saved, 1, sl, f) == sl && !memcmp(saved, url, sl)) {
            long start = ftell(f);
            fseek(f, 0, SEEK_END);
            long end = ftell(f);
            if (start > 0 && end > start && end - start <= MAX_PICTURE) {
                fseek(f, start, SEEK_SET);
                size_t n = (size_t)(end - start);
                out = malloc(n);
                if (out && fread(out, 1, n, f) == n) *len = n;
                else { free(out); out = NULL; }
            }
        }
        free(saved);
    }
    fclose(f);
    if (stale) remove(path);   /* fetched again and saved fresh */
    give();
    return out;
}

void picache_put(const char *url, const void *data, size_t len) {
    if (!picache_enabled() || !url || !url[0] || !data || !len || len > MAX_PICTURE) return;
    size_t ul = strlen(url);
    if (ul > 4000) return;
    char path[460], sub[420], dir[400], tmp[470];
    file_for(url, path, sizeof(path), sub, sizeof(sub));
    take();
    cache_dir(dir, sizeof(dir));
    mkdir(dir, 0777);
    mkdir(sub, 0777);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { give(); return; }
    unsigned char head[8] = { 'C', 'B', 'P', 'C', (unsigned char)(ul >> 24), (unsigned char)(ul >> 16),
                              (unsigned char)(ul >> 8), (unsigned char)ul };
    bool ok = fwrite(head, 1, 8, f) == 8 && fwrite(url, 1, ul, f) == ul && fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); give(); return; }
    load_total();
    struct stat st;
    if (stat(path, &st) == 0) {   /* replacing an older copy */
        unsigned long long old = (unsigned long long)st.st_size;
        total = total > old ? total - old : 0;
        remove(path);
    }
    if (rename(tmp, path) != 0) { remove(tmp); give(); return; }
    total += 8 + ul + len;
    unsigned long long lim = limit_bytes();
    if (total > lim) trim_to(lim / 10 * 8);
    else save_total();
    give();
}

unsigned long long picache_clear(void) {
    if (!storage_ok()) return 0;
    take();
    unsigned long long freed = trim_to(0);
    /* trim_to(0) measured and removed everything it could see; tidy up the
     * (now empty) folders and the total */
    char dir[400];
    cache_dir(dir, sizeof(dir));
    for (int s = 0; s < 16; s++) {
        char sub[420];
        snprintf(sub, sizeof(sub), "%s/%x", dir, s);
        /* leftovers from an interrupted save */
        DIR *d = opendir(sub);
        if (d) {
            struct dirent *e;
            char names[32][32];
            int k = 0;
            while ((e = readdir(d)) != NULL && k < 32)
                if (e->d_name[0] != '.' && strlen(e->d_name) < 32) snprintf(names[k++], 32, "%s", e->d_name);
            closedir(d);
            for (int i = 0; i < k; i++) {
                char p[460];
                snprintf(p, sizeof(p), "%s/%s", sub, names[i]);
                remove(p);
            }
        }
        rmdir(sub);
    }
    /* and the folder itself, so nothing is left if the data folder moves */
    char path[420];
    snprintf(path, sizeof(path), "%s/size", dir);
    remove(path);
    rmdir(dir);
    total = 0;
    total_known = true;
    give();
    return freed;
}
