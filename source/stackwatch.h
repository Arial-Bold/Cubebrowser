/* stackwatch.h - each long-lived thread's stack is filled with a known
 * pattern when it's made, so how much of it a thread has ever used can be
 * seen afterwards. A thread that runs off the end of its stack silently
 * overwrites whatever memory lies below it (anything from another thread's
 * numbers to the music's notes), so one coming close is noted in page.log. */
#pragma once
#include <stddef.h>

/* A stack of `size` bytes for the thread called `name`, or NULL (then let
 * the system make one, unwatched). */
void *stack_new(size_t size, const char *name);
/* Notes in page.log any watched thread that has come within 1 KB of the end
 * of its stack (once each). Cheap; call every few seconds. */
void stack_check(void);
