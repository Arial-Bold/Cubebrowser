/* netio.c - BBA networking via libogc's lwIP stack, a small DNS client (so a
 * custom DNS server can be used), and an SNTP client for the clock. */
#include "netio.h"
#include "settings.h"
#include "gc_entropy.h"
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static volatile NetState state = NET_OFF;
static char status[96] = "Network not started";
static char local_ip[16], netmask[16], gateway[16];
static bool time_synced;
static s64 time_base;   /* unix seconds at timebase zero */

NetState netio_state(void) { return state; }
const char *netio_status(void) { return status; }
const char *netio_ip(void) { return local_ip; }

static bool parse_ipv4(const char *s, u32 *out_be) {
    u32 parts[4];
    int n = 0, val = -1;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (*s - '0');
            if (val > 255) return false;
        } else if (*s == '.' || *s == 0) {
            if (val < 0 || n >= 4) return false;
            parts[n++] = (u32)val;
            val = -1;
            if (*s == 0) break;
        } else {
            return false;
        }
    }
    if (n != 4) return false;
    /* PowerPC is big-endian, so network order equals host order */
    *out_be = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

bool netio_bringup(void) {
    state = NET_STARTING;
    snprintf(status, sizeof(status), g_set.dhcp ? "Asking router for an address..." : "Configuring network...");
    if (g_set.dhcp) {
        local_ip[0] = netmask[0] = gateway[0] = 0;
    } else {
        snprintf(local_ip, sizeof(local_ip), "%s", g_set.ip);
        snprintf(netmask, sizeof(netmask), "%s", g_set.netmask);
        snprintf(gateway, sizeof(gateway), "%s", g_set.gateway);
    }
    s32 r = if_config(local_ip, netmask, gateway, g_set.dhcp ? true : false);
    if (r < 0) {
        state = NET_FAILED;
        snprintf(status, sizeof(status), "No network (error %d). Check the Broadband Adapter cable.", (int)r);
        return false;
    }
    state = NET_UP;
    snprintf(status, sizeof(status), "Connected as %s", local_ip);
    entropy_add(local_ip, strlen(local_ip));
    return true;
}

void netio_close(int fd) {
    if (fd >= 0) net_close(fd);
}

int netio_wait(int fd, bool for_write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    s32 r = net_select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
    if (r < 0) return -1;
    return r > 0 ? 1 : 0;
}

static int udp_exchange(u32 server_be, u16 port, const void *req, int req_len, void *resp, int resp_cap) {
    int fd = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = server_be;
    int got = -1;
    for (int attempt = 0; attempt < 3 && got < 0; attempt++) {
        if (net_sendto(fd, req, req_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) break;
        if (netio_wait(fd, false, 2000) == 1) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            got = net_recvfrom(fd, resp, resp_cap, 0, (struct sockaddr *)&from, &fl);
        }
    }
    net_close(fd);
    return got;
}

/* Minimal DNS A-record lookup. */
static int dns_query(u32 server_be, const char *host, u32 *ip_be) {
    unsigned char q[300], r[512];
    static u16 qid = 0x4321;
    qid += (u16)gettime();
    int n = 0;
    q[n++] = qid >> 8; q[n++] = qid & 0xFF;
    q[n++] = 0x01; q[n++] = 0x00;           /* recursion desired */
    q[n++] = 0; q[n++] = 1;                 /* one question */
    memset(q + n, 0, 6); n += 6;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len == 0 || len > 63 || n + len + 6 > (int)sizeof(q)) return -1;
        q[n++] = (unsigned char)len;
        memcpy(q + n, p, len); n += len;
        p += len;
        if (*p == '.') p++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;   /* type A */
    q[n++] = 0; q[n++] = 1;   /* class IN */

    int got = udp_exchange(server_be, 53, q, n, r, sizeof(r));
    if (got < 12 || r[0] != q[0] || r[1] != q[1]) return -1;
    if ((r[3] & 0x0F) != 0) return -2;      /* NXDOMAIN etc. */
    int qd = (r[4] << 8) | r[5], an = (r[6] << 8) | r[7];
    int i = 12;
    for (int k = 0; k < qd; k++) {
        while (i < got && r[i]) {
            if ((r[i] & 0xC0) == 0xC0) { i++; break; }
            i += r[i] + 1;
        }
        i += 5;
    }
    for (int k = 0; k < an && i < got; k++) {
        if ((r[i] & 0xC0) == 0xC0) i += 2;
        else { while (i < got && r[i]) i += r[i] + 1; i++; }
        if (i + 10 > got) break;
        int type = (r[i] << 8) | r[i + 1];
        int rdlen = (r[i + 8] << 8) | r[i + 9];
        i += 10;
        if (type == 1 && rdlen == 4 && i + 4 <= got) {
            *ip_be = ((u32)r[i] << 24) | ((u32)r[i + 1] << 16) | ((u32)r[i + 2] << 8) | r[i + 3];
            return 0;
        }
        i += rdlen;
    }
    return -1;
}

int netio_resolve(const char *host, u32 *ip_be) {
    if (parse_ipv4(host, ip_be)) return 0;
    /* The GameCube network library has no built-in name lookup, so we ask a
     * DNS server ourselves: the DNS setting if filled in, then the router
     * (most home routers answer DNS), then public servers as a backup. */
    const char *servers[4];
    int n = 0;
    if (g_set.dns[0]) servers[n++] = g_set.dns;
    if (gateway[0]) servers[n++] = gateway;
    servers[n++] = "1.1.1.1";
    servers[n++] = "8.8.8.8";
    for (int i = 0; i < n; i++) {
        u32 server;
        if (!parse_ipv4(servers[i], &server)) continue;
        int r = dns_query(server, host, ip_be);
        if (r == 0) return 0;
        if (r == -2) return -1;   /* the server says this name doesn't exist */
    }
    return -1;
}

int netio_connect(u32 ip_be, u16 port) {
    int fd = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = ip_be;
    if (net_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        net_close(fd);
        return -2;
    }
    entropy_add_event(ip_be);
    return fd;
}

bool netio_sync_time(void) {
    if (!g_set.ntp[0]) return false;
    u32 server;
    if (netio_resolve(g_set.ntp, &server) != 0) return false;
    unsigned char pkt[48], resp[64];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;   /* LI=0, version 3, client mode */
    int got = udp_exchange(server, 123, pkt, sizeof(pkt), resp, sizeof(resp));
    if (got < 48) return false;
    u32 secs = ((u32)resp[40] << 24) | ((u32)resp[41] << 16) | ((u32)resp[42] << 8) | resp[43];
    s64 unix_now = (s64)secs - 2208988800LL;
    if (unix_now < 1700000000LL) return false;  /* obviously bogus */
    time_base = unix_now - (s64)ticks_to_secs(gettime());
    time_synced = true;
    entropy_add(resp, got);
    return true;
}

bool netio_time_synced(void) { return time_synced; }

time_t netio_now(void) {
    if (time_synced) return (time_t)(time_base + (s64)ticks_to_secs(gettime()));
    return time(NULL);
}
