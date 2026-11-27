/* SPDX-License-Identifier: MIT
 * pk_lan_compat.h: what the project's UDP transport (gpsp/netdrv/transport_lan.c) needs, for the separate players.
 *
 * transport_lan.c was written beside the main app's Game Boy link driver and, in the app, takes a few small types from that
 * driver's headers (a datagram ring, a mutex, a thread, counters, the transport vtable). The players (snes.bin, md.bin) must
 * not depend on that driver's headers (third-party GPL-2.0 provenance), so a player build defines PK_PLAYER and gets THIS file
 * instead: written from scratch for playerkit, MIT, only what the transport and pk_link_net use. The names are kept so the
 * transport source stays one file for both builds; the code below is our own. The player build's licence check also fails if
 * any of the app driver's headers shows up in a compile's dependency file (see snes/vita and md/vita Makefiles).
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#ifndef PK_LAN_COMPAT_H
#define PK_LAN_COMPAT_H

#ifndef PK_PLAYER
#error "pk_lan_compat.h is for the player builds (-DPK_PLAYER); the app uses the netdrv headers"
#endif

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus   /* the C++ side (pk_link_net.cpp) sees the sizes and the vtable only */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef VITA
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#endif /* !__cplusplus */

/* Sizes. Kept equal to the app's defaults so a player's discovery datagrams carry the same geometry string. */
#ifndef ND_MAX_PAYLOAD
#define ND_MAX_PAYLOAD 560
#endif
#ifndef ND_WINDOW
#define ND_WINDOW 32
#endif
#define ND_HDR_SIZE  24
#define ND_MAX_FRAME (ND_HDR_SIZE + ND_MAX_PAYLOAD)
/* the first four bytes ('GPN1', little-endian) by which the transport's I/O thread routes a datagram to its frame ring */
#define ND_WIRE_MAGIC 0x47504E31u

/* The transport's vtable (what pk_link_net reads: only heard_age_us; the others serve the app's session driver). */
typedef struct nd_transport
{
	void *ctx;
	int  (*send_to)(void *ctx, const uint8_t mac[6], const void *buf, size_t len);
	int  (*broadcast)(void *ctx, const void *buf, size_t len);
	int  (*recv)(void *ctx, uint8_t src_mac[6], void *buf, size_t cap);
	void (*local_addr)(void *ctx, uint8_t mac[6]);
	int  (*pending)(void *ctx);
	uint64_t (*heard_age_us)(void *ctx, const uint8_t mac[6]);
} nd_transport;

#ifndef __cplusplus
/* ---- clock, sleep ---- */
static inline uint64_t nd_now_us(void)
{
#ifdef VITA
	return (uint64_t) sceKernelGetProcessTimeWide();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000u;
#endif
}

static inline void nd_sleep_us(uint32_t us)
{
#ifdef VITA
	sceKernelDelayThread(us);
#else
	struct timespec ts;
	ts.tv_sec = us / 1000000u;
	ts.tv_nsec = (long) (us % 1000000u) * 1000L;
	nanosleep(&ts, NULL);
#endif
}

/* ---- one helper thread ---- */
typedef struct nd_thread
{
	void (*fn)(void *);
	void *arg;
	int   started;
#ifdef VITA
	SceUID id;
#else
	pthread_t id;
#endif
} nd_thread;

#ifdef VITA
static int nd_thread_entry_(SceSize args, void *argp)
{
	nd_thread *t = *(nd_thread **) argp;
	(void) args;
	t->fn(t->arg);
	return 0;
}
#else
static void *nd_thread_entry_(void *p)
{
	nd_thread *t = (nd_thread *) p;
	t->fn(t->arg);
	return NULL;
}
#endif

static inline int nd_thread_start(nd_thread *t, const char *name, void (*fn)(void *), void *arg)
{
	t->fn = fn;
	t->arg = arg;
	t->started = 0;
#ifdef VITA
	t->id = sceKernelCreateThread(name, nd_thread_entry_, 0x10000100, 0x10000, 0, 0, NULL);
	if (t->id < 0) return -1;
	if (sceKernelStartThread(t->id, sizeof t, &t) < 0) { sceKernelDeleteThread(t->id); return -1; }
#else
	(void) name;
	if (pthread_create(&t->id, NULL, nd_thread_entry_, t) != 0) return -1;
#endif
	t->started = 1;
	return 0;
}

static inline void nd_thread_join(nd_thread *t)
{
	if (!t->started) return;
#ifdef VITA
	sceKernelWaitThreadEnd(t->id, NULL, NULL);
	sceKernelDeleteThread(t->id);
#else
	pthread_join(t->id, NULL);
#endif
	t->started = 0;
}

/* ---- mutex ---- */
typedef struct nd_mutex
{
#ifdef VITA
	SceKernelLwMutexWork w;
#else
	pthread_mutex_t m;
#endif
} nd_mutex;

static inline int nd_mutex_init(nd_mutex *m)
{
#ifdef VITA
	return sceKernelCreateLwMutex(&m->w, "pk_lan_mx", 0, 0, NULL) < 0 ? -1 : 0;
#else
	return pthread_mutex_init(&m->m, NULL) ? -1 : 0;
#endif
}
static inline void nd_mutex_destroy(nd_mutex *m)
{
#ifdef VITA
	sceKernelDeleteLwMutex(&m->w);
#else
	pthread_mutex_destroy(&m->m);
#endif
}
static inline void nd_mutex_lock(nd_mutex *m)
{
#ifdef VITA
	sceKernelLockLwMutex(&m->w, 1, NULL);
#else
	pthread_mutex_lock(&m->m);
#endif
}
static inline void nd_mutex_unlock(nd_mutex *m)
{
#ifdef VITA
	sceKernelUnlockLwMutex(&m->w, 1);
#else
	pthread_mutex_unlock(&m->m);
#endif
}

/* ---- statistics counters (relaxed atomics) ---- */
typedef atomic_uint_fast32_t nd_ctr;
static inline void nd_ctr_add(nd_ctr *c, uint32_t n) { atomic_fetch_add_explicit(c, n, memory_order_relaxed); }
static inline uint32_t nd_ctr_get(const nd_ctr *c) { return (uint32_t) atomic_load_explicit((nd_ctr *) c, memory_order_relaxed); }
static inline void nd_ctr_set(nd_ctr *c, uint32_t v) { atomic_store_explicit(c, v, memory_order_relaxed); }
static inline void nd_ctr_max(nd_ctr *c, uint32_t v) { if (v > nd_ctr_get(c)) nd_ctr_set(c, v); }

/* ---- datagram ring: one producer thread, one consumer thread ----
 * Each slot is a small header then the bytes. The producer fills a slot and publishes it with a release store of the head
 * index; the consumer reads the head with an acquire load before it touches the slot (ARMv7 is weakly ordered). */
typedef struct nd_ring_slot
{
	uint64_t due_us;          /* 0, or "not before" */
	uint16_t len;
	uint8_t  mac[6];
	uint8_t  data[8];         /* really slot_bytes long */
} nd_ring_slot;

typedef struct nd_ring
{
	uint8_t *mem;
	uint32_t cap;             /* a power of two */
	uint32_t slot_bytes;
	uint32_t stride;
	char     pad0_[64];
	atomic_uint_fast32_t head;   /* next slot to write */
	char     pad1_[64];
	atomic_uint_fast32_t tail;   /* next slot to read */
	char     pad2_[64];
} nd_ring;

static inline int nd_ring_init(nd_ring *r, uint32_t slots, uint32_t slot_bytes)
{
	uint32_t cap = 1;
	while (cap < slots) cap <<= 1;
	memset(r, 0, sizeof *r);
	r->stride = (uint32_t) ((offsetof(nd_ring_slot, data) + slot_bytes + 7u) & ~7u);
	r->mem = (uint8_t *) malloc((size_t) cap * r->stride);
	if (!r->mem) return -1;
	r->cap = cap;
	r->slot_bytes = slot_bytes;
	atomic_init(&r->head, 0);
	atomic_init(&r->tail, 0);
	return 0;
}
static inline void nd_ring_free(nd_ring *r) { free(r->mem); r->mem = NULL; r->cap = 0; }
static inline nd_ring_slot *nd_ring_slot_at_(const nd_ring *r, uint32_t i)
{
	return (nd_ring_slot *) (r->mem + (size_t) (i & (r->cap - 1)) * r->stride);
}
static inline int nd_ring_push(nd_ring *r, const uint8_t mac[6], const void *buf, size_t len)
{
	uint32_t head = (uint32_t) atomic_load_explicit(&r->head, memory_order_relaxed);
	uint32_t tail = (uint32_t) atomic_load_explicit(&r->tail, memory_order_acquire);
	nd_ring_slot *s;
	if (!r->mem || len > r->slot_bytes || head - tail >= r->cap) return 0;
	s = nd_ring_slot_at_(r, head);
	s->due_us = 0;
	s->len = (uint16_t) len;
	if (mac) memcpy(s->mac, mac, 6); else memset(s->mac, 0, 6);
	memcpy(s->data, buf, len);
	atomic_store_explicit(&r->head, head + 1, memory_order_release);
	return 1;
}
static inline const nd_ring_slot *nd_ring_peek(nd_ring *r)
{
	uint32_t tail = (uint32_t) atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint32_t head = (uint32_t) atomic_load_explicit(&r->head, memory_order_acquire);
	return (!r->mem || tail == head) ? NULL : nd_ring_slot_at_(r, tail);
}
static inline void nd_ring_drop(nd_ring *r)
{
	uint32_t tail = (uint32_t) atomic_load_explicit(&r->tail, memory_order_relaxed);
	atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}
static inline uint32_t nd_ring_depth(const nd_ring *r)
{
	nd_ring *w = (nd_ring *) r;
	return (uint32_t) atomic_load_explicit(&w->head, memory_order_relaxed) - (uint32_t) atomic_load_explicit(&w->tail, memory_order_relaxed);
}
static inline int nd_ring_nonempty(const nd_ring *r) { return nd_ring_depth(r) != 0; }
static inline void nd_ring_flush(nd_ring *r)
{
	atomic_store_explicit(&r->tail, atomic_load_explicit(&r->head, memory_order_acquire), memory_order_release);
}

#endif /* !__cplusplus */

#endif
