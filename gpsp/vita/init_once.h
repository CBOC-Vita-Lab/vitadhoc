/* init_once.h -- a one-time initialisation that two threads can race into.
 *
 * No pthread_once and no std::mutex on the Vita (newlib's are no-ops
 * there), so this is a three-state flag on the compiler's atomics: 0 = not
 * up, 1 = one thread is bringing it up, 2 = up.  A caller that finds 1 waits
 * (the pause hook) until the first one is done, then sees the result: up (it
 * does nothing) or not up (it tries itself).
 *
 *     if (!init_once_begin(&g, pause)) return 0;   // already up
 *     ... bring it up ...
 *     init_once_done(&g, ok);
 *     ... and on teardown: init_once_reset(&g);
 *
 * Header only, no heap.  Tests: tests/guard/test_init_once.c under TSan. */
#ifndef INIT_ONCE_H
#define INIT_ONCE_H

typedef struct init_once { int s; } init_once;

/* 1 = this caller must do the initialisation (and then call init_once_done),
 * 0 = it is already up. */
static inline int init_once_begin(init_once *g, void (*pause)(void))
{
   for (;;)
   {
      int s = __atomic_load_n(&g->s, __ATOMIC_ACQUIRE);
      int zero = 0;
      if (s == 2)
         return 0;
      if (s == 0 &&
          __atomic_compare_exchange_n(&g->s, &zero, 1, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE))
         return 1;
      if (pause)
         pause();
   }
}

static inline void init_once_done(init_once *g, int ok)
{
   __atomic_store_n(&g->s, ok ? 2 : 0, __ATOMIC_RELEASE);
}

static inline void init_once_reset(init_once *g)
{
   __atomic_store_n(&g->s, 0, __ATOMIC_RELEASE);
}

#endif
