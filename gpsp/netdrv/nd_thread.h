/* nd_thread.h -- the little the netdrv transports need from an OS: one helper
 * thread, a mutex for rarely-touched shared state, a monotonic clock, a
 * sleep, and a lock-free single-producer/single-consumer datagram ring.
 *
 * Header-only (static inline) so a transport is one .c file to add to a
 * build.  Two backends:
 *   VITA    sceKernel threads + LwMutex + sceKernelGetProcessTimeWide.
 *           No libpthread: the Vita frontend owns its own threading and a
 *           transport should not drag pthread-embedded's start-up into it.
 *   others  pthreads + CLOCK_MONOTONIC (Linux/macOS host builds and tests).
 *
 * WHY THE RING IS NOT THE PSP's.  transport_adhoc.c publishes its ring with
 * plain `volatile` indices and says so: "Single-CPU MIPS: volatile index
 * ordering is sufficient".  That is true there and false here.  The Vita has
 * four Cortex-A9 cores (three for the application), ARMv7 has a weak memory
 * model, and the RX thread really does run at the same instant as the
 * emulation thread: without a barrier the consumer can observe the new head
 * before the slot's bytes.  So the indices are C11 atomics: the producer
 * publishes with a RELEASE store after it wrote the slot, the consumer reads
 * the other side's index with an ACQUIRE load before it reads the slot.  On
 * ARMv7 that is one `dmb` per operation; on x86 it is free.  The indices sit
 * on separate cache lines so the two threads do not bounce one line.
 *
 * Threading contract (docs/ARCHITECTURE.md, netdrv): helper threads live
 * entirely BELOW the nd_transport line.  They never call into netdrv; they
 * only move datagrams between a socket and a ring.
 */
#ifndef ND_THREAD_H
#define ND_THREAD_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef VITA
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------ clock/sleep -- */

static inline uint64_t nd_now_us(void)
{
#ifdef VITA
   /* Microseconds since the process started; monotonic. */
   return (uint64_t)sceKernelGetProcessTimeWide();
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static inline void nd_sleep_us(uint32_t us)
{
#ifdef VITA
   sceKernelDelayThread(us);
#else
   struct timespec ts;
   ts.tv_sec  = us / 1000000u;
   ts.tv_nsec = (long)(us % 1000000u) * 1000L;
   nanosleep(&ts, NULL);
#endif
}

/* ----------------------------------------------------------------- thread -- */

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
/* 0x10000100 is the SDK's "default user priority" token (the value every
 * vitasdk sample passes).  The helper threads block in the network stack
 * almost all the time and the console has spare cores, so they need no
 * boost over the emulation thread.  HARDWARE: if a field log ever shows the
 * RX ring backing up (rx_ring_drop) while the emulation thread is busy, this
 * is the knob. */
#define ND_THREAD_PRIO_DEFAULT 0x10000100
#define ND_THREAD_STACK        0x10000

static int nd_thread_tramp_(SceSize args, void *argp)
{
   nd_thread *t;
   (void)args;
   /* sceKernelStartThread COPIES the argument block onto the new thread's
    * stack; the block we pass is one pointer. */
   t = *(nd_thread **)argp;
   t->fn(t->arg);
   return 0;
}
#else
static void *nd_thread_tramp_(void *p)
{
   nd_thread *t = (nd_thread *)p;
   t->fn(t->arg);
   return NULL;
}
#endif

/* 0 on success.  `t` must stay valid until nd_thread_join(). */
static inline int nd_thread_start(nd_thread *t, const char *name,
                                  void (*fn)(void *), void *arg)
{
   t->fn = fn;
   t->arg = arg;
   t->started = 0;
#ifdef VITA
   t->id = sceKernelCreateThread(name, nd_thread_tramp_,
                                 ND_THREAD_PRIO_DEFAULT, ND_THREAD_STACK,
                                 0, 0, NULL);
   if (t->id < 0)
      return -1;
   if (sceKernelStartThread(t->id, sizeof(t), &t) < 0)
   {
      sceKernelDeleteThread(t->id);
      return -1;
   }
#else
   (void)name;
   if (pthread_create(&t->id, NULL, nd_thread_tramp_, t) != 0)
      return -1;
#endif
   t->started = 1;
   return 0;
}

/* Wait for the thread function to return (the caller has already told it to,
 * through its own run flag) and release the thread. */
static inline void nd_thread_join(nd_thread *t)
{
   if (!t->started)
      return;
#ifdef VITA
   sceKernelWaitThreadEnd(t->id, NULL, NULL);
   sceKernelDeleteThread(t->id);
#else
   pthread_join(t->id, NULL);
#endif
   t->started = 0;
}

/* ------------------------------------------------------------------ mutex -- */

/* For state that changes a few times per session (the room a host announces,
 * the test-only delay queue).  Nothing on the per-frame path takes one. */
typedef struct nd_mutex
{
#ifdef VITA
   SceKernelLwMutexWork m;
#else
   pthread_mutex_t m;
#endif
} nd_mutex;

static inline int nd_mutex_init(nd_mutex *m)
{
#ifdef VITA
   return sceKernelCreateLwMutex(&m->m, "nd_mutex", 0, 0, NULL) < 0 ? -1 : 0;
#else
   return pthread_mutex_init(&m->m, NULL) != 0 ? -1 : 0;
#endif
}

static inline void nd_mutex_destroy(nd_mutex *m)
{
#ifdef VITA
   sceKernelDeleteLwMutex(&m->m);
#else
   pthread_mutex_destroy(&m->m);
#endif
}

static inline void nd_mutex_lock(nd_mutex *m)
{
#ifdef VITA
   sceKernelLockLwMutex(&m->m, 1, NULL);
#else
   pthread_mutex_lock(&m->m);
#endif
}

static inline void nd_mutex_unlock(nd_mutex *m)
{
#ifdef VITA
   sceKernelUnlockLwMutex(&m->m, 1);
#else
   pthread_mutex_unlock(&m->m);
#endif
}

/* -------------------------------------------------------------- semaphore -- */

/* Wakes a helper thread that has nothing to wait on but us (a TX thread
 * fed by a ring).  Counting, capped by the OS. */
typedef struct nd_sem
{
#ifdef VITA
   SceUID id;
#else
   sem_t s;
#endif
} nd_sem;

static inline int nd_sem_init(nd_sem *s, int max)
{
#ifdef VITA
   s->id = sceKernelCreateSema("nd_sem", 0, 0, max, NULL);
   return s->id < 0 ? -1 : 0;
#else
   (void)max;
   return sem_init(&s->s, 0, 0) != 0 ? -1 : 0;
#endif
}

static inline void nd_sem_destroy(nd_sem *s)
{
#ifdef VITA
   sceKernelDeleteSema(s->id);
#else
   sem_destroy(&s->s);
#endif
}

static inline void nd_sem_post(nd_sem *s)
{
#ifdef VITA
   /* fails harmlessly when the count is at its cap: the consumer is already
    * due to wake */
   sceKernelSignalSema(s->id, 1);
#else
   sem_post(&s->s);
#endif
}

/* Wait for a post or for timeout_us, whichever comes first. */
static inline void nd_sem_wait(nd_sem *s, uint32_t timeout_us)
{
#ifdef VITA
   SceUInt t = timeout_us;
   sceKernelWaitSema(s->id, 1, &t);
#else
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec  += timeout_us / 1000000u;
   ts.tv_nsec += (long)(timeout_us % 1000000u) * 1000L;
   if (ts.tv_nsec >= 1000000000L)
   {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
   }
   sem_timedwait(&s->s, &ts);
#endif
}

/* ---------------------------------------------------------- counters/flags -- */

/* A statistics counter that helper threads and the emulation thread may all
 * bump (a send can come from either side of a transport) and anybody may
 * read.  A real atomic add: a few nanoseconds on a Cortex-A9, and no lost
 * counts. */
typedef atomic_uint_fast32_t nd_ctr;

static inline void nd_ctr_add(nd_ctr *c, uint32_t n)
{
   atomic_fetch_add_explicit(c, n, memory_order_relaxed);
}
static inline uint32_t nd_ctr_get(const nd_ctr *c)
{
   return (uint32_t)atomic_load_explicit((nd_ctr *)c, memory_order_relaxed);
}
/* A high-water mark.  Two threads raising it at the same instant may keep
 * the smaller of their two values; it is telemetry. */
static inline void nd_ctr_max(nd_ctr *c, uint32_t v)
{
   if (v > nd_ctr_get(c))
      atomic_store_explicit(c, v, memory_order_relaxed);
}
static inline void nd_ctr_set(nd_ctr *c, uint32_t v)
{
   atomic_store_explicit(c, v, memory_order_relaxed);
}

/* ------------------------------------------------------- SPSC datagram ring -- */

/* One datagram per slot: where it came from (a 6-byte transport address),
 * its length, its bytes.  Exactly one thread calls nd_ring_push and exactly
 * one calls nd_ring_pop/peek for the life of the ring. */
typedef struct nd_ring_slot
{
   uint64_t due_us;              /* 0, or "not before" (a fault shim's delay) */
   uint16_t len;
   uint8_t  mac[6];
   uint8_t  data[1];             /* [slot_bytes] */
} nd_ring_slot;

typedef struct nd_ring
{
   uint8_t *mem;                 /* cap * stride bytes */
   uint32_t cap;                 /* power of two */
   uint32_t mask;
   uint32_t slot_bytes;          /* payload capacity of one slot */
   uint32_t stride;
   /* producer-owned index, then consumer-owned, a cache line apart (padding
    * rather than _Alignas: the ring lives inside calloc'd transports, and an
    * over-aligned member would make that allocation under-aligned) */
   char pad0_[64];
   atomic_uint_fast32_t head;    /* next write */
   char pad1_[64];
   atomic_uint_fast32_t tail;    /* next read */
   char pad2_[64];
} nd_ring;

/* slots is rounded UP to a power of two (free-running 32-bit indices then
 * stay correct across wrap-around).  0 on success. */
static inline int nd_ring_init(nd_ring *r, uint32_t slots, uint32_t slot_bytes)
{
   uint32_t cap = 1;
   while (cap < slots)
      cap <<= 1;
   memset(r, 0, sizeof(*r));
   r->stride = (uint32_t)((offsetof(nd_ring_slot, data) + slot_bytes + 7u) & ~7u);
   r->mem = (uint8_t *)malloc((size_t)cap * r->stride);
   if (!r->mem)
      return -1;
   r->cap = cap;
   r->mask = cap - 1;
   r->slot_bytes = slot_bytes;
   atomic_init(&r->head, 0);
   atomic_init(&r->tail, 0);
   return 0;
}

static inline void nd_ring_free(nd_ring *r)
{
   free(r->mem);
   r->mem = NULL;
   r->cap = 0;
}

static inline nd_ring_slot *nd_ring_at_(const nd_ring *r, uint32_t i)
{
   return (nd_ring_slot *)(r->mem + (size_t)(i & r->mask) * r->stride);
}

/* Producer.  1 = stored, 0 = ring full or len too large (caller counts the
 * drop; the layers above recover from loss by design). */
static inline int nd_ring_push_due(nd_ring *r, const uint8_t mac[6],
                                   const void *buf, size_t len, uint64_t due_us)
{
   uint32_t head = (uint32_t)atomic_load_explicit(&r->head, memory_order_relaxed);
   uint32_t tail = (uint32_t)atomic_load_explicit(&r->tail, memory_order_acquire);
   nd_ring_slot *s;
   if (!r->mem || len > r->slot_bytes || head - tail >= r->cap)
      return 0;
   s = nd_ring_at_(r, head);
   s->due_us = due_us;
   s->len = (uint16_t)len;
   if (mac)
      memcpy(s->mac, mac, 6);
   else
      memset(s->mac, 0, 6);
   memcpy(s->data, buf, len);
   /* publish AFTER the slot is written */
   atomic_store_explicit(&r->head, head + 1, memory_order_release);
   return 1;
}

static inline int nd_ring_push(nd_ring *r, const uint8_t mac[6],
                               const void *buf, size_t len)
{
   return nd_ring_push_due(r, mac, buf, len, 0);
}

/* Consumer.  The oldest datagram, still owned by the ring until
 * nd_ring_drop(); NULL when empty. */
static inline const nd_ring_slot *nd_ring_peek(nd_ring *r)
{
   uint32_t tail = (uint32_t)atomic_load_explicit(&r->tail, memory_order_relaxed);
   uint32_t head = (uint32_t)atomic_load_explicit(&r->head, memory_order_acquire);
   if (!r->mem || tail == head)
      return NULL;
   return nd_ring_at_(r, tail);
}

static inline void nd_ring_drop(nd_ring *r)
{
   uint32_t tail = (uint32_t)atomic_load_explicit(&r->tail, memory_order_relaxed);
   /* release the slot AFTER the consumer copied it out */
   atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}

/* Consumer.  Syscall-free and barrier-free "is anything there?" for
 * nd_transport.pending, which netdrv_poll_needed() calls hundreds of times
 * per emulated frame (ADR-0021).  A relaxed read may lag the producer by a
 * few instructions; the answer is then "no" one poll early, never wrong:
 * the pop that follows a "yes" uses the acquire load. */
static inline int nd_ring_nonempty(const nd_ring *r)
{
   nd_ring *w = (nd_ring *)r;
   return atomic_load_explicit(&w->tail, memory_order_relaxed) !=
          atomic_load_explicit(&w->head, memory_order_relaxed);
}

/* Either side; approximate by nature (telemetry). */
static inline uint32_t nd_ring_depth(const nd_ring *r)
{
   nd_ring *w = (nd_ring *)r;
   return (uint32_t)atomic_load_explicit(&w->head, memory_order_relaxed) -
          (uint32_t)atomic_load_explicit(&w->tail, memory_order_relaxed);
}

/* Consumer only: forget everything queued. */
static inline void nd_ring_flush(nd_ring *r)
{
   uint32_t head = (uint32_t)atomic_load_explicit(&r->head, memory_order_acquire);
   atomic_store_explicit(&r->tail, head, memory_order_release);
}

#endif /* ND_THREAD_H */
