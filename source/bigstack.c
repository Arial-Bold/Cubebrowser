/* bigstack.c - see bigstack.h */
#include "bigstack.h"

#ifdef GEKKO
#include <gccore.h>
#include <malloc.h>

#define BIG_STACK_SIZE (1024 * 1024)
#define BIG_STACK_PRIO 100

typedef struct { void (*fn)(void *); void *arg; } Job;

static void *trampoline(void *a) {
    Job *j = a;
    j->fn(j->arg);
    return NULL;
}

void run_with_big_stack(void (*fn)(void *), void *arg) {
    static void *stack;
    if (!stack) stack = memalign(32, BIG_STACK_SIZE);
    Job job = { fn, arg };
    lwp_t t;
    if (!stack || LWP_CreateThread(&t, trampoline, &job, stack, BIG_STACK_SIZE, BIG_STACK_PRIO) < 0) {
        fn(arg);   /* fall back to the current thread */
        return;
    }
    LWP_JoinThread(t, NULL);   /* wait for it; the stack is reused next time */
}
#else
void run_with_big_stack(void (*fn)(void *), void *arg) { fn(arg); }
#endif
