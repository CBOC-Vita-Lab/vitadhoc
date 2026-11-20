/*
 * pk_stdio_lock: a real lock around newlib's FILE-slot allocation.
 *
 * VitaSDK's newlib is built without locking: __sfp_lock_acquire() and
 * __sfp_lock_release() are one-instruction returns (bx lr), and __sfp() scans the table of FILE
 * structures for a free one and claims it with no lock at all.  Two threads in fopen() at the same
 * moment (the writer thread opening a save while the main thread opens a log, a state file or a
 * cover) can be handed the SAME FILE; both then read and write through one buffer.  The window is a
 * few instructions wide, so it is rare, and it has not been seen on a console; it is closed here
 * because the cost is nothing.
 *
 * The linker's --wrap redirects every undefined reference to these three functions, in every object
 * of libc.a (fopen, fdopen, freopen, fclose, fcntl, funopen, fmemopen, open_memstream, ...), to the
 * __wrap_ versions below:
 *
 *     -Wl,--wrap=__sfp -Wl,--wrap=__sfp_lock_acquire -Wl,--wrap=__sfp_lock_release
 *
 * __wrap___sfp takes the lock around the real scan (fdopen and fcntl call __sfp without the
 * acquire/release pair); the pair itself, which fopen and fclose call around their own work, becomes
 * a real lock too.  The lock is RECURSIVE (fopen holds it and then calls __sfp), made of a
 * lightweight mutex plus an owner and a depth, so no mutex attribute is relied on.  It is created
 * the first time it is needed, once (an atomic gate), and if the system refuses to make it the wrap
 * does nothing (the old behaviour).
 *
 * Both the VPK's eboot.bin and snes.bin link this file with the three --wrap flags (a Makefile
 * switch, STDIO_LOCK=1, the default).  The host builds do not: they have a real libc.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#include <stddef.h>
#include <psp2/kernel/threadmgr.h>

struct _reent;
void *__real___sfp(struct _reent *);

static SceKernelLwMutexWork g_work __attribute__((aligned(8)));
static int g_gate;          /* 0 not made, 1 being made, 2 made, 3 refused */
static int g_owner;         /* thread id holding the lock, 0 = nobody       */
static int g_depth;         /* the owner's nesting (owner only)             */

static int lock_ready(void)
{
   int s = __atomic_load_n(&g_gate, __ATOMIC_ACQUIRE), zero = 0;
   if (s == 2)
      return 1;
   if (s == 3)
      return 0;
   if (s == 0 && __atomic_compare_exchange_n(&g_gate, &zero, 1, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
   {
      int rc = sceKernelCreateLwMutex(&g_work, "pk stdio", 0, 0, NULL);
      __atomic_store_n(&g_gate, rc >= 0 ? 2 : 3, __ATOMIC_RELEASE);
      return rc >= 0;
   }
   while (__atomic_load_n(&g_gate, __ATOMIC_ACQUIRE) == 1)
      sceKernelDelayThread(100);
   return __atomic_load_n(&g_gate, __ATOMIC_ACQUIRE) == 2;
}

static void lock(void)
{
   int me;
   if (!lock_ready())
      return;
   me = sceKernelGetThreadId();
   if (__atomic_load_n(&g_owner, __ATOMIC_RELAXED) == me)
   {
      g_depth++;
      return;
   }
   if (sceKernelLockLwMutex(&g_work, 1, NULL) < 0)
      return;                    /* (no lock taken: the matching unlock must not run) */
   g_depth = 1;
   __atomic_store_n(&g_owner, me, __ATOMIC_RELAXED);
}

static void unlock(void)
{
   if (!lock_ready() || __atomic_load_n(&g_owner, __ATOMIC_RELAXED) != sceKernelGetThreadId())
      return;
   if (--g_depth > 0)
      return;
   __atomic_store_n(&g_owner, 0, __ATOMIC_RELAXED);
   sceKernelUnlockLwMutex(&g_work, 1);
}

void __wrap___sfp_lock_acquire(void) { lock(); }
void __wrap___sfp_lock_release(void) { unlock(); }

void *__wrap___sfp(struct _reent *r)
{
   void *fp;
   lock();
   fp = __real___sfp(r);
   unlock();
   return fp;
}
