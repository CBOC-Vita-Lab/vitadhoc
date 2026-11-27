/* render_thread.c -- see render_thread.h. */
#include "render_thread.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pspcompat.h"          /* pspcompat_time_us */
#include "fe_evt.h"
#include "handoff_glue.h"       /* core4_*: the fourth core */

/* The core's types, for video.h's capture structure (its layout does not
 * depend on the core's build flags). */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
#include "video.h"

/* cpu.h: reg[] index of "OAM changed, sort the objects again". */
#define RT_OAM_UPDATED 25

/* ---- the emulator's renderer state (instance A) ---------------------------- */
extern u16 oam_ram[512];
extern u16 palette_ram_converted[512];
extern u8  vram[1024 * 96];
extern u8  vram_clean[];              /* gba_memory.h: 1 KiB pages, 0 = written */
extern u32 reg[64];
extern int sprite_limit;
extern u32 skip_next_frame;
extern u16 *gba_screen_pixels;
extern u32 wide_margin;
extern u16 *wide_pixels;
extern u32 wide_selftest;

/* ---- the render thread's renderer (vita/corebuild.mk CORE_R) --------------- */
extern u16 gbar_oam_ram[512];
extern u16 gbar_palette_ram_converted[512];
extern u8  gbar_vram[1024 * 96];
extern u32 gbar_reg[64];
extern int gbar_sprite_limit;
extern u32 gbar_skip_next_frame;
extern u16 *gbar_gba_screen_pixels;
extern s32 gbar_affine_reference_x[2];
extern s32 gbar_affine_reference_y[2];
void gbar_me_replay_lines(const me_capture_frame *cap, int *reloads,
                          int use_log);

/* ---- this module ------------------------------------------------------------ */

#define RT_LOG_ENTRIES 12288          /* as the PSP's: 144 KiB */
#define RT_PITCH 240                  /* common.h GBA_SCREEN_PITCH */

static me_capture_frame g_cap __attribute__((aligned(64)));
#if ME_MIDFRAME_LOG
static me_log_entry g_log[RT_LOG_ENTRIES] __attribute__((aligned(64)));
/* VRAM as the log last saw it (video.h me_vram_shadow): where the log's
 * old values of a VRAM word come from. */
static u8 g_shadow[1024 * 96] __attribute__((aligned(64)));
#endif
/* Two pictures: the one shown stays whole while the next is drawn. */
static u16 g_out[2][RT_PITCH * (160 + 1)] __attribute__((aligned(64)));
static u16 g_wide_out[(RT_PITCH + 2 * WIDE_MARGIN_MAX) * 160] __attribute__((aligned(64)));
static int g_wide_valid;
static unsigned g_wide_margin;
static int g_cur;                     /* g_out[] the thread draws next     */

static int g_mode;                    /* the mode in force (RT_*)           */
static int g_fresh;                   /* the thread's object lists are old  */
static int g_vram_all;                /* the thread's VRAM is not the game's */
static unsigned g_vram_age;           /* frames since all of it was copied   */
static unsigned g_pages;              /* VRAM pages copied (statistics)      */
static int g_posted;                  /* the thread has a picture to draw (or is drawing one) */
static int g_wd = -1;                 /* watchdog slot (the thread is on the extra core) */
static int g_late;                    /* ...and it was not done in time     */
static int g_last = 1;                  /* g_out[] holding the last whole picture */
static unsigned g_timeouts, g_stale;  /* waits that ran out; frames shown stale */
static int g_ready;                   /* the thread is running              */
static int g_failed;                  /* it could not be started: never    */
static int g_logset;

/* statistics (the frame thread's, except render_*: the thread's own) */
static volatile unsigned g_render_us, g_render_max_us;
static unsigned g_frames, g_copy_us, g_wait_us, g_wait_max_us;
static unsigned g_checked, g_bad_frames, g_bad_px;
static unsigned g_log_entries, g_log_overflows;
static unsigned g_drawn;              /* frames the thread drew, ever       */

/* ---- the thread, and its two signals ---------------------------------------- */

static void rt_work(void)
{
   const unsigned t0 = pspcompat_time_us();
   unsigned dt;
   gbar_gba_screen_pixels = g_out[g_cur];
   gbar_skip_next_frame = 0;
   gbar_affine_reference_x[0] = g_cap.affine_seed[0];
   gbar_affine_reference_x[1] = g_cap.affine_seed[1];
   gbar_affine_reference_y[0] = g_cap.affine_seed[2];
   gbar_affine_reference_y[1] = g_cap.affine_seed[3];
   gbar_reg[RT_OAM_UPDATED] = g_cap.oam_updated | (u32)g_fresh;
   g_fresh = 0;
   gbar_me_replay_lines(&g_cap, NULL, 1);
   dt = pspcompat_time_us() - t0;
   g_render_us += dt;
   if (dt > g_render_max_us)
      g_render_max_us = dt;
}

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>

static SceUID g_go = -1, g_done = -1, g_th = -1;
static volatile int g_quit;
#define rt_stall() ((void)0)    /* (the twin's stall test is not built here) */

static int rt_entry(SceSize args, void *argp)
{
   (void)args; (void)argp;
   for (;;)
   {
      sceKernelWaitSema(g_go, 1, NULL);
      if (g_quit)
         break;
      rt_stall();
      rt_work();
      core4_beat(__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST));
      core4_busy(__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST), 0);
      sceKernelSignalSema(g_done, 1);
   }
   return 0;
}

static int rt_start(void)
{
   g_go = sceKernelCreateSema("gbadhoc_rt_go", 0, 0, 1, NULL);
   g_done = sceKernelCreateSema("gbadhoc_rt_done", 0, 0, 1, NULL);
   /* Above the upscalers' helpers (who share its cores and may be in the
    * middle of a long picture), below the sound: the emulator waits for
    * this at the end of the frame. */
   g_th = sceKernelCreateThread("gbadhoc_render", rt_entry, 0x60, 0x10000, 0,
                                (int)core4_mask(C4_RENDER, 0,
                                                SCE_KERNEL_CPU_MASK_USER_1 |
                                                SCE_KERNEL_CPU_MASK_USER_2),
                                NULL);
   /* On the extra core (core4_init proved it): the thread owes the emulator
    * a picture a frame, so the watchdog watches it (a trip moves it back to
    * the user cores); a plain three-core run gets no slot, no change. */
   __atomic_store_n(&g_wd, core4_wd_add("render", C4_RENDER, 0), __ATOMIC_SEQ_CST);
   if (g_go < 0 || g_done < 0 || g_th < 0 ||
       sceKernelStartThread(g_th, 0, NULL) < 0)
      return -1;
   core4_set_thread(C4_RENDER, 0, (int)g_th);
   return 0;
}

static void rt_post(void) { core4_busy(__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST), 1); sceKernelSignalSema(g_go, 1); }
/* 1: the picture is done; 0: `us` went by first (the semaphore is not
 * consumed: the thread's signal will still be there). */
static int rt_wait_to(unsigned us)
{
   SceUInt timeout = us;
   return sceKernelWaitSema(g_done, 1, &timeout) >= 0;
}

void rt_shutdown(void)
{
   if (!g_ready)
      return;
   g_quit = 1;
   sceKernelSignalSema(g_go, 1);
   sceKernelWaitThreadEnd(g_th, NULL, NULL);
   sceKernelDeleteThread(g_th);
   g_ready = 0;
}
#else
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "host_sem.h"

static host_sem_t g_go, g_done;
static pthread_t g_th;
static volatile int g_quit;
static void rt_stall(void);

static void *rt_entry(void *arg)
{
   (void)arg;
   for (;;)
   {
      while (host_sem_wait(&g_go) != 0)
         ;
      if (g_quit)
         break;
      rt_stall();
      rt_work();
      core4_beat(__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST));
      core4_busy(__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST), 0);
      host_sem_post(&g_done);
   }
   return NULL;
}

static int rt_start(void)
{
   if (host_sem_init(&g_go, 0) != 0 || host_sem_init(&g_done, 0) != 0)
      return -1;
   __atomic_store_n(&g_wd, core4_wd_add("render", C4_RENDER, 0), __ATOMIC_SEQ_CST);   /* (the twin's fake core) */
   core4_set_thread(C4_RENDER, 0, 0);
   return pthread_create(&g_th, NULL, rt_entry, NULL) == 0 ? 0 : -1;
}

static void rt_post(void) { core4_busy(__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST), 1); host_sem_post(&g_go); }
static int rt_wait_to(unsigned us)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_nsec += (long)(us % 1000000u) * 1000L;
   ts.tv_sec += us / 1000000u + ts.tv_nsec / 1000000000L;
   ts.tv_nsec %= 1000000000L;
   for (;;)
   {
      if (host_sem_timedwait(&g_done, &ts) == 0)
         return 1;
      if (errno != EINTR)
         return 0;
   }
}
/* A twin's test of the bounded wait (app_tests.py "stall"): the thread
 * sleeps GBADHOC_STALL_MS before each picture, as one the system has not
 * scheduled would.  Never built into the VPK. */
static void rt_stall(void)
{
   static int ms = -1, left = 30;      /* the first 30 pictures */
   if (ms < 0)
   {
      const char *e = getenv("GBADHOC_STALL_MS");
      ms = e ? atoi(e) : 0;
   }
   if (ms > 0 && left-- > 0)
      usleep((useconds_t)ms * 1000u);
}

void rt_shutdown(void)
{
   if (!g_ready)
      return;
   g_quit = 1;
   host_sem_post(&g_go);
   pthread_join(g_th, NULL);
   g_ready = 0;
}
#endif

/* The wait for the thread's picture is bounded.  50 ms: the picture is 2-6 ms
 * of work, so one not finished after three refreshes means the thread has
 * lost its core (the system's, or a sibling helper's); the emulator's own
 * frame is then shown with the last whole picture of the thread's -- one
 * repeated frame -- and the late one is collected when it lands, instead of
 * the game, its sound and its input stopping for as long as the other core
 * takes. */
#define RT_WAIT_US 50000u

/* The thread's picture, if it is done: 1 collected, 0 not yet (counted). */
static int rt_collect(unsigned us)
{
   if (!g_posted)
      return 1;
   if (!rt_wait_to(us))
   {
      g_timeouts++;
      /* On the extra core, a thread that does not answer is the system
       * taking the core back: say so, and the thread is moved to the user
       * cores (handoff_glue.cpp), where it finishes this picture. */
      if (__atomic_load_n(&g_wd, __ATOMIC_SEQ_CST) >= 0 &&
          core4_mark_lost("render thread stalled"))
         __atomic_store_n(&g_wd, -1, __ATOMIC_SEQ_CST);
      return 0;
   }
   g_posted = 0;
   g_last = g_cur;                    /* whole now; the thread draws the other */
   g_cur ^= 1;
   return 1;
}

/* ---- the frame ----------------------------------------------------------------- */

void rt_frame_begin(int mode)
{
   if (g_posted)                      /* (a frame end that never came, or a late picture) */
   {
      if (rt_collect(RT_WAIT_US))
         g_late = 0;
      else
         g_late = 1;
   }
   if (mode && !g_ready && !g_failed)
   {
      if (rt_start() == 0)
         g_ready = 1;
      else
         g_failed = 1;
   }
   /* (the wide view's self-check compares its strips with the picture the
    * core drew itself, line by line) */
   if (!g_ready || (wide_margin && wide_selftest))
      mode = RT_OFF;
   if (!mode || !wide_margin || wide_margin != g_wide_margin)
   {
      g_wide_valid = 0;
      g_wide_margin = mode ? wide_margin : 0;
   }
   if (mode == g_mode)
      return;
   /* The renderer that takes over has not seen the objects move: its
    * object lists are sorted again before its first line (mode 1 leaves
    * the core's own unsorted; mode 2 sorts in both). */
   if (mode != RT_ON)
      reg[RT_OAM_UPDATED] = 1;
   if (mode)
      g_fresh = g_vram_all = 1;
   g_mode = mode;
#if ME_MIDFRAME_LOG
   if (mode && !g_logset)
   {
      /* palette, OAM and VRAM written while the lines are drawn */
      me_vram_shadow = g_shadow;
      me_log_vram_always = 1;
      me_capture_log_setup(&g_cap, g_log, RT_LOG_ENTRIES);
      g_logset = 1;
   }
#endif
   me_capture_buf = mode ? &g_cap : NULL;
   me_capture_mode = mode == RT_CHECK ? 2u : mode == RT_ON ? 1u : 0u;
}

/* Line 160 (main.c): the capture of lines 0-159 is complete, and VRAM, OAM
 * and the palette are as the lines saw them at their end.  The core calls
 * this for every machine built from it; instance B has its own (renamed). */
void gpsp_visible_done_hook(void)
{
   unsigned t0;
   if (!g_mode || skip_next_frame || g_posted)
      return;
   /* A boot that starts on a late line (cpu.cc) has no whole first
    * frame: lines 0..start-1 were never captured, so the thread would
    * draw them from empty registers.  That one partial picture is not
    * handed over (the next frame is whole). */
   if (!g_cap.line0_seen)
      return;
   g_cap.line0_seen = 0;
   t0 = pspcompat_time_us();
   /* VRAM: the pages written since the last copy (every path that writes
    * VRAM marks its page: the C stores, DMA, a state loaded, and the code
    * generator's stubs); all of it when the thread starts, and every ten
    * seconds whatever happened. */
   if (g_vram_all || ++g_vram_age >= 600)
   {
      memcpy(gbar_vram, vram, sizeof(gbar_vram));
      g_pages += 96;
      g_vram_all = 0;
      g_vram_age = 0;
   }
   else
   {
      unsigned p;
      for (p = 0; p < 96; p++)
         if (!vram_clean[p])
         {
            memcpy(gbar_vram + (p << 10), vram + (p << 10), 1024);
            g_pages++;
         }
   }
#if ME_MIDFRAME_LOG
   me_vram_map_consumed();            /* copied: the map is clean again */
#endif
   memcpy(gbar_oam_ram, oam_ram, sizeof(gbar_oam_ram));
   memcpy(gbar_palette_ram_converted, palette_ram_converted,
          sizeof(gbar_palette_ram_converted));
   gbar_sprite_limit = sprite_limit;
#if ME_MIDFRAME_LOG
   g_log_entries += g_cap.log_n;
   g_log_overflows += (g_cap.log_flags & ME_LOGF_OVERFLOW) ? 1 : 0;
#endif
   g_copy_us += pspcompat_time_us() - t0;
   g_posted = 1;
   rt_post();
}

static const uint16_t *rt_stale_return(const uint16_t *pix, int mode,
                                       int allow_compose)
{
   if (!pix || mode != RT_ON)
      return pix;
   if (wide_margin && wide_pixels && pix == wide_pixels)
   {
      if (g_wide_valid)
         return g_wide_out;
      if (allow_compose && g_drawn)
      {
         wide_compose(g_out[g_last]);
         return pix;
      }
      return NULL;
   }
   return g_out[g_last];
}

const uint16_t *rt_frame_end(const uint16_t *pix)
{
   const u16 *out;
   unsigned t0, dt;
   const int mode = g_mode;

   if (!g_posted)
      return pix;
   /* A picture from an earlier frame is still being drawn, and this
    * frame was not given to the thread (the hook saw it busy): the last
    * whole picture is shown again.  (With the core drawing too, mode 2,
    * its own is as good and is not touched.) */
   t0 = pspcompat_time_us();
   if (g_late)
   {
      if (rt_collect(RT_WAIT_US))
         g_late = 0;
      g_stale++;
      return rt_stale_return(pix, mode, 0);
   }
   if (!rt_collect(RT_WAIT_US))
   {
      g_late = 1;
      g_stale++;
      g_wait_us += pspcompat_time_us() - t0;
      return rt_stale_return(pix, mode, 1);
   }
   dt = pspcompat_time_us() - t0;
   g_wait_us += dt;
   if (dt > g_wait_max_us)
      g_wait_max_us = dt;
   g_frames++;
   g_drawn++;
   out = g_out[g_last];
   if (!pix)
      return pix;
   if (mode == RT_CHECK)
   {
      /* the core drew the frame too (its 240 pixels a line in
       * gba_screen_pixels, whether or not the wide view is on) */
      const u16 *ref = gba_screen_pixels;
      unsigned y, x, bad = 0;
      for (y = 0; y < 160; y++)
         for (x = 0; x < 240; x++)
            bad += ref[y * RT_PITCH + x] != out[y * RT_PITCH + x];
      g_checked++;
      g_bad_frames += bad ? 1 : 0;
      g_bad_px += bad;
      if (bad && g_bad_frames <= 4)
      {
         unsigned first = 0;
         for (y = 0; y < 160 && !first; y++)
            for (x = 0; x < 240; x++)
               if (ref[y * RT_PITCH + x] != out[y * RT_PITCH + x])
               {
                  first = y + 1;
                  break;
               }
         fe_evt("render_check frame=%u bad_px=%u first_line=%u log=%u",
                g_drawn, bad, first - 1, g_cap.log_n);
      }
      return pix;
   }
   /* The wide view: the margins were drawn as the lines went by; the
    * middle goes in between them now (video.h wide_compose). */
   if (wide_margin && wide_pixels && pix == wide_pixels)
   {
      wide_compose(out);
      memcpy(g_wide_out, wide_pixels, sizeof(g_wide_out));
      g_wide_valid = 1;
      return pix;
   }
   /* (anything else the core hands over -- a post-processing pass of its
    * own, which this app does not use -- was made from a picture the core
    * did not draw: the thread's is the frame) */
   return out;
}

void rt_stats(rt_stats_t *out)
{
   const unsigned n = g_frames;
   /* The bounded wait's census (once a window): waits that ran out, and
    * the frames shown with the thread's last whole picture because of it. */
   if (g_ready)
   {
      static unsigned last_to;
      fe_evt("vita_rt_waits timeouts=%u window=%u stale_frames=%u limit_us=%u",
             g_timeouts, g_timeouts - last_to, g_stale, RT_WAIT_US);
      last_to = g_timeouts;
   }
   memset(out, 0, sizeof(*out));
   out->frames = n;
   if (n)
   {
      out->render_avg_us = g_render_us / n;
      out->copy_avg_us = g_copy_us / n;
      out->wait_avg_us = g_wait_us / n;
   }
   out->render_max_us = g_render_max_us;
   out->wait_max_us = g_wait_max_us;
   out->checked = g_checked;
   out->bad_frames = g_bad_frames;
   out->bad_px = g_bad_px;
   out->vram_pages = n ? g_pages / n : 0;
   out->log_entries = g_log_entries;
   out->log_overflows = g_log_overflows;
   g_frames = g_copy_us = g_wait_us = g_wait_max_us = 0;
   g_render_us = g_render_max_us = 0;
   g_checked = g_bad_frames = g_bad_px = 0;
   g_log_entries = g_log_overflows = 0;
   g_pages = 0;
}
