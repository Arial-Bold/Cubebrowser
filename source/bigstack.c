/* bigstack.c - see bigstack.h. One long-lived helper thread with a 1 MB
 * stack; jobs are handed to it and the caller waits for them to finish. */
#include "bigstack.h"

#ifdef GEKKO
#include <gccore.h>
#include <ogc/semaphore.h>
#include <malloc.h>
#include "stackwatch.h"

#define BIG_STACK_SIZE (1024 * 1024)
#define BIG_STACK_PRIO 100

static lwp_t helper;
static sem_t go, done;
static void (*volatile job_fn)(void *);
static void *volatile job_arg;
static int state;   /* 0 = not started, 1 = ready, -1 = unavailable */

static void *helper_main(void *a) {
    (void)a;
    for (;;) {
        LWP_SemWait(go);
        job_fn(job_arg);
        LWP_SemPost(done);
    }
    return NULL;
}

void run_with_big_stack(void (*fn)(void *), void *arg) {
    if (state == 0) {
        void *stack = stack_new(BIG_STACK_SIZE, "page layout");
        if (!stack) stack = memalign(32, BIG_STACK_SIZE);
        state = -1;
        if (stack && LWP_SemInit(&go, 0, 1) >= 0 && LWP_SemInit(&done, 0, 1) >= 0 &&
            LWP_CreateThread(&helper, helper_main, NULL, stack, BIG_STACK_SIZE, BIG_STACK_PRIO) >= 0)
            state = 1;
    }
    if (state != 1) { fn(arg); return; }
    job_fn = fn;
    job_arg = arg;
    LWP_SemPost(go);
    LWP_SemWait(done);
}
#else
void run_with_big_stack(void (*fn)(void *), void *arg) { fn(arg); }
#endif
