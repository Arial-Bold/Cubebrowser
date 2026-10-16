/* entropy.c - SHA-256 entropy pool feeding mbedTLS via mbedtls_hardware_poll(). */
#include "gc_entropy.h"
#include "storage.h"
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <stdlib.h>

#define RING 64

static unsigned char pool[32];
static u64 ring[RING];
static int ring_n;
static u32 counter;
static mutex_t lock;
static bool ready;

static void fold(const void *data, size_t len) {
    /* caller holds lock */
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, pool, sizeof(pool));
    if (len) mbedtls_sha256_update(&c, data, len);
    u64 t = gettime();
    mbedtls_sha256_update(&c, (const unsigned char *)&t, sizeof(t));
    mbedtls_sha256_finish(&c, pool);
    mbedtls_sha256_free(&c);
}

void entropy_init(void) {
    LWP_MutexInit(&lock, false);
    ready = true;
    LWP_MutexLock(lock);
    fold("cubebrowser-entropy-v1", 22);
    size_t len = 0;
    char path[400];
    storage_path(path, sizeof(path), "seed.bin");
    char *seed = storage_read(path, &len);
    if (seed) {
        fold(seed, len);
        free(seed);
    }
    LWP_MutexUnlock(lock);
}

void entropy_add(const void *data, size_t len) {
    if (!ready) return;
    LWP_MutexLock(lock);
    fold(data, len);
    LWP_MutexUnlock(lock);
}

void entropy_add_event(u32 value) {
    if (!ready) return;
    LWP_MutexLock(lock);
    ring[ring_n++] = gettime() ^ ((u64)value << 32);
    if (ring_n == RING) {
        fold(ring, sizeof(ring));
        ring_n = 0;
    }
    LWP_MutexUnlock(lock);
}

void entropy_save_seed(const unsigned char *seed32) {
    char path[400], tmp[400];
    storage_path(path, sizeof(path), "seed.bin");
    storage_path(tmp, sizeof(tmp), "seed.tmp");
    storage_write_atomic(path, tmp, seed32, 32);
}

/* Timing jitter: the time a small, data-dependent workload takes varies with
 * cache state, interrupts and DMA activity. Only the low bits are useful. */
static void gather_jitter(u32 *out, int n) {
    volatile u32 acc = 0x9E3779B9u;
    for (int i = 0; i < n; i++) {
        u64 t0 = gettime();
        int loops = 50 + (int)(t0 & 63);
        for (int k = 0; k < loops; k++) acc = (acc << 5) ^ (acc >> 3) ^ (u32)k;
        u64 t1 = gettime();
        out[i] = (u32)(t1 - t0) ^ (u32)t1 ^ acc;
    }
}

/* Called by mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT). */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    u32 jitter[32];
    gather_jitter(jitter, 32);
    if (!ready) entropy_init();
    LWP_MutexLock(lock);
    fold(jitter, sizeof(jitter));
    if (ring_n) { fold(ring, ring_n * sizeof(u64)); ring_n = 0; }
    size_t done = 0;
    while (done < len) {
        unsigned char block[32];
        counter++;
        mbedtls_sha256_context c;
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, pool, sizeof(pool));
        mbedtls_sha256_update(&c, (const unsigned char *)&counter, sizeof(counter));
        mbedtls_sha256_update(&c, (const unsigned char *)"out", 3);
        mbedtls_sha256_finish(&c, block);
        mbedtls_sha256_free(&c);
        size_t n = len - done < 32 ? len - done : 32;
        memcpy(output + done, block, n);
        done += n;
        fold(&counter, sizeof(counter));   /* ratchet: output can't reveal future pool */
    }
    LWP_MutexUnlock(lock);
    *olen = len;
    return 0;
}
