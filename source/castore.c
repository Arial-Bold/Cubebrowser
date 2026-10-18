/* castore.c - see castore.h. */
#include "castore.h"
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t der, der_len;     /* offsets into blob */
    size_t subj, subj_len;   /* the subject name, also inside blob */
} Root;

static unsigned char *blob;
static size_t blob_len, blob_cap;
static Root *roots;
static int nroots, roots_cap;

static bool grow(size_t need) {
    if (blob_len + need <= blob_cap) return true;
    size_t nc = blob_cap ? blob_cap : 128 * 1024;
    while (nc < blob_len + need) nc *= 2;
    unsigned char *nb = realloc(blob, nc);
    if (!nb) return false;
    blob = nb;
    blob_cap = nc;
    return true;
}

int castore_load(const char *pem, size_t len) {
    static const char begin[] = "-----BEGIN CERTIFICATE-----", end[] = "-----END CERTIFICATE-----";
    const char *p = pem, *stop = pem + len;
    while (p < stop) {
        const char *b = strstr(p, begin);
        if (!b || b >= stop) break;
        b += sizeof(begin) - 1;
        const char *e = strstr(b, end);
        if (!e || e >= stop) break;
        p = e + sizeof(end) - 1;
        /* base64 between the markers (decode skips line breaks) */
        size_t need = 0;
        mbedtls_base64_decode(NULL, 0, &need, (const unsigned char *)b, (size_t)(e - b));
        if (!need || !grow(need)) continue;
        size_t got = 0;
        if (mbedtls_base64_decode(blob + blob_len, blob_cap - blob_len, &got, (const unsigned char *)b, (size_t)(e - b)) != 0)
            continue;
        /* parse once to find its subject name, then let the parsed form go */
        mbedtls_x509_crt tmp;
        mbedtls_x509_crt_init(&tmp);
        if (mbedtls_x509_crt_parse_der_nocopy(&tmp, blob + blob_len, got) == 0 && tmp.subject_raw.p) {
            if (nroots == roots_cap) {
                int nc = roots_cap ? roots_cap * 2 : 160;
                Root *nr = realloc(roots, sizeof(Root) * (size_t)nc);
                if (!nr) { mbedtls_x509_crt_free(&tmp); break; }
                roots = nr;
                roots_cap = nc;
            }
            Root *r = &roots[nroots++];
            r->der = blob_len;
            r->der_len = got;
            r->subj = (size_t)(tmp.subject_raw.p - blob);
            r->subj_len = tmp.subject_raw.len;
            blob_len += got;
        }
        mbedtls_x509_crt_free(&tmp);
    }
    /* give back the spare room */
    if (blob_len && blob_len < blob_cap) {
        unsigned char *nb = realloc(blob, blob_len);
        if (nb) { blob = nb; blob_cap = blob_len; }
    }
    return nroots;
}

int castore_count(void) { return nroots; }

int castore_ca_cb(void *ctx, mbedtls_x509_crt const *child, mbedtls_x509_crt **candidates) {
    (void)ctx;
    *candidates = NULL;
    mbedtls_x509_crt *list = NULL;
    for (int i = 0; i < nroots; i++) {
        const Root *r = &roots[i];
        if (r->subj_len != child->issuer_raw.len || memcmp(blob + r->subj, child->issuer_raw.p, r->subj_len)) continue;
        if (!list) {
            list = mbedtls_calloc(1, sizeof(*list));
            if (!list) return MBEDTLS_ERR_X509_ALLOC_FAILED;
            mbedtls_x509_crt_init(list);
        }
        mbedtls_x509_crt_parse_der_nocopy(list, blob + r->der, r->der_len);   /* adds to the list */
    }
    if (list && !list->raw.p) {   /* none parsed after all */
        mbedtls_x509_crt_free(list);
        mbedtls_free(list);
        list = NULL;
    }
    *candidates = list;
    return 0;
}
