/* fetch.h - background worker thread. Brings the network up, syncs the clock,
 * then runs one page download at a time so the UI keeps animating at 60fps. */
#pragma once
#include <stdbool.h>
#include "http.h"

typedef enum { FETCH_IDLE, FETCH_BUSY, FETCH_DONE } FetchState;

void        fetch_init(void);
bool        fetch_start(const char *url, const char *post_body);
void        fetch_cancel(void);
FetchState  fetch_state(void);
const char *fetch_stage(void);
int         fetch_progress(void);
bool        fetch_take(HttpResult *out);   /* when DONE: moves result to caller, back to IDLE */
bool        fetch_net_ready(void);
void        fetch_retry_network(void);
