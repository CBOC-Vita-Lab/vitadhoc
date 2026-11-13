/* ThreadSanitizer run of the core watchdog: real threads beat, one stops, the poller finds exactly it
 * (make tsan, part of make test). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_cores.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

using namespace pk;

namespace {
struct Sys : CoreSys {
	uint64_t now_us() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t) ts.tv_sec * 1000000u + ts.tv_nsec / 1000; }
	void sleep_us(uint32_t us) { usleep(us); }
	int probe_once(uint32_t, int) { return 1000; }
};
Sys g_sys;
Cores *g_c;
std::atomic<bool> g_quit(false), g_a_stop(false);
int g_slot_a, g_slot_b, g_slot_c;

void *thread_a(void *)       /* beats until told to stop, then goes silent while "busy": a dead core */
{
	while (!g_quit.load()) {
		if (!g_a_stop.load()) g_c->watchdog().beat(g_slot_a);
		usleep(2000);
	}
	return 0;
}
void *thread_b(void *)       /* beats for ever */
{
	while (!g_quit.load()) { g_c->watchdog().beat(g_slot_b); usleep(1000); }
	return 0;
}
void *thread_c(void *)       /* idle helper: busy flag flips on and off, no beats while off */
{
	while (!g_quit.load()) {
		g_c->watchdog().set_busy(g_slot_c, true);
		g_c->watchdog().beat(g_slot_c);
		usleep(1500);
		g_c->watchdog().set_busy(g_slot_c, false);
		usleep(30000);
	}
	return 0;
}
}  // namespace

int main()
{
	Cores c(&g_sys);
	g_c = &c;
	c.probe();
	g_slot_a = c.watchdog().add("a");
	g_slot_b = c.watchdog().add("b");
	g_slot_c = c.watchdog().add("c");
	c.watchdog().watch(g_slot_a, true);
	c.watchdog().watch(g_slot_b, true);
	c.watchdog().watch(g_slot_c, true);
	c.watchdog().set_busy(g_slot_a, true);
	c.watchdog().set_busy(g_slot_b, true);
	pthread_t ta, tb, tc;
	pthread_create(&ta, 0, thread_a, 0);
	pthread_create(&tb, 0, thread_b, 0);
	pthread_create(&tc, 0, thread_c, 0);
	int bad = 0;
	/* 1 s of everything alive: no trip */
	for (int i = 0; i < 100; i++) {
		usleep(10000);
		c.watchdog().ref_beat();
		if (c.poll_watchdog(g_sys.now_us())) { bad++; fprintf(stderr, "false trip while all alive\n"); break; }
	}
	g_a_stop.store(true);
	bool tripped = false;
	for (int i = 0; i < 100 && !tripped; i++) {
		usleep(10000);
		c.watchdog().ref_beat();
		tripped = c.poll_watchdog(g_sys.now_us());
	}
	if (!tripped) { bad++; fprintf(stderr, "dead thread not found\n"); }
	if (c.core3_usable() || c.trips() != 1) { bad++; fprintf(stderr, "state after trip wrong\n"); }
	g_quit.store(true);
	pthread_join(ta, 0);
	pthread_join(tb, 0);
	pthread_join(tc, 0);
	printf("cores_tsan: %s\n", bad ? "FAILED" : "ok");
	return bad ? 1 : 0;
}
