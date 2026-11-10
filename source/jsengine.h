/* jsengine.h - JavaScript in CubeBrowser, using QuickJS (by Fabrice Bellard
 * and Charlie Gordon, MIT licence). For now it's the JavaScript speed test on
 * about:js: how fast the GameCube runs scripts, and how much memory they use.
 * Running pages' own scripts comes next. */
#pragma once
#include <stddef.h>

/* Runs the speed test (a few seconds) and writes its results as a page. */
void js_speed_test_html(char *out, size_t cap);
