#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include "menu_load.h"
#include "plat.h"

#define ML_GET(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define ML_SET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
enum { MS_FREE, MS_WRITING, MS_READY, MS_PINNED, MS_DISCARD };
enum { MB_FREE = 0, MB_BUSY = 1 };
#define ML_BATCH_POOL 4
typedef struct ml_batch
{
   unsigned revision, count;
   menu_load_job jobs[1 + ML_NEIGHBORS];
} ml_batch;
typedef struct ml_slot
{
   int state;
   unsigned revision;
   menu_load_result result;
} ml_slot;
/* Fixed static batch pool + MS_DISCARD keep the UI thread completely free of
 * malloc()/free(), preventing priority inversion on newlib's global heap lock
 * while background workers decode PNGs. */
static ml_batch g_batches[ML_BATCH_POOL];
static int g_batch_state[ML_BATCH_POOL];
static int g_pending_idx = -1;
static ml_slot g_slots[ML_SLOTS];
static menu_load_fn g_load;
static unsigned g_revision, g_current_revision;
static int g_started, g_failed, g_quit, g_done, g_paused;
static unsigned g_phase, g_kind, g_submitted, g_published, g_stale;
static unsigned g_errors, g_dropped, g_job_us, g_wait_us;

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
static SceUID g_thread = -1, g_wake = -1;
static void ml_wake(void)
{
   if (g_wake >= 0) sceKernelSignalSema(g_wake, 1);
}
static void ml_wait(void)
{
   SceUInt timeout = 10000;
   sceKernelWaitSema(g_wake, 1, &timeout);
}
#else
#include <pthread.h>
#include <time.h>
static pthread_t g_thread;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static void ml_wake(void) { pthread_cond_signal(&g_cond); }
static void ml_wait(void)
{
   struct timespec t;
   clock_gettime(CLOCK_REALTIME, &t);
   t.tv_nsec += 10000000;
   if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
   pthread_mutex_lock(&g_mutex);
   pthread_cond_timedwait(&g_cond, &g_mutex, &t);
   pthread_mutex_unlock(&g_mutex);
}
#endif

static int ml_cancelled(void)
{
   return ML_GET(g_quit) || ML_GET(g_paused) ||
          g_current_revision != ML_GET(g_revision);
}
static void ml_free_slot(unsigned i)
{
   free(g_slots[i].result.pixels);
   g_slots[i].result.pixels = NULL;
   ML_SET(g_slots[i].state, MS_FREE);
}
static void ml_sweep_discards(void)
{
   for (unsigned i = 0; i < ML_SLOTS; i++)
   {
      int state = MS_DISCARD;
      if (__atomic_compare_exchange_n(&g_slots[i].state, &state, MS_WRITING,
                                      0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         ml_free_slot(i);
   }
}
static int ml_claim(int image)
{
   unsigned first = image ? ML_META_SLOTS : 0;
   unsigned end = image ? ML_SLOTS : ML_META_SLOTS, i;
   for (i = first; i < end; i++)
   {
      int state = MS_FREE;
      if (__atomic_compare_exchange_n(&g_slots[i].state, &state, MS_WRITING,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         return (int)i;
      if (state == MS_DISCARD)
      {
         if (__atomic_compare_exchange_n(&g_slots[i].state, &state, MS_WRITING,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         {
            free(g_slots[i].result.pixels);
            g_slots[i].result.pixels = NULL;
            return (int)i;
         }
      }
      if (state == MS_READY && g_slots[i].revision != ML_GET(g_revision))
      {
         state = MS_READY;
         if (__atomic_compare_exchange_n(&g_slots[i].state, &state, MS_WRITING,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         {
            free(g_slots[i].result.pixels);
            g_slots[i].result.pixels = NULL;
            __atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
            return (int)i;
         }
      }
   }
   return -1;
}
static void ml_run(void)
{
   int active_idx = -1;
   while (!ML_GET(g_quit))
   {
      int next_idx;
      ml_batch *batch;
      unsigned j = 0, kind = 0;
      int slot;
      uint64_t t;
      ml_sweep_discards();
      next_idx = __atomic_exchange_n(&g_pending_idx, -1, __ATOMIC_ACQ_REL);
      if (next_idx >= 0)
      {
         if (active_idx >= 0)
            ML_SET(g_batch_state[active_idx], MB_FREE);
         active_idx = next_idx;
      }
      batch = active_idx >= 0 ? &g_batches[active_idx] : NULL;
      if (batch && batch->revision != ML_GET(g_revision))
      {
         ML_SET(g_batch_state[active_idx], MB_FREE);
         active_idx = -1;
         batch = NULL;
      }
      if (!ML_GET(g_paused) && batch)
         for (j = 0; j < batch->count; j++)
            if (batch->jobs[j].tasks)
            {
               unsigned tasks = batch->jobs[j].tasks;
               kind = (tasks & ML_TILE) ? ML_TILE :
                      tasks & (0u - tasks);
               break;
            }
      if (!kind)
      {
         ML_SET(g_phase, ML_GET(g_paused) ? 3 : 0);
         ml_wait();
         continue;
      }
      slot = ml_claim(kind != ML_META);
      if (slot < 0)
      {
         ML_SET(g_phase, 2);
         t = plat_time_us();
         ml_wait();
         unsigned elapsed = (unsigned)(plat_time_us() - t);
         if (elapsed > ML_GET(g_wait_us)) ML_SET(g_wait_us, elapsed);
         continue;
      }
      ml_slot *s = &g_slots[slot];
      memset(&s->result, 0, sizeof(s->result));
      s->revision = batch->revision;
      s->result.job = batch->jobs[j];
      s->result.kind = kind;
      s->result.slot = (unsigned)slot;
      s->result.meta.battery = ML_UNKNOWN;
      for (unsigned k = 0; k < ML_STATE_SLOTS; k++)
         s->result.meta.state[k] = ML_UNKNOWN;
      g_current_revision = batch->revision;
      ML_SET(g_phase, 1); ML_SET(g_kind, kind);
      t = plat_time_us();
      s->result.status = ml_cancelled() ? ML_CANCELLED :
                         g_load(&s->result.job, kind, &s->result, ml_cancelled);
      unsigned elapsed = (unsigned)(plat_time_us() - t);
      if (elapsed > ML_GET(g_job_us)) ML_SET(g_job_us, elapsed);
      batch->jobs[j].tasks &= ~kind;
      if (ml_cancelled())
      {
         __atomic_add_fetch(&g_stale, 1, __ATOMIC_RELAXED);
         ml_free_slot((unsigned)slot);
         continue;
      }
      /* A processor error must finish its request; only an actual revision
       * change may silently discard work the UI still expects to receive. */
      if (s->result.status == ML_CANCELLED) s->result.status = ML_ERROR;
      if (kind != ML_META && s->result.status == ML_OK &&
          (!s->result.pixels || !s->result.w || !s->result.h ||
           s->result.w > ML_IMAGE_MAX_W || s->result.h > ML_IMAGE_MAX_H))
         s->result.status = ML_ERROR;
      if (s->result.status != ML_OK)
      {
         free(s->result.pixels); s->result.pixels = NULL;
         if (s->result.status == ML_ERROR || s->result.status == ML_INVALID)
            __atomic_add_fetch(&g_errors, 1, __ATOMIC_RELAXED);
      }
      s->result.finished_us = plat_time_us();
      __atomic_add_fetch(&g_published, 1, __ATOMIC_RELAXED);
      ML_SET(s->state, MS_READY);
   }
   if (active_idx >= 0)
      ML_SET(g_batch_state[active_idx], MB_FREE);
   {
      int rem = __atomic_exchange_n(&g_pending_idx, -1, __ATOMIC_ACQ_REL);
      if (rem >= 0)
         ML_SET(g_batch_state[rem], MB_FREE);
   }
   ml_sweep_discards();
   ML_SET(g_phase, 0);
   ML_SET(g_done, 1);
}
#ifdef __vita__
static int ml_entry(SceSize argc, void *argv)
{ (void)argc; (void)argv; ml_run(); return 0; }
#else
static void *ml_entry(void *arg) { (void)arg; ml_run(); return NULL; }
#endif

int menu_load_start(menu_load_fn load)
{
   if (ML_GET(g_quit) || ML_GET(g_failed) || !load) return -1;
   if (ML_GET(g_started)) return load == g_load ? 0 : -1;
   g_load = load;
   g_pending_idx = -1;
   memset(g_batch_state, 0, sizeof(g_batch_state));
#ifdef __vita__
   g_wake = sceKernelCreateSema("menu-local-wake", 0, 0, 1, NULL);
   if (g_wake < 0) goto fail;
   g_thread = sceKernelCreateThread("menu-local-io", ml_entry, 0xB8, 0x10000,
                                     0, 0, NULL);
   if (g_thread < 0) { sceKernelDeleteSema(g_wake); g_wake = -1; goto fail; }
   if (sceKernelStartThread(g_thread, 0, NULL) < 0)
   {
      sceKernelDeleteThread(g_thread); g_thread = -1;
      sceKernelDeleteSema(g_wake); g_wake = -1; goto fail;
   }
   plat_thread_watch("menu-local", g_thread);
#else
   if (pthread_create(&g_thread, NULL, ml_entry, NULL) != 0) goto fail;
#endif
   ML_SET(g_started, 1);
   return 0;
fail:
   ML_SET(g_failed, 1);
   return -1;
}

int menu_load_submit(const menu_load_job *jobs, unsigned count)
{
   int idx, prev;
   ml_batch *batch;
   if (!ML_GET(g_started) || ML_GET(g_quit) || ML_GET(g_paused) || !jobs ||
       !count || count > 1 + ML_NEIGHBORS) return 0;
   for (unsigned i = 0; i < count; i++)
      if (!memchr(jobs[i].rom, 0, ML_PATH) ||
          !memchr(jobs[i].root, 0, ML_PATH) ||
          !memchr(jobs[i].state1, 0, ML_PATH) ||
          !memchr(jobs[i].stem, 0, ML_STEM) ||
          jobs[i].focus != jobs[0].focus ||
          jobs[i].generation != jobs[0].generation ||
          (jobs[i].tasks & ~(((unsigned)ML_STATE0 << ML_STATE_SLOTS) - 1u)))
         return 0;
   /* Reclaim any unconsumed pending batch first, else claim a free pool slot
    * (worker holds at most 2 slots at a time, so >= 1 of 4 is always free). */
   idx = __atomic_exchange_n(&g_pending_idx, -1, __ATOMIC_ACQ_REL);
   if (idx < 0)
   {
      for (int i = 0; i < ML_BATCH_POOL; i++)
      {
         int free_st = MB_FREE;
         if (__atomic_compare_exchange_n(&g_batch_state[i], &free_st, MB_BUSY,
                                         0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         { idx = i; break; }
      }
      if (idx < 0) return 0;
   }
   batch = &g_batches[idx];
   batch->count = count;
   memcpy(batch->jobs, jobs, count * sizeof(*jobs));
   batch->revision = __atomic_add_fetch(&g_revision, 1, __ATOMIC_ACQ_REL);
   prev = __atomic_exchange_n(&g_pending_idx, idx, __ATOMIC_ACQ_REL);
   if (prev >= 0)
      ML_SET(g_batch_state[prev], MB_FREE);
   __atomic_add_fetch(&g_submitted, 1, __ATOMIC_RELAXED);
   ml_wake();
   return 1;
}
void menu_load_pause(void)
{
   int prev;
   ML_SET(g_paused, 1);
   __atomic_add_fetch(&g_revision, 1, __ATOMIC_ACQ_REL);
   prev = __atomic_exchange_n(&g_pending_idx, -1, __ATOMIC_ACQ_REL);
   if (prev >= 0)
      ML_SET(g_batch_state[prev], MB_FREE);
   /* Results of the old revision can never be taken now: hand them back to the
    * worker to free off the UI thread so pause never contends on heap locks. */
   for (unsigned i = 0; i < ML_SLOTS; i++)
   {
      int state = MS_READY;
      __atomic_compare_exchange_n(&g_slots[i].state, &state, MS_DISCARD,
                                  0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
   }
   ml_wake();
}
void menu_load_resume(void)
{
   if (!ML_GET(g_quit)) { ML_SET(g_paused, 0); ml_wake(); }
}
int menu_load_take(int image, menu_load_result *out)
{
   unsigned first = image ? ML_META_SLOTS : 0;
   unsigned end = image ? ML_SLOTS : ML_META_SLOTS;
   if (!out) return 0;
   for (unsigned i = first; i < end; i++)
   {
      int state = MS_READY;
      if (!__atomic_compare_exchange_n(&g_slots[i].state, &state, MS_PINNED,
                                      0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         continue;
      if (g_slots[i].revision != ML_GET(g_revision) || ML_GET(g_paused))
      { ML_SET(g_slots[i].state, MS_DISCARD); ml_wake(); continue; }
      *out = g_slots[i].result;
      return 1;
   }
   return 0;
}
void menu_load_release(unsigned slot)
{
   if (slot < ML_SLOTS && ML_GET(g_slots[slot].state) == MS_PINNED)
   { ML_SET(g_slots[slot].state, MS_DISCARD); ml_wake(); }
}
int menu_load_shutdown(uint32_t timeout_us)
{
   if (!ML_GET(g_started)) return 1;
   ML_SET(g_quit, 1);
   menu_load_pause();
#ifdef __vita__
   SceUInt timeout = timeout_us;
   if (sceKernelWaitThreadEnd(g_thread, NULL, &timeout) < 0) return 0;
   sceKernelDeleteThread(g_thread); g_thread = -1;
   sceKernelDeleteSema(g_wake); g_wake = -1;
#else
   struct timespec t;
   uint64_t until, now;
   clock_gettime(CLOCK_MONOTONIC, &t);
   until = (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000 + timeout_us;
   while (!ML_GET(g_done))
   {
      clock_gettime(CLOCK_MONOTONIC, &t);
      now = (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
      if (now >= until) return 0;
      struct timespec nap = { 0, 1000000 }; nanosleep(&nap, NULL);
   }
   pthread_join(g_thread, NULL);
#endif
   ML_SET(g_started, 0);
   for (unsigned i = 0; i < ML_SLOTS; i++)
   {
      int st = ML_GET(g_slots[i].state);
      if (st == MS_READY || st == MS_DISCARD) ml_free_slot(i);
   }
   return 1;
}
void menu_load_get_diag(menu_load_diag *out)
{
   if (!out) return;
   memset(out, 0, sizeof(*out));
   out->started = ML_GET(g_started); out->paused = ML_GET(g_paused);
   out->phase = ML_GET(g_phase); out->kind = ML_GET(g_kind);
   out->submitted = ML_GET(g_submitted); out->published = ML_GET(g_published);
   out->stale = ML_GET(g_stale); out->errors = ML_GET(g_errors);
   out->dropped = ML_GET(g_dropped); out->job_us = ML_GET(g_job_us);
   out->wait_us = ML_GET(g_wait_us);
   for (unsigned i = 0; i < ML_SLOTS; i++)
   {
      int state = ML_GET(g_slots[i].state);
      out->ready += state == MS_READY; out->pinned += state == MS_PINNED;
   }
}
