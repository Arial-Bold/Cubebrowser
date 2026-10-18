/* castore.h - the trusted root certificates (cacert.pem), kept compactly.
 *
 * Parsing all ~150 roots up front costs about 400 KB of memory. Instead each
 * certificate is kept only in its compact binary (DER) form, and when a site's
 * certificate has to be checked, mbedTLS asks for the roots whose name matches
 * its issuer: just those one or two are parsed, used, and freed again. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <mbedtls/x509_crt.h>

/* Takes the text of a PEM bundle. Returns how many certificates were kept. */
int castore_load(const char *pem, size_t len);
int castore_count(void);

/* For mbedtls_ssl_conf_ca_cb(). */
int castore_ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates);
