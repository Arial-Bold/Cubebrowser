/* fetch.c - network worker thread (libogc LWP). */
#include "stackwatch.h"
#include "fetch.h"
#include "netio.h"
#include "conn.h"
#include "diag.h"
#include "cookies.h"
#include "update.h"
#include "picache.h"
#include "memstat.h"
#include "settings.h"
#include "sevenz.h"
#include <gccore.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool strcasestr_s(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return true;
    return false;
}

#define WORKER_STACK (256 * 1024)
#define WORKER_PRIO 40   /* below the UI thread, which yields every vsync */
/* Largest image decoded directly (proxy images are already small). */
#define MAX_SRC_PIXELS (1600L * 1000L)

static lwp_t worker;
static volatile FetchState state = FETCH_IDLE;
static volatile JobKind kind;
static volatile int job_pending, cancel_flag, progress, retry_net;
static char job_url[URL_MAX];
static char *job_post;
static int job_max_w, job_max_h, job_tag;
static HttpResult result;
static ImageResult img_result;
static char stage_buf[192] = "";
static volatile bool net_ready;
static char task_path[600], task_dest[600];
static DownloadProgress dl_progress;
static UnzipProgress uz_progress;
static bool task_ok;
static char task_err[256];

/* File types that are always downloads, whatever the server calls them. */
bool is_download_name(const char *url) {
    static const char *const ext[] = { ".dol", ".elf", ".iso", ".gcm", ".gcz", ".ciso", ".rvz", ".tgc", ".zip", ".7z",
                                       ".rar", ".gz", ".tar", ".bin", ".img", ".dff", ".nkit.iso", ".exe", ".pdf",
                                       ".mp3", ".mp4", ".wav", ".ogg", ".flac", ".gci", ".sav", ".raw", ".mid", ".midi",
                                       ".kar", ".rmi", ".sf2", NULL };
    char path[URL_MAX];
    snprintf(path, sizeof(path), "%s", url);
    char *q = strpbrk(path, "?#");
    if (q) *q = 0;
    size_t n = strlen(path);
    for (int i = 0; ext[i]; i++) {
        size_t e = strlen(ext[i]);
        if (n > e && !strcasecmp(path + n - e, ext[i])) return true;
    }
    return false;
}

/* Page downloads stop before the body if it isn't something we can display. */
static volatile bool job_force_save;
static char job_referer[URL_MAX];

static bool page_wants_body(const char *url, const char *ct) {
    if (job_force_save) return false;
    if (is_download_name(url)) return false;
    if (!ct || !ct[0]) return true;
    return strcasestr_s(ct, "html") || strcasestr_s(ct, "xml") || strcasestr_s(ct, "text/");
}

static void set_stage(const char *msg) { snprintf(stage_buf, sizeof(stage_buf), "%s", msg); }
static void no_stage(const char *msg) { (void)msg; }

static void bring_up(void) {
    net_ready = false;
    if (!netio_bringup()) { set_stage(netio_status()); return; }
    set_stage("Setting the clock...");
    netio_sync_time();
    set_stage("Loading certificates...");
    char err[160];
    if (!tls_global_init(err, sizeof(err))) set_stage(err);
    else set_stage(netio_status());
    net_ready = true;
}

static void run_image_job(void) {
    diag_note(DIAG_WORKER, "downloading picture %.120s", job_url);
    ImageResult ir;
    memset(&ir, 0, sizeof(ir));
    ir.tag = job_tag;
    HttpHooks hooks = { &cancel_flag, &progress, no_stage, NULL, strstr(job_url, "images.weserv.nl") != NULL, NULL };
    /* saved on the SD card from an earlier visit? */
    size_t clen = 0;
    void *cached = picache_get(job_url, &clen);
    if (cached) {
        ir.ok = image_decode((const unsigned char *)cached, clen, job_max_w, job_max_h, MAX_SRC_PIXELS,
                             &ir.img, ir.error, sizeof(ir.error));
        free(cached);
        if (ir.ok) {
            http_log_note("pic     | from SD card cache (%u bytes) | %.70s", (unsigned)clen, job_url);
            img_result = ir;
            diag_note(DIAG_WORKER, "idle");
            return;
        }
        memset(&ir, 0, sizeof(ir));   /* unreadable: download it again */
        ir.tag = job_tag;
    }
    HttpResult r;
    if (!http_fetch(job_url, NULL, "image/jpeg,image/png,image/gif;q=0.8,image/bmp;q=0.5", &r, &hooks)) {
        snprintf(ir.error, sizeof(ir.error), "%s", r.error);
    } else if (r.status >= 400) {
        snprintf(ir.error, sizeof(ir.error), "server error %d", r.status);
    } else if (r.truncated && r.body_len < (size_t)g_set.max_page_kb * 1024) {
        /* cut short without reaching the size limit: no memory to hold it */
        snprintf(ir.error, sizeof(ir.error), "not enough memory");
        ir.low_memory = true;
        http_log_note("pic     | not enough memory for this picture (free %u KB) | %.70s",
                      (unsigned)(cb_mem_free() / 1024), job_url);
    } else if (!cancel_flag) {
        diag_note(DIAG_WORKER, "decoding picture (%u bytes)", (unsigned)r.body_len);
        ir.ok = image_decode((const unsigned char *)r.body, r.body_len, job_max_w, job_max_h, MAX_SRC_PIXELS,
                             &ir.img, ir.error, sizeof(ir.error));
        if (ir.ok && r.status == 200 && !r.truncated && !cancel_flag) picache_put(job_url, r.body, r.body_len);
        if (!ir.ok && cb_mem_free() < 2u * 1024u * 1024u) ir.low_memory = true;
        if (!ir.ok) http_log_note("pic     | couldn't show: %.60s (%s, %u bytes) | %.70s", ir.error, r.content_type,
                                  (unsigned)r.body_len, job_url);
    }
    http_result_free(&r);
    img_result = ir;
    diag_note(DIAG_WORKER, "idle");
}

static void *worker_main(void *arg) {
    (void)arg;
    bring_up();
    int idle_ticks = 0;
    for (;;) {
        if (retry_net && netio_state() == NET_FAILED) { retry_net = 0; bring_up(); }
        /* save page.log once loading has been quiet for 2 seconds, and
         * every 10 seconds after that if something new was noted (such as
         * the music's notes); it's only written when there's something new */
        if (!job_pending && ++idle_ticks >= 200 && idle_ticks % 1000 == 200) { stack_check(); http_log_flush(); }
        if (job_pending) {
            idle_ticks = 0;
            job_pending = 0;
            if (kind == JOB_IMAGE) {
                run_image_job();
            } else if (kind == JOB_DOWNLOAD) {
                diag_note(DIAG_WORKER, "saving file %.120s", task_path);
                task_ok = download_to_file(job_url, task_path, &dl_progress, &cancel_flag, task_err, sizeof(task_err));
            } else if (kind == JOB_UPDATE_CHECK) {
                update_worker_check(&cancel_flag);
            } else if (kind == JOB_UPDATE_INSTALL) {
                update_worker_install(&cancel_flag);
            } else if (kind == JOB_UNZIP) {
                diag_note(DIAG_WORKER, "unzipping %.120s", task_path);
                size_t tl = strlen(task_path);
                if (tl > 3 && !strcasecmp(task_path + tl - 3, ".7z"))
                    task_ok = sevenz_file(task_path, task_dest, &uz_progress, &cancel_flag, task_err, sizeof(task_err));
                else
                    task_ok = unzip_file(task_path, task_dest, &uz_progress, &cancel_flag, task_err, sizeof(task_err));
            } else if (kind == JOB_CSS) {
                diag_note(DIAG_WORKER, "downloading stylesheet %.120s", job_url);
                HttpResult r;
                size_t clen = 0;
                char *cached = picache_get(job_url, &clen);
                if (cached) {   /* saved on the SD card from an earlier visit */
                    memset(&r, 0, sizeof(r));
                    snprintf(r.url, sizeof(r.url), "%s", job_url);
                    r.status = 200;
                    snprintf(r.content_type, sizeof(r.content_type), "text/css");
                    r.body = cached;
                    r.body_len = clen;
                    r.length = (long long)clen;
                    http_log_note("css     | from SD card cache (%u KB) | %.70s", (unsigned)((clen + 1023) / 1024), job_url);
                } else {
                    HttpHooks hooks = { &cancel_flag, &progress, no_stage };
                    bool ok = http_fetch(job_url, NULL, "text/css,*/*;q=0.1", &r, &hooks);
                    if (ok && r.status >= 400) http_result_free(&r);
                    else if (ok && r.status == 200 && r.body && !r.truncated && !cancel_flag)
                        picache_put(job_url, r.body, r.body_len);
                }
                result = r;
            } else {
                diag_note(DIAG_WORKER, "downloading page %.120s", job_url);
                HttpHooks hooks = { &cancel_flag, &progress, set_stage, page_wants_body, false,
                                    job_referer[0] ? job_referer : NULL };
                HttpResult r;
                http_fetch(job_url, job_post, NULL, &r, &hooks);
                free(job_post);
                job_post = NULL;
                result = r;
            }
            cookies_save_if_needed();   /* keep logins on the card as they change */
            state = FETCH_DONE;
            diag_note(DIAG_WORKER, "idle");
        }
        usleep(10000);
    }
    return NULL;
}

void fetch_init(void) {
    LWP_CreateThread(&worker, worker_main, NULL, stack_new(WORKER_STACK, "loading"), WORKER_STACK, WORKER_PRIO);
}

bool fetch_start_save(const char *url) {
    if (!fetch_start(url, NULL)) return false;
    job_force_save = true;
    return true;
}

void fetch_set_referer(const char *page_url) {
    if (page_url && (!strncasecmp(page_url, "http://", 7) || !strncasecmp(page_url, "https://", 8)))
        snprintf(job_referer, sizeof(job_referer), "%s", page_url);
    else
        job_referer[0] = 0;
}

bool fetch_start(const char *url, const char *post_body) {
    if (state != FETCH_IDLE) return false;
    job_force_save = false;
    snprintf(job_url, sizeof(job_url), "%s", url);
    job_post = post_body ? strdup(post_body) : NULL;
    kind = JOB_PAGE;
    cancel_flag = 0;
    progress = 0;
    set_stage("Starting...");
    state = FETCH_BUSY;
    job_pending = 1;
    return true;
}

bool fetch_start_image(const char *url, int max_w, int max_h, int tag) {
    if (state != FETCH_IDLE || !net_ready) return false;
    if (strlen(url) >= sizeof(job_url)) return false;
    snprintf(job_url, sizeof(job_url), "%s", url);
    job_post = NULL;
    job_max_w = max_w;
    job_max_h = max_h;
    job_tag = tag;
    kind = JOB_IMAGE;
    cancel_flag = 0;
    progress = 0;
    state = FETCH_BUSY;
    job_pending = 1;
    return true;
}

bool fetch_start_css(const char *url, int tag) {
    if (state != FETCH_IDLE || !net_ready || strlen(url) >= sizeof(job_url)) return false;
    snprintf(job_url, sizeof(job_url), "%s", url);
    job_post = NULL;
    job_tag = tag;
    kind = JOB_CSS;
    cancel_flag = 0;
    progress = 0;
    state = FETCH_BUSY;
    job_pending = 1;
    return true;
}

bool fetch_take_css(HttpResult *out, int *tag) {
    if (state != FETCH_DONE || kind != JOB_CSS) return false;
    *out = result;
    *tag = job_tag;
    memset(&result, 0, sizeof(result));
    state = FETCH_IDLE;
    return true;
}

static bool start_task(int k) {
    task_ok = false;
    task_err[0] = 0;
    kind = (JobKind)k;
    cancel_flag = 0;
    progress = 0;
    state = FETCH_BUSY;
    job_pending = 1;
    return true;
}

bool fetch_start_download(const char *url, const char *dest_path) {
    if (state != FETCH_IDLE || !net_ready) return false;
    snprintf(job_url, sizeof(job_url), "%s", url);
    snprintf(task_path, sizeof(task_path), "%s", dest_path);
    memset(&dl_progress, 0, sizeof(dl_progress));
    dl_progress.total = -1;
    return start_task(JOB_DOWNLOAD);
}

bool fetch_start_unzip(const char *zip_path, const char *dest_dir) {
    if (state != FETCH_IDLE) return false;
    snprintf(task_path, sizeof(task_path), "%s", zip_path);
    snprintf(task_dest, sizeof(task_dest), "%s", dest_dir);
    memset(&uz_progress, 0, sizeof(uz_progress));
    return start_task(JOB_UNZIP);
}

const DownloadProgress *fetch_download_progress(void) { return &dl_progress; }
const UnzipProgress *fetch_unzip_progress(void) { return &uz_progress; }

bool fetch_take_task(bool *ok, char *err, size_t errlen) {
    if (state != FETCH_DONE || (kind != JOB_DOWNLOAD && kind != JOB_UNZIP)) return false;
    *ok = task_ok;
    snprintf(err, errlen, "%s", task_err);
    state = FETCH_IDLE;
    return true;
}

/* Page navigation cancels the current job, except a software update
 * (cut off halfway, it would just have to start again). */
static bool is_update_kind(JobKind k) {
    return k == JOB_UPDATE_CHECK || k == JOB_UPDATE_INSTALL;
}
bool fetch_job_is_update(void) { return is_update_kind(kind); }
void fetch_cancel(void) { if (state == FETCH_BUSY && !is_update_kind(kind)) cancel_flag = 1; }
void fetch_cancel_update(void) { if (state == FETCH_BUSY && is_update_kind(kind)) cancel_flag = 1; }

bool fetch_start_update(bool install) {
    if (state != FETCH_IDLE || !net_ready) return false;
    memset(&dl_progress, 0, sizeof(dl_progress));
    dl_progress.total = -1;
    memset(&uz_progress, 0, sizeof(uz_progress));
    return start_task(install ? JOB_UPDATE_INSTALL : JOB_UPDATE_CHECK);
}

bool fetch_take_update(void) {
    if (state != FETCH_DONE || (kind != JOB_UPDATE_CHECK && kind != JOB_UPDATE_INSTALL)) return false;
    state = FETCH_IDLE;
    return true;
}
FetchState fetch_state(void) { return state; }
JobKind fetch_job_kind(void) { return kind; }
const char *fetch_stage(void) { return stage_buf; }
int fetch_progress(void) { return progress; }
bool fetch_net_ready(void) { return net_ready; }
void fetch_retry_network(void) { retry_net = 1; }

bool fetch_take(HttpResult *out) {
    if (state != FETCH_DONE || kind != JOB_PAGE) return false;
    *out = result;
    memset(&result, 0, sizeof(result));
    state = FETCH_IDLE;
    return true;
}

bool fetch_take_image(ImageResult *out) {
    if (state != FETCH_DONE || kind != JOB_IMAGE) return false;
    *out = img_result;
    memset(&img_result, 0, sizeof(img_result));
    state = FETCH_IDLE;
    return true;
}

void fetch_discard(void) {
    if (state != FETCH_DONE) return;
    if (kind == JOB_DOWNLOAD || kind == JOB_UNZIP) { state = FETCH_IDLE; return; }
    if (is_update_kind(kind)) return;   /* update.c collects these */
    if (kind == JOB_IMAGE) image_free(&img_result.img);
    else http_result_free(&result);
    state = FETCH_IDLE;
}
