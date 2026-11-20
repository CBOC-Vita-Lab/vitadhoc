/* pk_stdio_lock.c on the host: a FILE table whose "scan for a free slot, then claim it" is as unlocked as
 * newlib's __sfp, hit by eight threads.  Unwrapped, two threads get the same slot (the table shows duplicate
 * claims); through __wrap___sfp (-DWRAPPED) none does, nesting through acquire + __sfp + release works, and
 * fopen's pattern (acquire, __sfp, release) from many threads stays exclusive.
 * make -C playerkit stdio_lock */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2/kernel/threadmgr.h>

/* ---- the fake console ---- */
static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
int sceKernelCreateLwMutex(SceKernelLwMutexWork *w, const char *n, unsigned a, int i, void *o) { (void)w; (void)n; (void)a; (void)i; (void)o; return 0; }
int sceKernelLockLwMutex(SceKernelLwMutexWork *w, int c, unsigned *t) { (void)w; (void)c; (void)t; return pthread_mutex_lock(&g_m); }
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *w, int c) { (void)w; (void)c; return pthread_mutex_unlock(&g_m); }
int sceKernelGetThreadId(void) { return (int)(long)pthread_self() & 0x7fffffff; }
int sceKernelDelayThread(unsigned us) { return usleep(us); }

struct _reent;
void *__sfp(struct _reent *r);                    /* in fake_sfp.c: another object, so --wrap reaches the call */
void __sfp_lock_acquire(void);
void __sfp_lock_release(void);
extern volatile int g_claims[];
#define SLOTS 4096
#define SFP(r) __sfp(r)

static void *worker(void *arg)
{
   int i, mode = (int)(long)arg;
   for (i = 0; i < 400; i++)
   {
      if (mode & 1)
      {                                       /* fopen's pattern: the pair around __sfp (nested) */
         __sfp_lock_acquire();
         SFP(0);
         __sfp_lock_release();
      }
      else
         SFP(0);
   }
   return 0;
}

int main(void)
{
   pthread_t t[8];
   int i, dup = 0, claimed = 0;
   for (i = 0; i < 8; i++) pthread_create(&t[i], 0, worker, (void *)(long)i);
   for (i = 0; i < 8; i++) pthread_join(t[i], 0);
   for (i = 0; i < SLOTS; i++)
   {
      if (g_claims[i] > 1) dup += g_claims[i] - 1;
      if (g_claims[i]) claimed++;
   }
   printf("stdio lock (%s): %d slots claimed by 3200 calls, %d double claims\n",
#ifdef WRAPPED
          "wrapped",
#else
          "unwrapped",
#endif
          claimed, dup);
#ifdef WRAPPED
   return dup == 0 && claimed == 3200 ? 0 : 1;
#else
   return 0;      /* informational: the race, shown */
#endif
}
