/* VitAdhoc: the FM worker (see fm_thread.h for the design). Copyright CBOC-Vita; Genesis Plus GX licence (md/core/LICENSE.txt). */
#include "shared.h"
#include "fm_thread.h"

int fmw_on;

enum { OP_WRITE = 1, OP_RUN, OP_RESET, OP_STOP };

#define QCAP_WORDS (1u << 14)             /* 64 KB: a frame is a few hundred writes (two words each with their runs) */
#define WAKE_SAMPLES 96                    /* samples queued between wake-ups of a sleeping worker (a frame is about 890) */

typedef struct {
	const md_thread_ops *ops;
	void *sem;
	uint32 *ring;
	unsigned mask, cap;
	unsigned head;                         /* published by the producer */
	unsigned tail;                         /* advanced by whoever consumes (the worker, or the emulating thread taking over), under `lock` */
	int lock;                              /* 1 while a thread is consuming */
	int waiting;                           /* the worker sleeps on the semaphore */
	int *buf;
	/* producer private */
	unsigned ph, tail_seen;
	int pend;
	int debug;
	unsigned long check_errors;
	fmw_census_t cen;
	unsigned long long busy;               /* written by the worker only (a stale value in the census is fine) */
} FW;
static FW W;
static const md_thread_ops *g_ops;
static int g_want;

#define LOADA(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define STOREA(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

void fmw_set_ops(const md_thread_ops *ops, int want) { g_ops = ops; g_want = ops && want; }
int fmw_wanted(void) { return g_want; }

/* ---- consuming (worker, or the emulating thread taking over) ------------------------------------------------------------------------- */

static int try_lock(void) { return __atomic_exchange_n(&W.lock, 1, __ATOMIC_ACQUIRE) == 0; }
static void unlock(void) { __atomic_store_n(&W.lock, 0, __ATOMIC_RELEASE); }

/* one record at `tail`; returns 0 at OP_STOP (not consumed) */
static int consume_one(unsigned tail)
{
	const uint32 w0 = W.ring[tail & W.mask];
	switch (w0 & 0xFF) {
	case OP_WRITE: YM2612Write((w0 >> 8) & 3, w0 >> 16); STOREA(W.tail, tail + 1); return 1;
	case OP_RUN: {
		const uint32 w1 = W.ring[(tail + 1) & W.mask];
		YM2612Update(W.buf + w1, (int) (w0 >> 8));
		STOREA(W.tail, tail + 2);
		return 1;
	}
	case OP_RESET: YM2612ResetChip(); STOREA(W.tail, tail + 1); return 1;
	default: return 0;
	}
}

static void worker_main(void *arg)
{
	(void) arg;
	for (;;) {
		unsigned tail = LOADA(W.tail), head = LOADA(W.head);
		if (tail == head) {
			__atomic_store_n(&W.waiting, 1, __ATOMIC_SEQ_CST);
			head = __atomic_load_n(&W.head, __ATOMIC_SEQ_CST);
			tail = LOADA(W.tail);
			if (tail != head) {
				if (__atomic_exchange_n(&W.waiting, 0, __ATOMIC_SEQ_CST) == 0) W.ops->sem_wait(W.ops->user, W.sem);   /* the producer posted meanwhile */
			} else {
				W.ops->sem_wait(W.ops->user, W.sem);
			}
			continue;
		}
		if (!try_lock()) {                 /* the emulating thread is finishing the queue itself */
			if (W.ops->nap) W.ops->nap(W.ops->user); else W.ops->yield(W.ops->user);
			continue;
		}
		{
			const unsigned long long t0 = W.ops->now_us ? W.ops->now_us(W.ops->user) : 0;
			int stop = 0;
			for (;;) {
				tail = LOADA(W.tail); head = LOADA(W.head);
				if (tail == head) break;
				if (!consume_one(tail)) { if (W.ring[tail & W.mask] == OP_STOP) { STOREA(W.tail, tail + 1); stop = 1; } break; }
			}
			if (t0) __atomic_store_n(&W.busy, W.busy + (W.ops->now_us(W.ops->user) - t0), __ATOMIC_RELAXED);
			unlock();
			if (stop) return;
		}
	}
}

/* ---- producer ------------------------------------------------------------------------------------------------------------------------- */

static void publish(int wake)
{
	__atomic_store_n(&W.head, W.ph, __ATOMIC_SEQ_CST);
	if (wake) {
		W.pend = 0;
		if (__atomic_exchange_n(&W.waiting, 0, __ATOMIC_SEQ_CST)) { W.ops->sem_post(W.ops->user, W.sem); W.cen.wakes++; }
	}
}

/* the emulating thread does what the worker has not got to: the same records, the same code, the same chip */
static void take_over(void)
{
	while (!try_lock()) { if (W.ops->nap) W.ops->nap(W.ops->user); else W.ops->yield(W.ops->user); }     /* the worker is inside one record: it ends soon */
	for (;;) {
		const unsigned tail = LOADA(W.tail);
		if (tail == W.ph) break;
		if (!consume_one(tail)) break;
	}
	unlock();
	W.tail_seen = LOADA(W.tail);
	W.cen.takeovers++;
}

static void reserve(unsigned n)
{
	if (W.ph + n - W.tail_seen <= W.cap) return;
	W.tail_seen = LOADA(W.tail);
	if (W.ph + n - W.tail_seen <= W.cap) return;
	W.cen.full_waits++;
	publish(1);
	take_over();
}

static void wait_idle(void)
{
	unsigned spins = 0;
	const unsigned long long t0 = W.ops->now_us ? W.ops->now_us(W.ops->user) : 0;
	publish(1);
	if (W.debug & 4) { take_over(); }
	while (LOADA(W.tail) != W.ph) {
		W.cen.join_wait_polls++;
		if (++spins > 32) W.ops->yield(W.ops->user);
		if (t0 ? (W.ops->now_us(W.ops->user) - t0 > FMW_BOUND_US) : (spins > 4000)) { take_over(); break; }
	}
	W.tail_seen = W.ph;
	if (t0) W.cen.join_wait_us += (unsigned long) (W.ops->now_us(W.ops->user) - t0);
}

void fmw_q_write(unsigned a, unsigned v)
{
	reserve(1);
	W.ring[W.ph++ & W.mask] = OP_WRITE | ((a & 3) << 8) | ((v & 0xFF) << 16);
	W.cen.records++; W.cen.writes++;
	publish(0);
}

void fmw_q_run(int samples, int offset)
{
	reserve(2);
	W.ring[W.ph++ & W.mask] = OP_RUN | ((uint32) samples << 8);
	W.ring[W.ph++ & W.mask] = (uint32) offset;
	W.cen.records++; W.cen.runs++; W.cen.samples += (unsigned long) samples;
	W.pend += samples;
	publish(W.pend >= WAKE_SAMPLES);
	if (W.debug & 2) fmw_join();
}

void fmw_q_reset(void)
{
	reserve(1);
	W.ring[W.ph++ & W.mask] = OP_RESET;
	W.cen.records++;
	publish(0);
}

void fmw_join(void)
{
	if (!fmw_on) return;
	W.cen.joins++;
	wait_idle();
	if ((W.debug & 1) && YM2612ShadowCompare()) {
		W.check_errors++;
		fprintf(stderr, "fmw check: the timer shadow differs from the chip\n");
	}
}

void fmw_set_debug(int flags) { W.debug = flags; if (fmw_on) { W.cap = (flags & 8) ? 24 : QCAP_WORDS; } }
unsigned long fmw_check_errors(void) { return W.check_errors; }

void fmw_census(fmw_census_t *c)
{
	if (!c) return;
	*c = W.cen;
	c->fm_us = (unsigned long) __atomic_load_n(&W.busy, __ATOMIC_RELAXED);
}

int fmw_start(int *buffer)
{
	const md_thread_ops *ops = g_ops;
	if (fmw_on || !ops || !ops->spawn || !ops->sem_create) return 1;
	{ const int dbg = W.debug; memset(&W, 0, sizeof W); W.debug = dbg; }
	W.ops = ops;
	W.buf = buffer;
	W.ring = (uint32 *) malloc(QCAP_WORDS * sizeof(uint32));
	if (!W.ring) return 2;
	W.mask = QCAP_WORDS - 1;
	W.cap = (W.debug & 8) ? 24 : QCAP_WORDS;
	W.sem = ops->sem_create(ops->user);
	if (!W.sem) { free(W.ring); return 3; }
	fmw_on = 1;
	if (ops->spawn(ops->user, MD_WORKER_FM, worker_main, NULL)) {
		fmw_on = 0;
		ops->sem_destroy(ops->user, W.sem);
		free(W.ring);
		return 4;
	}
	return 0;
}

void fmw_stop(void)
{
	if (!fmw_on) return;
	wait_idle();
	reserve(1);
	W.ring[W.ph++ & W.mask] = OP_STOP;
	publish(1);
	W.ops->wait_end(W.ops->user, MD_WORKER_FM);
	fmw_on = 0;
	W.ops->sem_destroy(W.ops->user, W.sem);
	free(W.ring);
	W.ring = 0;
}
