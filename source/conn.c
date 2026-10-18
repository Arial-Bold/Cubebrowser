/* conn.c - TCP and TLS streams. TLS uses mbedTLS 3.6 (TLS 1.2 and 1.3) with
 * full certificate-chain and hostname verification against a CA bundle on SD. */
#include "conn.h"
#include "netio.h"
#include "settings.h"
#include "storage.h"
#include "gc_entropy.h"
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include "castore.h"
#include <mbedtls/error.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/net_sockets.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECV_TIMEOUT_MS 20000
#define ERR_CANCELLED (-0x7F01)

static mbedtls_entropy_context ent;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_ssl_config conf;
static bool inited, ca_ok;

/* The random generator is shared by every connection, and downloads now use
 * several at once, so calls into it are serialised. */
#ifdef GEKKO
#include <ogc/mutex.h>
static mutex_t rng_lock;
static bool rng_lock_ok;
#endif

static int rng_locked(void *ctx, unsigned char *out, size_t len) {
#ifdef GEKKO
    if (rng_lock_ok) LWP_MutexLock(rng_lock);
#endif
    int r = mbedtls_ctr_drbg_random(ctx, out, len);
#ifdef GEKKO
    if (rng_lock_ok) LWP_MutexUnlock(rng_lock);
#endif
    return r;
}
static int ca_count;

/* Clock for certificate validity checks: SNTP time when available. */
static mbedtls_time_t gc_time(mbedtls_time_t *t) {
    mbedtls_time_t now = (mbedtls_time_t)netio_now();
    if (t) *t = now;
    return now;
}

/* Required because MBEDTLS_PLATFORM_MS_TIME_ALT is set in gc_mbedtls_config.h */
mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)ticks_to_millisecs(gettime());
}

bool tls_ca_loaded(void) { return ca_ok; }
int tls_ca_count(void) { return ca_count; }

bool tls_global_init(char *err, size_t errlen) {
    if (inited) return true;
    mbedtls_platform_set_time(gc_time);

    psa_status_t ps = psa_crypto_init();
    if (ps != PSA_SUCCESS) {
        snprintf(err, errlen, "Crypto init failed (%d)", (int)ps);
        return false;
    }

    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    const char *pers = "CubeBrowser TLS";
    int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)pers, strlen(pers));
    if (r != 0) {
        snprintf(err, errlen, "Random generator failed (-0x%04X)", -r);
        return false;
    }
    unsigned char seed[32];
    if (mbedtls_ctr_drbg_random(&drbg, seed, sizeof(seed)) == 0) entropy_save_seed(seed);

    size_t len = 0;
    char *pem = storage_read(g_set.ca_path, &len);
    if (!pem) {   /* fall back to the copy in the data folder */
        char def[400];
        storage_path(def, sizeof(def), "cacert.pem");
        pem = storage_read(def, &len);
    }
    if (pem) {
        /* kept compactly; each site's root is parsed only when it's needed */
        ca_count = castore_load(pem, len);
        free(pem);
        ca_ok = ca_count > 0;
    }

    mbedtls_ssl_config_init(&conf);
    r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        snprintf(err, errlen, "TLS config failed (-0x%04X)", -r);
        return false;
    }
#ifdef GEKKO
    rng_lock_ok = LWP_MutexInit(&rng_lock, false) >= 0;
#endif
    mbedtls_ssl_conf_rng(&conf, rng_locked, &drbg);
    mbedtls_ssl_conf_ca_cb(&conf, castore_ca_cb, NULL);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    inited = true;
    return true;
}

/* Wait for data with a timeout, checking the cancel flag every 250 ms. */
static int wait_readable(Conn *c) {
    for (int waited = 0; waited < RECV_TIMEOUT_MS; waited += 250) {
        if (c->cancel && *c->cancel) return ERR_CANCELLED;
        int w = netio_wait(c->fd, false, 250);
        if (w == 1) return 0;
        if (w < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return MBEDTLS_ERR_SSL_TIMEOUT;
}

static int raw_recv(Conn *c, unsigned char *buf, size_t len) {
    int w = wait_readable(c);
    if (w != 0) return w;
    s32 r = net_recv(c->fd, buf, (s32)len, 0);
    if (r < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    if (r > 0) entropy_add_event((u32)r);
    return r;   /* 0 = connection closed */
}

static int raw_send(Conn *c, const unsigned char *buf, size_t len) {
    if (c->cancel && *c->cancel) return ERR_CANCELLED;
    s32 r = net_send(c->fd, buf, (s32)len, 0);
    if (r < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    return r;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len) { return raw_send((Conn *)ctx, buf, len); }
static int bio_recv(void *ctx, unsigned char *buf, size_t len) { return raw_recv((Conn *)ctx, buf, len); }

static void describe_error(int r, char *err, size_t errlen) {
    if (r == ERR_CANCELLED) { snprintf(err, errlen, "Stopped."); return; }
    if (r == MBEDTLS_ERR_SSL_TIMEOUT) { snprintf(err, errlen, "The site took too long to respond."); return; }
    char tmp[120];
    mbedtls_strerror(r, tmp, sizeof(tmp));
    snprintf(err, errlen, "Secure connection failed: %s", tmp);
}

static u32 ms_now(void) { return (u32)ticks_to_millisecs(gettime()); }

bool conn_open(Conn *c, const char *host, unsigned short port, bool tls, volatile int *cancel,
               void (*stage)(const char *), char *err, size_t errlen) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->cancel = cancel;
    if (stage) stage("Looking up site...");
    u32 ip, t0 = ms_now();
    int resolved = netio_resolve(host, &ip);
    u32 t1 = ms_now();
    c->ms_dns = (int)(t1 - t0);
    if (resolved != 0) {
        snprintf(err, errlen, "Couldn't find \"%s\". Check the address or DNS setting.", host);
        return false;
    }
    if (cancel && *cancel) { snprintf(err, errlen, "Stopped."); return false; }
    if (stage) stage("Connecting...");
    c->fd = netio_connect(ip, port);
    u32 t2 = ms_now();
    c->ms_connect = (int)(t2 - t1);
    if (c->fd < 0) {
        snprintf(err, errlen, "Couldn't connect to %s.", host);
        return false;
    }
    if (!tls) return true;

    if (!inited && !tls_global_init(err, errlen)) { conn_close(c); return false; }
    if (g_set.verify_certs && !ca_ok) {
        snprintf(err, errlen, "No trusted certificates loaded from %s, so secure sites can't be checked.",
                 g_set.ca_path);
        conn_close(c);
        return false;
    }
    if (stage) stage("Securing connection...");
    c->tls = true;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_conf_authmode(&conf, g_set.verify_certs ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
    int r = mbedtls_ssl_setup(&c->ssl, &conf);
    if (r == 0) r = mbedtls_ssl_set_hostname(&c->ssl, host);   /* SNI + name check */
    if (r != 0) { describe_error(r, err, errlen); conn_close(c); return false; }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            char info[256];
            mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
            char *nl;
            while ((nl = strchr(info, '\n'))) *nl = ' ';
            const char *clock_hint = "";
            if ((flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) && !netio_time_synced())
                clock_hint = " The console clock wasn't set from the network, which can cause this.";
            snprintf(err, errlen, "This site's certificate couldn't be verified: %s.%s", info, clock_hint);
        } else {
            describe_error(r, err, errlen);
        }
        conn_close(c);
        return false;
    }
    c->verified = g_set.verify_certs && mbedtls_ssl_get_verify_result(&c->ssl) == 0;
    c->ms_tls = (int)(ms_now() - t2);
    return true;
}

bool conn_write_all(Conn *c, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len > 0) {
        int r = c->tls ? mbedtls_ssl_write(&c->ssl, p, len) : raw_send(c, p, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        p += r;
        len -= (size_t)r;
    }
    return true;
}

int conn_read(Conn *c, void *buf, size_t len) {
    if (!c->tls) {
        int r = raw_recv(c, buf, len);
        return r < 0 ? -1 : r;
    }
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, buf, len);
        if (r > 0) return r;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (r == MBEDTLS_ERR_NET_CONN_RESET) return 0;
        return -1;
    }
}

void conn_close(Conn *c) {
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        c->tls = false;
    }
    if (c->fd >= 0) netio_close(c->fd);
    c->fd = -1;
}
