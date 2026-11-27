/* SPDX-License-Identifier: MIT
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 * pk_ra_plat.h on Linux: the thread of ra_http.c is a pthread. */
#include "pk_ra_plat.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static pthread_t g_th;
static atomic_int g_run;     /* 0 none, 1 running, 2 ended (not joined yet) */
static void (*g_fn)(void *);
static void *g_arg;

static void *entry(void *p)
{
	(void) p;
	g_fn(g_arg);
	atomic_store(&g_run, 2);
	return 0;
}

int plat_net_start(void (*fn)(void *), void *arg)
{
	if (atomic_load(&g_run) != 0) return -1;
	g_fn = fn;
	g_arg = arg;
	atomic_store(&g_run, 1);
	if (pthread_create(&g_th, 0, entry, 0) != 0) { atomic_store(&g_run, 0); return -1; }
	return 0;
}

int plat_net_join(uint32_t timeout_us)
{
	uint32_t waited = 0;
	if (atomic_load(&g_run) == 0) return 1;
	while (atomic_load(&g_run) != 2 && waited < timeout_us) { usleep(1000); waited += 1000; }
	if (atomic_load(&g_run) != 2) return 0;
	pthread_join(g_th, 0);
	atomic_store(&g_run, 0);
	return 1;
}

void plat_thread_sleep_us(uint32_t us) { usleep(us); }
int pk_ra_net_up(void) { return 0; }
int pk_ra_net_online(void) { return 1; }
