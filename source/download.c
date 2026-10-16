/* download.c - see download.h */
#include "download.h"
#include "http.h"
#include "diag.h"
#include "settings.h"
#include "thr.h"
#include "storage.h"
#include "url.h"
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
 * One connection is limited by the network stack's small receive window
 * (about 3 KB), so large files are fetched as 512 KB blocks over several
 * connections at once. Blocks are written to the card in order, so the .part
 * file always holds one contiguous run of data and resuming works exactly as
 * for single-connection downloads.
 *
 * Things that go wrong on the GameCube, and what happens:
 * - A connection freezes (lost packets, then long retransmit back-off): after
 *   4 s without data its connection is dropped and the rest of the block is
 *   requested again (the bytes already received are kept).
 * - The thread can't respond, usually because it's stuck opening a new
 *   connection (lwIP retries a lost connection request for ~90 s and can't be
 *   interrupted): after 4 more seconds the block is taken away and given to
 *   another connection. Whatever the stuck thread receives later is refused,
 *   because each hand-over bumps the block's generation number.
 * - A connection that can't connect (busy server, or one that limits how
 *   many connections you may open) gives its block to the others and waits
 *   longer each time (1, 2, 4 ... 15 s) before trying again, so the download
 *   carries on with fewer connections. It only gives up if nothing at all
 *   arrives for a minute, or the server refuses the file outright.
 * - The number of connections stays as set in Settings: on the console,
 *   freezes happen at any count, and fewer connections was only ever slower
 *   (2: ~65 KB/s, 4: ~139 KB/s).
 * - Stopping or finishing never waits more than a few seconds for stuck
 *   threads; they clean up after themselves (the shared state is freed by
 *   whoever leaves last). */

#define BLOCK (512 * 1024)
#define CONN_STACK (128 * 1024)
#define CONN_PRIO 40
#define MAX_CONNS 6

enum { B_FREE, B_FILLING, B_READY, B_ORPHAN /* partly filled, waiting for a new connection */ };

typedef struct {
    unsigned char *data;
    long long offset, len, got;   /* got: bytes of this block in data (changed under the lock) */
    int state, owner;
    unsigned gen;                 /* bumped whenever the block changes hands */
    /* stall watch (main loop only) */
    long long seen_got;
    unsigned seen_gen;
    double moved_at, abort_at;
} Buf;

typedef struct {
    char url[URL_MAX];
    long long total, next;          /* next byte offset to hand out */
    Buf *bufs;
    int nbufs, made, running, refs;
    thr_mutex m;
    volatile int *cancel;
    volatile int abort_conn[MAX_CONNS];   /* per connection: drop the connection and retry */
    volatile int active_limit;            /* connections allowed to take new blocks */
    volatile bool fatal, stop;
    char err[256];
} Par;

typedef struct { Par *p; int id; } ConnArg;
typedef struct { Par *p; Buf *b; int id; unsigned gen; } Sink;

/* Last one out frees everything (a stuck thread may outlive the download). */
static void par_release(Par *p) {
    thr_lock(&p->m);
    bool last = --p->refs == 0;
    thr_unlock(&p->m);
    if (!last) return;
    for (int i = 0; i < p->made; i++) free(p->bufs[i].data);
    free(p->bufs);
    thr_mutex_free(&p->m);
    free(p);
}

static bool block_sink(void *ctx, const unsigned char *data, size_t len) {
    Sink *s = ctx;
    Par *p = s->p;
    Buf *b = s->b;
    bool ok;
    thr_lock(&p->m);
    ok = b->gen == s->gen && b->owner == s->id && b->state == B_FILLING && b->got + (long long)len <= b->len;
    if (ok) {
        memcpy(b->data + b->got, data, len);
        b->got += (long long)len;
    }
    thr_unlock(&p->m);
    return ok;
}

static void *conn_main(void *arg) {
    Par *p = ((ConnArg *)arg)->p;
    int id = ((ConnArg *)arg)->id;
    free(arg);
    HttpKeep *k = http_keep_new();
    HttpHooks hooks = { .cancel = &p->abort_conn[id] };
    char err[256];
    int fails = 0;   /* consecutive failed attempts on this connection */
    for (;;) {
        Buf *b = NULL;
        unsigned gen = 0;
        thr_lock(&p->m);
        bool stop = p->stop || p->fatal || (p->cancel && *p->cancel) || !k;
        if (!stop) {
            /* a block taken from a stuck connection comes first: the whole
             * download is waiting on it (any connection may take it, even one
             * paused by the speed control) */
            for (int i = 0; i < p->nbufs; i++)
                if (p->bufs[i].state == B_ORPHAN && (!b || p->bufs[i].offset < b->offset)) b = &p->bufs[i];
            if (!b && id < p->active_limit && p->next < p->total)
                for (int i = 0; i < p->nbufs; i++)
                    if (p->bufs[i].state == B_FREE) {
                        b = &p->bufs[i];
                        b->offset = p->next;
                        b->len = p->total - p->next < BLOCK ? p->total - p->next : BLOCK;
                        b->got = 0;
                        p->next += b->len;
                        break;
                    }
            if (b) {
                b->state = B_FILLING;
                b->owner = id;
                gen = ++b->gen;
            }
        }
        thr_unlock(&p->m);
        if (stop) break;
        if (!b) { thr_sleep_ms(20); continue; }

        Sink sink = { p, b, id, gen };
        int rescues = 0;
        bool done = false, lost = false, permanent = false, give_back = false;
        for (;;) {
            p->abort_conn[id] = 0;
            thr_lock(&p->m);
            long long from = b->offset + b->got, last = b->offset + b->len - 1;
            lost = b->gen != gen;
            thr_unlock(&p->m);
            if (lost) break;
            err[0] = 0;
            long long got_before = b->got;
            long long r = http_range_get(k, p->url, from, last, block_sink, &sink, &hooks, err, sizeof(err));
            thr_lock(&p->m);
            lost = b->gen != gen;      /* taken away while we were stuck */
            if (!lost && r >= 0 && b->got == b->len) { b->state = B_READY; done = true; }
            if (!lost && b->got > got_before) fails = 0;   /* data arrived: this connection works */
            thr_unlock(&p->m);
            if (done || lost) break;
            if (p->stop || (p->cancel && *p->cancel)) break;
            if (r == -2) { permanent = true; break; }
            if (p->abort_conn[id]) {             /* stall rescue: retry at once on a fresh connection */
                if (++rescues > 40) { give_back = true; break; }
                continue;
            }
            /* couldn't connect, or the connection broke: first time retry the
             * same block shortly; after that let another connection have it */
            fails++;
            thr_lock(&p->m);
            snprintf(p->err, sizeof(p->err), "%s", err[0] ? err : "The connection kept failing.");
            thr_unlock(&p->m);
            if (fails >= 2) { give_back = true; break; }
            thr_sleep_ms(500);
        }
        if (done) { fails = 0; continue; }
        if (lost) continue;
        if (p->stop || (p->cancel && *p->cancel)) break;
        if (permanent) {
            thr_lock(&p->m);
            if (!p->fatal) { p->fatal = true; snprintf(p->err, sizeof(p->err), "%s", err); }
            thr_unlock(&p->m);
            break;
        }
        if (give_back) {
            thr_lock(&p->m);
            if (b->gen == gen) { b->state = B_ORPHAN; b->owner = -1; b->gen++; }
            thr_unlock(&p->m);
            /* wait 1, 2, 4, 8, 15 s before this connection tries again */
            int wait_ms = fails <= 1 ? 1000 : (fails >= 5 ? 15000 : 1000 << (fails - 1));
            for (int w = 0; w < wait_ms && !p->stop && !(p->cancel && *p->cancel); w += 100) thr_sleep_ms(100);
        }
    }
    http_keep_free(k);
    thr_lock(&p->m);
    p->running--;
    thr_unlock(&p->m);
    par_release(p);
    return NULL;
}

/* Downloads from `have` to `total` into part (which already holds `have` bytes).
 * Returns 1 done, 0 failed (err set), -1 couldn't start (use one connection). */
static int download_parallel(const char *url, const char *part, long long have, long long total, int conns,
                             DownloadProgress *pg, volatile int *cancel, char *err, size_t errlen) {
    if (conns > MAX_CONNS) conns = MAX_CONNS;
    Par *p = calloc(1, sizeof(Par));
    if (!p) return -1;
    snprintf(p->url, sizeof(p->url), "%s", url);
    p->total = total;
    p->next = have;
    p->cancel = cancel;
    p->nbufs = conns * 2;
    p->bufs = calloc((size_t)p->nbufs, sizeof(Buf));
    if (!p->bufs || !thr_mutex_init(&p->m)) { free(p->bufs); free(p); return -1; }
    for (int i = 0; i < p->nbufs; i++) {
        p->bufs[i].data = malloc(BLOCK);
        if (!p->bufs[i].data) break;
        p->made++;
    }
    p->refs = 1;
    if (p->made < 2) { par_release(p); return -1; }   /* not enough memory: let the caller use one connection */
    p->nbufs = p->made;
    if (conns > p->made) conns = p->made;
    p->active_limit = conns;
    FILE *f = fopen(part, have ? "ab" : "wb");
    if (!f) {
        par_release(p);
        snprintf(err, errlen, "Couldn't create the file on the SD card.");
        return 0;
    }
    int started = 0;
    for (int i = 0; i < conns; i++) {
        ConnArg *a = malloc(sizeof(ConnArg));
        if (!a) break;
        a->p = p;
        a->id = started;
        thr_lock(&p->m);
        p->refs++;
        p->running++;
        thr_unlock(&p->m);
        thr_t th;
        if (thr_create(&th, conn_main, a, CONN_STACK, CONN_PRIO)) {
            thr_detach(th);   /* never joined: a stuck thread must not hold up the browser */
            started++;
        } else {
            free(a);
            thr_lock(&p->m);
            p->refs--;
            p->running--;
            thr_unlock(&p->m);
        }
    }
    if (!started) { fclose(f); par_release(p); return -1; }
    thr_lock(&p->m);
    if (p->active_limit > started) p->active_limit = started;
    thr_unlock(&p->m);
    pg->connections = p->active_limit;

    long long commit = have, mark = have;
    double tstart = now_s(), tmark = tstart;
    int rescues = 0, takeovers = 0, secs = 0;
    long long last_amount = have;
    double last_progress = tstart;
    bool write_failed = false;
    /* download log: one line per second, kept in memory and saved at the end
     * (download.log in the data folder) so it doesn't compete with the file */
    size_t logcap = 48 * 1024, loglen = 0;
    char *dlog = malloc(logcap);
    if (dlog)
        loglen = (size_t)snprintf(dlog, logcap, "%s\nsize %lld, from %lld, %d connections\n sec  KB/s conns rescues takeovers\n",
                                  url, total, have, started);
    for (int i = 0; i < p->nbufs; i++) p->bufs[i].seen_gen = (unsigned)-1;
    diag_note(DIAG_WORKER, "download: %d connections", started);

    for (;;) {
        bool cancelled = cancel && *cancel;
        if (cancelled)
            for (int i = 0; i < MAX_CONNS; i++) p->abort_conn[i] = 1;
        Buf *ready = NULL;
        long long inflight = 0;
        int moving = 0, filling = 0;
        double t = now_s();
        thr_lock(&p->m);
        for (int i = 0; i < p->nbufs; i++) {
            Buf *b = &p->bufs[i];
            if (b->state == B_FREE) continue;
            if (b->offset == commit && b->state == B_READY) ready = b;
            inflight += b->state == B_READY ? b->len : b->got;
            if (b->state != B_FILLING) continue;
            filling++;
            if (b->seen_gen != b->gen || b->seen_got != b->got) {
                b->abort_at = 0;   /* data is flowing (or a new owner started) */
                b->seen_gen = b->gen;
                b->seen_got = b->got;
                b->moved_at = t;
            }
            if (t - b->moved_at < 2.0) moving++;
        }
        if (!cancelled)
            for (int i = 0; i < p->nbufs; i++) {
                Buf *b = &p->bufs[i];
                if (b->state != B_FILLING) continue;
                /* Stall rescue: nothing for 4 s while others are moving (or
                 * while it's the only block left). If everything has gone
                 * quiet at once it's more likely the whole network: 10 s. */
                double limit = (moving > 0 || filling == 1) ? 4.0 : 10.0;
                if (b->abort_at == 0 && t - b->moved_at > limit) {
                    p->abort_conn[b->owner] = 1;
                    b->abort_at = t;
                    rescues++;
                } else if (b->abort_at > 0 && t - b->abort_at > 4.0) {
                    /* the connection didn't recover: hand the block over */
                    b->state = B_ORPHAN;
                    b->owner = -1;
                    b->gen++;
                    b->abort_at = 0;
                    b->moved_at = t;
                    takeovers++;
                }
            }
        int running = p->running;
        thr_unlock(&p->m);

        if (ready) {
            if (!write_failed && fwrite(ready->data, 1, (size_t)ready->len, f) != (size_t)ready->len) {
                write_failed = true;
                p->fatal = true;
            }
            commit += ready->len;
            thr_lock(&p->m);
            ready->state = B_FREE;
            thr_unlock(&p->m);
            continue;
        }
        if (commit >= total || p->fatal || cancelled || running == 0) break;

        /* progress and speed */
        pg->done = commit + inflight;
        if (pg->done != last_amount) { last_amount = pg->done; last_progress = t; }
        else if (t - last_progress > 60.0) {
            /* every connection has been failing for a minute: give up (Retry continues) */
            thr_lock(&p->m);
            p->fatal = true;
            if (!p->err[0]) snprintf(p->err, sizeof(p->err), "Nothing was received for a minute.");
            thr_unlock(&p->m);
            break;
        }
        if (t - tmark >= 1.0) {
            int inst = (int)((pg->done - mark) / (t - tmark));
            if (inst < 0) inst = 0;
            pg->speed = pg->speed ? (pg->speed * 2 + inst) / 3 : inst;
            mark = pg->done;
            tmark = t;
            secs++;
            pg->connections = p->active_limit;
            if (dlog && loglen + 64 < logcap)
                loglen += (size_t)snprintf(dlog + loglen, logcap - loglen, "%4d %5d %5d %7d %9d\n", secs, inst / 1024,
                                           p->active_limit, rescues, takeovers);
        }
        thr_sleep_ms(20);
    }

    /* Wind down: tell every connection to stop, wait a little, then leave
     * any that are stuck inside the network stack to finish on their own. */
    p->stop = true;
    for (int i = 0; i < MAX_CONNS; i++) p->abort_conn[i] = 1;
    int stuck = 0;
    for (int w = 0; w < 150; w++) {   /* up to 3 s */
        thr_lock(&p->m);
        stuck = p->running;
        thr_unlock(&p->m);
        if (!stuck) break;
        thr_sleep_ms(20);
    }
    bool closed = fclose(f) == 0;
    pg->done = commit;
    bool was_cancelled = cancel && *cancel;
    if (dlog) {
        double el = now_s() - tstart;
        if (loglen + 200 < logcap)
            loglen += (size_t)snprintf(dlog + loglen, logcap - loglen,
                                       "%s after %.0f s: %lld of %lld bytes, average %d KB/s, %d rescues, %d takeovers, %d threads still stuck\n%s\n",
                                       commit >= total ? "finished" : (was_cancelled ? "stopped" : "failed"), el, commit, total,
                                       el > 0 ? (int)((commit - have) / el / 1024) : 0, rescues, takeovers, stuck, p->err);
        char lp[400];
        storage_path(lp, sizeof(lp), "download.log");
        FILE *lf = fopen(lp, "wb");   /* only the most recent download */
        if (lf) { fwrite(dlog, 1, loglen, lf); fclose(lf); }
        free(dlog);
    }
    char perr[256];
    snprintf(perr, sizeof(perr), "%s", p->err);
    par_release(p);   /* stuck threads keep it alive until they finish */

    if (was_cancelled) { remove(part); snprintf(err, errlen, "Stopped."); return 0; }
    if (write_failed || !closed) { snprintf(err, errlen, "Couldn't write to the SD card. It may be full."); return 0; }
    if (commit < total) {
        snprintf(err, errlen, "%s Choose Retry to continue where it stopped.", perr[0] ? perr : "The download ended early.");
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
