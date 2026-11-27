/* B12: the event log is written by more than one thread.
 *
 * The ring behind fe_evt()/fe_log() was a single-producer ring, but on the
 * Vita the I/O worker logs (sram_flush ...) while the emulation thread logs.
 * Two producers raced on the write index: lines were overwritten, published
 * twice or garbled, and the counters disagreed.  Four producer threads and a
 * consumer thread hammer the ring here; afterwards
 *
 *   - every line in the file is whole ("EVT t<id> n=<seq> <pad>"),
 *   - no line appears twice,
 *   - lines kept + fe_evt_drops() == lines produced (nothing vanishes
 *     unaccounted: a contended or full ring drops a line AND counts it).
 *
 * Production fe_evt.c is compiled in.  Host (pthreads), not a Vita run. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../frontend-common/fe_evt.c"

#define THREADS 4
#define PER     5000

static int g_stop;
static void no_wake(void) { }

static void *producer(void *p)
{
   long id = (long)(size_t)p;
   int n;
   for (n = 0; n < PER; n++)
   {
      fe_evt("t%ld n=%d %s", id, n, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
      if ((n & 3) == 3)                 /* paced: the ring is not the point */
      {
         struct timespec ts = { 0, 20000 };
         nanosleep(&ts, NULL);
      }
   }
   return NULL;
}

static void *consumer(void *p)
{
   (void)p;
   while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE))
   {
      struct timespec ts = { 0, 50000 };
      fe_evt_service();
      nanosleep(&ts, NULL);
   }
   return NULL;
}

int main(int argc, char **argv)
{
   const char *logp = argc > 1 ? argv[1] : "/tmp/t_evt_threads.log";
   pthread_t pt[THREADS], ct;
   static unsigned char seen[THREADS][PER];
   char line[600];
   unsigned kept = 0, bad = 0, dup = 0, total = THREADS * PER;
   long i;
   FILE *f;

   assert(fe_evt_init(logp, 0) == 0);
   fe_evt_set_async(no_wake);
   pthread_create(&ct, NULL, consumer, NULL);
   for (i = 0; i < THREADS; i++)
      pthread_create(&pt[i], NULL, producer, (void *)(size_t)i);
   for (i = 0; i < THREADS; i++)
      pthread_join(pt[i], NULL);
   __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
   pthread_join(ct, NULL);
   fe_evt_set_async(NULL);
   fe_evt("final");
   fe_evt_close();

   f = fopen(logp, "r");
   assert(f);
   while (fgets(line, sizeof(line), f))
   {
      long id; int n;
      char pad[64];
      if (!strncmp(line, "EVT evt_gap", 11) || !strncmp(line, "EVT final", 9))
         continue;
      if (sscanf(line, "EVT t%ld n=%d %63s", &id, &n, pad) != 3 || id < 0 ||
          id >= THREADS || n < 0 || n >= PER ||
          strcmp(pad, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") != 0)
      {
         bad++;
         continue;
      }
      if (seen[id][n]) dup++;
      seen[id][n] = 1;
      kept++;
   }
   fclose(f);
   printf("  evt threads: %u produced, %u kept, %u dropped (counted), "
          "%u garbled, %u duplicated\n", total, kept, fe_evt_drops(), bad, dup);
   assert(bad == 0);
   assert(dup == 0);
   assert(kept + fe_evt_drops() == total);
   return 0;
}
