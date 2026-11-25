// The twin programs that run the player's own code (player_twin, link_twin) have no thread switches of their own: MD_TWIN_FM=1 (or check, stress,
// inline, tiny: the debug flags of md_twin's --fm-*) turns the YM2612 worker on for the whole program, so every proof they make (rewind, Quick response,
// states, linked sessions) can be run with the worker and must give the same results. Copyright CBOC-Vita. MIT licence for this file.
#pragma once
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "thread_ops.h"

namespace twinthr {
struct SpawnArg { void (*fn)(void *); void *arg; };
static pthread_t g_th[MD_WORKERS];
static SpawnArg g_sa[MD_WORKERS];
static void *thr_main(void *p) { SpawnArg *a = (SpawnArg *)p; a->fn(a->arg); return NULL; }
static void *sc(void *) { sem_t *s = new sem_t; sem_init(s, 0, 0); return s; }
static void sd(void *, void *s) { sem_destroy((sem_t *)s); delete (sem_t *)s; }
static void sp(void *, void *s) { sem_post((sem_t *)s); }
static void sw(void *, void *s) { while (sem_wait((sem_t *)s) != 0) {} }
static int spawn(void *, int id, void (*fn)(void *), void *arg) { g_sa[id].fn = fn; g_sa[id].arg = arg; return pthread_create(&g_th[id], NULL, thr_main, &g_sa[id]); }
static void wend(void *, int id) { pthread_join(g_th[id], NULL); }
static void yld(void *) { sched_yield(); }
static void nap(void *) { struct timespec ts = { 0, 50000 }; nanosleep(&ts, NULL); }
static unsigned long long now(void *) { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (unsigned long long) ts.tv_sec * 1000000ull + (unsigned long long) ts.tv_nsec / 1000; }
static md_thread_ops g_ops = { NULL, sc, sd, sp, sw, spawn, wend, yld, now, nap };

// returns 1 when the worker was switched on
static inline int fm_from_env(void)
{
	const char *e = getenv("MD_TWIN_FM");
	if (!e || !*e || !strcmp(e, "0")) return 0;
	int flags = 0;
	if (!strcmp(e, "check")) flags = 1;
	else if (!strcmp(e, "stress")) flags = 1 | 2;
	else if (!strcmp(e, "inline")) flags = 1 | 4;
	else if (!strcmp(e, "tiny")) flags = 1 | 8;
	md_host_set_threads(&g_ops, MD_THREAD_FM);
	md_host_fm_debug(flags);
	return 1;
}
}
