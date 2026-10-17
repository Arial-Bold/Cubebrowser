/* swissupd.h - updates Swiss from github.com/emukidid/swiss-gc.
 *
 * The latest release's tag (e.g. "v0.6r2119") gives its revision. Its
 * swiss_rNNNN.tar.xz holds swiss_rNNNN/DOL/swiss_rNNNN.dol, which replaces
 * the Swiss .dol the Exit button starts (Settings > Data), or is saved under
 * the name chosen in Settings > Updates (boot.dol / ipl.dol for setups that
 * start Swiss that way). A file it replaces is kept as <name>.old.
 *
 * Which revision is installed: the one this updater last installed there,
 * else the number in the file name, else unknown. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "update.h"   /* UpdateState, UpdateStep */

#define SWISS_REPO "emukidid/swiss-gc"

UpdateState swiss_state(void);
UpdateStep  swiss_step(void);
const char *swiss_latest(void);       /* e.g. "r2119" */
const char *swiss_installed(void);    /* e.g. "r2092", or "" if unknown */
const char *swiss_notes(void);
const char *swiss_error(void);
const char *swiss_target(void);       /* the Swiss .dol being (or that was) replaced */

void swiss_request_check(void);
void swiss_request_install(void);
void swiss_poll(void);                /* every frame, UI thread */
bool swiss_launch(char *err, size_t errlen);   /* starts the newly installed Swiss */

/* Worker-thread side (fetch.c). */
void swiss_worker_check(volatile int *cancel);
void swiss_worker_install(volatile int *cancel);

/* Finds the Swiss .dol: the one set in Settings, else one named swiss*.dol. */
bool swiss_find_dol(char *out, size_t cap);

/* Settings > Updates > Save new Swiss as (g_set.swiss_name). */
enum { SWISS_NAME_KEEP, SWISS_NAME_NUMBERED, SWISS_NAME_SWISS, SWISS_NAME_BOOT, SWISS_NAME_IPL };
/* Where the new Swiss will be saved (after a check has found it). */
void swiss_destination(char *out, size_t cap);
