/* update.h - checks GitHub for a newer CubeBrowser release and installs it.
 *
 * Releases come from github.com/Arial-Bold/Cubebrowser. The latest release's
 * tag (e.g. "v1.9") is compared with CB_VERSION (version.h); its .zip asset
 * holds a cubebrowser folder: default.dol, opening.bnr and
 * gcbrowser/cacert.pem. Installing replaces the running .dol (the old one
 * stays as <name>.old only until the new one has started), the banner and
 * cacert.pem; settings, bookmarks and cookies are never touched.
 *
 * The network work runs on the fetch worker thread (fetch.c); everything
 * else here is called from the UI thread. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define UPDATE_REPO "Arial-Bold/Cubebrowser"

typedef enum {
    UPD_NONE,          /* not checked yet */
    UPD_CHECKING,
    UPD_LATEST,        /* this is the newest version */
    UPD_AVAILABLE,     /* a newer version can be installed */
    UPD_CHECK_FAILED,
    UPD_INSTALLING,    /* downloading, unpacking, replacing */
    UPD_INSTALLED,     /* done: restart to use it */
    UPD_FAILED         /* install failed; nothing was replaced */
} UpdateState;

typedef enum { UPD_STEP_DOWNLOAD, UPD_STEP_UNPACK, UPD_STEP_REPLACE } UpdateStep;

UpdateState update_state(void);
UpdateStep  update_step(void);
const char *update_latest_version(void);   /* e.g. "1.9" (without the v) */
const char *update_notes(void);            /* release notes, plain text */
const char *update_error(void);
const char *update_target(void);           /* the .dol that will be / was replaced */

void update_request_check(bool quiet);     /* quiet: no message unless something is found */
void update_request_install(void);
void update_poll(void);                    /* call every frame (UI thread): starts queued jobs, collects results */
bool update_restart(char *err, size_t errlen);   /* starts the newly installed .dol */
/* At start: deletes the previous version left by an update (<name>.old), now
 * that this one has started, so old versions don't pile up on the card. */
void update_remove_old(void);

/* Helpers shared with the Swiss updater: "key": "value" in GitHub's JSON,
 * and Markdown release notes tidied for plain display. */
bool update_json_string(const char *json, const char *key, char *out, size_t cap, const char **after);
void update_tidy_notes(char *s);

/* Worker-thread side (called by fetch.c). */
void update_worker_check(volatile int *cancel);
void update_worker_install(volatile int *cancel);
