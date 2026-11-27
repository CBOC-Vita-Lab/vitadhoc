/* clip_export.c -- see clip_export.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apng_write.h"
#include "clip_export.h"
#include "clip_vita.h"
#include "plat.h"

#define CX_PATH 600

static struct
{
   volatile int state;           /* 0 idle, 1 running, 2 ended (to take)      */
   volatile int stop;
   int ok;
   clip_player *p;
   char path[CX_PATH];
   char tmp[CX_PATH + 8];
   unsigned frames, bytes;
} X;

#define CX_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_SEQ_CST)
#define CX_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_SEQ_CST)

static int sink_file(void *ctx, const void *d, size_t n)
{
   return fwrite(d, 1, n, (FILE *)ctx) == n ? 0 : -1;
}

/* libretro RGB565 -> 8 bits a channel, the way the screenshots do it */
static void to_rgb(const uint16_t *s, uint8_t *d, size_t n)
{
   size_t i;
   for (i = 0; i < n; i++, d += 3)
   {
      unsigned v = s[i], r = v >> 11, g = (v >> 5) & 0x3F, b = v & 0x1F;
      d[0] = (uint8_t)((r << 3) | (r >> 2));
      d[1] = (uint8_t)((g << 2) | (g >> 4));
      d[2] = (uint8_t)((b << 3) | (b >> 2));
   }
}

static void job_main(void *arg)
{
   FILE *f = NULL;
   apng *a = NULL;
   uint8_t *cur = NULL, *held = NULL, *tmp;
   unsigned w, h, n, skip, idx = 0, held_ticks = 0, ticks, us;
   uint64_t t_us = 0, held_start = 0;
   int ended = 0, ok = 0, have_held = 0;
   const uint16_t *pix;

   (void)arg;
   w = clip_w(X.p);
   h = clip_h(X.p);
   skip = clip_skip(X.p);
   n = clip_frames(X.p) - skip;
   us = clip_us_per_tick(X.p);
   cur = (uint8_t *)malloc((size_t)w * h * 3);
   held = (uint8_t *)malloc((size_t)w * h * 3);
   f = fopen(X.tmp, "wb");
   if (!cur || !held || !f)
      goto done;
   a = apng_begin(sink_file, f, w, h, n);
   if (!a)
      goto done;
   /* A frame is shown until the next one comes: its delay is the NEXT
    * frame's ticks (a frame's own are how long it waited for it).  The time
    * is summed in microseconds and cut into whole milliseconds, so the clip
    * does not drift from the game's real length. */
   while (!CX_LOAD(X.stop))
   {
      pix = clip_next(X.p, &ticks, &ended);
      if (!pix)
         break;
      if (idx++ < skip)
         continue;                      /* decoded through, not part of it */
      to_rgb(pix, cur, (size_t)w * h);
      if (have_held)
      {
         uint64_t end = t_us + (uint64_t)ticks * us;
         unsigned d = (unsigned)((end + 500) / 1000 - (held_start + 500) / 1000);
         if (apng_add(a, held, d) != 0)
            goto done;
         held_start = end;
         t_us = end;
      }
      tmp = held;
      held = cur;
      cur = tmp;
      have_held = 1;
      held_ticks = ticks;
   }
   if (CX_LOAD(X.stop) || !ended || !have_held)
      goto done;
   {
      /* the last picture stays as long as it was shown for: one kept frame */
      unsigned d = (unsigned)(((uint64_t)(held_ticks ? held_ticks : 2) * us + 500) / 1000);
      if (apng_add(a, held, d ? d : 1) != 0)
         goto done;
   }
   ok = apng_end(a) == 0;
   a = NULL;
done:
   if (a)
      (void)apng_end(a);
   if (f)
   {
      long len;
      if (ok && fflush(f) != 0)
         ok = 0;
      len = ok ? ftell(f) : -1;
      if (fclose(f) != 0)
         ok = 0;
      f = NULL;
      X.bytes = len > 0 ? (unsigned)len : 0;
   }
   if (ok && rename(X.tmp, X.path) != 0)
      ok = 0;
   if (!ok)
      remove(X.tmp);
   X.frames = ok ? n : 0;
   X.ok = ok;
   free(cur);
   free(held);
   clip_close(X.p);
   X.p = NULL;
   CX_STORE(X.state, 2);
}

int clip_export_start(const char *path, unsigned us_per_tick, int sync_ok)
{
   uint8_t *img = NULL;
   size_t len = 0;
   int rc;

   if (CX_LOAD(X.state) != 0)
      return CLIPX_BUSY;
   if (!path || strlen(path) >= CX_PATH)
      return CLIPX_FAIL;
   rc = clip_snapshot(us_per_tick, &img, &len);
   if (rc > 0)
      return CLIPX_NONE;
   if (rc < 0)
      return CLIPX_FAIL;
   X.p = clip_open_image(img, len);       /* takes the image */
   if (!X.p)
      return CLIPX_FAIL;
   snprintf(X.path, sizeof(X.path), "%s", path);
   snprintf(X.tmp, sizeof(X.tmp), "%s.tmp", path);
   CX_STORE(X.stop, 0);
   X.ok = 0;
   X.frames = X.bytes = 0;
   CX_STORE(X.state, 1);
   if (plat_job_start(job_main, NULL) != 0)
   {
      if (!sync_ok)
      {
         clip_close(X.p);
         X.p = NULL;
         CX_STORE(X.state, 0);
         return CLIPX_FAIL;
      }
      job_main(NULL);
   }
   return CLIPX_STARTED;
}

int clip_export_busy(void)
{
   return CX_LOAD(X.state) == 1;
}

int clip_export_take(int *ok, char *path, size_t cap, unsigned *frames,
                     unsigned *bytes)
{
   if (CX_LOAD(X.state) != 2)
      return 0;
   (void)plat_job_join(1000000);          /* it has finished: collect it */
   if (ok)
      *ok = X.ok;
   if (path && cap)
      snprintf(path, cap, "%s", X.path);
   if (frames)
      *frames = X.frames;
   if (bytes)
      *bytes = X.bytes;
   CX_STORE(X.state, 0);
   return 1;
}

void clip_export_stop(uint32_t timeout_us)
{
   if (CX_LOAD(X.state) == 0)
      return;
   CX_STORE(X.stop, 1);
   if (plat_job_join(timeout_us))
      CX_STORE(X.state, 0);
}
