/* conn.h - a byte stream that is either plain TCP or TLS (mbedTLS). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <mbedtls/ssl.h>

typedef struct {
    int fd;
    bool tls;
    bool verified;              /* certificate chain checked successfully */
    mbedtls_ssl_context ssl;
    volatile int *cancel;
} Conn;

/* One-time setup: seeds the RNG, loads the CA bundle. err gets a message on failure. */
bool tls_global_init(char *err, size_t errlen);
bool tls_ca_loaded(void);
int  tls_ca_count(void);

bool conn_open(Conn *c, const char *host, unsigned short port, bool tls, volatile int *cancel,
               void (*stage)(const char *), char *err, size_t errlen);
bool conn_write_all(Conn *c, const void *data, size_t len);
int  conn_read(Conn *c, void *buf, size_t len);  /* >0 bytes, 0 EOF, <0 error */
void conn_close(Conn *c);
