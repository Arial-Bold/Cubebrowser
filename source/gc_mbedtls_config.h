/* gc_mbedtls_config.h - applied on top of mbedTLS 3.6's default configuration
 * (via MBEDTLS_USER_CONFIG_FILE). Only the platform-specific pieces change:
 * no POSIX sockets/timers/filesystem, and our own entropy and clock sources.
 * TLS 1.2 and TLS 1.3, ECDHE, AES-GCM and ChaCha20-Poly1305 stay enabled. */
#pragma once

/* libogc has its own socket API; conn.c supplies the I/O callbacks. */
#undef MBEDTLS_NET_C
/* timing.c only supports Unix/Windows. */
#undef MBEDTLS_TIMING_C
/* We read files ourselves (libfat) and parse from memory. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

/* No /dev/urandom: gc_entropy.c provides mbedtls_hardware_poll(). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Clock comes from SNTP (conn.c / netio.c). */
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* x86/ARM acceleration and hand-written asm don't apply to the Gekko CPU. */
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C

/* Not needed in the shipped program. Server-side features are left at their
 * defaults to avoid config dependency errors; trim them later if size matters. */
#undef MBEDTLS_SELF_TEST
