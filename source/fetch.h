/* fetch.h - background worker thread. Brings the network up, syncs the clock,
 * then runs one download at a time (a page, or an image which it also
 * decodes) so the UI keeps animating at 60fps. */
#pragma once
#include <stdbool.h>
#include "http.h"
#include "image.h"
#include "download.h"
#include "unzip.h"

typedef enum { FETCH_IDLE, FETCH_BUSY, FETCH_DONE } FetchState;
typedef enum { JOB_PAGE, JOB_IMAGE, JOB_CSS, JOB_DOWNLOAD, JOB_UNZIP } JobKind;

typedef struct {
    int tag;              /* caller's identifier */
    bool ok;
    DecodedImage img;
    char error[96];
} ImageResult;

void        fetch_init(void);
bool        fetch_start(const char *url, const char *post_body);
bool        fetch_start_image(const char *url, int max_w, int max_h, int tag);
void        fetch_cancel(void);
FetchState  fetch_state(void);
JobKind     fetch_job_kind(void);
const char *fetch_stage(void);
int         fetch_progress(void);
bool        fetch_take(HttpResult *out);          /* page result; back to IDLE */
bool        fetch_take_image(ImageResult *out);   /* image result; back to IDLE */
bool        fetch_start_css(const char *url, int tag);
bool        fetch_take_css(HttpResult *out, int *tag);
void        fetch_discard(void);                  /* drop whatever finished */
bool        fetch_net_ready(void);
bool        fetch_start_download(const char *url, const char *dest_path);
bool        fetch_start_unzip(const char *zip_path, const char *dest_dir);
const DownloadProgress *fetch_download_progress(void);
const UnzipProgress *fetch_unzip_progress(void);
bool        fetch_take_task(bool *ok, char *err, size_t errlen);   /* finished download/unzip */
bool        is_download_name(const char *url);
void        fetch_retry_network(void);
