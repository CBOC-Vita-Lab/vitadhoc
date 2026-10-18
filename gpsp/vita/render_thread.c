/* render_thread.c -- see render_thread.h. */
#include "render_thread.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pspcompat.h"          /* pspcompat_time_us */
#include "fe_evt.h"

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
static int g_cur;                     /* g_out[] the thread draws next     */

static int g_mode;                    /* the mode in force (RT_*)           */
static int g_fresh;                   /* the thread's object lists are old  */
static int g_vram_all;                /* the thread's VRAM is not the game's */
static unsigned g_vram_age;           /* frames since all of it was copied   */
static unsigned g_pages;              /* VRAM pages copied (statistics)      */
static int g_posted;                  /* this frame went to the thread      */
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

static int rt_entry(SceSize args, void *argp)
{
   (void)args; (void)argp;
   for (;;)
   {
      sceKernelWaitSema(g_go, 1, NULL);
      if (g_quit)
         break;
      rt_work();
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
                                SCE_KERNEL_CPU_MASK_USER_1 |
                                SCE_KERNEL_CPU_MASK_USER_2, NULL);
   if (g_go < 0 || g_done < 0 || g_th < 0 ||
       sceKernelStartThread(g_th, 0, NULL) < 0)
      return -1;
   return 0;
}

static void rt_post(void) { sceKernelSignalSema(g_go, 1); }
static void rt_wait(void) { sceKernelWaitSema(g_done, 1, NULL); }

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
#include <pthread.h>
#include <semaphore.h>

static sem_t g_go, g_done;
static pthread_t g_th;
static volatile int g_quit;

static void *rt_entry(void *arg)
{
   (void)arg;
   for (;;)
   {
      while (sem_wait(&g_go) != 0)
         ;
      if (g_quit)
         break;
      rt_work();
      sem_post(&g_done);
   }
   return NULL;
}

static int rt_start(void)
{
   if (sem_init(&g_go, 0, 0) != 0 || sem_init(&g_done, 0, 0) != 0)
      return -1;
   return pthread_create(&g_th, NULL, rt_entry, NULL) == 0 ? 0 : -1;
}

static void rt_post(void) { sem_post(&g_go); }
static void rt_wait(void) { while (sem_wait(&g_done) != 0) ; }

void rt_shutdown(void)
{
   if (!g_ready)
      return;
   g_quit = 1;
   sem_post(&g_go);
   pthread_join(g_th, NULL);
   g_ready = 0;
}
#endif

/* ---- the frame ----------------------------------------------------------------- */

void rt_frame_begin(int mode)
{
   if (g_posted)                      /* (a frame end that never came) */
   {
      rt_wait();
      g_posted = 0;
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

const uint16_t *rt_frame_end(const uint16_t *pix)
{
   const u16 *out;
   unsigned t0, dt;
   const int mode = g_mode;

   if (!g_posted)
      return pix;
   t0 = pspcompat_time_us();
   rt_wait();
   g_posted = 0;
   dt = pspcompat_time_us() - t0;
   g_wait_us += dt;
   if (dt > g_wait_max_us)
      g_wait_max_us = dt;
   g_frames++;
   g_drawn++;
   out = g_out[g_cur];
   g_cur ^= 1;
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
