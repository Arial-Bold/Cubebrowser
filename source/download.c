/* download.c - see download.h */
#include "download.h"
#include "http.h"
#include "diag.h"
#include "settings.h"
#include "thr.h"
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

/* ===================================================== parallel downloading
 * One slow connection is limited by the network stack's small receive
 * window, so large files are fetched as 512 KB blocks over several
 * connections at once. Blocks are handed out in order and written in order,
 * so the .part file always holds one contiguous run of data and resuming
 * works exactly as for single-connection downloads. */

#define BLOCK (512 * 1024)
#define CONN_STACK (128 * 1024)
#define CONN_PRIO 40

enum { B_FREE, B_FILLING, B_READY };

typedef struct {
    unsigned char *data;
    long long offset, len;
    int state;
} Buf;

typedef struct {
    const char *url;
    long long total, next;          /* next byte offset to hand out */
    Buf *bufs;
    int nbufs, running;
    thr_mutex m;
    volatile int *cancel;
    volatile bool fatal;
    char err[256];
    volatile long long received;    /* every byte that arrived (for speed/progress) */
} Par;

static void *conn_main(void *arg) {
    Par *p = arg;
    HttpKeep *k = http_keep_new();
    HttpHooks hooks = { .cancel = p->cancel };
    for (;;) {
        Buf *b = NULL;
        thr_lock(&p->m);
        bool stop = p->fatal || (p->cancel && *p->cancel) || p->next >= p->total || !k;
        if (!stop)
            for (int i = 0; i < p->nbufs; i++)
                if (p->bufs[i].state == B_FREE) { b = &p->bufs[i]; break; }
        if (b) {
            b->state = B_FILLING;
            b->offset = p->next;
            b->len = p->total - p->next < BLOCK ? p->total - p->next : BLOCK;
            p->next += b->len;
        }
        thr_unlock(&p->m);
        if (stop) break;
        if (!b) { thr_sleep_ms(5); continue; }   /* all buffers busy: wait for the writer */
        bool ok = false;
        char err[256] = "";
        for (int attempt = 1; attempt <= 5 && !ok; attempt++) {
            volatile long long got = 0;
            long long r = http_range_get(k, p->url, b->offset, b->offset + b->len - 1, b->data, &got, &hooks, err, sizeof(err));
            if (r == b->len) { ok = true; p->received += got; break; }
            if (p->cancel && *p->cancel) break;
            thr_sleep_ms(500 * attempt);   /* brief back-off, then retry the block */
        }
        thr_lock(&p->m);
        if (ok) b->state = B_READY;
        else if (!p->fatal && !(p->cancel && *p->cancel)) { p->fatal = true; snprintf(p->err, sizeof(p->err), "%s", err); }
        thr_unlock(&p->m);
        if (!ok) break;
    }
    http_keep_free(k);
    thr_lock(&p->m);
    p->running--;
    thr_unlock(&p->m);
    return NULL;
}

/* Downloads from `have` to `total` into part (which already holds `have` bytes). */
static int download_parallel(const char *url, const char *part, long long have, long long total, int conns,
                             DownloadProgress *pg, volatile int *cancel, char *err, size_t errlen) {
    Par p;
    memset(&p, 0, sizeof(p));
    p.url = url;
    p.total = total;
    p.next = have;
    p.cancel = cancel;
    p.nbufs = conns * 2;
    p.bufs = calloc((size_t)p.nbufs, sizeof(Buf));
    if (!p.bufs || !thr_mutex_init(&p.m)) { free(p.bufs); return -1; }
    int made = 0;
    for (int i = 0; i < p.nbufs; i++) {
        p.bufs[i].data = malloc(BLOCK);
        if (!p.bufs[i].data) break;
        made++;
    }
    if (made < 2) {   /* not enough memory: let the caller use one connection */
        for (int i = 0; i < made; i++) free(p.bufs[i].data);
        free(p.bufs);
        thr_mutex_free(&p.m);
        return -1;
    }
    p.nbufs = made;
    if (conns > made) conns = made;
    FILE *f = fopen(part, have ? "ab" : "wb");
    if (!f) {
        for (int i = 0; i < made; i++) free(p.bufs[i].data);
        free(p.bufs);
        thr_mutex_free(&p.m);
        snprintf(err, errlen, "Couldn't create the file on the SD card.");
        return 0;
    }
    thr_t th[8];
    int started = 0;
    p.running = conns;
    for (int i = 0; i < conns && i < 8; i++)
        if (thr_create(&th[i], conn_main, &p, CONN_STACK, CONN_PRIO)) started++;
    thr_lock(&p.m);
    p.running = started;
    thr_unlock(&p.m);

    long long commit = have;
    bool write_failed = false;
    double tmark = now_s();
    long long mark = 0;
    diag_note(DIAG_WORKER, "download: %d connections", started);
    while (started) {
        Buf *ready = NULL;
        thr_lock(&p.m);
        for (int i = 0; i < p.nbufs; i++)
            if (p.bufs[i].state == B_READY && p.bufs[i].offset == commit) { ready = &p.bufs[i]; break; }
        int running = p.running;
        thr_unlock(&p.m);
        if (ready) {
            if (!write_failed && fwrite(ready->data, 1, (size_t)ready->len, f) != (size_t)ready->len) {
                write_failed = true;
                p.fatal = true;
            }
            commit += ready->len;
            thr_lock(&p.m);
            ready->state = B_FREE;
            thr_unlock(&p.m);
        } else {
            if (running == 0) break;
            thr_sleep_ms(20);
        }
        long long seen = have + p.received;
        pg->done = seen > commit ? seen : commit;
        double t = now_s();
        if (t - tmark >= 1.0) {
            int inst = (int)((p.received - mark) / (t - tmark));
            pg->speed = pg->speed ? (pg->speed * 2 + inst) / 3 : inst;
            mark = p.received;
            tmark = t;
        }
    }
    for (int i = 0; i < started; i++) thr_join(th[i]);
    bool closed = fclose(f) == 0;
    for (int i = 0; i < made; i++) free(p.bufs[i].data);
    free(p.bufs);
    thr_mutex_free(&p.m);
    pg->done = commit;
    if (cancel && *cancel) { remove(part); snprintf(err, errlen, "Stopped."); return 0; }
    if (write_failed || !closed) { snprintf(err, errlen, "Couldn't write to the SD card. It may be full."); return 0; }
    if (commit < total) {
        snprintf(err, errlen, "%s Choose Retry to continue where it stopped.", p.err[0] ? p.err : "The download ended early.");
        return 0;
    }
    return 1;
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

    /* Large file on a server that supports partial downloads: use several
     * connections (the first response is only used to learn the size). */
    int conns = g_set.dl_connections;
    if (conns > 1 && info.ranged && info.total > 0 && info.total - have >= 2LL * BLOCK) {
        char final_url[URL_MAX];
        snprintf(final_url, sizeof(final_url), "%s", info.url);   /* after redirects */
        http_stream_close(hs);
        int r = download_parallel(final_url, part, have, info.total, conns > 6 ? 6 : conns, pg, cancel, err, errlen);
        if (r == 1) {
            remove(dest);
            if (rename(part, dest) != 0) { snprintf(err, errlen, "Couldn't finish saving the file."); return false; }
            return true;
        }
        if (r == 0) return false;
        /* r < 0: couldn't set up (memory): reopen and continue with one connection */
        hs = http_stream_open(url, have, &hooks, &info, err, errlen);
        if (!hs) return false;
        if (!info.ranged) have = 0;
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
