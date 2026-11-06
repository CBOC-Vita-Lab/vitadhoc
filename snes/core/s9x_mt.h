/*
 * CBOC-Vita local addition (not part of upstream Snes9x). MIT licence.
 * Minimal portable threading layer for the phase-2 worker threads:
 * threads with a CPU core hint, a counting semaphore, a monotonic clock and
 * spin helpers. Linux (pthread) for the twin, Vita kernel threads for snes.bin.
 */
#ifndef S9X_MT_H
#define S9X_MT_H

#include <stdint.h>
#include <atomic>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#endif

namespace s9xmt {

typedef void (*ThreadFn)(void *);

static inline uint64_t now_ns(void)
{
#ifdef __vita__
	return sceKernelGetProcessTimeWide() * 1000ull;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}

/* CPU time used by the calling thread (Linux only; 0 on the Vita, where the
 * SDK headers do not expose it - the busy counters below are used instead). */
static inline uint64_t thread_cpu_ns(void)
{
#ifdef __vita__
	return 0;
#else
	struct timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}

static inline void cpu_relax(void)
{
#if defined(__arm__)
	__asm__ __volatile__("" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}

static inline void yield_cpu(void)
{
#ifdef __vita__
	sceKernelDelayThread(1);
#else
	sched_yield();
#endif
}

struct Sem {
#ifdef __vita__
	SceUID id;
	void init() { id = sceKernelCreateSema("s9xsem", 0, 0, 0x7fffffff, NULL); }
	void destroy() { sceKernelDeleteSema(id); }
	void post() { sceKernelSignalSema(id, 1); }
	void wait() { sceKernelWaitSema(id, 1, NULL); }
#else
	sem_t s;
	void init() { sem_init(&s, 0, 0); }
	void destroy() { sem_destroy(&s); }
	void post() { sem_post(&s); }
	void wait() { while (sem_wait(&s) != 0) {} }
#endif
};

struct Thread {
	ThreadFn fn;
	void *arg;
	int core;
#ifdef __vita__
	SceUID id;
	static int entry(SceSize, void *p) { Thread *t = *(Thread **) p; t->fn(t->arg); return 0; }
	/* core: 0..2 pins the thread to that core, -1 leaves it to the system. prio is the Vita
	 * priority (lower = more urgent, 0x40 is a normal user value). */
	bool start(ThreadFn f, void *a, const char *name, int c, int prio)
	{
		fn = f; arg = a; core = c;
		int mask = c >= 0 ? (1 << c) : 0; /* 0 = SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT */
		id = sceKernelCreateThread(name, entry, prio, 0x10000, 0, mask, NULL);
		if (id < 0) return false;
		Thread *self = this;
		return sceKernelStartThread(id, sizeof self, &self) >= 0;
	}
	void join() { sceKernelWaitThreadEnd(id, NULL, NULL); sceKernelDeleteThread(id); }
#else
	pthread_t th;
	static void *entry(void *p) { Thread *t = (Thread *) p; t->fn(t->arg); return 0; }
	bool start(ThreadFn f, void *a, const char *, int c, int)
	{
		fn = f; arg = a; core = c;
		return pthread_create(&th, 0, entry, this) == 0;
	}
	void join() { pthread_join(th, 0); }
#endif
};

} // namespace s9xmt

#endif
