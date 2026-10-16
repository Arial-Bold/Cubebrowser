/* thr.h - tiny threading layer: libogc threads on the GameCube, POSIX
 * threads on a PC (used by the test programs). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#ifdef GEKKO
#include <gccore.h>
#include <ogc/mutex.h>
typedef lwp_t thr_t;
typedef mutex_t thr_mutex;
static inline bool thr_create(thr_t *t, void *(*fn)(void *), void *arg, size_t stack, int prio) {
    return LWP_CreateThread(t, fn, arg, NULL, (u32)stack, (u8)prio) >= 0;
}
static inline void thr_join(thr_t t) { LWP_JoinThread(t, NULL); }
static inline void thr_detach(thr_t t) { LWP_DetachThread(t); }
static inline bool thr_mutex_init(thr_mutex *m) { return LWP_MutexInit(m, false) >= 0; }
static inline void thr_mutex_free(thr_mutex *m) { LWP_MutexDestroy(*m); }
static inline void thr_lock(thr_mutex *m) { LWP_MutexLock(*m); }
static inline void thr_unlock(thr_mutex *m) { LWP_MutexUnlock(*m); }
#else
#include <pthread.h>
typedef pthread_t thr_t;
typedef pthread_mutex_t thr_mutex;
static inline bool thr_create(thr_t *t, void *(*fn)(void *), void *arg, size_t stack, int prio) {
    (void)prio;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack);
    bool ok = pthread_create(t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    return ok;
}
static inline void thr_join(thr_t t) { pthread_join(t, NULL); }
static inline void thr_detach(thr_t t) { pthread_detach(t); }
static inline bool thr_mutex_init(thr_mutex *m) { return pthread_mutex_init(m, NULL) == 0; }
static inline void thr_mutex_free(thr_mutex *m) { pthread_mutex_destroy(m); }
static inline void thr_lock(thr_mutex *m) { pthread_mutex_lock(m); }
static inline void thr_unlock(thr_mutex *m) { pthread_mutex_unlock(m); }
#endif

static inline void thr_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
