/* init_once.h: eight threads race into one initialisation; it runs once, and
 * every caller returns only after it is up.  A failed first try lets the next
 * caller try again.  gcc -fsanitize=thread -I vita vita/tests/guard/test_init_once.c -lpthread */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include "init_once.h"

static init_once g;
static int ran, shared_state;      /* written by the initialiser only */
static int fail_first;

static void pause_hook(void) { usleep(50); }

static void *racer(void *arg)
{
   (void)arg;
   if (!init_once_begin(&g, pause_hook))
      return (void *)(long)shared_state;     /* up: ordered after the writer by the flag */
   {
      int ok;
      __atomic_fetch_add(&ran, 1, __ATOMIC_RELAXED);
      usleep(20000);               /* a slow bring-up: the others arrive meanwhile */
      ok = !fail_first;
      fail_first = 0;
      if (ok)
         shared_state = 42;
      init_once_done(&g, ok);
   }
   return (void *)42;                        /* the initialiser itself */
}

int main(void)
{
   pthread_t t[8];
   int i, bad = 0;
   /* 1: one initialiser, all see the result */
   for (i = 0; i < 8; i++) pthread_create(&t[i], 0, racer, 0);
   for (i = 0; i < 8; i++)
   {
      void *r;
      pthread_join(t[i], &r);
      if ((long)r != 42) bad++;   /* every waiter saw the finished state */
   }
   assert(ran == 1 && bad == 0);
   /* 2: a failed bring-up: the next caller does it again */
   init_once_reset(&g); ran = 0; shared_state = 0; fail_first = 1;
   for (i = 0; i < 8; i++) pthread_create(&t[i], 0, racer, 0);
   for (i = 0; i < 8; i++) pthread_join(t[i], 0);
   assert(ran == 2 && shared_state == 42);
   /* 3: reset (term) and again */
   init_once_reset(&g); ran = 0;
   assert(init_once_begin(&g, 0) == 1);
   init_once_done(&g, 1);
   assert(init_once_begin(&g, 0) == 0);
   puts("init once: ok");
   return 0;
}
