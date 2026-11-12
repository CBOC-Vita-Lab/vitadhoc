/* Force-included (-include) into the core's rtc.c only: its time(NULL) call is routed to the host's clock source. */
#ifndef WSWAN_TIME_SHIM_H
#define WSWAN_TIME_SHIM_H
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
time_t wswan_host_time_hook(time_t *t);
#ifdef __cplusplus
}
#endif
#define time(x) wswan_host_time_hook(x)
#endif
