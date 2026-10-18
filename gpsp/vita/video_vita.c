/* video_vita.c -- psp/video_psp.h for the PS Vita (see vita/video_vita.h).
 *
 * The PSP frontend's whole UI is drawn through the small overlay API of
 * psp/video_psp.h.  This file implements that API -- every function of it --
 * on the backend interface of vita/vback.h, so ui_psp.c and osd_psp.c run
 * unchanged in their 480x272 LOGICAL coordinates while every pixel is
 * produced at the Vita's native 960x544:
 *
 *   rects, gradients   exact: a logical pixel is a 2x2 native block
 *   text               Inter rasterised at 2x the PSP's sizes
 *                      (video_vita_text.c); advances are the PSP's, so no
 *                      layout moves
 *   wordmark           the PSP's grid-snapped bake at an 8 px stroke
 *   images             GPU textures, bilinear (or nearest where the PSP
 *                      asked for nearest)
 *   the game picture   one 240x160 / 160x144 texture upload and one quad,
 *                      scaled straight to native pixels
 *
 * ONE FRAME, ONE SCENE.  The PSP builds one GE display list per call group
 * (the blit, then the OSD overlay, then a swap).  GXM renders a frame as one
 * scene and cannot nest scenes, so here the frame is opened lazily by the
 * first thing that draws and stays open until vid_swap(): vid_draw_frame
 * followed by osd_draw's vid_overlay_begin(0) is one scene.  The only other
 * scene is sharp bilinear's prescale into a render target, which is drawn
 * BEFORE the frame opens (vid_draw_frame and the menu backdrop are both the
 * first draw of their frame); should it ever come later, the backend
 * suspends and resumes the frame.
 *
 * THE "FILLED" RULE is the PSP's (video_psp.c g_fb_filled): the Vita is
 * triple-buffered, so a swap with nothing drawn would show a frame three
 * presents old.  vid_swap() presents only when something filled the frame --
 * vid_draw_frame, vid_draw_frame_pair or vid_overlay_begin(1).
 *
 * All calls from the main thread only.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "video_vita.h"
#include "video_vita_priv.h"
#include "video_vita_logo.h"
#include "vstyle.h"
#include "ambient_look.h"
#include "fe_evt.h"
#include "upscale_vita.h"
#include "stb_image_write.h"

/* pspcompat.c: microseconds, wrapping (the PSP's sceKernelGetSystemTimeLow). */
unsigned pspcompat_time_us(void);

/* The other half of the pixel-format link guard; see gba_memory.c.  A core
 * built with the PSP's channel order must not link against this file, which
 * uploads the game frame as libretro RGB565.  (The host twin has no core.) */
#if defined(__vita__) && !defined(VIDEO_VITA_NO_CORE)
extern const char gpsp_core_pixfmt_rgb565;
__attribute__((used)) static const char *const gpsp_pixfmt_agreed =
   &gpsp_core_pixfmt_rgb565;
#endif

typedef char vv_scale_is_two[(VV_S * VID_SCR_W == VB_W &&
                              VV_S * VID_SCR_H == VB_H) ? 1 : -1];

/* ---- state -------------------------------------------------------------- */

static int g_inited;
static int g_scale  = VID_SCALE_INT3;
static int g_filter = VID_FILTER_NEAREST;
/* The smoothing the TRIANGLE presets use: the last non-nearest filter
 * vid_set_mode was given.  Sharp until the player picks another: at the
 * Vita's non-integer scales plain bilinear is the soft one. */
static int g_smooth = VID_FILTER_SHARP;

static int g_clear_req;       /* the frame, when it opens, starts black     */
static int g_filled;          /* a whole frame was drawn since the last swap */
static unsigned g_frame_no;   /* presents so far: the ping-pong parity      */

/* Blit profile, for a main loop that reports it (video_psp.h). */
static unsigned g_prof_frames, g_stage_us_tot, g_stage_us_max;
static unsigned g_gu_us_tot, g_gu_us_max;
/* vid_draw_parts: the slowest of each part of vid_draw_frame this window */
static unsigned g_dp[VID_DP_N];
#define DP_MAX(i, us) do { unsigned d_ = (us); \
                           if (d_ > g_dp[i]) g_dp[i] = d_; } while (0)

/* ---- colours ------------------------------------------------------------ */

/* libretro RGB565 + alpha -> the backend's 0xAABBGGRR. */
static uint32_t c565(uint16_t c, int alpha)
{
   unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
   r = (r << 3) | (r >> 2);
   g = (g << 2) | (g >> 4);
   b = (b << 3) | (b >> 2);
   if (alpha < 0) alpha = 0;
   if (alpha > 255) alpha = 255;
   return VB_RGBA(r, g, b, alpha);
}

static const uint16_t *vv_look_lut(void);

void vid_frame_to_psp565(uint16_t *dst, int dst_stride, const uint16_t *src,
                         unsigned w, unsigned h, size_t pitch_bytes)
{
   /* In the colours the game is being shown in (vid_set_look): this is the
    * picture behind the menu and on a save state's thumbnail. */
   const uint16_t *lut = vv_look_lut();
   unsigned x, y;
   for (y = 0; y < h; y++)
   {
      const uint16_t *s =
         (const uint16_t *)((const uint8_t *)src + (size_t)y * pitch_bytes);
      uint16_t *d = dst + (size_t)y * dst_stride;
      for (x = 0; x < w; x++)
      {
         unsigned v = lut ? lut[s[x]] : s[x];
         d[x] = (uint16_t)((v >> 11) | (v & 0x07E0u) | ((v & 0x1Fu) << 11));
      }
   }
}

/* ---- the frame ---------------------------------------------------------- */

void vv_frame_need(void)
{
   if (!vb_frame_open())
   {
      vb_frame_begin(g_clear_req);
      g_clear_req = 0;
   }
}

void vid_init(void)
{
   if (g_inited)
      return;
   if (vb_init() != 0)
      fe_log("video: backend %s failed to start", vb_name());
   if (vv_text_init() != 0)
      fe_log("video: font atlas did not fit");
   g_inited = 1;
   g_clear_req = 0;
   g_filled = 0;
   {
      int ch0 = 0, ch1 = 0;
      float m0 = 0.0f, m1 = 0.0f;
      float w0 = vv_text_worst_dev(0, &ch0, &m0);
      float w1 = vv_text_worst_dev(1, &ch1, &m1);
      FE_EVT_ONLY(w0); FE_EVT_ONLY(w1);
      fe_evt("video_init backend=%s native=%dx%d ui_adv_dev=%.2f(%c) "
             "mean=%.3f hd_adv_dev=%.2f(%c) mean=%.3f", vb_name(), VB_W, VB_H,
             w0, ch0, m0, w1, ch1, m1);
   }
}

const char *vid_backend_name(void) { return vb_name(); }

float vid_text_advance_dev(int hd, int *worst_char, float *mean_abs)
{
   return vv_text_worst_dev(hd, worst_char, mean_abs);
}

void vid_set_vsync(int on) { vb_set_vsync(on); }
void vid_set_common_dialog(int on) { vb_set_common_dialog(on); }

/* ---- scale / filter ----------------------------------------------------- */

void vid_set_mode(int scale, int filter)
{
   if (scale >= 0 && scale < VID_SCALE_MODES)
      g_scale = scale;
   g_filter = (filter >= VID_FILTER_SHARP && filter < VID_FILTER_MODES)
            ? filter : filter ? VID_FILTER_BILINEAR : VID_FILTER_NEAREST;
   if (g_filter != VID_FILTER_NEAREST)
      g_smooth = g_filter;
}

int vid_scale_mode(void) { return g_scale; }
int vid_filter(void)     { return g_filter; }

const char *vid_scale_name(int scale)
{
   switch (scale)
   {
   case VID_SCALE_FIT:     return "fit";
   case VID_SCALE_STRETCH: return "stretch";
   case VID_SCALE_INT2:    return "4x crop";
   case VID_SCALE_INT3:    return "3x";
   default:                return "2x";
   }
}

const char *vid_filter_name(int filter)
{
   return filter == VID_FILTER_LCD       ? "LCD grid"
        : filter == VID_FILTER_SHARP     ? "sharp bilinear"
        : filter == VID_FILTER_XBRZ      ? "xBRZ smooth"
        : filter == VID_FILTER_SCALE     ? "Scale3x"
        : filter == VID_FILTER_SCANLINES ? "scanlines"
        : filter ? "bilinear" : "nearest";
}

int vid_scale_step(int scale, int dir)
{
   static const int order[VID_SCALE_MODES] = {
      VID_SCALE_1X, VID_SCALE_INT3, VID_SCALE_FIT, VID_SCALE_INT2,
      VID_SCALE_STRETCH
   };
   int i;
   for (i = 0; i < VID_SCALE_MODES; i++)
      if (order[i] == scale)
         break;
   if (i == VID_SCALE_MODES)
      i = 0;
   i = (i + VID_SCALE_MODES + (dir < 0 ? -1 : 1)) % VID_SCALE_MODES;
   return order[i];
}

const char *vid_cycle_preset(void)
{
   /* 3x -> fit -> fit smoothed -> stretch -> stretch smoothed -> 3x: the
    * PSP's cycle with 3x where its 1x was.  2x and the cropped 4x are opt-in
    * from Settings; from either, the cycle continues at fit. */
   static const struct { int s, f; const char *name; } preset[5] = {
      { VID_SCALE_INT3,    0, "3x"      },
      { VID_SCALE_FIT,     0, "fit"     },
      { VID_SCALE_FIT,     1, "fit"     },
      { VID_SCALE_STRETCH, 0, "stretch" },
      { VID_SCALE_STRETCH, 1, "stretch" },
   };
   static char name[32];
   int i;
   for (i = 0; i < 5; i++)
      if (preset[i].s == g_scale &&
          (preset[i].f ? g_smooth : VID_FILTER_NEAREST) == g_filter)
         break;
   i = (i + 1) % 5;
   g_scale  = preset[i].s;
   g_filter = preset[i].f ? g_smooth : VID_FILTER_NEAREST;
   if (!preset[i].f)
      return preset[i].name;
   snprintf(name, sizeof(name), "%s %s", preset[i].name,
            g_smooth == VID_FILTER_LCD ? "LCD"
            : g_smooth == VID_FILTER_SHARP ? "sharp"
            : g_smooth == VID_FILTER_XBRZ ? "xBRZ"
            : g_smooth == VID_FILTER_SCALE ? "Scale3x"
            : g_smooth == VID_FILTER_SCANLINES ? "scanlines" : "bilinear");
   return name;
}

/* The PSP's staging-placement and deferred-sync switches have no meaning on
 * a GPU with its own memory; the calls exist so shared code links. */
int  vid_set_blit_mode(int mode) { (void)mode; return 0; }
int  vid_blit_mode(void)         { return 0; }
const char *vid_blit_mode_name(int mode) { (void)mode; return "gxm"; }
int  vid_gu_defer(void)          { return 0; }
void vid_set_gu_defer(int on)    { (void)on; }
void vid_gu_flush(void)          { }
void vid_blank_all(void)         { }

void vid_draw_parts(unsigned out[VID_DP_N])
{
   int i;
   for (i = 0; i < VID_DP_N; i++)
   {
      out[i] = g_dp[i];
      g_dp[i] = 0;
   }
}

void vid_blit_prof(unsigned *frames,
                   unsigned *stage_us, unsigned *stage_max,
                   unsigned *gu_us,    unsigned *gu_max,
                   unsigned *wait_us,  unsigned *wait_max)
{
   if (frames)    *frames    = g_prof_frames;
   if (stage_us)  *stage_us  = g_stage_us_tot;
   if (stage_max) *stage_max = g_stage_us_max;
   if (gu_us)     *gu_us     = g_gu_us_tot;
   if (gu_max)    *gu_max    = g_gu_us_max;
   if (wait_us)   *wait_us   = 0;
   if (wait_max)  *wait_max  = 0;
   g_prof_frames = g_stage_us_tot = g_stage_us_max = 0;
   g_gu_us_tot = g_gu_us_max = 0;
}

/* ---- where the picture goes --------------------------------------------- */

/* Native pixels.  Only the cropped 4x draws fewer rows than the source has
 * (sy0 > 0, sh < h), exactly as the PSP's 2x does. */
typedef struct
{
   int ox, oy, dw, dh;
   int sy0, sh;
} vid_geo;

static void dest_geo(unsigned w, unsigned h, vid_geo *g)
{
   int k = 0;
   g->sy0 = 0;
   g->sh  = (int)h;
   g->dw  = (int)w;
   g->dh  = (int)h;
   switch (g_scale)
   {
   case VID_SCALE_FIT:
      /* Fill the height, keep the aspect: 240x160 -> 816x544 (3.4x). */
      g->dh = VB_H;
      g->dw = (int)((unsigned)VB_H * w / (h ? h : 1));
      if (g->dw > VB_W)
         g->dw = VB_W;
      break;
   case VID_SCALE_STRETCH:
      g->dw = VB_W;
      g->dh = VB_H;
      break;
   case VID_SCALE_INT2: k = 4; break;
   case VID_SCALE_INT3: k = 3; break;
   default:             k = 2; break;
   }
   if (k)
   {
      /* A source too wide for the factor takes the largest that fits. */
      while (k > 1 && (unsigned)k * w > VB_W)
         k--;
      if ((unsigned)k * h > VB_H)
      {
         if (g_scale == VID_SCALE_INT2)
         {
            /* Crop in the SOURCE: the middle VB_H / k rows, so every output
             * row is exactly k copies of a source row. */
            g->sh  = VB_H / k;
            g->sy0 = ((int)h - g->sh) / 2;
         }
         else
            while (k > 1 && (unsigned)k * h > VB_H)
               k--;
      }
      g->dw = k * (int)w;
      g->dh = k * g->sh;
   }
   g->ox = (VB_W - g->dw) / 2;
   g->oy = (VB_H - g->dh) / 2;
}

/* The wide picture (gpsp/wide.h) has `margin` more columns on each side of
 * the console's w: how many of them have room on the screen at the scale in
 * use.  All of them at "fit" and the whole factors up to 3; none where the
 * console's own picture already takes the width (stretch, 4x). */
int vid_margin_fit(unsigned w, unsigned h, int margin)
{
   vid_geo g;
   int fit;
   if (!w || !h || margin <= 0)
      return 0;
   dest_geo(w, h, &g);
   if (g.dw <= 0 || g.ox <= 0)
      return 0;
   fit = (int)((long)g.ox * (long)w / g.dw);
   return fit > margin ? margin : fit;
}

void vid_picture_rect(unsigned w, unsigned h, vid_rect_t *out)
{
   vid_geo g;
   dest_geo(w, h, &g);
   if (out)
   {
      out->x = g.ox; out->y = g.oy; out->w = g.dw; out->h = g.dh;
   }
}

static int geo_covers(const vid_geo *g)
{
   return g->ox <= 0 && g->oy <= 0 && g->ox + g->dw >= VB_W &&
          g->oy + g->dh >= VB_H;
}

/* ---- GPU textures ------------------------------------------------------- */

/* The game frame is uploaded every frame.  A deferred renderer may still be
 * reading last frame's texture when this frame's bytes arrive, so each
 * purpose has two textures and alternates. */
enum { ST_LIVE = 0, ST_PARTNER = 1, ST_SNAP = 2, ST_SOFT = 3,
       ST_PURPOSES = 4 };
static struct { vb_tex *t; int w, h, fmt; } g_stage[ST_PURPOSES][2];

/* The textures written every frame (these, the upscalers'): in the GPU's
 * memory, or with vita.ini tex_mem = 1 in the main RAM (vb_tex_mem).  On a
 * console the main RAM was no faster to write and took 80 ms to make a
 * texture in: the GPU's is the default. */
static int g_tex_cpu = 0;

void vid_set_tex_mem(int cpu_written)
{
   g_tex_cpu = cpu_written ? 1 : 0;
}

static vb_tex *stage_tex(int purpose, int w, int h, int fmt)
{
   int par = (int)(g_frame_no & 1u);
   if (g_stage[purpose][par].t &&
       (g_stage[purpose][par].w != w || g_stage[purpose][par].h != h ||
        g_stage[purpose][par].fmt != fmt))
   {
      vb_tex_destroy(g_stage[purpose][par].t);
      g_stage[purpose][par].t = NULL;
   }
   if (!g_stage[purpose][par].t)
   {
      vb_tex_mem(g_tex_cpu);
      g_stage[purpose][par].t = vb_tex_create(w, h, fmt, 0);
      vb_tex_mem(0);
      g_stage[purpose][par].w = w;
      g_stage[purpose][par].h = h;
      g_stage[purpose][par].fmt = fmt;
   }
   return g_stage[purpose][par].t;
}

/* Textures the UI owns (art, thumbnails, the state preview): keyed by the
 * caller's pointer, made on first draw, dropped by vid_tex_invalidate. */
#define TC_MAX 64
typedef struct
{
   const uint16_t *key;
   int texw, srcw, srch;
   vb_tex *tex;
   unsigned stamp;
} tc_ent;
static tc_ent   g_tc[TC_MAX];
static unsigned g_tc_clock;

static vb_tex *tc_get(const uint16_t *pix, int texw, int srcw, int srch)
{
   int i, victim = 0;
   if (!pix || srcw <= 0 || srch <= 0 || texw < srcw)
      return NULL;
   for (i = 0; i < TC_MAX; i++)
      if (g_tc[i].tex && g_tc[i].key == pix && g_tc[i].texw == texw &&
          g_tc[i].srcw == srcw && g_tc[i].srch == srch)
      {
         g_tc[i].stamp = ++g_tc_clock;
         return g_tc[i].tex;
      }
   for (i = 0; i < TC_MAX; i++)
   {
      if (!g_tc[i].tex)
      {
         victim = i;
         break;
      }
      if (g_tc[i].stamp < g_tc[victim].stamp)
         victim = i;
   }
   if (g_tc[victim].tex)
      vb_tex_destroy(g_tc[victim].tex);
   g_tc[victim].tex = vb_tex_create(srcw, srch, VB_FMT_BGR565, 0);
   if (!g_tc[victim].tex)
      return NULL;
   vb_tex_update(g_tc[victim].tex, 0, 0, srcw, srch, pix, (size_t)texw * 2);
   g_tc[victim].key = pix;
   g_tc[victim].texw = texw;
   g_tc[victim].srcw = srcw;
   g_tc[victim].srch = srch;
   g_tc[victim].stamp = ++g_tc_clock;
   return g_tc[victim].tex;
}

void vid_tex_invalidate(const void *p, size_t bytes)
{
   const uint8_t *a = (const uint8_t *)p;
   int i;
   if (!p)
      return;
   for (i = 0; i < TC_MAX; i++)
   {
      const uint8_t *k = (const uint8_t *)g_tc[i].key;
      size_t span = (size_t)g_tc[i].texw * (size_t)g_tc[i].srch * 2;
      if (g_tc[i].tex && k < a + (bytes ? bytes : 1) && a < k + span)
      {
         /* A NEW texture is made on the next draw rather than this one
          * rewritten: a frame in flight may still be reading it. */
         vb_tex_destroy(g_tc[i].tex);
         g_tc[i].tex = NULL;
         g_tc[i].key = NULL;
      }
   }
}

/* ---- overlay primitives ------------------------------------------------- */

void vid_overlay_begin(int clear)
{
   if (!g_inited)
      vid_init();
   if (clear)
   {
      g_filled = 1;
      if (vb_frame_open())
      {
         /* Mid-frame: black over everything is the clear. */
         vb_scissor(0, 0, 0, 0, 0);
         vb_quad(0, 0, VB_W, VB_H, 0xFF000000u, 0xFF000000u, 0xFF000000u,
                 0xFF000000u);
      }
      else
         g_clear_req = 1;
   }
   vb_scissor(0, 0, 0, 0, 0);
}

void vid_overlay_end(void)
{
   /* The frame stays open for whatever else this frame draws; vid_swap()
    * closes it. */
}

void vid_rect(int x, int y, int w, int h, uint16_t rgb565, int alpha)
{
   uint32_t c = c565(rgb565, alpha);
   if (w <= 0 || h <= 0 || alpha <= 0)
      return;
   vv_frame_need();
   vb_quad((float)(x * VV_S), (float)(y * VV_S), (float)((x + w) * VV_S),
           (float)((y + h) * VV_S), c, c, c, c);
}

void vid_gradient_a(int x, int y, int w, int h,
                    uint16_t rgb565, int a_top, int a_bot)
{
   uint32_t ct = c565(rgb565, a_top), cb = c565(rgb565, a_bot);
   if (w <= 0 || h <= 0)
      return;
   vv_frame_need();
   vb_quad((float)(x * VV_S), (float)(y * VV_S), (float)((x + w) * VV_S),
           (float)((y + h) * VV_S), ct, ct, cb, cb);
}

void vid_gradient(int x, int y, int w, int h,
                  uint16_t top565, uint16_t bot565, int alpha)
{
   uint32_t ct = c565(top565, alpha), cb = c565(bot565, alpha);
   if (w <= 0 || h <= 0)
      return;
   vv_frame_need();
   vb_quad((float)(x * VV_S), (float)(y * VV_S), (float)((x + w) * VV_S),
           (float)((y + h) * VV_S), ct, ct, cb, cb);
}

void vid_clip(int x, int y, int w, int h)
{
   vv_frame_need();
   vb_scissor(1, x * VV_S, y * VV_S, w * VV_S, h * VV_S);
}

void vid_clip_off(void)
{
   vb_scissor(0, 0, 0, 0, 0);
}

/* ---- text --------------------------------------------------------------- */

int vid_text_w(const char *str)    { return vv_text_width(0, str); }
int vid_text_hd_w(const char *str) { return vv_text_width(1, str); }

/* OPTICAL CENTRING, the PSP's arithmetic over the PSP's atlas metrics
 * (video_psp.c fu_cap): the cap height of a ui line is logical rows 4..14. */
int vid_band_y(int text_y, int band_h)
{
   int top, h;
   vv_text_cap(&top, &h);
   return text_y + top - (band_h - h) / 2;
}

int vid_text_y_in(int box_y, int box_h)
{
   int top, h;
   vv_text_cap(&top, &h);
   return box_y + (box_h - h) / 2 - top;
}

void vid_text(int x, int y, const char *str, uint16_t rgb565)
{
   vv_frame_need();
   vv_text_draw(0, x * VV_S, y * VV_S, str, c565(rgb565, 255));
}

void vid_text_hd(int x, int y, const char *str, uint16_t rgb565)
{
   vv_frame_need();
   vv_text_draw(1, x * VV_S, y * VV_S, str, c565(rgb565, 255));
}

void vid_text_a(int x, int y, const char *str, uint16_t rgb565, int alpha)
{
   if (alpha <= 0)
      return;
   vv_frame_need();
   vv_text_draw(0, x * VV_S, y * VV_S, str, c565(rgb565, alpha));
}

void vid_text_center(int y, const char *str, uint16_t rgb565)
{
   vid_text((VID_SCR_W - vid_text_w(str)) / 2, y, str, rgb565);
}

/* The wordmark: coverage tinted by the theme, drawn 1:1 at native size. */
static vb_tex *g_logo_tex;

void vid_logo(int x, int y, uint16_t rgb565, int alpha)
{
   /* VitAdhoc's wordmark (vstyle.c), in the old one's 302 x 48 box: the
    * screens laid out around VID_LOGO_W keep their places. */
   vs_wordmark((float)(x * VV_S), (float)(y * VV_S) + 1.0f, 46.0f,
               c565(rgb565, alpha & 0xFF));
}

/* The GBAdhoc wordmark this replaced, kept for the PSP-faithful tests. */
void vid_logo_old(int x, int y, uint16_t rgb565, int alpha)
{
   vb_tquad q;
   if (!g_logo_tex)
   {
      g_logo_tex = vb_tex_create(VV_LOGO_W, VV_LOGO_H, VB_FMT_A8, 0);
      if (!g_logo_tex)
         return;
      vb_tex_update(g_logo_tex, 0, 0, VV_LOGO_W, VV_LOGO_H, vv_logo_a,
                    VV_LOGO_W);
   }
   vv_frame_need();
   q.x0 = (float)(x * VV_S); q.y0 = (float)(y * VV_S);
   q.x1 = q.x0 + VV_LOGO_W;  q.y1 = q.y0 + VV_LOGO_H;
   q.u0 = 0; q.v0 = 0; q.u1 = VV_LOGO_W; q.v1 = VV_LOGO_H;
   vb_tquads(g_logo_tex, &q, 1, c565(rgb565, alpha & 0xFF), VB_NEAREST);
}

/* ---- images ------------------------------------------------------------- */

/* The source rows [v0,v1) x columns [0,srcw) of a UI texture onto a native
 * rect. */
static void image_native(float x0, float y0, float x1, float y1,
                         const uint16_t *pix, int texw, int srcw, int srch,
                         int v0, int v1, int alpha, int filt)
{
   vb_tex *t = tc_get(pix, texw, srcw, srch);
   vb_tquad q;
   if (!t || alpha <= 0)
      return;
   vv_frame_need();
   q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;
   q.u0 = 0; q.v0 = (float)v0; q.u1 = (float)srcw; q.v1 = (float)v1;
   vb_tquads(t, &q, 1, VB_RGBA(255, 255, 255, alpha > 255 ? 255 : alpha),
             filt);
}

void vid_image(int x, int y, int w, int h, const uint16_t *pix,
               int texw, int texh, int srcw, int srch, int alpha)
{
   (void)texh;
   image_native((float)(x * VV_S), (float)(y * VV_S),
                (float)((x + w) * VV_S), (float)((y + h) * VV_S),
                pix, texw, srcw, srch, 0, srch, alpha, VB_LINEAR);
}

void vid_image_nearest(int x, int y, int w, int h, const uint16_t *pix,
                       int texw, int texh, int srcw, int srch)
{
   (void)texh;
   image_native((float)(x * VV_S), (float)(y * VV_S),
                (float)((x + w) * VV_S), (float)((y + h) * VV_S),
                pix, texw, srcw, srch, 0, srch, 255, VB_NEAREST);
}

/* The PSP draws w x h texels on w x h pixels; here each texel is a 2x2
 * block, nearest. */
void vid_image_px(int x, int y, int w, int h, const uint16_t *pix,
                  int texw, int texh)
{
   (void)texh;
   image_native((float)(x * VV_S), (float)(y * VV_S),
                (float)((x + w) * VV_S), (float)((y + h) * VV_S),
                pix, texw, w, h, 0, h, 255, VB_NEAREST);
}

/* ---- the save states page's preview (docs/UI-OVERLAY.md section 13) ------
 * The PSP keeps this texture in VRAM; here it is 256 x 160 texels of RAM
 * that ui_psp.c fills row by row and a cached GPU texture made from it. */
#define SHOT_TEX_W 256
#define SHOT_TEX_H 160
static uint16_t g_shot[SHOT_TEX_W * SHOT_TEX_H];

uint16_t *vid_shot_tex(void) { return g_shot; }

void vid_shot_rows(int y0, const void *src, int rows)
{
   if (y0 < 0 || rows <= 0 || y0 + rows > SHOT_TEX_H || !src)
      return;
   memcpy(g_shot + (size_t)y0 * SHOT_TEX_W, src,
          (size_t)rows * SHOT_TEX_W * sizeof(uint16_t));
   vid_tex_invalidate(g_shot, sizeof(g_shot));
}

void vid_shot_draw(int x, int y, int w, int h)
{
   if (w <= 0 || h <= 0 || w > SHOT_TEX_W || h > SHOT_TEX_H)
      return;
   /* "1:1" on the PSP is an exact 2x here: 240x160 -> 480x320, nearest. */
   image_native((float)(x * VV_S), (float)(y * VV_S),
                (float)((x + w) * VV_S), (float)((y + h) * VV_S),
                g_shot, SHOT_TEX_W, w, h, 0, h, 255, VB_NEAREST);
}

/* ===========================================================================
 * AMBIENT BARS (docs/DISPLAY-FEATURES.md, look in psp/ambient_look.h)
 *
 * The bake is the PSP's, number for number: the art (or an early game frame)
 * is cover-cropped to the screen's shape, box-averaged down to AMB_W x AMB_H,
 * blurred and graded ONCE.  Drawing is simpler than on the PSP: the texture
 * goes behind the picture as one full-screen bilinear quad, the scrim folded
 * into the tint, instead of one sprite per bar -- overdraw costs a GPU
 * nothing worth four sets of vertices.
 * ======================================================================== */

enum { AMB_SRC_NONE = 0, AMB_SRC_ART = 1, AMB_SRC_SNAP = 2 };

static vb_tex  *g_amb_tex;
static int      g_amb_mode;      /* PCFG_AMB_* (0 off, 1 art, 2 art|game) */
static int      g_amb_src;
static int      g_amb_scrim;
static uint16_t g_amb_shade;
static int      g_amb_have_shade;
static unsigned g_amb_frames;
static int      g_amb_snap_done;

void vid_ambient_mode(int mode)
{
   g_amb_mode = (mode >= 0 && mode <= 2) ? mode : 0;
   g_amb_frames = 0;
}

void vid_ambient_fallback(uint16_t rgb565)
{
   g_amb_shade = rgb565;
   g_amb_have_shade = 1;
}

int vid_ambient_source(void) { return g_amb_src; }

void vid_ambient_resnap(void)
{
   if (g_amb_src == AMB_SRC_SNAP || (g_amb_src == AMB_SRC_NONE &&
                                     g_amb_snap_done))
   {
      g_amb_snap_done = 0;
      g_amb_frames = AMB_SNAP_AFTER - 1;
      if (g_amb_src == AMB_SRC_SNAP)
         g_amb_src = AMB_SRC_NONE;
   }
}

void vid_ambient_drop(void)
{
   if (g_amb_src == AMB_SRC_ART)
      g_amb_src = AMB_SRC_NONE;
}

int vid_ambient_image(int x, int y, int w, int h, int alpha)
{
   vb_tquad q;
   if (!g_amb_tex || g_amb_src != AMB_SRC_ART)
      return 0;
   vv_frame_need();
   q.x0 = (float)(x * VV_S); q.y0 = (float)(y * VV_S);
   q.x1 = (float)((x + w) * VV_S); q.y1 = (float)((y + h) * VV_S);
   q.u0 = 0; q.v0 = 0; q.u1 = AMB_W; q.v1 = AMB_H;
   vb_tquads(g_amb_tex, &q, 1, VB_RGBA(255, 255, 255, alpha), VB_LINEAR);
   return 1;
}

/* `c` darkened as a black scrim of alpha `a` over it would leave it. */
static uint32_t amb_dim(uint16_t c, int a)
{
   uint32_t v = c565(c, 255);
   unsigned k = 255u - (unsigned)a;
   unsigned r = (v & 0xFF) * k / 255u;
   unsigned g = ((v >> 8) & 0xFF) * k / 255u;
   unsigned b = ((v >> 16) & 0xFF) * k / 255u;
   return VB_RGBA(r, g, b, 255);
}

/* What the bars will be: 2 textured, 1 the palette ramp, 0 black. */
static int amb_kind(void)
{
   if (!g_amb_mode)
      return 0;
   if (g_amb_tex && (g_amb_src == AMB_SRC_ART ||
                     (g_amb_src == AMB_SRC_SNAP && g_amb_mode == 2)))
      return 2;
   return g_amb_have_shade ? 1 : 0;
}

/* One separable box pass over an AMB_W x AMB_H x 3 image (the PSP's). */
static void amb_box(unsigned char *img, unsigned char *line, int r, int along_x)
{
   int n_lines = along_x ? AMB_H : AMB_W;
   int len     = along_x ? AMB_W : AMB_H;
   int step    = along_x ? 3 : AMB_W * 3;
   int li, i, c, div = 2 * r + 1;

   for (li = 0; li < n_lines; li++)
   {
      unsigned char *base = img + (along_x ? li * AMB_W * 3 : li * 3);
      for (i = 0; i < len; i++)
         for (c = 0; c < 3; c++)
            line[i * 3 + c] = base[i * step + c];
      for (c = 0; c < 3; c++)
      {
         int sum = 0, k;
         for (k = -r; k <= r; k++)
         {
            int j = k < 0 ? 0 : (k >= len ? len - 1 : k);
            sum += line[j * 3 + c];
         }
         for (i = 0; i < len; i++)
         {
            int out = i - r, in = i + r + 1;
            base[i * step + c] = (unsigned char)(sum / div);
            if (out < 0) out = 0;
            if (in >= len) in = len - 1;
            sum += line[in * 3 + c] - line[out * 3 + c];
         }
      }
   }
}

static int amb_clamp255(int v)
{
   return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* THE BAKE (video_psp.c amb_bake).  `libretro_order`: the source is a live
 * game frame (R high) rather than a PSP-order UI texture.  Returns 0 baked,
 * 1 too dark (luma_min), -1 no texture. */
static int amb_bake(const uint16_t *src, int stride, int sw, int sh,
                    int max_taps, int luma_min, int libretro_order,
                    int extra_passes)
{
   static unsigned char img[AMB_H * AMB_W * 3];
   static unsigned char out[AMB_H * AMB_W * 4];
   unsigned char line[(AMB_W > AMB_H ? AMB_W : AMB_H) * 3];
   int cx = 0, cy = 0, cw = sw, ch = sh, tx, ty, p;
   unsigned luma = 0;

   if (!src || sw <= 0 || sh <= 0)
      return -1;

   /* Cover crop: the widest/tallest screen-shaped window, centred. */
   if ((long)sw * VID_SCR_H > (long)sh * VID_SCR_W)
   {
      cw = (int)((long)sh * VID_SCR_W / VID_SCR_H);
      cx = (sw - cw) / 2;
   }
   else
   {
      ch = (int)((long)sw * VID_SCR_H / VID_SCR_W);
      cy = (sh - ch) / 2;
   }
   if (cw < 1) cw = 1;
   if (ch < 1) ch = 1;

   for (ty = 0; ty < AMB_H; ty++)
   {
      int y0 = cy + ty * ch / AMB_H, y1 = cy + (ty + 1) * ch / AMB_H;
      int ys;
      if (y1 <= y0) y1 = y0 + 1;
      ys = (y1 - y0 + max_taps - 1) / max_taps;
      for (tx = 0; tx < AMB_W; tx++)
      {
         int x0 = cx + tx * cw / AMB_W, x1 = cx + (tx + 1) * cw / AMB_W;
         unsigned r = 0, gg = 0, b = 0, n = 0;
         int xs, x, y;
         if (x1 <= x0) x1 = x0 + 1;
         xs = (x1 - x0 + max_taps - 1) / max_taps;
         for (y = (max_taps == 1 ? (y0 + y1) / 2 : y0); y < y1; y += ys)
         {
            for (x = (max_taps == 1 ? (x0 + x1) / 2 : x0); x < x1; x += xs)
            {
               unsigned c = src[(long)y * stride + x];
               unsigned cr = c & 31, cg = (c >> 5) & 63, cb = (c >> 11) & 31;
               if (libretro_order)
               {
                  unsigned t = cr; cr = cb; cb = t;
               }
               r  += (cr << 3) | (cr >> 2);
               gg += (cg << 2) | (cg >> 4);
               b  += (cb << 3) | (cb >> 2);
               n++;
               if (max_taps == 1)
                  break;
            }
            if (max_taps == 1)
               break;
         }
         if (!n) n = 1;
         p = (ty * AMB_W + tx) * 3;
         img[p]     = (unsigned char)(r / n);
         img[p + 1] = (unsigned char)(gg / n);
         img[p + 2] = (unsigned char)(b / n);
         luma += (77u * img[p] + 150u * img[p + 1] + 29u * img[p + 2]) >> 8;
      }
   }
   luma /= (unsigned)(AMB_W * AMB_H);
   if (luma_min > 0 && (int)luma < luma_min)
      return 1;

   for (p = 0; p < AMB_BLUR_PASSES; p++)
   {
      amb_box(img, line, AMB_BLUR_RADIUS, 1);
      amb_box(img, line, AMB_BLUR_RADIUS, 0);
   }
   for (p = 0; p < extra_passes; p++)
   {
      amb_box(img, line, AMB_COVER_BLUR_RADIUS, 1);
      amb_box(img, line, AMB_COVER_BLUR_RADIUS, 0);
   }

   for (p = 0; p < AMB_W * AMB_H; p++)
   {
      int r = img[p * 3], gg = img[p * 3 + 1], b = img[p * 3 + 2];
      int l = (77 * r + 150 * gg + 29 * b) >> 8;
      r  = l + ((r  - l) * AMB_SATURATION >> 8);
      gg = l + ((gg - l) * AMB_SATURATION >> 8);
      b  = l + ((b  - l) * AMB_SATURATION >> 8);
      r  += (AMB_TINT_R - r)  * AMB_TINT_AMOUNT >> 8;
      gg += (AMB_TINT_G - gg) * AMB_TINT_AMOUNT >> 8;
      b  += (AMB_TINT_B - b)  * AMB_TINT_AMOUNT >> 8;
      out[p * 4]     = (unsigned char)amb_clamp255(r);
      out[p * 4 + 1] = (unsigned char)amb_clamp255(gg);
      out[p * 4 + 2] = (unsigned char)amb_clamp255(b);
      out[p * 4 + 3] = 255;
   }

   /* A new texture each bake: the old one may be on a frame in flight. */
   if (g_amb_tex)
      vb_tex_destroy(g_amb_tex);
   g_amb_tex = vb_tex_create(AMB_W, AMB_H, VB_FMT_RGBA8, 0);
   if (!g_amb_tex)
      return -1;
   vb_tex_update(g_amb_tex, 0, 0, AMB_W, AMB_H, out, AMB_W * 4);
   return 0;
}

int vid_ambient_bake_art(const uint16_t *src, int stride, int w, int h,
                         int precomposed)
{
   unsigned t0 = pspcompat_time_us(), us;
   int rc = amb_bake(src, stride, w, h, 4, 0, 0, 0);
   us = pspcompat_time_us() - t0;
   g_amb_src = rc == 0 ? AMB_SRC_ART : AMB_SRC_NONE;
   g_amb_scrim = precomposed ? AMB_SCRIM_HERO : AMB_SCRIM_BOXART;
   fe_evt("ambient_bake src=art w=%d h=%d hero=%d rc=%d us=%u", w, h,
          precomposed, rc, us);
   FE_EVT_ONLY(us);
   return rc == 0 ? 0 : -1;
}

int vid_ambient_bake_cover(const uint16_t *src, int stride, int w, int h)
{
   unsigned t0 = pspcompat_time_us(), us;
   int rc = amb_bake(src, stride, w, h, 4, 0, 0, AMB_COVER_BLUR_PASSES);
   us = pspcompat_time_us() - t0;
   g_amb_src = rc == 0 ? AMB_SRC_ART : AMB_SRC_NONE;
   g_amb_scrim = AMB_SCRIM_BOXART;
   fe_evt("ambient_bake src=cover w=%d h=%d rc=%d us=%u", w, h, rc, us);
   FE_EVT_ONLY(us);
   return rc == 0 ? 0 : -1;
}

/* The snapshot fallback: once per presented frame; does nothing unless
 * "art, else game" is on and there is no art. */
static void amb_snap_maybe(const uint16_t *src, int stride, unsigned w,
                           unsigned h)
{
   int rc;
   if (g_amb_mode != 2 || g_amb_src != AMB_SRC_NONE || g_amb_snap_done || !src)
      return;
   g_amb_frames++;
   if (g_amb_frames < AMB_SNAP_AFTER ||
       (g_amb_frames - AMB_SNAP_AFTER) % AMB_SNAP_RETRY)
      return;
   if (g_amb_frames > AMB_SNAP_GIVE_UP)
   {
      g_amb_snap_done = 1;
      fe_evt("ambient_bake src=game rc=gave_up frame=%u", g_amb_frames);
      return;
   }
   rc = amb_bake(src, stride, (int)w, (int)h, 1, AMB_SNAP_MIN_LUMA, 1, 0);
   if (rc == 0)
   {
      g_amb_src = AMB_SRC_SNAP;
      g_amb_scrim = AMB_SCRIM_FRAME;
      g_amb_snap_done = 1;
   }
   else if (rc < 0)
      g_amb_snap_done = 1;
   fe_evt("ambient_bake src=game rc=%d frame=%u w=%u h=%u", rc, g_amb_frames,
          w, h);
}

/* Open the frame for a draw that defines every pixel, and paint what will
 * show around the picture: the ambient texture, the palette ramp, or black.
 * `covers`: the picture hides all of it, so only the open matters. */
static void backdrop_draw(int covers)
{
   int fresh = 0, kind = amb_kind();
   if (!vb_frame_open())
   {
      vb_frame_begin(1);
      fresh = 1;
   }
   g_clear_req = 0;
   vb_scissor(0, 0, 0, 0, 0);
   if (covers)
      return;
   if (kind == 2)
   {
      unsigned k = 255u - (unsigned)g_amb_scrim;
      vb_tquad q;
      q.x0 = 0; q.y0 = 0; q.x1 = VB_W; q.y1 = VB_H;
      q.u0 = 0; q.v0 = 0; q.u1 = AMB_W; q.v1 = AMB_H;
      /* The scrim, for free: texel x (k,k,k) is the black-alpha blend. */
      vb_tquads(g_amb_tex, &q, 1, VB_RGBA(k, k, k, 255), VB_LINEAR);
   }
   else if (kind == 1)
   {
      uint32_t top = amb_dim(g_amb_shade, AMB_PAL_RAMP_TOP);
      uint32_t bot = amb_dim(g_amb_shade, AMB_PAL_RAMP_BOT);
      vb_quad(0, 0, VB_W, VB_H, top, top, bot, bot);
   }
   else if (!fresh)
      vb_quad(0, 0, VB_W, VB_H, 0xFF000000u, 0xFF000000u, 0xFF000000u,
              0xFF000000u);
}

/* ===========================================================================
 * THE GAME PICTURE
 *
 *   nearest          one quad, point sampled
 *   bilinear         one quad, linear
 *   sharp bilinear   docs/SHARP-BILINEAR.md, with the GPU doing both passes:
 *                    the frame is drawn NEAREST at an integer factor
 *                    k = ceil(scale) into a render target, and that is drawn
 *                    LINEAR at the final size -- crisp square pixels, only
 *                    the output pixel on each source-pixel edge blended.
 *                    At an exact integer scale it IS nearest, so it draws
 *                    nearest (2x, 3x and the cropped 4x are always that).
 *   LCD grid         sharp bilinear (or nearest), then a grid texture over
 *                    the picture: see lcd_tex().
 *
 * The render target is 960 x 640 -- GBA at k = 4, the largest prescale any
 * mode asks for (fit and stretch are both 3.4x tall) -- and is taken on the
 * first sharp frame.  Without it sharp falls back to bilinear.
 * ======================================================================== */

#define SHARP_RT_W 1152        /* the wide picture's 282 columns, 4 times */
#define SHARP_RT_H 640

enum { PIC_NEAREST = 0, PIC_LINEAR = 1, PIC_SHARP = 2 };

static vb_tex *g_sharp_rt;
static int     g_sharp_tried;

static vb_tex *sharp_target(void)
{
   if (!g_sharp_tried)
   {
      g_sharp_tried = 1;
      g_sharp_rt = vb_tex_create(SHARP_RT_W, SHARP_RT_H, VB_FMT_RGBA8, 1);
      fe_evt("sharp_target rc=%d size=%dx%d", g_sharp_rt ? 0 : -1,
             SHARP_RT_W, SHARP_RT_H);
   }
   return g_sharp_rt;
}

static int geo_integer(unsigned w, const vid_geo *g)
{
   return w && g->sh > 0 && g->dw % (int)w == 0 && g->dh % g->sh == 0;
}

/* How the picture is sampled; kx, ky are set for PIC_SHARP. */
static int picture_mode(unsigned w, const vid_geo *g, int *kx, int *ky)
{
   int x, y;
   if (geo_integer(w, g) || g_filter == VID_FILTER_NEAREST)
      return PIC_NEAREST;
   if (g_filter == VID_FILTER_BILINEAR)
      return PIC_LINEAR;
   x = (g->dw + (int)w - 1) / (int)w;
   y = (g->dh + g->sh - 1) / g->sh;
   if (x * (int)w > SHARP_RT_W || y * g->sh > SHARP_RT_H || !sharp_target())
      return PIC_LINEAR;
   *kx = x;
   *ky = y;
   return PIC_SHARP;
}

/* The LCD grid: one alpha byte per DESTINATION pixel of the picture.  The
 * last native pixel of every source pixel -- its right column and its bottom
 * row -- is darkened, the way the gaps between an LCD's cells are.  Computed
 * as coverage, so a non-integer scale (fit is 3.4x) gets an anti-aliased
 * grid and no moire.  Rebuilt only when the picture's geometry changes. */
#define LCD_ALPHA 62           /* the gap's darkness at 3x and above, of 255 */
#define SCAN_ALPHA 96          /* ...and of a scanline                       */
static vb_tex *g_lcd_tex;
static int     g_lcd_dw, g_lcd_dh, g_lcd_w, g_lcd_sh, g_lcd_rows;

static void lcd_axis(unsigned char *cov, int dst, int src)
{
   /* Source pixel n ends at X = (n+1) * dst / src; its gap is [X-1, X). */
   int i;
   double k = (double)dst / (double)src;
   for (i = 0; i < dst; i++)
   {
      double a = (double)i, b = a + 1.0, c = 0.0;
      int n0 = (int)(a / k), n;
      for (n = n0; n <= n0 + 2 && n < src; n++)
      {
         double hi = (double)(n + 1) * k, lo = hi - 1.0;
         double l = a > lo ? a : lo, h = b < hi ? b : hi;
         if (h > l)
            c += h - l;
      }
      cov[i] = (unsigned char)(c >= 1.0 ? 255 : (int)(c * 255.0 + 0.5));
   }
}

static vb_tex *lcd_tex(unsigned w, const vid_geo *g)
{
   static unsigned char cx[VB_W], cy[VB_H];
   unsigned char *px;
   int x, y, strength;
   /* SCANLINES: the row gaps alone, a little darker than the grid's. */
   const int rows_only = (g_filter == VID_FILTER_SCANLINES);
   double k;
   if (g->dw > VB_W || g->dh > VB_H || g->dw <= 0 || g->dh <= 0)
      return NULL;
   if (g_lcd_tex && g_lcd_dw == g->dw && g_lcd_dh == g->dh &&
       g_lcd_w == (int)w && g_lcd_sh == g->sh && g_lcd_rows == rows_only)
      return g_lcd_tex;
   if (g_lcd_tex)
      vb_tex_destroy(g_lcd_tex);
   g_lcd_tex = NULL;
   px = (unsigned char *)malloc((size_t)g->dw * g->dh);
   if (!px)
      return NULL;
   /* A 1 px gap in a 2 px cell is half the picture: ease it off below 3x. */
   k = (double)g->dw / (double)w;
   strength = k >= 3.0 ? LCD_ALPHA : (int)(LCD_ALPHA * (k - 1.0) / 2.0);
   if (strength < 0)
      strength = 0;
   if (rows_only)
      strength = strength * SCAN_ALPHA / LCD_ALPHA;
   lcd_axis(cx, g->dw, (int)w);
   lcd_axis(cy, g->dh, g->sh);
   for (y = 0; y < g->dh; y++)
      for (x = 0; x < g->dw; x++)
      {
         /* 1 - (1-cx)(1-cy): the union of the column gap and the row gap. */
         int u = rows_only ? cy[y]
                 : 255 - ((255 - cx[x]) * (255 - cy[y]) + 127) / 255;
         px[(size_t)y * g->dw + x] = (unsigned char)(u * strength / 255);
      }
   g_lcd_tex = vb_tex_create(g->dw, g->dh, VB_FMT_A8, 0);
   if (g_lcd_tex)
      vb_tex_update(g_lcd_tex, 0, 0, g->dw, g->dh, px, (size_t)g->dw);
   free(px);
   g_lcd_dw = g->dw; g_lcd_dh = g->dh; g_lcd_w = (int)w; g_lcd_sh = g->sh;
   g_lcd_rows = rows_only;
   return g_lcd_tex;
}

/* ---- xBRZ on the GPU ------------------------------------------------------
 * The "xBRZ smooth" filter drawn by the GPU (vback.h VB_PROG_XBRZ0..2; the
 * shaders are gpsp/vita/shaders/xbrz_*.inc) instead of the CPU upscalers:
 *
 *   pass 0  every point where four picture pixels meet, into a render
 *           target 1:1 (one pixel larger each way): which of the four it
 *           cuts, and how hard -- xBRZ's own preprocessing
 *   pass 1  per picture pixel, its four corners from those points and
 *           whether each blends as a line, into a second target 1:1
 *   pass 2  the picture at its final size: every output pixel worked out
 *           from the picture and pass 1 -- no 4x picture in between, no
 *           linear squeeze from 4x to 3.4x, every frame
 *
 * The CPU's cores do nothing for it, so there is no factor to drop at a
 * fade and no picture that comes a frame late.  OFF unless vita.ini
 * gpu_xbrz = 1: measured on the console (16 Oct, Emerald at fit), the GPU
 * took 31 ms over a frame with it against 2.6 ms with the CPU's xBRZ
 * textures -- passes 0 and 1 about 7 ms, pass 2's 444,000 pixels the rest
 * -- so it cannot hold 60 frames a second; the guard below turns it off
 * after two samples if it is asked for anyway.  It also needs the
 * system's shader compiler (ur0:data/libshacccg.suprx), which took 26 s
 * the first time (the programs are kept on the card afterwards).  Not for
 * the two pictures of a pair, which keep the CPU's. */
#define GX_RT_W 384            /* the widest picture: the wide view's 282+ */
#define GX_RT_H 256
/* THE GUARD.  Whether the GPU keeps up is measured, not assumed: after the
 * first GX_FIRST pictures, then every GX_EVERY, the next GX_SAMPLE presents
 * wait for the GPU and time it (vb_gpu_time).  Over GX_SLOW_US of GPU time
 * a frame, it is measured again GX_FIRST pictures later; two slow samples
 * in a row and xBRZ goes back to the CPU's cores for the rest of the
 * session (`gpu_xbrz off reason=slow`). */
#define GX_FIRST   60
#define GX_EVERY   1800
#define GX_SAMPLE  8
#define GX_SLOW_US 10000      /* the drain serialises CPU and GPU: 16.7 ms less
                                * the frame's ~3-5 ms of CPU, with margin */
static int     g_gx_on = 0;    /* vid_set_gpu_xbrz                          */
static int     g_gx_state;     /* 0 untried, 1 ready, -1 not available,
                                * -2 too slow (the guard)                   */
static vb_tex *g_gx_rt, *g_gx_rt0;   /* pass 1's target, pass 0's      */
static unsigned g_gx_frames;   /* pictures drawn this way (vid_gpu_xbrz)   */
static int      g_gx_always;   /* vid_set_gpu_time: every present timed    */
static int      g_gx_sampling, g_gx_slow;
static unsigned g_gx_mark, g_gx_samples, g_gx_avg, g_gx_max;

void vid_set_gpu_xbrz(int on)
{
   g_gx_on = on ? 1 : 0;
}

void vid_set_gpu_time(int on)
{
   g_gx_always = on ? 1 : 0;
   vb_gpu_time(on ? -1 : 0);
}

/* After each present (vid_swap). */
static void gx_guard(void)
{
   unsigned n, avg, mx;
   if (g_gx_always)
   {
      vb_gpu_stats(&n, &avg, &mx);
      if (n)
      {
         g_gx_avg = avg;
         if (mx > g_gx_max)
            g_gx_max = mx;
         g_gx_samples++;
      }
      return;
   }
   if (g_gx_state <= 0)
      return;
   if (g_gx_sampling)
   {
      if (vb_gpu_timing() > 0)
         return;
      vb_gpu_stats(&n, &avg, &mx);
      g_gx_sampling = 0;
      g_gx_mark = g_gx_frames;
      if (!n)
         return;
      g_gx_samples++;
      g_gx_avg = avg;
      g_gx_max = mx;
      fe_evt("gpu_xbrz_time frames=%u gpu_us=%u/%u", n, avg, mx);
      g_gx_slow = avg > GX_SLOW_US ? g_gx_slow + 1 : 0;
      if (g_gx_slow >= 2)
      {
         g_gx_state = -2;
         fe_evt("gpu_xbrz off reason=slow gpu_us=%u", avg);
      }
      return;
   }
   /* (a slow sample is checked again soon, not in half a minute) */
   if (g_gx_frames - g_gx_mark >= (g_gx_samples && !g_gx_slow ? GX_EVERY
                                                               : GX_FIRST))
   {
      vb_gpu_time(GX_SAMPLE);
      g_gx_sampling = 1;
   }
}

/* vita.ini gpu_probe: what the shader compiler says to test programs
 * (vback.h vb_prog_probe), one `gpu_probe` line each. */
void vid_gpu_probe(void)
{
   char line[512];
   int n;
   if (!g_inited)
      vid_init();
   for (n = 0; vb_prog_probe(n, line, sizeof(line)); n++)
      fe_evt("gpu_probe %s", line);
}

/* The GPU's time over a frame, last measured (0 = never), and the slowest
 * since the last call. */
void vid_gpu_time_stats(unsigned *samples, unsigned *avg_us, unsigned *max_us)
{
   if (samples) *samples = g_gx_samples;
   if (avg_us) *avg_us = g_gx_avg;
   if (max_us) *max_us = g_gx_max;
   g_gx_max = 0;
}

static int gx_ready(void)
{
   if (!g_gx_on || g_filter != VID_FILTER_XBRZ)
      return 0;
   if (!g_gx_state)
   {
      /* (the first time on a console: the compiler runs, a hitch of the
       * size `us` says, once -- afterwards the programs come from the
       * card) */
      const unsigned t0 = pspcompat_time_us();
      vb_prog *p0 = vb_prog_get(VB_PROG_XBRZ0);
      vb_prog *p1 = p0 ? vb_prog_get(VB_PROG_XBRZ1) : NULL;
      vb_prog *p2 = p1 ? vb_prog_get(VB_PROG_XBRZ2) : NULL;
      if (p0 && p1 && p2)
      {
         g_gx_rt0 = vb_tex_create(GX_RT_W, GX_RT_H, VB_FMT_RGBA8, 1);
         g_gx_rt = vb_tex_create(GX_RT_W, GX_RT_H, VB_FMT_RGBA8, 1);
      }
      g_gx_state = p0 && p1 && p2 && g_gx_rt && g_gx_rt0 ? 1 : -1;
      fe_evt("gpu_xbrz on=%d pass0=%s pass1=%s pass2=%s rt=%d us=%u",
             g_gx_state > 0, vb_prog_status(VB_PROG_XBRZ0),
             vb_prog_status(VB_PROG_XBRZ1), vb_prog_status(VB_PROG_XBRZ2),
             g_gx_rt && g_gx_rt0 ? 1 : 0, pspcompat_time_us() - t0);
   }
   return g_gx_state > 0;
}

/* Is the GPU drawing xBRZ (for the log), and how many pictures so far. */
int vid_gpu_xbrz(unsigned *frames)
{
   if (frames)
      *frames = g_gx_frames;
   return g_gx_state > 0 && g_gx_on;
}

/* vita.ini gpu_probe = N >= 2: at the Nth GPU picture, what passes 0 and 1
 * left in their targets
 * (texels not zero, the largest byte of each channel, a row of samples):
 * is the GPU doing them at all. */
static int g_gx_debug;
void vid_set_gpu_debug(int on) { g_gx_debug = on; }
/* vita.ini gpu_xbrz_view = 1: the screen shows pass 1's bytes (red = top
 * left corner, green = top right, blue = the bottom two); 2: pass 0's,
 * 40 times brighter.  A diagnostic: is the GPU working the passes out. */
static int g_gx_view;
void vid_set_gpu_view(int v) { g_gx_view = v; }

static void gx_debug_dump(vb_tex *t, unsigned w, unsigned h)
{
   static uint32_t px[GX_RT_W * GX_RT_H];
   vb_tex *rts[3] = { g_gx_rt0, g_gx_rt, g_gx_rt };
   const unsigned ws[3] = { w + 1, w, w }, hs[3] = { h + 1, h, h };
   int k;
   for (k = 0; k < 3; k++)
   {
      if (k == 2)
      {
         /* (a control: the picture itself, drawn by libvita2d's own
          * program into the same target -- can it be read back at all) */
         vb_tquad q;
         q.x0 = 0; q.y0 = 0; q.x1 = (float)w; q.y1 = (float)h;
         q.u0 = 0; q.v0 = 0; q.u1 = (float)w; q.v1 = (float)h;
         vb_rt_begin(g_gx_rt);
         vb_tquads(t, &q, 1, 0xFFFFFFFFu, VB_NEAREST);
         vb_rt_end();
      }
      unsigned x, y, nz = 0, mx[4] = { 0, 0, 0, 0 };
      char row[200];
      int at = 0;
      if (vb_tex_read(rts[k], px) != 0)
      {
         fe_evt("gpu_rt pass=%d read=failed", k);
         continue;
      }
      for (y = 0; y < hs[k]; y++)
         for (x = 0; x < ws[k]; x++)
         {
            uint32_t c = px[y * GX_RT_W + x];
            int b;
            if (c & 0x00FFFFFFu)
               nz++;
            for (b = 0; b < 4; b++)
               if (((c >> (8 * b)) & 0xFF) > mx[b])
                  mx[b] = (c >> (8 * b)) & 0xFF;
         }
      /* a few texels of the middle row */
      for (x = 0; x < 8 && at < (int)sizeof(row) - 12; x++)
         at += snprintf(row + at, sizeof(row) - (size_t)at, "%s%08x",
                        x ? "," : "",
                        (unsigned)px[(hs[k] / 2) * GX_RT_W + ws[k] / 2 + x]);
      fe_evt("gpu_rt pass=%d w=%u h=%u nonzero=%u max=%u/%u/%u/%u mid=%s", k,
             ws[k], hs[k], nz, mx[0], mx[1], mx[2], mx[3], row);
   }
}

/* The w x h texture `t`, rows sy0 .. sy0+sh of it, onto g's rect.  Passes
 * 0 and 1 open scenes of their own: called before anything else of the
 * frame is drawn (as sharp bilinear's prescale is), they cost no suspended
 * frame. */
static int gx_draw(vb_tex *t, unsigned w, unsigned h, const vid_geo *g)
{
   float u[VB_PROG_UNIFORMS][4];
   vb_tquad q;
   vb_prog *p0 = vb_prog_get(VB_PROG_XBRZ0), *p1 = vb_prog_get(VB_PROG_XBRZ1);
   vb_prog *p2 = vb_prog_get(VB_PROG_XBRZ2);
   if (!p0 || !p1 || !p2 || !g_gx_rt || !g_gx_rt0 || w + 1 > GX_RT_W ||
       h + 1 > GX_RT_H || !w || !h || g->sh <= 0)
      return 0;
   memset(u, 0, sizeof(u));
   u[0][0] = (float)w; u[0][1] = (float)h;
   u[0][2] = 1.0f / (float)w; u[0][3] = 1.0f / (float)h;
   /* 0: the points, texel (i+1, j+1) for pixel (i, j) */
   q.x0 = 0; q.y0 = 0; q.x1 = (float)(w + 1); q.y1 = (float)(h + 1);
   q.u0 = -1; q.v0 = -1; q.u1 = (float)w; q.v1 = (float)h;
   vb_rt_begin(g_gx_rt0);
   vb_pquads(p0, t, NULL, &q, 1, u);
   vb_rt_end();
   /* 1: each pixel's corners */
   u[1][0] = 1.0f / (float)GX_RT_W;
   u[1][1] = 1.0f / (float)GX_RT_H;
   u[1][2] = g_gx_view == 2 ? 1.0f : 0.0f;
   q.x0 = 0; q.y0 = 0; q.x1 = (float)w; q.y1 = (float)h;
   q.u0 = 0; q.v0 = 0; q.u1 = (float)w; q.v1 = (float)h;
   vb_rt_begin(g_gx_rt);
   vb_pquads(p1, t, g_gx_rt0, &q, 1, u);
   vb_rt_end();
   if (g_gx_debug && g_gx_frames == (unsigned)g_gx_debug)
      gx_debug_dump(t, w, h);
   /* 2: the picture at its size */
   backdrop_draw(geo_covers(g));
   u[1][0] = (float)w / (float)GX_RT_W;
   u[1][1] = (float)h / (float)GX_RT_H;
   u[1][2] = 0.0f;
   u[2][0] = (float)g->dw / (float)w;
   u[2][1] = (float)g->dh / (float)g->sh;
   u[2][2] = g_gx_view ? 1.0f : 0.0f;
   q.x0 = (float)g->ox; q.y0 = (float)g->oy;
   q.x1 = (float)(g->ox + g->dw); q.y1 = (float)(g->oy + g->dh);
   q.u0 = 0; q.v0 = (float)g->sy0;
   q.u1 = (float)w; q.v1 = (float)(g->sy0 + g->sh);
   vb_pquads(p2, t, g_gx_rt, &q, 1, u);
   g_gx_frames++;
   return 1;
}

/* A whole frame: the backdrop, then the w-wide source texture `t` at g. */
static void picture_draw(vb_tex *t, unsigned w, const vid_geo *g)
{
   int kx = 1, ky = 1, mode;
   vb_tquad q;

   if (!t)
   {
      backdrop_draw(0);
      return;
   }
   if (gx_ready() && gx_draw(t, w, (unsigned)vb_tex_h(t), g))
      return;
   mode = picture_mode(w, g, &kx, &ky);
   if (mode == PIC_SHARP)
   {
      /* 1. PRESCALE, nearest, into the render target -- before the frame
       *    opens whenever this is the frame's first draw (it always is for
       *    the live picture and for the menu's backdrop). */
      int iw = kx * (int)w, ih = ky * g->sh, n = 0;
      vb_tquad e[4];
      float last = (float)(g->sy0 + g->sh);
      e[n].x0 = 0; e[n].y0 = 0; e[n].x1 = (float)iw; e[n].y1 = (float)ih;
      e[n].u0 = 0; e[n].v0 = (float)g->sy0; e[n].u1 = (float)w; e[n].v1 = last;
      n++;
      /* The last column and row once more, one texel past the picture, when
       * the target is larger than it: a bilinear tap at the far edge then
       * reads the edge colour, never last frame's leftovers. */
      if (iw < SHARP_RT_W)
      {
         e[n].x0 = (float)iw; e[n].y0 = 0;
         e[n].x1 = (float)iw + 1; e[n].y1 = (float)ih;
         e[n].u0 = (float)w - 1; e[n].v0 = (float)g->sy0;
         e[n].u1 = (float)w; e[n].v1 = last;
         n++;
      }
      if (ih < SHARP_RT_H)
      {
         e[n].x0 = 0; e[n].y0 = (float)ih;
         e[n].x1 = (float)iw; e[n].y1 = (float)ih + 1;
         e[n].u0 = 0; e[n].v0 = last - 1; e[n].u1 = (float)w; e[n].v1 = last;
         n++;
      }
      if (iw < SHARP_RT_W && ih < SHARP_RT_H)
      {
         e[n].x0 = (float)iw; e[n].y0 = (float)ih;
         e[n].x1 = (float)iw + 1; e[n].y1 = (float)ih + 1;
         e[n].u0 = (float)w - 1; e[n].v0 = last - 1;
         e[n].u1 = (float)w; e[n].v1 = last;
         n++;
      }
      vb_rt_begin(g_sharp_rt);
      vb_tquads(t, e, n, 0xFFFFFFFFu, VB_NEAREST);
      vb_rt_end();

      /* 2. RESAMPLE the target, linear, at the final size. */
      backdrop_draw(geo_covers(g));
      q.x0 = (float)g->ox; q.y0 = (float)g->oy;
      q.x1 = (float)(g->ox + g->dw); q.y1 = (float)(g->oy + g->dh);
      q.u0 = 0; q.v0 = 0; q.u1 = (float)iw; q.v1 = (float)ih;
      vb_tquads(g_sharp_rt, &q, 1, 0xFFFFFFFFu, VB_LINEAR);
   }
   else
   {
      backdrop_draw(geo_covers(g));
      q.x0 = (float)g->ox; q.y0 = (float)g->oy;
      q.x1 = (float)(g->ox + g->dw); q.y1 = (float)(g->oy + g->dh);
      q.u0 = 0; q.v0 = (float)g->sy0;
      q.u1 = (float)w; q.v1 = (float)(g->sy0 + g->sh);
      vb_tquads(t, &q, 1, 0xFFFFFFFFu,
                mode == PIC_LINEAR ? VB_LINEAR : VB_NEAREST);
   }
   if (g_filter == VID_FILTER_LCD || g_filter == VID_FILTER_SCANLINES)
   {
      vb_tex *lcd = lcd_tex(w, g);
      if (lcd)
      {
         q.x0 = (float)g->ox; q.y0 = (float)g->oy;
         q.x1 = (float)(g->ox + g->dw); q.y1 = (float)(g->oy + g->dh);
         q.u0 = 0; q.v0 = 0; q.u1 = (float)g->dw; q.v1 = (float)g->dh;
         /* Black, the texture's byte as alpha. */
         vb_tquads(lcd, &q, 1, VB_RGBA(0, 0, 0, 255), VB_NEAREST);
      }
   }
}

static void prof_add(unsigned stage, unsigned gu)
{
   g_stage_us_tot += stage;
   if (stage > g_stage_us_max)
      g_stage_us_max = stage;
   g_gu_us_tot += gu;
   if (gu > g_gu_us_max)
      g_gu_us_max = gu;
   g_prof_frames++;
}

/* ---- colour and motion (vid_set_look) -----------------------------------
 * Two things a handheld's LCD did to a game's picture, done here to the
 * SOURCE frame before it is staged -- so the exact, sharp and LCD paths, the
 * CPU upscalers and both machines of a pair all show it, and so does the
 * snapshot behind the menu (vid_frame_to_psp565):
 *
 *   colour  GBA games were drawn for a dim, washed-out reflective screen and
 *           are garish on a modern panel (most of all the 1000's OLED).  The
 *           table is gpSP's own correction (tools/generate_cc_lut.c: the
 *           channel mix measured from a real GBA, 2.2 gamma), built at run
 *           time so it can be had with or without that tool's extra
 *           darkening.
 *   blend   each frame mixed half and half with the one before it, as the
 *           slow LCD did: games that flicker a sprite every other frame to
 *           fake transparency show it steady.
 *
 * 38,400 pixels through a table: a few hundred microseconds on the console. */
static int       g_color, g_blend;
static uint16_t *g_cc;                 /* 65536 entries, libretro 565 -> 565 */
static int       g_cc_mode;            /* the mode g_cc was built for       */
static uint16_t *g_look_out[2], *g_look_prev[2];
static size_t    g_look_cap[2];
static unsigned  g_look_w[2], g_look_h[2];   /* the frame in prev; 0 = none */

static void cc_build(int mode)
{
   /* generate_cc_lut.c's constants */
   static const float LUM = 0.94f;
   static const float M[3][3] = {
      { 0.82f,  0.24f,  -0.06f },      /* red   <- r, g, b */
      { 0.125f, 0.665f,  0.21f },      /* green            */
      { 0.195f, 0.075f,  0.73f } };    /* blue             */
   const float gamma = 2.2f + (mode == VID_COLOR_GBA_DARK ? 1.0f : 0.0f);
   /* Compression (x ^ 1/2.2) through a table over sqrt(x): steep near black
    * in x, nearly straight in sqrt(x), so 2048 steps are far finer than a
    * 6-bit result.  A table and sqrtf, not 200,000 powf calls: nothing
    * stalls when the row is changed. */
   enum { OUT_N = 2048 };
   static float out[OUT_N + 1];
   float lin5[32], lin6[64];
   unsigned v;
   int i;

   if (!g_cc)
      g_cc = (uint16_t *)malloc(65536 * sizeof(uint16_t));
   if (!g_cc)
      return;
   for (i = 0; i < 32; i++)
      lin5[i] = powf((float)i / 31.0f, gamma);
   for (i = 0; i < 64; i++)
      lin6[i] = powf((float)i / 63.0f, gamma);
   for (i = 0; i <= OUT_N; i++)
      out[i] = powf((float)i / (float)OUT_N, 2.0f / 2.2f);
   for (v = 0; v < 65536; v++)
   {
      const float r = lin5[v >> 11], g = lin6[(v >> 5) & 63], b = lin5[v & 31];
      unsigned c[3];
      for (i = 0; i < 3; i++)
      {
         float x = LUM * (M[i][0] * r + M[i][1] * g + M[i][2] * b);
         if (x < 0.0f) x = 0.0f;
         if (x > 1.0f) x = 1.0f;
         x = out[(int)(sqrtf(x) * (float)OUT_N + 0.5f)];
         c[i] = (unsigned)(x * (i == 1 ? 63.0f : 31.0f) + 0.5f);
      }
      g_cc[v] = (uint16_t)((c[0] << 11) | (c[1] << 5) | c[2]);
   }
   g_cc_mode = mode;
}

void vid_set_look(int color, int blend)
{
   if (color < 0 || color >= VID_COLOR_MODES)
      color = VID_COLOR_RAW;
   if (color != VID_COLOR_RAW && (!g_cc || g_cc_mode != color))
      cc_build(color);
   g_color = (color != VID_COLOR_RAW && g_cc) ? color : VID_COLOR_RAW;
   g_blend = blend ? 1 : 0;
   g_look_w[0] = g_look_w[1] = 0;     /* nothing to mix the next frame with */
}

static const uint16_t *vv_look_lut(void)
{
   return g_color ? g_cc : NULL;
}

int vid_look_color(void) { return g_color; }
int vid_look_blend(void) { return g_blend; }

/* The frame as it is to be shown: `pix` itself when nothing is switched on,
 * else a private copy (tightly packed: *pitch_bytes is updated).  `who` is 0
 * for the one picture or the pair's left, 1 for the pair's right; each keeps
 * its own previous frame. */
static const uint16_t *look_frame(int who, const uint16_t *pix, unsigned w,
                                  unsigned h, size_t *pitch_bytes)
{
   const uint16_t *lut = g_color ? g_cc : NULL;
   const size_t n = (size_t)w * h, pitch = *pitch_bytes / 2;
   uint16_t *o, *p;
   unsigned x, y;
   int mix;

   if (!lut && !g_blend)
      return pix;
   if (n > g_look_cap[who])
   {
      free(g_look_out[who]);
      free(g_look_prev[who]);
      g_look_out[who]  = (uint16_t *)malloc(n * sizeof(uint16_t));
      g_look_prev[who] = (uint16_t *)malloc(n * sizeof(uint16_t));
      g_look_w[who] = 0;
      if (!g_look_out[who] || !g_look_prev[who])
      {
         free(g_look_out[who]);
         free(g_look_prev[who]);
         g_look_out[who] = g_look_prev[who] = NULL;
         g_look_cap[who] = 0;
         return pix;
      }
      g_look_cap[who] = n;
   }
   mix = g_blend && g_look_w[who] == w && g_look_h[who] == h;
   for (y = 0; y < h; y++)
   {
      const uint16_t *s = pix + pitch * y;
      o = g_look_out[who] + (size_t)w * y;
      p = g_look_prev[who] + (size_t)w * y;
      if (mix)
         for (x = 0; x < w; x++)
         {
            /* the mean of two 565 pixels, field by field, without unpacking */
            const unsigned a = s[x], b = p[x];
            const unsigned m = (a & b) + (((a ^ b) & 0xF7DEu) >> 1);
            p[x] = (uint16_t)a;
            o[x] = lut ? lut[m] : (uint16_t)m;
         }
      else if (g_blend)
         for (x = 0; x < w; x++)
         {
            p[x] = s[x];
            o[x] = lut ? lut[s[x]] : s[x];
         }
      else
         for (x = 0; x < w; x++)
            o[x] = lut[s[x]];
   }
   g_look_w[who] = g_blend ? w : 0;
   g_look_h[who] = h;
   *pitch_bytes = (size_t)w * sizeof(uint16_t);
   return g_look_out[who];
}

/* ---- the CPU upscalers (upscale_vita.h) ----------------------------------
 * With VID_FILTER_XBRZ / _SCALE the picture is scaled from its source size
 * by a whole factor on the cores the emulator leaves idle, into a texture,
 * and that is what is drawn: 1:1 where the factor is the scale (3x), linear
 * to the final rect otherwise (fit is 3.4x).
 *
 * Each picture (the game's, and the pair's second) has TWO textures: the
 * one on screen and the one being written.  A frame hands its picture over
 * and draws whatever was finished last -- on a console that is the frame
 * before; the game is never held up.  When nothing is finished yet (the
 * first frame, or no memory) the caller falls back to the sharp path. */
static struct
{
   vb_tex  *t[2];
   int      kind, f, front, pending;
   int      pf, ff;              /* the factor the pending / front picture
                                  * was scaled by (f, or one less: it then
                                  * fills the top left of the texture)   */
   int      pox, poy, fox, foy;  /* the ring's turn (upscale_ring) of the
                                  * pending picture and of the front one */
   unsigned w, h, seen;        /* seen: g_frame_no + 1 of the last use */
} g_upt[UPSCALE_SLOTS];

static int up_kind(void)
{
   return g_filter == VID_FILTER_XBRZ ? UPSCALE_XBRZ
        : g_filter == VID_FILTER_SCALE ? UPSCALE_SCALE : UPSCALE_NONE;
}

/* Nothing is being written into the slot's textures any more, and they are
 * gone. */
static void up_drop(int slot)
{
   int k;
   upscale_forget(slot);
   for (k = 0; k < 2; k++)
      if (g_upt[slot].t[k])
      {
         vb_tex_destroy(g_upt[slot].t[k]);
         g_upt[slot].t[k] = NULL;
      }
   g_upt[slot].kind = UPSCALE_NONE;
   g_upt[slot].front = -1;
   g_upt[slot].pf = g_upt[slot].ff = 0;
}

/* The picture handed over last is finished: it is the one to draw. */
static void up_front(int slot)
{
   g_upt[slot].front = g_upt[slot].pending;
   g_upt[slot].ff = g_upt[slot].pf;
   g_upt[slot].fox = g_upt[slot].pox;
   g_upt[slot].foy = g_upt[slot].poy;
}

static vb_tex *up_stage(int slot, const uint16_t *pix, unsigned w,
                        unsigned h, size_t pitch_bytes, int dw, int *f_out)
{
   int kind = up_kind(), f, k;
   void *px[2];
   size_t tp[2] = { 0, 0 };

   if (kind == UPSCALE_NONE || !(f = upscale_factor(slot, kind, w, h, dw)))
   {
      if (g_upt[slot].t[0] || g_upt[slot].t[1])
         up_drop(slot);
      return NULL;
   }
   unsigned ta = pspcompat_time_us(), tb;
   if (g_upt[slot].kind != kind || g_upt[slot].f != f ||
       g_upt[slot].w != w || g_upt[slot].h != h || !g_upt[slot].t[0] ||
       !g_upt[slot].t[1])
   {
      unsigned t0 = pspcompat_time_us();
      up_drop(slot);
      vb_tex_mem(g_tex_cpu);
      for (k = 0; k < 2; k++)
         g_upt[slot].t[k] = vb_tex_create(f * (int)w, f * (int)h,
                                          VB_FMT_RGBA8, 0);
      vb_tex_mem(0);
      fe_evt("up_tex slot=%d f=%d %ux%u mem=%s us=%u", slot, f, f * w, f * h,
             vb_tex_where(g_upt[slot].t[0]), pspcompat_time_us() - t0);
      if (!g_upt[slot].t[0] || !g_upt[slot].t[1])
      {
         up_drop(slot);
         return NULL;
      }
      g_upt[slot].kind = kind; g_upt[slot].f = f;
      g_upt[slot].w = w; g_upt[slot].h = h;
      DP_MAX(VID_DP_REMAKE, pspcompat_time_us() - t0);
   }
   else if (g_upt[slot].seen != g_frame_no && g_upt[slot].seen != g_frame_no + 1)
   {
      /* Not drawn last frame (a menu was up, the pair had stopped): what
       * the textures hold is an old moment of the game. */
      upscale_collect(slot);
      g_upt[slot].front = -1;
   }
   g_upt[slot].seen = g_frame_no + 1;
   tb = pspcompat_time_us();
   for (k = 0; k < 2; k++)
      px[k] = vb_tex_lock(g_upt[slot].t[k], &tp[k]);
   DP_MAX(VID_DP_LOCK, pspcompat_time_us() - tb);
   if (!px[0] || !px[1] || tp[0] != tp[1])
      return NULL;

   tb = pspcompat_time_us();
   if (upscale_collect(slot))
      up_front(slot);
   DP_MAX(VID_DP_COLLECT, pspcompat_time_us() - tb);
   tb = pspcompat_time_us();
   {
      /* (a slot still busy with the last picture turns this one away and
       * counts it: the stats' `dropped`, a frame the smoothing missed) */
      int target = g_upt[slot].front == 0 ? 1 : 0;
      if (upscale_submit(slot, kind, f, pix, w, h, pitch_bytes, 0, px, tp[0],
                         target) == 1)
      {
         const int pf = upscale_job_factor(slot);
         if (pf != g_upt[slot].pf)
            fe_evt("up_factor slot=%d f=%d tex_f=%d", slot, pf, f);
         g_upt[slot].pending = target;
         g_upt[slot].pf = pf;
         upscale_ring(slot, &g_upt[slot].pox, &g_upt[slot].poy);
      }
   }
   DP_MAX(VID_DP_SUBMIT, pspcompat_time_us() - tb);
   DP_MAX(VID_DP_STAGE, pspcompat_time_us() - ta);
   /* (a host twin: the picture just handed over is finished already) */
   if (upscale_collect(slot))
      up_front(slot);
   if (g_upt[slot].front < 0)
      return NULL;
   *f_out = g_upt[slot].ff;
   return g_upt[slot].t[g_upt[slot].front];
}

/* Slot `slot`'s front texture (f times the source) onto a rect; u, v in
 * SOURCE texels of the picture.  The texture holds the picture as a ring
 * (upscale_ring): it is drawn turned back, in up to four pieces split
 * where the ring's seam falls. */
static void up_quad(int slot, vb_tex *t, int f, float x0, float y0, float x1,
                    float y1, float u0, float v0, float u1, float v1)
{
   vb_tquad q[4];
   const float w = (float)g_upt[slot].w, h = (float)g_upt[slot].h;
   const float ox = (float)g_upt[slot].fox, oy = (float)g_upt[slot].foy;
   const float sx = (x1 - x0) / (u1 - u0), sy = (y1 - y0) / (v1 - v0);
   /* the seam: picture column w - ox / row h - oy (none when not turned) */
   const float cu = w - ox, cv = h - oy;
   float us[3], vs[3];
   int nu = 0, nv = 0, i, j, n = 0;
   int exact = (int)(x1 - x0 + 0.5f) == (int)((u1 - u0) * (float)f + 0.5f) &&
               (int)(y1 - y0 + 0.5f) == (int)((v1 - v0) * (float)f + 0.5f);
   const int small = f < g_upt[slot].f && !exact;
   const float umax = w * (float)f, vmax = h * (float)f;

   us[nu++] = u0;
   if (ox > 0 && cu > u0 && cu < u1)
      us[nu++] = cu;
   us[nu] = u1;
   vs[nv++] = v0;
   if (oy > 0 && cv > v0 && cv < v1)
      vs[nv++] = cv;
   vs[nv] = v1;
   for (j = 0; j < nv; j++)
      for (i = 0; i < nu; i++)
      {
         /* the piece's place in the ring: before the seam, turned forward;
          * after it, back round to the ring's start */
         const float ru = us[i] < cu ? ox : ox - w;
         const float rv = vs[j] < cv ? oy : oy - h;
         q[n].x0 = i ? x0 + (us[i] - u0) * sx : x0;
         q[n].x1 = i + 1 < nu ? x0 + (us[i + 1] - u0) * sx : x1;
         q[n].y0 = j ? y0 + (vs[j] - v0) * sy : y0;
         q[n].y1 = j + 1 < nv ? y0 + (vs[j + 1] - v0) * sy : y1;
         q[n].u0 = (us[i] + ru) * (float)f;
         q[n].u1 = (us[i + 1] + ru) * (float)f;
         q[n].v0 = (vs[j] + rv) * (float)f;
         q[n].v1 = (vs[j + 1] + rv) * (float)f;
         if (small)
         {
            /* (a picture at one factor less than its texture: the texels
             * past its right and bottom edges are an older picture's, and
             * a linear filter reaches half a texel out) */
            if (q[n].u1 > umax - 0.5f)
               q[n].u1 = umax - 0.5f;
            if (q[n].v1 > vmax - 0.5f)
               q[n].v1 = vmax - 0.5f;
         }
         n++;
      }
   vb_tquads(t, q, n, 0xFFFFFFFFu, exact ? VB_NEAREST : VB_LINEAR);
}

void vid_upscale_stats(upscale_stats_t *out)
{
   upscale_stats(out);
}

void vid_draw_frame(const uint16_t *pix, unsigned w, unsigned h,
                    size_t pitch_bytes)
{
   unsigned t0, t1, t2;
   vid_geo g;
   vb_tex *t, *tu;
   int f = 0;

   if (!g_inited)
      vid_init();
   if (!pix || !w || !h)
      return;
   dest_geo(w, h, &g);

   t0 = pspcompat_time_us();
   pix = look_frame(0, pix, w, h, &pitch_bytes);
   DP_MAX(VID_DP_LOOK, pspcompat_time_us() - t0);
   if (gx_ready())
   {
      /* xBRZ on the GPU (picture_draw): the CPU upscalers' textures go */
      if (g_upt[0].t[0] || g_upt[0].t[1])
         up_drop(0);
      tu = NULL;
   }
   else
      tu = up_stage(0, pix, w, h, pitch_bytes, g.dw, &f);
   if (tu)
   {
      t1 = pspcompat_time_us();
      g_filled = 1;
      backdrop_draw(geo_covers(&g));
      up_quad(0, tu, f, (float)g.ox, (float)g.oy, (float)(g.ox + g.dw),
              (float)(g.oy + g.dh), 0, (float)g.sy0, (float)w,
              (float)(g.sy0 + g.sh));
      t2 = pspcompat_time_us();
      DP_MAX(VID_DP_DRAW, t2 - t1);
      prof_add(t1 - t0, t2 - t1);
      amb_snap_maybe(pix, (int)(pitch_bytes / 2), w, h);
      return;
   }
   t = stage_tex(ST_LIVE, (int)w, (int)h, VB_FMT_RGB565);
   if (t)
      vb_tex_update(t, 0, 0, (int)w, (int)h, pix, pitch_bytes);
   t1 = pspcompat_time_us();
   DP_MAX(VID_DP_UPLOAD, t1 - t0);

   g_filled = 1;
   picture_draw(t, w, &g);
   t2 = pspcompat_time_us();
   DP_MAX(VID_DP_DRAW, t2 - t1);
   prof_add(t1 - t0, t2 - t1);

   /* Outside the profile brackets: a snapshot is a one-off. */
   amb_snap_maybe(pix, (int)(pitch_bytes / 2), w, h);
}

/* The PSP's Media Engine path: a frame someone else already laid out at a
 * 256-texel stride.  Nothing stages for a GPU; it is the same upload. */
void vid_draw_prestaged(const uint16_t *staged, unsigned w, unsigned h)
{
   vid_draw_frame(staged, w, h, 256 * sizeof(uint16_t));
}

/* The frame the game last showed, onto the game's own rect -- behind the
 * in-game menu and the wake overlay.  PSP-order 565 at `texw` texels, like
 * every texture the UI hands over (a main loop makes it with
 * vid_frame_to_psp565).  Opaque: drawn through the very code the live
 * picture is, so opening the menu does not resample the game behind it. */
void vid_image_screen(const uint16_t *pix, int texw, int texh,
                      int srcw, int srch, int alpha)
{
   vid_geo g;
   vb_tex *t;
   (void)texh;
   if (!pix || srcw <= 0 || srch <= 0 || alpha <= 0)
      return;
   dest_geo((unsigned)srcw, (unsigned)srch, &g);
   t = stage_tex(ST_SNAP, srcw, srch, VB_FMT_BGR565);
   if (!t)
      return;
   vb_tex_update(t, 0, 0, srcw, srch, pix, (size_t)texw * 2);
   if (alpha < 255)
   {
      vb_tquad q;
      int kx, ky;
      vv_frame_need();
      q.x0 = (float)g.ox; q.y0 = (float)g.oy;
      q.x1 = (float)(g.ox + g.dw); q.y1 = (float)(g.oy + g.dh);
      q.u0 = 0; q.v0 = (float)g.sy0;
      q.u1 = (float)srcw; q.v1 = (float)(g.sy0 + g.sh);
      vb_tquads(t, &q, 1, VB_RGBA(255, 255, 255, alpha),
                picture_mode((unsigned)srcw, &g, &kx, &ky) == PIC_NEAREST
                   ? VB_NEAREST : VB_LINEAR);
      return;
   }
   picture_draw(t, (unsigned)srcw, &g);
}

/* The same frame after the UI softened it (ui_psp.c ov_soften), on the same
 * rect: what the menus put under their text.  Always linear and without the
 * LCD grid -- a blurred picture drawn as hard 3x3 blocks, or ruled with
 * dots, is detail again, and detail under text is what this removes.
 * alpha < 255 blends it over what is there (the menu's opening: the sharp
 * frame turns soft over a few refreshes instead of at once).  Its own
 * staging texture: the sharp one may be drawn in the same frame. */
void vid_image_screen_soft(const uint16_t *pix, int texw, int srcw, int srch,
                           int alpha)
{
   vid_geo g;
   vb_tex *t;
   vb_tquad q;
   if (!pix || srcw <= 0 || srch <= 0 || alpha <= 0)
      return;
   dest_geo((unsigned)srcw, (unsigned)srch, &g);
   t = stage_tex(ST_SOFT, srcw, srch, VB_FMT_BGR565);
   if (!t)
      return;
   vb_tex_update(t, 0, 0, srcw, srch, pix, (size_t)texw * 2);
   if (alpha >= 255)
      backdrop_draw(geo_covers(&g));
   else
      vv_frame_need();
   q.x0 = (float)g.ox; q.y0 = (float)g.oy;
   q.x1 = (float)(g.ox + g.dw); q.y1 = (float)(g.oy + g.dh);
   q.u0 = 0; q.v0 = (float)g.sy0;
   q.u1 = (float)srcw; q.v1 = (float)(g.sy0 + g.sh);
   vb_tquads(t, &q, 1, VB_RGBA(255, 255, 255, alpha > 255 ? 255 : alpha),
             VB_LINEAR);
}

/* ---- two pictures -------------------------------------------------------- */

void vid_pair_rects(unsigned w, unsigned h, int layout, vid_rect_t *left,
                    vid_rect_t *right)
{
   vid_rect_t a, b;
   int km, kp;
   if (!w || !h)
      w = 240, h = 160;
   if (layout == VID_PAIR_INSET)
   {
      /* Main at the largest integer scale that fits; the partner beside it
       * at the largest that fits the space left, and smaller than main. */
      for (km = 1; (unsigned)(km + 1) * w <= VB_W &&
                   (unsigned)(km + 1) * h <= VB_H; km++)
         ;
      if (km > 1 && (unsigned)km * w + w > VB_W)
         km--;
      kp = (VB_W - km * (int)w) / (int)w;
      if (kp >= km)
         kp = km - 1;
      if (kp < 1)
         kp = 1;
      a.w = km * (int)w; a.h = km * (int)h;
      b.w = kp * (int)w; b.h = kp * (int)h;
      a.x = (VB_W - a.w - b.w) / 2;
      a.y = (VB_H - a.h) / 2;
      b.x = a.x + a.w;
      b.y = a.y;
   }
   else
   {
      for (km = 1; 2u * (unsigned)(km + 1) * w <= VB_W &&
                   (unsigned)(km + 1) * h <= VB_H; km++)
         ;
      a.w = b.w = km * (int)w;
      a.h = b.h = km * (int)h;
      a.x = (VB_W - 2 * a.w) / 2;
      a.y = b.y = (VB_H - a.h) / 2;
      b.x = a.x + a.w;
   }
   if (left)  *left = a;
   if (right) *right = b;
}

void vid_draw_frame_pair(const uint16_t *left, const uint16_t *right,
                         unsigned w, unsigned h, size_t pitch_bytes,
                         int layout)
{
   unsigned t0, t1, t2;
   vid_rect_t r[2];
   const uint16_t *src[2];
   vb_tex *t[2] = { NULL, NULL };
   int up[2] = { 0, 0 };
   size_t amb_pb = pitch_bytes;
   int i;

   if (!g_inited)
      vid_init();
   if (!w || !h)
      return;
   src[0] = left; src[1] = right;
   vid_pair_rects(w, h, layout, &r[0], &r[1]);

   t0 = pspcompat_time_us();
   for (i = 0; i < 2; i++)
      if (src[i])
      {
         size_t pb = pitch_bytes;
         src[i] = look_frame(i, src[i], w, h, &pb);
         /* Each machine's picture through the upscaler too, when one is
          * chosen: twice the work, on the cores the pair leaves idle. */
         t[i] = up_stage(i, src[i], w, h, pb, r[i].w, &up[i]);
         if (!t[i])
         {
            up[i] = 0;
            t[i] = stage_tex(i ? ST_PARTNER : ST_LIVE, (int)w, (int)h,
                             VB_FMT_RGB565);
            if (t[i])
               vb_tex_update(t[i], 0, 0, (int)w, (int)h, src[i], pb);
         }
         if (i == 0)
            amb_pb = pb;
      }
   t1 = pspcompat_time_us();

   g_filled = 1;
   backdrop_draw(0);
   for (i = 0; i < 2; i++)
      if (t[i] && up[i])
         up_quad(i, t[i], up[i], (float)r[i].x, (float)r[i].y,
                 (float)(r[i].x + r[i].w), (float)(r[i].y + r[i].h),
                 0, 0, (float)w, (float)h);
      else if (t[i])
      {
         vb_tquad q;
         q.x0 = (float)r[i].x; q.y0 = (float)r[i].y;
         q.x1 = (float)(r[i].x + r[i].w); q.y1 = (float)(r[i].y + r[i].h);
         q.u0 = 0; q.v0 = 0; q.u1 = (float)w; q.v1 = (float)h;
         vb_tquads(t[i], &q, 1, 0xFFFFFFFFu, VB_NEAREST);
      }
   t2 = pspcompat_time_us();
   prof_add(t1 - t0, t2 - t1);
   if (src[0])
      amb_snap_maybe(src[0], (int)(amb_pb / 2), w, h);
}

/* ---- present / readback ------------------------------------------------- */

int vid_present_state(void)
{
   return g_inited ? vb_present_state() : 0;
}

void vid_swap(void)
{
   /* NOTHING DREW A WHOLE FRAME: hold what is on screen (see the header). */
   if (!g_filled)
      return;
   /* A clear nobody drew into is still a (black) frame.  A frame the
    * readback already closed is complete: present it as it is. */
   if (!vb_frame_open() && g_clear_req)
      vb_frame_begin(1);
   g_clear_req = 0;
   vb_present();
   g_filled = 0;
   g_frame_no++;
   gx_guard();
}

int vid_readback(uint32_t *dst)
{
   if (!vb_frame_open() && g_clear_req)
   {
      /* A cleared frame nobody drew into is a black frame. */
      vb_frame_begin(1);
      g_clear_req = 0;
   }
   return vb_readback(dst);
}

static int ends_with(const char *s, const char *suf)
{
   size_t n = strlen(s), m = strlen(suf);
   return n >= m && strcmp(s + n - m, suf) == 0;
}

int vid_dump_ge(const char *path)
{
   uint32_t *px;
   unsigned char *rgb;
   int x, y, rc = -1;

   if (!path)
      return -1;
   px = (uint32_t *)malloc((size_t)VB_W * VB_H * 4);
   rgb = (unsigned char *)malloc((size_t)VB_W * VB_H * 3);
   if (!px || !rgb || vid_readback(px) != 0)
   {
      free(px);
      free(rgb);
      return -1;
   }
   if (ends_with(path, ".png"))
   {
      for (y = 0; y < VB_H; y++)
         for (x = 0; x < VB_W; x++)
         {
            uint32_t c = px[(size_t)y * VB_W + x];
            unsigned char *d = rgb + ((size_t)y * VB_W + x) * 3;
            d[0] = (unsigned char)c;
            d[1] = (unsigned char)(c >> 8);
            d[2] = (unsigned char)(c >> 16);
         }
      rc = stbi_write_png(path, VB_W, VB_H, 3, rgb, VB_W * 3) ? 0 : -1;
   }
   else
   {
      /* 24-bit bottom-up BMP (the PSP's dump format). */
      unsigned char hdr[54];
      unsigned size = 54u + (unsigned)VB_W * VB_H * 3u;
      FILE *f = fopen(path, "wb");
      if (f)
      {
         memset(hdr, 0, sizeof(hdr));
         hdr[0] = 'B'; hdr[1] = 'M';
         hdr[2] = (unsigned char)size; hdr[3] = (unsigned char)(size >> 8);
         hdr[4] = (unsigned char)(size >> 16);
         hdr[5] = (unsigned char)(size >> 24);
         hdr[10] = 54; hdr[14] = 40;
         hdr[18] = (unsigned char)(VB_W & 0xFF);
         hdr[19] = (unsigned char)(VB_W >> 8);
         hdr[22] = (unsigned char)(VB_H & 0xFF);
         hdr[23] = (unsigned char)(VB_H >> 8);
         hdr[26] = 1; hdr[28] = 24;
         for (y = 0; y < VB_H; y++)
            for (x = 0; x < VB_W; x++)
            {
               uint32_t c = px[(size_t)(VB_H - 1 - y) * VB_W + x];
               unsigned char *d = rgb + ((size_t)y * VB_W + x) * 3;
               d[0] = (unsigned char)(c >> 16);
               d[1] = (unsigned char)(c >> 8);
               d[2] = (unsigned char)c;
            }
         rc = (fwrite(hdr, 1, 54, f) == 54 &&
               fwrite(rgb, 1, (size_t)VB_W * VB_H * 3, f) ==
                  (size_t)VB_W * VB_H * 3) ? 0 : -1;
         fclose(f);
      }
   }
   free(px);
   free(rgb);
   return rc;
}

void vid_term(void)
{
   {
      int k;
      for (k = 0; k < UPSCALE_SLOTS; k++)
         up_drop(k);
   }
   upscale_shutdown();
   free(g_cc);
   g_cc = NULL;
   {
      int k;
      for (k = 0; k < 2; k++)
      {
         free(g_look_out[k]);
         free(g_look_prev[k]);
         g_look_out[k] = g_look_prev[k] = NULL;
         g_look_cap[k] = 0;
         g_look_w[k] = 0;
      }
   }
   g_color = VID_COLOR_RAW;

   int i, j;
   if (!g_inited)
      return;
   for (i = 0; i < TC_MAX; i++)
      if (g_tc[i].tex)
      {
         vb_tex_destroy(g_tc[i].tex);
         g_tc[i].tex = NULL;
      }
   for (i = 0; i < ST_PURPOSES; i++)
      for (j = 0; j < 2; j++)
      {
         vb_tex_destroy(g_stage[i][j].t);
         g_stage[i][j].t = NULL;
      }
   vb_tex_destroy(g_logo_tex);  g_logo_tex = NULL;
   vb_tex_destroy(g_amb_tex);   g_amb_tex = NULL;
   vb_tex_destroy(g_lcd_tex);   g_lcd_tex = NULL;
   vb_tex_destroy(g_sharp_rt);  g_sharp_rt = NULL;
   g_sharp_tried = 0;
   g_amb_src = AMB_SRC_NONE;
   vs_term();
   vv_text_term();
   vb_term();
   g_inited = 0;
}
