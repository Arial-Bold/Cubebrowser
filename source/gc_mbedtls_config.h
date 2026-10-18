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

/* Not needed in the shipped program. */
#undef MBEDTLS_SELF_TEST

/* ---- trimmed to what a web browser (a TLS client) uses, to save memory ---- */
/* client only: no server, no DTLS */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_SRTP
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
/* ciphers and modes web servers don't offer to a modern client */
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_SSL_EARLY_DATA
#undef MBEDTLS_ECJPAKE_C
/* unusual curves (servers use P-256/P-384 and X25519) */
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED
/* certificate/key file formats and writers the browser never uses */
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_GENPRIME
#undef MBEDTLS_DEBUG_C
/* requests are small: a 4 KB send buffer instead of 16 KB per connection
 * (received records can still be the full 16 KB) */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
/* trusted roots are looked up when needed (castore.c) instead of all being
 * parsed into memory at start-up */
#define MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
