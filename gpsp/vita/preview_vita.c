/* A newest-request mailbox and two immutable CPU frames, never GPU objects. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <string.h>
#include "clip_vita.h"
#include "preview_vita.h"
#include "plat.h"

#define PV_SLOTS 2
#define PV_READ (128u * 1024u)
#define PV_PATH_MAX 512
#define PV_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define PV_STORE(x, value) __atomic_store_n(&(x), (value), __ATOMIC_RELEASE)

enum { PV_FREE = 0, PV_WRITING, PV_READY, PV_READING };
typedef struct pv_request
{
   uint32_t token;
   char path[PV_PATH_MAX];
} pv_request;
typedef struct pv_slot
{
   int state;
   clip_preview_frame frame;
   uint16_t pixels[CLIP_W_MAX * CLIP_H_MAX];
} pv_slot;

static pv_request *g_pending;
static pv_slot g_slots[PV_SLOTS];
static uint32_t g_wanted;
static uint32_t g_restart;
static int g_quit, g_done, g_started, g_failed;
static unsigned g_diag_seq, g_diag_token, g_diag_phase, g_diag_since;
static unsigned g_open_us, g_read_us, g_decode_us, g_published, g_stale, g_bad;

static void pv_phase(unsigned phase, unsigned token)
{
   if (PV_LOAD(g_diag_phase) == phase && PV_LOAD(g_diag_token) == token) return;
   __atomic_add_fetch(&g_diag_seq, 1, __ATOMIC_ACQ_REL);
   PV_STORE(g_diag_token, token);
   PV_STORE(g_diag_since, (unsigned)plat_time_us());
   PV_STORE(g_diag_phase, phase);
   __atomic_add_fetch(&g_diag_seq, 1, __ATOMIC_RELEASE);
}
static void pv_cost(unsigned *maximum, uint64_t started)
{
   unsigned elapsed = (unsigned)(plat_time_us() - started);
   if (elapsed > PV_LOAD(*maximum)) PV_STORE(*maximum, elapsed);
}
void clip_preview_get_diag(clip_preview_diag *out)
{
   unsigned i, seq, since = 0;
   if (!out) return;
   memset(out, 0, sizeof(*out));
   out->wanted = PV_LOAD(g_wanted);
   for (i = 0; i < 4; i++)
   {
      seq = PV_LOAD(g_diag_seq);
      if (seq & 1) continue;
      out->token = PV_LOAD(g_diag_token);
      out->phase = PV_LOAD(g_diag_phase);
      since = PV_LOAD(g_diag_since);
      if (seq == PV_LOAD(g_diag_seq)) { out->coherent = 1; break; }
   }
   if (out->coherent && since) out->age_us = (unsigned)plat_time_us() - since;
   out->open_us = PV_LOAD(g_open_us);
   out->read_us = PV_LOAD(g_read_us);
   out->decode_us = PV_LOAD(g_decode_us);
   out->published = PV_LOAD(g_published);
   out->stale = PV_LOAD(g_stale);
   out->bad = PV_LOAD(g_bad);
   for (i = 0; i < PV_SLOTS; i++)
   {
      int state = PV_LOAD(g_slots[i].state);
      out->ready += state == PV_READY;
      out->pinned += state == PV_READING;
   }
}

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
static SceUID g_thread = -1, g_wake = -1;
static void pv_wake(void)
{
   if (g_wake >= 0)
      sceKernelSignalSema(g_wake, 1);
}
static void pv_wait(void)
{
   SceUInt timeout = 10000;
   sceKernelWaitSema(g_wake, 1, &timeout);
}
#else
#include <pthread.h>
#include <time.h>
static pthread_t g_thread;
static pthread_mutex_t g_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wait_cond = PTHREAD_COND_INITIALIZER;
static void pv_wake(void) { pthread_cond_signal(&g_wait_cond); }
static void pv_wait(void)
{
   struct timespec until;
   clock_gettime(CLOCK_REALTIME, &until);
   until.tv_nsec += 10000000;
   if (until.tv_nsec >= 1000000000)
   {
      until.tv_sec++;
      until.tv_nsec -= 1000000000;
   }
   pthread_mutex_lock(&g_wait_mutex);
   pthread_cond_timedwait(&g_wait_cond, &g_wait_mutex, &until);
   pthread_mutex_unlock(&g_wait_mutex);
}
#endif

static int pv_claim(void)
{
   int i;
   for (i = 0; i < PV_SLOTS; i++)
   {
      int free_state = PV_FREE;
      if (__atomic_compare_exchange_n(&g_slots[i].state, &free_state, PV_WRITING,
                                      0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         return i;
   }
   return -1;
}

static void pv_run(void)
{
   pv_request *job = NULL;
   clip_player *player = NULL;
   unsigned sequence = 0;
   int terminal = 0;
   while (!PV_LOAD(g_quit))
   {
      pv_request *next = __atomic_exchange_n(&g_pending, NULL, __ATOMIC_ACQ_REL);
      int slot, ended = 0, bad = 0;
      unsigned ticks = 0;
      const uint16_t *pixels = NULL;
      pv_slot *result;
      if (next || (job && job->token != PV_LOAD(g_wanted)))
      {
         pv_phase(CLIP_DIAG_CLOSE, job ? job->token : 0);
         clip_close(player);
         player = NULL;
         free(job);
         job = next;
         sequence = 0;
         terminal = 0;
         if (job && job->token == PV_LOAD(g_wanted))
         {
            uint64_t t = plat_time_us();
            pv_phase(CLIP_DIAG_OPEN, job->token);
            player = clip_open(job->path);
            pv_cost(&g_open_us, t);
         }
      }
      if (__atomic_exchange_n(&g_restart, 0, __ATOMIC_ACQ_REL) ==
          (job ? job->token : 0) && job && player && terminal &&
          job->token == PV_LOAD(g_wanted))
      {
         clip_restart(player);
         sequence = 0;
         terminal = 0;
      }
      if (!job || job->token != PV_LOAD(g_wanted) || terminal)
      {
         pv_phase(CLIP_DIAG_IDLE, job ? job->token : 0);
         pv_wait();
         continue;
      }
      slot = pv_claim();
      if (slot < 0)
      {
         pv_phase(CLIP_DIAG_SLOTS, job->token);
         pv_wait();
         continue;
      }
      result = &g_slots[slot];
      if (!player)
         ended = bad = 1;
      else
      {
         uint64_t t = plat_time_us();
         int rc;
         pv_phase(CLIP_DIAG_READ, job->token);
         rc = clip_load(player, PV_READ);
         pv_cost(&g_read_us, t);
         if (rc < 0) ended = bad = 1;
         else if (job->token == PV_LOAD(g_wanted) && !PV_LOAD(g_quit))
         {
            t = plat_time_us();
            pv_phase(CLIP_DIAG_DECODE, job->token);
            pixels = clip_next(player, &ticks, &ended);
            pv_cost(&g_decode_us, t);
            bad = ended && clip_pos(player) < clip_frames(player);
         }
      }
      if (job->token != PV_LOAD(g_wanted) || PV_LOAD(g_quit) || (!pixels && !ended))
      {
         if (job->token != PV_LOAD(g_wanted))
            __atomic_add_fetch(&g_stale, 1, __ATOMIC_RELAXED);
         PV_STORE(result->state, PV_FREE);
         continue;
      }
      memset(&result->frame, 0, sizeof(result->frame));
      result->frame.token = job->token;
      result->frame.w = clip_w(player);
      result->frame.h = clip_h(player);
      result->frame.ticks = ticks;
      result->frame.us_per_tick = clip_us_per_tick(player);
      result->frame.frames = clip_frames(player);
      result->frame.sequence = sequence++;
      result->frame.ended = ended;
      result->frame.bad = bad;
      result->frame.slot = (unsigned)slot;
      result->frame.pixels = pixels ? result->pixels : NULL;
      if (pixels)
         memcpy(result->pixels, pixels,
                (size_t)result->frame.w * result->frame.h * sizeof(uint16_t));
      PV_STORE(result->state, PV_READY);
      __atomic_add_fetch(&g_published, 1, __ATOMIC_RELAXED);
      if (bad) __atomic_add_fetch(&g_bad, 1, __ATOMIC_RELAXED);
      /* Cancellation can race publication, but never a slot already pinned. */
      if (job->token != PV_LOAD(g_wanted))
      {
         int ready = PV_READY;
         if (__atomic_compare_exchange_n(&result->state, &ready, PV_FREE, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            __atomic_add_fetch(&g_stale, 1, __ATOMIC_RELAXED);
      }
      if (ended)
         terminal = 1;
   }
   clip_close(player);
   free(job);
   free(__atomic_exchange_n(&g_pending, NULL, __ATOMIC_ACQ_REL));
   PV_STORE(g_done, 1);
}

#ifdef __vita__
static int pv_entry(SceSize n, void *p)
{
   (void)n; (void)p;
   pv_run();
   return 0;
}
#else
static void *pv_entry(void *p)
{
   (void)p;
   pv_run();
   return NULL;
}
#endif

int clip_preview_start(void)
{
   if (g_started)
      return PV_LOAD(g_quit) ? -1 : 0;
   if (g_failed)
      return -1;
#ifdef __vita__
   g_wake = sceKernelCreateSema("gbadhoc_preview", 0, 0, 1, NULL);
   if (g_wake < 0)
      goto fail;
   g_thread = sceKernelCreateThread("gbadhoc_preview", pv_entry, 0xB8,
                                    0x10000, 0, 0, NULL);
   if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0)
   {
      if (g_thread >= 0)
         sceKernelDeleteThread(g_thread);
      sceKernelDeleteSema(g_wake);
      g_thread = g_wake = -1;
      goto fail;
   }
   plat_thread_watch("preview", g_thread);
#else
   if (pthread_create(&g_thread, NULL, pv_entry, NULL) != 0)
      goto fail;
#endif
   g_started = 1;
   return 0;
fail:
   g_failed = 1;
   return -1;
}

void clip_preview_cancel(uint32_t token)
{
   int i;
   PV_STORE(g_wanted, token);
   PV_STORE(g_restart, 0);
   free(__atomic_exchange_n(&g_pending, NULL, __ATOMIC_ACQ_REL));
   for (i = 0; i < PV_SLOTS; i++)
   {
      int ready = PV_READY;
      if (__atomic_compare_exchange_n(&g_slots[i].state, &ready, PV_FREE, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         __atomic_add_fetch(&g_stale, 1, __ATOMIC_RELAXED);
   }
   pv_wake();
}

int clip_preview_restart(uint32_t token)
{
   if (!token || !g_started || PV_LOAD(g_quit) || token != PV_LOAD(g_wanted))
      return -1;
   PV_STORE(g_restart, token);
   pv_wake();
   return 0;
}

int clip_preview_request(uint32_t token, const char *path)
{
   pv_request *request;
   size_t n = path ? strlen(path) : 0;
   clip_preview_cancel(token);
   if (!token || !n || n >= PV_PATH_MAX || clip_preview_start() != 0)
      return -1;
   request = (pv_request *)malloc(sizeof(*request));
   if (!request)
      return -1;
   request->token = token;
   memcpy(request->path, path, n + 1);
   free(__atomic_exchange_n(&g_pending, request, __ATOMIC_ACQ_REL));
   pv_wake();
   return 0;
}

int clip_preview_take(uint32_t token, clip_preview_frame *out)
{
   int i, chosen = -1;
   for (i = 0; i < PV_SLOTS; i++)
   {
      int ready = PV_READY;
      if (!__atomic_compare_exchange_n(&g_slots[i].state, &ready, PV_READING, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         continue;
      if (g_slots[i].frame.token != token)
         PV_STORE(g_slots[i].state, PV_FREE);
      else if (chosen < 0)
         chosen = i;
      else if (g_slots[i].frame.sequence < g_slots[chosen].frame.sequence)
      {
         PV_STORE(g_slots[chosen].state, PV_READY);
         chosen = i;
      }
      else
         PV_STORE(g_slots[i].state, PV_READY);
   }
   if (chosen < 0)
      return 0;
   *out = g_slots[chosen].frame;
   return 1;
}

void clip_preview_release(unsigned slot)
{
   if (slot < PV_SLOTS)
   {
      int reading = PV_READING;
      if (__atomic_compare_exchange_n(&g_slots[slot].state, &reading, PV_FREE, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
         pv_wake();
   }
}

int clip_preview_shutdown(unsigned timeout_us)
{
   if (!g_started)
      return 1;
   PV_STORE(g_quit, 1);
   pv_wake();
#ifdef __vita__
   {
      SceUInt timeout = timeout_us;
      if (sceKernelWaitThreadEnd(g_thread, NULL, &timeout) < 0)
         return 0;
      sceKernelDeleteThread(g_thread);
      sceKernelDeleteSema(g_wake);
      g_thread = g_wake = -1;
   }
#else
   {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_sec += timeout_us / 1000000u;
      until.tv_nsec += (long)(timeout_us % 1000000u) * 1000;
      if (until.tv_nsec >= 1000000000)
      {
         until.tv_sec++;
         until.tv_nsec -= 1000000000;
      }
#ifdef __linux__
      if (pthread_timedjoin_np(g_thread, NULL, &until) != 0)
         return 0;
#else
      unsigned waited = 0;
      while (!PV_LOAD(g_done) && waited < timeout_us)
      {
         plat_thread_sleep_us(1000);
         waited += 1000;
      }
      if (!PV_LOAD(g_done))
         return 0;
      pthread_join(g_thread, NULL);
#endif
   }
#endif
   g_started = 0;
   g_failed = 1;
   return 1;
}
