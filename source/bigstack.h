/* bigstack.h - runs a function on a thread with a large stack. Page parsing,
 * styling and layout recurse once per level of HTML nesting, and some sites
 * nest hundreds of levels deep, more than the main thread's stack can hold. */
#pragma once

void run_with_big_stack(void (*fn)(void *), void *arg);
/* GameCube: 1 MB stack; on a PC (tests) it simply calls fn. */
