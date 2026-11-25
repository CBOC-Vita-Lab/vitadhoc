/* vback_vita2d.c -- the PS Vita backend of vback.h: GXM through libvita2d.
 *
 * COMPILED AND LINK-CHECKED AGAINST VITASDK; NOT RUN.  The development
 * machine has no Vita and no emulator, so every statement about what the
 * GPU does below is from the GXM/libvita2d sources and documentation, not
 * from a screen.  The software backend (vback_soft.c) is the reference for
 * what each call must produce; where the two could differ on hardware is
 * listed at the end of this comment.
 *
 * HOW vback.h MAPS ONTO GXM
 *   - A frame is ONE scene on the back buffer (vita2d_start_drawing_advanced
 *     with no target ... vita2d_end_drawing), opened by vb_frame_begin and
 *     closed by vb_present, which then queues the swap.
 *   - A render target is a vita2d render-target texture; drawing into it is
 *     its own scene.  GXM cannot nest scenes, so vb_rt_begin inside an open
 *     frame ends the frame's scene, and vb_rt_end starts another on the same
 *     back buffer.  video_vita.c arranges for that never to happen in
 *     practice (the prescale is the first thing a frame does).
 *   - Coloured rectangles are vita2d's colour program (a colour per vertex);
 *     textured ones its tint program (one tint per draw), both blending
 *     src-over.  Rectangles are two triangles each, batched per call.
 *   - libvita2d's projection is fixed at 960x544 and a scene's viewport is
 *     its render target, so coordinates inside a render-target scene are
 *     scaled by 960/w and 544/h to land on the target's own pixels.
 *   - Formats: RGB565 = U5U6U5_RGB (R in the high bits), BGR565 = U5U6U5_BGR
 *     (the PSP's order), A8 = U8_R111 (white, the byte as alpha: what
 *     libvita2d's own fonts use), RGBA8 = U8U8U8U8_ABGR.
 *   - The scissor costs the GPU nothing: every primitive is an axis-aligned
 *     rectangle, so it is clipped HERE, before it becomes vertices (positions,
 *     texture coordinates and corner colours cut at the scissor's edges).
 *     That is pixel exact, which GXM's region clip is not (it works in 32-px
 *     tiles), and it avoids libvita2d's stencil clip, which redraws the
 *     stencil over the whole screen twice at every change -- ui_psp.c changes
 *     the scissor dozens of times a frame.
 *   - vb_tex_destroy never frees a texture a queued scene may still read:
 *     each free waits, past a present, for the texture's fragment fence (the
 *     scenes that read or wrote it).  Only a full queue or vb_term waits for
 *     the GPU with sceGxmFinish.
 *   - A latched GXM error (EndScene, the display queue) is cleared at the
 *     next skipped present by vita2d_recover, a few times in a row at most.
 *   - Three full-capacity vertex banks retire by scene notifications. If
 *     none is available, the whole frame is skipped without waiting.
 *
 * TO CHECK ON HARDWARE (see the port report):
 *   1. the 565 channel orders above and the A8 swizzle;
 *   2. that a second scene on the back buffer keeps the first one's pixels
 *      (only reached by the readback and by a mid-frame render-target pass);
 *   3. the render-target coordinate scaling (sharp bilinear).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vita2d.h>

#include "vback.h"
#include "menu_diag.h"
/* fe_evt, weakly: the backend's GPU tests link without the log */
extern void fe_evt(const char *fmt, ...) __attribute__((weak, format(printf, 1, 2)));

struct vb_tex
{
   vita2d_texture *t;
   int w, h, fmt, rt, bpp;
   vita2d_fragment_fence readers;
   int unfenced;               /* a GPU use went unrecorded: free after Finish */
};

#define POOL_BYTES   (2 * 1024 * 1024)
#define DEFER_MAX    256
#define QUAD_BATCH   128

static int g_inited;
static int g_open;            /* the frame's scene is open                  */
static int g_suspended;       /* ...and was closed for a render-target pass */
static int g_skip;            /* keep this entire frame off the queue      */
static int g_clear_pending;   /* retain a skipped frame's clear request    */
static int g_frame_started;
static unsigned g_started_frames, g_skipped_frames, g_texture_blocked;
static vb_tex *g_rt;          /* render target bound, or NULL               */
static int g_sc_on, g_sc_x, g_sc_y, g_sc_w, g_sc_h;
static int g_dialog;          /* run the common dialog before each swap     */
static struct
{
   vita2d_texture *t;
   vita2d_fragment_fence readers;   /* the texture's, when it was destroyed */
   int unfenced;
} g_defer[DEFER_MAX];
static int g_defer_n;
static unsigned g_recovered;
static int g_recover_gave_up;

const char *vb_name(void) { return "vita2d"; }

int vb_init(void)
{
   if (g_inited)
      return 0;
   if (vita2d_init_advanced(POOL_BYTES) != 1)
      return -1;
   vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
   vita2d_set_vblank_wait(1);
   vita2d_disable_clipping();
   g_inited = 1;
   g_recovered = 0;
   g_recover_gave_up = 0;
   g_open = g_suspended = g_sc_on = g_defer_n = 0;
   g_skip = g_clear_pending = 0;
   g_frame_started = 0;
   g_started_frames = g_skipped_frames = g_texture_blocked = 0;
   g_rt = NULL;
   return 0;
}

/* Free the deferred textures the GPU is done with; never waits. */
static void defer_sweep(void)
{
   int i, n = 0;
   unsigned long long t;
   if (!g_defer_n)
      return;
   t = md_begin();
   for (i = 0; i < g_defer_n; i++)
   {
      if (!g_defer[i].unfenced &&
          vita2d_fragment_fence_done(&g_defer[i].readers))
         vita2d_free_texture(g_defer[i].t);
      else
         g_defer[n++] = g_defer[i];
   }
   g_defer_n = n;
   md_end(MD_DEFER, t);
}

/* Free them all: no scene may be open.  Waits for the GPU. */
static void defer_flush(void)
{
   int i;
   unsigned long long t;
   if (!g_defer_n)
      return;
   t = md_begin();
   /* Nothing queued may still be sampling these. */
   vita2d_wait_rendering_done();
   for (i = 0; i < g_defer_n; i++)
      vita2d_free_texture(g_defer[i].t);
   g_defer_n = 0;
   md_end(MD_DEFER, t);
}

static void recover(void);

void vb_term(void)
{
   if (!g_inited)
      return;
   if (g_open)
      vita2d_end_drawing();
   g_open = 0;
   defer_flush();
   vita2d_fini();
   g_inited = 0;
}

void vb_set_vsync(int on)
{
   vita2d_set_vblank_wait(on ? 1 : 0);
}

/* A system dialog (the ad hoc NetCheck dialog, the IME) draws itself into
 * the finished frame: sceCommonDialogUpdate between the scene's end and the
 * swap, which is where libvita2d's own samples put it. */
void vb_set_common_dialog(int on)
{
   g_dialog = on ? 1 : 0;
}

/* ---- textures ---------------------------------------------------------- */

static int g_tex_cpu;          /* vb_tex_mem */

void vb_tex_mem(int cpu_written)
{
   g_tex_cpu = cpu_written ? 1 : 0;
}

const char *vb_tex_where(const vb_tex *t)
{
   SceKernelMemBlockInfo info;
   if (!t || !t->t)
      return "?";
   memset(&info, 0, sizeof(info));
   info.size = sizeof(info);
   if (sceKernelGetMemBlockInfoByAddr(vita2d_texture_get_datap(t->t), &info) < 0)
      return "?";
   return info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? "gpu"
        : info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE ? "ram-nc"
        : "other";
}

vb_tex *vb_tex_create(int w, int h, int fmt, int rt)
{
   static const SceGxmTextureFormat gxm_fmt[4] = {
      SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB,
      SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR,
      SCE_GXM_TEXTURE_FORMAT_U8_R111,
      SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR
   };
   static const int bpp[4] = { 2, 2, 1, 4 };
   vb_tex *t;
   if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || fmt < 0 || fmt > 3)
      return NULL;
   /* libvita2d lays texture rows out at the width rounded up to 8 but gives
    * a render target's colour surface the plain width as its stride: the two
    * only agree at a multiple of 8. */
   if (rt && ((w & 7) || fmt != VB_FMT_RGBA8))
      return NULL;
   t = (vb_tex *)calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   /* libvita2d falls back to the GPU's memory by itself when the main
    * RAM's is refused. */
   vita2d_texture_set_alloc_memblock_type(!rt && g_tex_cpu
                                          ? SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE
                                          : 0);
   t->t = rt ? vita2d_create_empty_texture_rendertarget(
                  (unsigned)w, (unsigned)h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8)
             : vita2d_create_empty_texture_format((unsigned)w, (unsigned)h,
                                                  gxm_fmt[fmt]);
   vita2d_texture_set_alloc_memblock_type(0);
   if (!t->t)
   {
      free(t);
      return NULL;
   }
   sceGxmTextureSetUAddrMode(&t->t->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
   sceGxmTextureSetVAddrMode(&t->t->gxm_tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
   vita2d_texture_set_filters(t->t, SCE_GXM_TEXTURE_FILTER_POINT,
                              SCE_GXM_TEXTURE_FILTER_POINT);
   t->w = w; t->h = h; t->fmt = fmt; t->rt = rt; t->bpp = bpp[fmt];
   return t;
}

void vb_tex_destroy(vb_tex *t)
{
   if (!t)
      return;
   /* The bound target: close its scene first (the frame's, if suspended,
    * resumes), so no scene is left open on a texture that is gone. */
   if (t == g_rt)
      vb_rt_end();
   if (g_defer_n == DEFER_MAX)
      defer_sweep();
   if (g_defer_n == DEFER_MAX && !g_open && !g_rt)
      defer_flush();       /* still full and no scene is open: drain it now */
   if (g_defer_n < DEFER_MAX)
   {
      g_defer[g_defer_n].t = t->t;
      g_defer[g_defer_n].readers = t->readers;
      g_defer[g_defer_n].unfenced = t->unfenced;
      g_defer_n++;
   }
   /* else: leak one texture rather than free one in use (256 destroys
    * still in flight inside one frame does not happen). */
   free(t);
}

int vb_tex_w(const vb_tex *t) { return t ? t->w : 0; }
int vb_tex_h(const vb_tex *t) { return t ? t->h : 0; }

int vb_tex_writable(const vb_tex *t)
{
   if (!t || !t->t)
      return 0;
   if (!vita2d_fragment_fence_done(&t->readers))
   {
      g_texture_blocked++;
      return 0;
   }
   return 1;
}

void vb_ownership_get(vb_ownership_diag *out)
{
   vita2d_ownership_diag d;
   if (!out)
      return;
   vita2d_ownership_get(&d);
   memset(out, 0, sizeof(*out));
   out->started_frames = g_started_frames;
   out->skipped_frames = g_skipped_frames;
   out->submitted_frames = d.submitted_frames;
   out->pending_swaps = d.pending_swaps;
   out->pool_bank = d.pool_bank;
   out->busy_banks = d.busy_banks;
   out->submitted_scenes = d.submitted_scenes;
   out->vertex_pending = d.vertex_pending;
   out->fragment_pending = d.fragment_pending;
   out->texture_blocked = g_texture_blocked;
   out->fatal = d.fatal;
   out->scene_serial = d.scene_serial;
   out->vertex_word = d.vertex_word;
   out->fragment_word = d.fragment_word;
}

void *vb_tex_lock(vb_tex *t, size_t *pitch_bytes)
{
   if (!t)
      return NULL;
   if (pitch_bytes)
      *pitch_bytes = vita2d_texture_get_stride(t->t);
   return vita2d_texture_get_datap(t->t);
}

int vb_tex_read(vb_tex *t, uint32_t *dst)
{
   const unsigned char *base;
   unsigned stride;
   int j;
   if (!t || !dst || t->fmt != VB_FMT_RGBA8)
      return -1;
   vita2d_wait_rendering_done();
   base = (const unsigned char *)vita2d_texture_get_datap(t->t);
   stride = vita2d_texture_get_stride(t->t);
   if (!base)
      return -1;
   for (j = 0; j < t->h; j++)
      memcpy(dst + (size_t)j * t->w, base + (size_t)j * stride,
             (size_t)t->w * 4);
   return 0;
}

void vb_tex_update(vb_tex *t, int x, int y, int w, int h,
                   const void *pix, size_t pitch_bytes)
{
   unsigned char *base;
   unsigned stride;
   int j;
   unsigned long long started;
   if (!t || !pix || x < 0 || y < 0 || x + w > t->w || y + h > t->h)
      return;
   started = md_begin();
   base = (unsigned char *)vita2d_texture_get_datap(t->t);
   stride = vita2d_texture_get_stride(t->t);
   for (j = 0; j < h; j++)
      memcpy(base + (size_t)(y + j) * stride + (size_t)x * t->bpp,
             (const unsigned char *)pix + (size_t)j * pitch_bytes,
             (size_t)w * t->bpp);
   md_end(MD_UPLOAD, started);
}

/* ---- scenes ------------------------------------------------------------ */

void vb_text_mark(void) { }

void vb_frame_begin(int clear)
{
   if (g_open || g_rt)
      return;
   if (clear)
      g_clear_pending = 1;
   if (g_skip)
      return;
   if (!vita2d_try_start_drawing_advanced(NULL, 0))
   {
      g_skip = 1;
      return;
   }
   if (!g_frame_started)
   {
      g_frame_started = 1;
      g_started_frames++;
   }
   if (g_clear_pending)
   {
      vita2d_clear_screen();
      g_clear_pending = 0;
   }
   g_open = 1;
}

void vb_frame_end(void)
{
   if (!g_open)
      return;
   if (!vita2d_end_drawing_checked())
      g_skip = g_clear_pending = 1;
   g_open = 0;
}

int vb_frame_open(void) { return g_open; }

static int g_gpu_time;          /* presents still to time; -1 = all */
static unsigned g_gpu_n, g_gpu_sum, g_gpu_max;

void vb_gpu_time(int frames)
{
   g_gpu_time = frames < 0 ? -1 : frames;
}

int vb_gpu_timing(void)
{
   return g_gpu_time;
}

void vb_gpu_stats(unsigned *frames, unsigned *avg_us, unsigned *max_us)
{
   if (frames) *frames = g_gpu_n;
   if (avg_us) *avg_us = g_gpu_n ? g_gpu_sum / g_gpu_n : 0;
   if (max_us) *max_us = g_gpu_max;
   g_gpu_n = g_gpu_sum = g_gpu_max = 0;
}

void vb_present(void)
{
   if (g_rt)
      vb_rt_end();
   vb_frame_end();
   if (g_skip)
   {
      /* Do not enqueue a partial frame, reopen a scene, update a dialog or
       * drain deferred textures while GPU/display backpressure is active. */
      g_skip = g_suspended = g_sc_on = 0;
      g_frame_started = 0;
      g_skipped_frames++;
      g_clear_pending = 1;
      vita2d_pool_reset();
      recover();
      return;
   }
   if (g_gpu_time)
   {
      /* everything this frame queued, finished: the GPU's share of it */
      SceUInt32 t0 = sceKernelGetProcessTimeLow(), dt;
      vita2d_wait_rendering_done();
      dt = sceKernelGetProcessTimeLow() - t0;
      if (g_gpu_time > 0)
         g_gpu_time--;
      g_gpu_n++;
      g_gpu_sum += dt;
      if (dt > g_gpu_max)
         g_gpu_max = dt;
   }
   if (g_dialog)
      vita2d_common_dialog_update();
   unsigned long long swap_t = md_begin();
   vita2d_swap_buffers();
   md_end(MD_SWAP, swap_t);
   defer_sweep();
   /* Release CPU ownership only; the bank retains its completion fences. */
   vita2d_pool_reset();
   g_sc_on = 0;
   g_frame_started = 0;
   g_clear_pending = 0;
}

/* A latched GXM error refuses every frame from then on (a frozen screen):
 * clear it, a bounded number of times in a row (vita2d_recover). */
static void recover(void)
{
   int r = vita2d_recover();
   if (r > 0 && ++g_recovered == 1 && fe_evt)
      fe_evt("gxm_recover n=1");
   else if (r < 0 && !g_recover_gave_up)
   {
      vita2d_ownership_diag d;
      vita2d_ownership_get(&d);
      if (d.fatal)
      {
         g_recover_gave_up = 1;
         if (fe_evt)
            fe_evt("gxm_recover_gave_up n=%u fatal=%u", g_recovered, d.fatal);
      }
   }
}

int vb_present_state(void)
{
   return vita2d_swap_state();
}

void vb_rt_begin(vb_tex *rt)
{
   if (!rt || !rt->rt || g_rt || g_skip)
      return;
   if (g_open)
   {
      int ended = vita2d_end_drawing_checked();
      g_open = 0;
      if (!ended)
      {
         g_skip = g_clear_pending = 1;
         return;
      }
      g_suspended = 1;
   }
   if (!vita2d_try_start_drawing_advanced(rt->t, 0))
   {
      g_suspended = 0;
      g_skip = g_clear_pending = 1;
      return;
   }
   g_rt = rt;
   if (!g_frame_started)
   {
      g_frame_started = 1;
      g_started_frames++;
   }
   if (!vita2d_fragment_fence_record(&rt->readers))
      rt->unfenced = 1;
}

void vb_rt_end(void)
{
   if (!g_rt)
      return;
   if (!vita2d_end_drawing_checked())
      g_skip = g_clear_pending = 1;
   g_rt = NULL;
   if (g_suspended)
   {
      g_suspended = 0;
      if (!g_skip && vita2d_try_start_drawing_advanced(NULL, 0))
         g_open = 1;
      else
         g_skip = g_clear_pending = 1;
   }
}

void vb_scissor(int on, int x, int y, int w, int h)
{
   g_sc_on = on;
   g_sc_x = x; g_sc_y = y; g_sc_w = w; g_sc_h = h;
}

/* ---- drawing ----------------------------------------------------------- */

/* Target pixels -> libvita2d's fixed 960x544 projection (see the header). */
static float sx(float x) { return g_rt ? x * (float)VB_W / (float)g_rt->w : x; }
static float sy(float y) { return g_rt ? y * (float)VB_H / (float)g_rt->h : y; }

/* The scissor (frame only, never inside a render target): cut [*a0, *a1) to
 * [lo, hi) and report where the cut ends lie along the original, 0..1.
 * Returns 0 when nothing is left. */
static int clip_axis(float *a0, float *a1, float lo, float hi, float *t0,
                     float *t1)
{
   float p0 = *a0, p1 = *a1, d = p1 - p0;
   *t0 = 0.0f;
   *t1 = 1.0f;
   if (p0 < lo) { *t0 = (lo - p0) / d; *a0 = lo; }
   if (p1 > hi) { *t1 = (hi - p0) / d; *a1 = hi; }
   return *a1 > *a0;
}

static uint32_t mix4(uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br,
                     float tx, float ty)
{
   uint32_t out = 0;
   int sh;
   for (sh = 0; sh < 32; sh += 8)
   {
      float a = (float)((tl >> sh) & 0xFF), b = (float)((tr >> sh) & 0xFF);
      float c = (float)((bl >> sh) & 0xFF), e = (float)((br >> sh) & 0xFF);
      float top = a + (b - a) * tx, bot = c + (e - c) * tx;
      out |= (uint32_t)(top + (bot - top) * ty + 0.5f) << sh;
   }
   return out;
}

void vb_quad(float x0, float y0, float x1, float y1,
             uint32_t c_tl, uint32_t c_tr, uint32_t c_bl, uint32_t c_br)
{
   vita2d_color_vertex *v;
   if ((!g_open && !g_rt) || !(x1 > x0) || !(y1 > y0))
      return;
   if (g_sc_on && !g_rt)
   {
      float tx0, tx1, ty0, ty1;
      if (!clip_axis(&x0, &x1, (float)g_sc_x, (float)(g_sc_x + g_sc_w),
                     &tx0, &tx1) ||
          !clip_axis(&y0, &y1, (float)g_sc_y, (float)(g_sc_y + g_sc_h),
                     &ty0, &ty1))
         return;
      if ((tx0 > 0.0f || tx1 < 1.0f || ty0 > 0.0f || ty1 < 1.0f) &&
          !(c_tl == c_tr && c_tl == c_bl && c_tl == c_br))
      {
         uint32_t tl = mix4(c_tl, c_tr, c_bl, c_br, tx0, ty0);
         uint32_t tr = mix4(c_tl, c_tr, c_bl, c_br, tx1, ty0);
         uint32_t bl = mix4(c_tl, c_tr, c_bl, c_br, tx0, ty1);
         uint32_t br = mix4(c_tl, c_tr, c_bl, c_br, tx1, ty1);
         c_tl = tl; c_tr = tr; c_bl = bl; c_br = br;
      }
   }
   v = (vita2d_color_vertex *)vita2d_pool_memalign(
          4 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
   if (!v)
      return;
   v[0].x = sx(x0); v[0].y = sy(y0); v[0].z = 0.5f; v[0].color = c_tl;
   v[1].x = sx(x1); v[1].y = sy(y0); v[1].z = 0.5f; v[1].color = c_tr;
   v[2].x = sx(x0); v[2].y = sy(y1); v[2].z = 0.5f; v[2].color = c_bl;
   v[3].x = sx(x1); v[3].y = sy(y1); v[3].z = 0.5f; v[3].color = c_br;
   vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void vb_cquads(const vb_cquad *q, int n)
{
   vita2d_color_vertex *v;
   int i, k = 0;
   if ((!g_open && !g_rt) || !q || n <= 0)
      return;
   if ((g_sc_on && !g_rt) || n > VB_CQUADS_MAX)
   {
      /* (clipped quads change their corner colours: vb_quad's path) */
      for (i = 0; i < n; i++)
         vb_quad(q[i].x0, q[i].y0, q[i].x1, q[i].y1,
                 q[i].c_tl, q[i].c_tr, q[i].c_bl, q[i].c_br);
      return;
   }
   /* one draw of two triangles per rectangle, in the strip's order and
    * winding (tl tr bl, bl tr br) */
   v = (vita2d_color_vertex *)vita2d_pool_memalign(
          (size_t)n * 6 * sizeof(vita2d_color_vertex),
          sizeof(vita2d_color_vertex));
   if (!v)
      return;
   for (i = 0; i < n; i++)
   {
      const vb_cquad *r = &q[i];
      float x0 = sx(r->x0), y0 = sy(r->y0), x1 = sx(r->x1), y1 = sy(r->y1);
      if (!(r->x1 > r->x0) || !(r->y1 > r->y0))
         continue;
      v[k].x = x0; v[k].y = y0; v[k].z = 0.5f; v[k++].color = r->c_tl;
      v[k].x = x1; v[k].y = y0; v[k].z = 0.5f; v[k++].color = r->c_tr;
      v[k].x = x0; v[k].y = y1; v[k].z = 0.5f; v[k++].color = r->c_bl;
      v[k].x = x0; v[k].y = y1; v[k].z = 0.5f; v[k++].color = r->c_bl;
      v[k].x = x1; v[k].y = y0; v[k].z = 0.5f; v[k++].color = r->c_tr;
      v[k].x = x1; v[k].y = y1; v[k].z = 0.5f; v[k++].color = r->c_br;
   }
   if (k)
      vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, (size_t)k);
}

void vb_tquads(vb_tex *t, const vb_tquad *q, int n, uint32_t tint, int filter)
{
   SceGxmTextureFilter f = filter == VB_LINEAR ? SCE_GXM_TEXTURE_FILTER_LINEAR
                                               : SCE_GXM_TEXTURE_FILTER_POINT;
   float iw, ih;
   int clip = g_sc_on && !g_rt;
   float cx0 = (float)g_sc_x, cy0 = (float)g_sc_y;
   float cx1 = (float)(g_sc_x + g_sc_w), cy1 = (float)(g_sc_y + g_sc_h);
   if (!t || !q || n <= 0 || (!g_open && !g_rt) || t == g_rt)
      return;
   iw = 1.0f / (float)t->w;
   ih = 1.0f / (float)t->h;
   /* The filter lives in the texture's control words, which GXM copies when
    * the texture is bound: set it before each draw. */
   vita2d_texture_set_filters(t->t, f, f);
   while (n > 0)
   {
      int m = n > QUAD_BATCH ? QUAD_BATCH : n, i, k = 0;
      vita2d_texture_vertex *v = (vita2d_texture_vertex *)vita2d_pool_memalign(
         (unsigned)(m * 6) * sizeof(vita2d_texture_vertex),
         sizeof(vita2d_texture_vertex));
      if (!v)
         return;
      for (i = 0; i < m; i++, q++)
      {
         float x0 = q->x0, y0 = q->y0, x1 = q->x1, y1 = q->y1;
         float u0 = q->u0, v0 = q->v0, u1 = q->u1, v1 = q->v1;
         vita2d_texture_vertex *p;
         if (!(x1 > x0) || !(y1 > y0))
            continue;
         if (clip)
         {
            float tx0, tx1, ty0, ty1, du = u1 - u0, dv = v1 - v0;
            if (!clip_axis(&x0, &x1, cx0, cx1, &tx0, &tx1) ||
                !clip_axis(&y0, &y1, cy0, cy1, &ty0, &ty1))
               continue;
            u1 = u0 + du * tx1; u0 = u0 + du * tx0;
            v1 = v0 + dv * ty1; v0 = v0 + dv * ty0;
         }
         x0 = sx(x0); y0 = sy(y0); x1 = sx(x1); y1 = sy(y1);
         u0 *= iw; v0 *= ih; u1 *= iw; v1 *= ih;
         p = v + k++ * 6;
         p[0].x = x0; p[0].y = y0; p[0].z = 0.5f; p[0].u = u0; p[0].v = v0;
         p[1].x = x1; p[1].y = y0; p[1].z = 0.5f; p[1].u = u1; p[1].v = v0;
         p[2].x = x0; p[2].y = y1; p[2].z = 0.5f; p[2].u = u0; p[2].v = v1;
         p[3] = p[2];
         p[4] = p[1];
         p[5].x = x1; p[5].y = y1; p[5].z = 0.5f; p[5].u = u1; p[5].v = v1;
      }
      if (k)
      {
         if (!vita2d_fragment_fence_record(&t->readers))
            t->unfenced = 1;
         vita2d_draw_array_textured(t->t, SCE_GXM_PRIMITIVE_TRIANGLES, v,
                                    (size_t)(k * 6), tint);
      }
      n -= m;
   }
}

/* vb_tquads_mul: the same draw with vita2d's multiply program set
 * (third_party/vita2d, vita2d_set_blend_mode_mul): dst * texel, no tint. */
void vb_tquads_mul(vb_tex *t, const vb_tquad *q, int n, int filter)
{
   if (!vita2d_set_blend_mode_mul(1))
      return;                       /* no multiply programs: no mask */
   vb_tquads(t, q, n, 0xFFFFFFFFu, filter);
   vita2d_set_blend_mode_mul(0);
}

int vb_readback(uint32_t *dst)
{
   const uint32_t *fb;
   if (!dst || !g_inited)
      return -1;
   vb_frame_end();
   vita2d_wait_rendering_done();
   fb = (const uint32_t *)vita2d_get_back_fb();
   if (!fb)
      return -1;
   /* The display buffers are 960 pixels wide at a 960 stride, A8B8G8R8:
    * the same words vback.h's colours are. */
   memcpy(dst, fb, (size_t)VB_W * VB_H * sizeof(uint32_t));
   return 0;
}
