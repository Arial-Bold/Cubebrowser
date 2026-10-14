/* fetch.c - network worker thread (libogc LWP). */
#include "fetch.h"
#include "netio.h"
#include "conn.h"
#include <gccore.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_STACK (128 * 1024)
#define WORKER_PRIO 40   /* below the UI thread, which yields every vsync */

static lwp_t worker;
static volatile FetchState state = FETCH_IDLE;
static volatile int job_pending, cancel_flag, progress, retry_net;
static char job_url[URL_MAX];
static char *job_post;
static HttpResult result;
static char stage_buf[192] = "";
static volatile bool net_ready;

static void set_stage(const char *msg) { snprintf(stage_buf, sizeof(stage_buf), "%s", msg); }

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

static void *worker_main(void *arg) {
    (void)arg;
    bring_up();
    for (;;) {
        if (retry_net && netio_state() == NET_FAILED) { retry_net = 0; bring_up(); }
        if (job_pending) {
            job_pending = 0;
            HttpHooks hooks = { &cancel_flag, &progress, set_stage };
            HttpResult r;
            http_fetch(job_url, job_post, &r, &hooks);
            free(job_post);
            job_post = NULL;
            result = r;
            state = FETCH_DONE;
        }
        usleep(10000);
    }
    return NULL;
}

void fetch_init(void) {
    LWP_CreateThread(&worker, worker_main, NULL, NULL, WORKER_STACK, WORKER_PRIO);
}

bool fetch_start(const char *url, const char *post_body) {
    if (state != FETCH_IDLE) return false;
    snprintf(job_url, sizeof(job_url), "%s", url);
    job_post = post_body ? strdup(post_body) : NULL;
    cancel_flag = 0;
    progress = 0;
    set_stage("Starting...");
    state = FETCH_BUSY;
    job_pending = 1;
    return true;
}

void fetch_cancel(void) { if (state == FETCH_BUSY) cancel_flag = 1; }
FetchState fetch_state(void) { return state; }
const char *fetch_stage(void) { return stage_buf; }
int fetch_progress(void) { return progress; }
bool fetch_net_ready(void) { return net_ready; }
void fetch_retry_network(void) { retry_net = 1; }

bool fetch_take(HttpResult *out) {
    if (state != FETCH_DONE) return false;
    *out = result;
    memset(&result, 0, sizeof(result));
    state = FETCH_IDLE;
    return true;
}
