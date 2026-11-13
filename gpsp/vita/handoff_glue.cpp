/* See handoff_glue.h.  playerkit's mailbox (pk_handoff, MIT) compiled into
 * this translation unit, with the console's semaphores and drawing thread.
 * No C++ runtime: the program links with the C driver, so the two
 * allocations pk_handoff makes (operator new / delete of its state) are
 * malloc and free here; no static constructor runs (placement new on use). */
#include "handoff_glue.h"
#include "plat.h"
#include "../../playerkit/src/pk_handoff.cpp"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#endif

void *operator new(size_t n) { return malloc(n ? n : 1); }
void *operator new[](size_t n) { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* the largest picture: the NES wide view, 256 + 2 x 96 columns by 240 */
#define HO_PIX_BYTES (448u * 240u * 2u)

namespace {

#ifdef __vita__
struct Sys : pk::HandoffSys {
   uint64_t now_us() { return sceKernelGetProcessTimeWide(); }
   void *sem_create()
   {
      SceUID id = sceKernelCreateSema("gbadhoc_ho", 0, 0, 1 << 16, NULL);
      return id < 0 ? NULL : (void *)(intptr_t)id;
   }
   void sem_destroy(void *s) { sceKernelDeleteSema((SceUID)(intptr_t)s); }
   void sem_post(void *s) { sceKernelSignalSema((SceUID)(intptr_t)s, 1); }
   bool sem_wait(void *s, uint32_t timeout_us)
   {
      SceUInt t = timeout_us;
      return sceKernelWaitSema((SceUID)(intptr_t)s, 1,
                               timeout_us == 0xFFFFFFFFu ? NULL : &t) >= 0;
   }
};
#else
/* The twin: POSIX semaphores, the run's own (simulated) clock, so the
 * census counts game time the way the console's does. */
struct Sys : pk::HandoffSys {
   uint64_t now_us() { return plat_time_us(); }
   void *sem_create()
   {
      sem_t *s = (sem_t *)malloc(sizeof(sem_t));
      if (s && sem_init(s, 0, 0) != 0) { free(s); s = NULL; }
      return s;
   }
   void sem_destroy(void *s) { ::sem_destroy((sem_t *)s); free(s); }
   void sem_post(void *s) { ::sem_post((sem_t *)s); }
   bool sem_wait(void *s, uint32_t timeout_us)
   {
      if (timeout_us == 0xFFFFFFFFu) {
         while (::sem_wait((sem_t *)s) != 0 && errno == EINTR) {}
         return true;
      }
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      const uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)timeout_us * 1000u;
      ts.tv_sec += (time_t)(ns / 1000000000u);
      ts.tv_nsec = (long)(ns % 1000000000u);
      int r;
      while ((r = ::sem_timedwait((sem_t *)s, &ts)) != 0 && errno == EINTR) {}
      return r == 0;
   }
};
#endif

alignas(pk::Handoff) unsigned char g_box_mem[sizeof(pk::Handoff)];
alignas(Sys) unsigned char g_sys_mem[sizeof(Sys)];
pk::Handoff *g_box;
Sys *g_sys;
ho_draw_fn g_draw;
size_t g_extra;            /* the host's bytes, at the start of each buffer */
int g_running, g_threaded, g_enabled = 1;
pk::HandoffFrame *g_staged;
volatile unsigned g_guard_waits;
uint64_t g_draw_sum;          /* the drawing side's work, in total */
uint32_t g_draw_n;
char g_where[48] = "off";
#ifdef __vita__
SceUID g_thid = -1;
volatile int g_draw_tid = -1;
#else
pthread_t g_th;
volatile int g_have_th;
unsigned g_slow_us;
#endif

void draw_one(pk::HandoffFrame *f)
{
   const uint64_t t0 = g_sys->now_us();
   g_draw((const uint16_t *)(f->data + g_extra), (unsigned)f->w, (unsigned)f->h,
          (size_t)f->pitch, f->seq, f->user, g_extra ? f->data : NULL);
#ifndef __vita__
   if (g_threaded && g_slow_us)
      usleep(g_slow_us);
   /* a twin's clock is the emulating thread's: the drawing thread's time on
    * it is not its work (and would make the run depend on the scheduler) */
   const uint32_t d = g_threaded ? 0u : (uint32_t)(g_sys->now_us() - t0);
#else
   const uint64_t dt = g_sys->now_us() - t0;
   const uint32_t d = dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dt;
#endif
   __atomic_add_fetch(&g_draw_sum, (uint64_t)d, __ATOMIC_RELAXED);
   __atomic_add_fetch(&g_draw_n, 1u, __ATOMIC_RELAXED);
   g_box->release(f, d);
}

#ifdef __vita__
int draw_entry(SceSize, void *)
{
   g_draw_tid = sceKernelGetThreadId();
   for (;;) {
      pk::HandoffFrame *f = g_box->take();
      if (!f) break;
      draw_one(f);
   }
   return sceKernelExitThread(0);
}
#else
void *draw_entry(void *)
{
   for (;;) {
      pk::HandoffFrame *f = g_box->take();
      if (!f) break;
      draw_one(f);
   }
   return NULL;
}
#endif

bool on_draw_thread()
{
#ifdef __vita__
   return g_thid >= 0 && sceKernelGetThreadId() == g_draw_tid;
#else
   return g_have_th && pthread_equal(pthread_self(), g_th);
#endif
}

}  // namespace

extern "C" int ho_start(uint32_t frame_us, size_t extra_bytes, ho_draw_fn draw,
                        int draw_core, int draw_prio)
{
   pk::HandoffConfig c;
   if (g_running)
      ho_stop();
   if (!draw)
      return 0;
   g_sys = new (g_sys_mem) Sys();
   g_box = new (g_box_mem) pk::Handoff();
   g_extra = (extra_bytes + 15u) & ~(size_t)15u;
   g_draw = draw;
   c.frame_bytes = (uint32_t)(g_extra + HO_PIX_BYTES);
   c.refresh_us = 16683;
   c.frame_us = frame_us ? frame_us : 16743;
   if (!g_box->init(c, g_sys))
   {
      g_box->~Handoff();
      g_box = NULL;
      snprintf(g_where, sizeof(g_where), "NOT started (memory)");
      return 0;
   }
   g_staged = NULL;
   g_enabled = 1;
#ifdef __vita__
   {
      const int core = draw_core >= 0 && draw_core <= 2 ? draw_core : 1;
      const int prio = draw_prio > 0x40 && draw_prio < 0xF0 ? draw_prio : 0x60;
      const unsigned mask = core == 0 ? SCE_KERNEL_CPU_MASK_USER_0
                          : core == 2 ? SCE_KERNEL_CPU_MASK_USER_2
                                      : SCE_KERNEL_CPU_MASK_USER_1;
      g_draw_tid = -1;
      g_thid = sceKernelCreateThread("gbadhoc_draw", draw_entry, prio, 0x20000,
                                     0, (int)mask, NULL);
      if (g_thid >= 0 && sceKernelStartThread(g_thid, 0, NULL) < 0)
      {
         sceKernelDeleteThread(g_thid);
         g_thid = -1;
      }
      if (g_thid < 0)
      {
         g_box->shutdown();
         g_box->~Handoff();
         g_box = NULL;
         snprintf(g_where, sizeof(g_where), "NOT started (thread)");
         return 0;
      }
      /* the thread says who it is before anything is committed: wait */
      while (g_draw_tid < 0)
         sceKernelDelayThread(100);
      plat_thread_watch("draw", g_thid);
      g_threaded = 1;
      snprintf(g_where, sizeof(g_where), "core %d prio 0x%x", core, prio);
   }
#else
   (void)draw_core; (void)draw_prio;
   {
      const char *e = getenv("GBADHOC_HANDOFF_THREAD");
      const char *s = getenv("GBADHOC_HANDOFF_SLOW_US");
      g_slow_us = s ? (unsigned)atoi(s) : 0;
      g_threaded = e && atoi(e) > 0;
      g_have_th = 0;
      if (g_threaded)
      {
         if (pthread_create(&g_th, NULL, draw_entry, NULL) != 0)
            g_threaded = 0;
         else
            g_have_th = 1;
      }
      snprintf(g_where, sizeof(g_where), "%s", g_threaded ? "thread (twin)"
                                                          : "inline (twin)");
   }
#endif
   g_running = 1;
   return 1;
}

extern "C" void ho_stop(void)
{
   if (!g_running)
      return;
   if (g_staged)
      ho_abandon();
   g_box->flush();
   g_box->shutdown();
#ifdef __vita__
   if (g_thid >= 0)
   {
      sceKernelWaitThreadEnd(g_thid, NULL, NULL);
      sceKernelDeleteThread(g_thid);
      g_thid = -1;
      g_draw_tid = -1;
   }
#else
   if (g_have_th)
   {
      pthread_join(g_th, NULL);
      g_have_th = 0;
   }
#endif
   g_box->~Handoff();
   g_box = NULL;
   g_running = 0;
   g_threaded = 0;
   snprintf(g_where, sizeof(g_where), "off");
}

extern "C" int ho_running(void) { return g_running; }
extern "C" int ho_active(void) { return g_running && g_enabled; }
extern "C" int ho_threaded(void) { return g_running && g_threaded; }
extern "C" const char *ho_where(void) { return g_where; }

extern "C" void ho_set_enabled(int on)
{
   if (!g_running || (on != 0) == (g_enabled != 0))
      return;
   if (!on && g_staged)
      ho_abandon();
   g_box->set_enabled(on != 0);
   g_enabled = on != 0;
}

extern "C" int ho_stage(const uint16_t *pix, unsigned w, unsigned h,
                        size_t pitch, uint32_t seq, int user)
{
   pk::HandoffFrame *f;
   unsigned y;
   if (!ho_active() || !pix || !w || !h || (size_t)w * h * 2u > HO_PIX_BYTES)
      return 0;
   if (g_staged)                 /* (a second picture in one pass: the newer) */
      ho_abandon();
   f = g_box->begin_write();
   if (!f)
      return 0;
   for (y = 0; y < h; y++)
      memcpy(f->data + g_extra + (size_t)y * w * 2u,
             (const uint8_t *)pix + (size_t)y * pitch, (size_t)w * 2u);
   f->w = (int)w;
   f->h = (int)h;
   f->pitch = (int)(w * 2u);
   f->seq = seq;
   f->user = user;
   g_staged = f;
   return 1;
}

extern "C" int ho_staged(void) { return g_staged != NULL; }
extern "C" void *ho_staged_extra(void)
{
   return g_staged && g_extra ? g_staged->data : NULL;
}

extern "C" void ho_commit(uint32_t emu_us)
{
   if (!g_staged)
      return;
   g_staged->emu_us = emu_us;
   g_box->commit(g_staged);
   g_staged = NULL;
}

extern "C" void ho_abandon(void)
{
   if (!g_staged)
      return;
   g_box->abandon(g_staged);
   g_staged = NULL;
}

extern "C" void ho_consume_inline(void)
{
   pk::HandoffFrame *f;
   if (!g_running || g_threaded)
      return;
   while ((f = g_box->try_take()) != NULL)
      draw_one(f);
}

extern "C" void ho_note_emulated(uint32_t n) { if (g_running) g_box->note_emulated(n); }
extern "C" void ho_set_pace(int half)
{
   if (g_running) g_box->set_pace_mode(half ? pk::PACE_HALF : pk::PACE_FULL);
}
extern "C" void ho_set_force_drop(int on) { if (g_running) g_box->set_force_drop(on != 0); }
extern "C" int ho_poll_cost(uint32_t *c) { return g_running && g_box->poll_cost(c); }
extern "C" int ho_flush(void)
{
   if (!g_running || on_draw_thread())
      return 1;
   ho_consume_inline();
   return g_box->flush(2000000);
}
extern "C" int ho_discard(void)
{
   if (!g_running || on_draw_thread())
      return 1;
   return g_box->discard(2000000);
}
extern "C" void ho_time_baseline(void) { if (g_running) g_box->time_baseline(); }
extern "C" int ho_census_line(char *buf, size_t n)
{
   return g_running && g_box->census_line(g_sys->now_us(), buf, n);
}

extern "C" void ho_guard(void)
{
   if (!g_running || !g_threaded || on_draw_thread())
      return;
   /* only when something is queued or being drawn does it wait */
   if (!g_box->flush(0))
   {
      g_guard_waits++;
      g_box->flush(2000000);
   }
}

extern "C" unsigned ho_guard_waits(void) { return g_guard_waits; }
extern "C" uint32_t ho_shown_total(void)
{
   return g_running ? g_box->census().shown : 0;
}

extern "C" void ho_draw_stats(uint64_t *sum_us, uint32_t *n)
{
   *sum_us = __atomic_load_n(&g_draw_sum, __ATOMIC_RELAXED);
   *n = __atomic_load_n(&g_draw_n, __ATOMIC_RELAXED);
}
