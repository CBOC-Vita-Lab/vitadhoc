/* ThreadSanitizer run of pk::SpinLock (bounded spin, then give the CPU up): eight threads bump a plain counter
 * under it, one holder stays long enough that the waiters must yield. make tsan, part of make test.
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_util.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static pk::SpinLock g_lk;
static long g_count;   /* plain: only ever touched under g_lk */

static void *bump(void *) { for (int i = 0; i < 30000; i++) { pk::SpinGuard g(g_lk); g_count++; } return 0; }
static void *hold(void *) { pk::SpinGuard g(g_lk); usleep(40000); g_count += 1000000; return 0; }

int main()
{
	pthread_t t[8], h;
	pthread_create(&h, 0, hold, 0);
	usleep(2000);
	for (int i = 0; i < 8; i++) pthread_create(&t[i], 0, bump, 0);
	for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
	pthread_join(h, 0);
	unsigned w = pk::spin_waits();
	bool ok = g_count == 8 * 30000 + 1000000 && w > 0;
	printf("spin_tsan: count %ld, %u waits: %s\n", g_count, w, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
