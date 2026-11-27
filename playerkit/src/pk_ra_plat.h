/* SPDX-License-Identifier: MIT
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 *
 * What gpsp/vita/ra_http.c (the web-request thread, dual MIT OR GPL-2.0-or-later) asks of its platform, for the players:
 * ra_http.c includes this instead of the main app's plat.h when it is built with -DPK_RA_PLAYER. Two implementations: playerkit/vita (the
 * console) and playerkit/ra/pk_ra_plat_posix.c (the Linux twins and the tests). */
#ifndef PK_RA_PLAT_H
#define PK_RA_PLAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One low-priority thread running fn(arg) to its end; 0 = started (at most one at a time). */
int  plat_net_start(void (*fn)(void *), void *arg);
/* Wait up to timeout_us for it to end: 1 = it did and is gone (or none ran), 0 = still running. */
int  plat_net_join(uint32_t timeout_us);
/* Sleep in real time (the net thread's). */
void plat_thread_sleep_us(uint32_t us);

/* The console's network stack: brought up once, when a signed-in game starts. 0 = up (the twin: always). */
int  pk_ra_net_up(void);
/* The network may be used now: Wi-Fi connected (the twin: always 1). */
int  pk_ra_net_online(void);

#ifdef __cplusplus
}
#endif

#endif
