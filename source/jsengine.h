/* jsengine.h - JavaScript in CubeBrowser, using QuickJS (by Fabrice Bellard
 * and Charlie Gordon, MIT licence). Runs each page's scripts, gives them the
 * page to work with (jsprelude.js builds document, elements, events and
 * timers on the functions here), and passes clicks and changes to them.
 *
 * Every script runs on the browser's big stack (bigstack.c), with a time
 * limit so a runaway script can't freeze the console, and a memory limit. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "page.h"
#include "http.h"

/* A new page is showing (after page_set_source): its scripts are queued.
 * Does nothing with JavaScript off. js_page_end lets the old page go. */
void js_page_begin(Page *p);
void js_page_end(void);

/* The page's script files: the next one to download (its index, url filled
 * in) or -1, and its delivery (body is taken, NULL if it failed). */
int  js_next_script(char *url, size_t cap);
void js_script_loaded(int idx, char *body, size_t len);
bool js_scripts_pending(void);   /* still downloading scripts the page needs */

/* Data the page's scripts asked for: the next one to download (its number,
 * the fields filled in, pointing into the engine's copy) or -1; then its
 * answer (NULL if it couldn't be sent). */
typedef struct { const char *url, *post, *content_type, *accept; bool cookies; } JsRequest;
int  js_next_request(JsRequest *out);
void js_request_done(int idx, HttpResult *r);

/* Once a frame: runs the scripts whose turn it is, then the page's timers. */
void js_frame(void);

/* The player clicked an element (a link, a button...): true if a script
 * dealt with it, so the browser shouldn't do its own thing. */
bool js_click(Node *n);
/* A form field changed (type is "change" or "input"), or a form is being sent
 * ("submit": true means a script stopped it). */
bool js_event(Node *n, const char *type);

/* What the page's scripts asked the browser to do (each is taken once). */
enum { JS_NAV_NONE, JS_NAV_GO, JS_NAV_REPLACE, JS_NAV_BACK, JS_NAV_SUBMIT };
int  js_take_navigation(char *url, size_t cap, int *field);
const char *js_take_message(void);   /* alert() text, or a note about a stopped script */
bool js_busy(void);
bool js_net_pending(void);               /* data scripts asked for hasn't come yet */                  /* scripts still to run before the page is ready */

/* about:console: the last page's script files, errors and console messages. */
void js_console_html(char *out, size_t cap);

/* The speed test on about:js (a few seconds). */
void js_speed_test_html(char *out, size_t cap);
