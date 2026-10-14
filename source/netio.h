/* netio.h - Broadband Adapter bring-up, DNS, sockets with timeouts, SNTP clock.
 * Everything here except the getters runs on the network worker thread. */
#pragma once
#include <gccore.h>
#include <stdbool.h>
#include <time.h>

typedef enum { NET_OFF, NET_STARTING, NET_UP, NET_FAILED } NetState;

bool        netio_bringup(void);        /* blocking; uses g_set network options */
NetState    netio_state(void);
const char *netio_status(void);
const char *netio_ip(void);

int  netio_resolve(const char *host, u32 *ip_be);   /* 0 on success */
int  netio_connect(u32 ip_be, u16 port);            /* socket or <0 */
int  netio_wait(int fd, bool for_write, int timeout_ms);  /* 1 ready, 0 timeout, <0 error */
void netio_close(int fd);

bool   netio_sync_time(void);           /* SNTP */
bool   netio_time_synced(void);
time_t netio_now(void);                 /* UTC unix time */
