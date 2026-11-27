/* The libc side of tests/stdio_lock: __sfp as unlocked as newlib's (scan, claim), and the two no-op lock functions. */
#include <sched.h>
#define SLOTS 4096
volatile int g_used[SLOTS];
volatile int g_claims[SLOTS];
struct _reent;
void *__sfp(struct _reent *r)
{
   int i;
   (void)r;
   for (i = 0; i < SLOTS; i++)
      if (!g_used[i])
      {
         if ((i & 7) == 0) sched_yield();       /* a preemption inside the window */
         g_used[i] = 1;
         __atomic_fetch_add(&g_claims[i], 1, __ATOMIC_RELAXED);
         return (void *)&g_used[i];
      }
   return 0;
}
void __sfp_lock_acquire(void) {}
void __sfp_lock_release(void) {}
