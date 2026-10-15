/* download.c - see download.h */
#include "download.h"
#include "http.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#define CHUNK (256 * 1024)

long long free_space(const char *path) {
    struct statvfs st;
    char root[16];
    const char *colon = strchr(path, ':');
    if (colon && (size_t)(colon - path) < sizeof(root) - 2) {
        memcpy(root, path, (size_t)(colon - path) + 1);
        root[colon - path + 1] = '/';
        root[colon - path + 2] = 0;
        path = root;
    }
    if (statvfs(path, &st) != 0) return -1;
    return (long long)st.f_bavail * (long long)(st.f_frsize ? st.f_frsize : st.f_bsize);
}

/* ------------------------------------------------------------ writer
 * The network side fills one buffer while this thread saves the other.
 * free_sem counts empty buffers, full_sem counts buffers waiting to be saved;
 * buffers are always handed over in alternating order. */

#ifdef GEKKO
#include <gccore.h>
#include <ogc/semaphore.h>
#include <ogc/lwp_watchdog.h>
#endif

typedef struct {
    FILE *f;
    unsigned char *buf[2];
    size_t len[2];
    volatile bool failed, stop;
#ifdef GEKKO
    lwp_t thread;
    sem_t free_sem, full_sem;
    int next_write;
#endif
} Writer;

#ifdef GEKKO
static void *writer_main(void *arg) {
    Writer *w = arg;
    for (;;) {
        LWP_SemWait(w->full_sem);
        if (w->stop) break;
        int i = w->next_write;
        w->next_write ^= 1;
        if (!w->failed && fwrite(w->buf[i], 1, w->len[i], w->f) != w->len[i]) w->failed = true;
        LWP_SemPost(w->free_sem);
    }
    return NULL;
}

static bool writer_start(Writer *w) {
    w->next_write = 0;
    if (LWP_SemInit(&w->free_sem, 2, 2) < 0 || LWP_SemInit(&w->full_sem, 0, 2) < 0) return false;
    return LWP_CreateThread(&w->thread, writer_main, w, NULL, 32 * 1024, 50) >= 0;
}

static void writer_acquire(Writer *w) { LWP_SemWait(w->free_sem); }
static void writer_release_unused(Writer *w) { LWP_SemPost(w->free_sem); }
static void writer_submit(Writer *w, int i, size_t len) { w->len[i] = len; LWP_SemPost(w->full_sem); }

static void writer_finish(Writer *w) {
    LWP_SemWait(w->free_sem);   /* both buffers back = everything saved */
    LWP_SemWait(w->free_sem);
    w->stop = true;
    LWP_SemPost(w->full_sem);
    LWP_JoinThread(w->thread, NULL);
    LWP_SemDestroy(w->free_sem);
    LWP_SemDestroy(w->full_sem);
}
#else
static bool writer_start(Writer *w) { (void)w; return true; }
static void writer_acquire(Writer *w) { (void)w; }
static void writer_release_unused(Writer *w) { (void)w; }
static void writer_submit(Writer *w, int i, size_t len) {
    if (!w->failed && fwrite(w->buf[i], 1, len, w->f) != len) w->failed = true;
}
static void writer_finish(Writer *w) { (void)w; }
#endif

static double now_s(void) {
#ifdef GEKKO
    return ticks_to_millisecs(gettime()) / 1000.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}

bool download_to_file(const char *url, const char *dest, DownloadProgress *pg, volatile int *cancel,
                      char *err, size_t errlen) {
    char part[600];
    snprintf(part, sizeof(part), "%s.part", dest);
    struct stat st;
    long long have = (stat(part, &st) == 0) ? (long long)st.st_size : 0;
    pg->done = have;
    pg->total = -1;
    pg->speed = 0;
    pg->resumed = false;

    HttpHooks hooks = { cancel, NULL, NULL, NULL };
    HttpStreamInfo info;
    diag_note(DIAG_WORKER, "download: connecting %.120s", url);
    HttpStream *hs = http_stream_open(url, have, &hooks, &info, err, errlen);
    if (!hs) return false;
    if (!info.ranged) have = 0;   /* server can't resume: start over */
    pg->done = have;
    pg->resumed = have > 0;
    pg->total = info.total;
    if (info.total > 0xFFFFFFFFLL) {
        snprintf(err, errlen, "This file is larger than 4 GB, the limit for FAT32 SD cards.");
        http_stream_close(hs);
        return false;
    }
    long long space = free_space(dest);
    if (info.total > 0 && space >= 0 && info.total - have > space) {
        snprintf(err, errlen, "Not enough space on the SD card (needs %lld MB more).", (info.total - have - space) / (1024 * 1024) + 1);
        http_stream_close(hs);
        return false;
    }

    Writer w;
    memset(&w, 0, sizeof(w));
    w.f = fopen(part, have ? "ab" : "wb");
    w.buf[0] = malloc(CHUNK);
    w.buf[1] = malloc(CHUNK);
    if (!w.f || !w.buf[0] || !w.buf[1] || !writer_start(&w)) {
        snprintf(err, errlen, w.f ? "Out of memory." : "Couldn't create the file on the SD card.");
        if (w.f) fclose(w.f);
        free(w.buf[0]);
        free(w.buf[1]);
        http_stream_close(hs);
        return false;
    }

    bool ok = true, stopped = false;
    int cur = 0;
    double t0 = now_s(), tmark = t0;
    long long mark = pg->done;
    for (;;) {
        writer_acquire(&w);             /* wait until buffer `cur` is empty */
        size_t fill = 0;
        int n = 1;
        while (fill < CHUNK) {
            n = http_stream_read(hs, w.buf[cur] + fill, CHUNK - fill);
            if (n <= 0) break;
            fill += (size_t)n;
            pg->done += n;
            double t = now_s();
            if (t - tmark >= 1.0) {     /* smoothed speed, updated each second */
                int inst = (int)((pg->done - mark) / (t - tmark));
                pg->speed = pg->speed ? (pg->speed * 2 + inst) / 3 : inst;
                mark = pg->done;
                tmark = t;
            }
            if (cancel && *cancel) { stopped = true; break; }
        }
        if (fill) { writer_submit(&w, cur, fill); cur ^= 1; }
        else writer_release_unused(&w);
        if (stopped) break;
        if (n < 0) { snprintf(err, errlen, "The connection was lost. Choose Retry to continue where it stopped."); ok = false; break; }
        if (n == 0) break;
        if (w.failed) break;
    }
    writer_finish(&w);
    http_stream_close(hs);
    bool closed = fclose(w.f) == 0;
    free(w.buf[0]);
    free(w.buf[1]);
    (void)t0;
    if (stopped) { remove(part); snprintf(err, errlen, "Stopped."); return false; }
    if (w.failed || !closed) { snprintf(err, errlen, "Couldn't write to the SD card. It may be full."); return false; }
    if (!ok) return false;
    if (pg->total > 0 && pg->done < pg->total) {
        snprintf(err, errlen, "The download ended early. Choose Retry to continue where it stopped.");
        return false;
    }
    remove(dest);
    if (rename(part, dest) != 0) { snprintf(err, errlen, "Couldn't finish saving the file."); return false; }
    if (pg->total < 0) pg->total = pg->done;
    return true;
}
