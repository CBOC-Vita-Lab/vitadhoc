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
 *     frees wait for the next present, after sceGxmFinish.
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
#include <psp2/shacccg.h>
#include <vita2d.h>

#include "vback.h"
#include "menu_diag.h"
#include "gpu_shaders.h"     /* build/vita/gen: the shaders/ files as strings */

struct vb_tex
{
   vita2d_texture *t;
   int w, h, fmt, rt, bpp;
   vita2d_fragment_fence readers;
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
static vita2d_texture *g_defer[DEFER_MAX];
static int g_defer_n;

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
   g_open = g_suspended = g_sc_on = g_defer_n = 0;
   g_skip = g_clear_pending = 0;
   g_frame_started = 0;
   g_started_frames = g_skipped_frames = g_texture_blocked = 0;
   g_rt = NULL;
   return 0;
}

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
      vita2d_free_texture(g_defer[i]);
   g_defer_n = 0;
   md_end(MD_DEFER, t);
}

static void prog_release(void);

void vb_term(void)
{
   if (!g_inited)
      return;
   if (g_open)
      vita2d_end_drawing();
   g_open = 0;
   defer_flush();
   prog_release();
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
   if (t == g_rt)
      g_rt = NULL;
   if (g_defer_n < DEFER_MAX)
      g_defer[g_defer_n++] = t->t;
   else if (!g_open && !g_rt)
   {
      /* The queue is full and no scene is open: drain it now. */
      defer_flush();
      g_defer[g_defer_n++] = t->t;
   }
   /* else: leak one texture rather than free one in use (256 destroys in
    * one frame does not happen). */
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
   defer_flush();
   /* Release CPU ownership only; the bank retains its completion fences. */
   vita2d_pool_reset();
   g_sc_on = 0;
   g_frame_started = 0;
   g_clear_pending = 0;
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
   (void)vita2d_fragment_fence_record(&rt->readers);
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
         (void)vita2d_fragment_fence_record(&t->readers);
         vita2d_draw_array_textured(t->t, SCE_GXM_PRIMITIVE_TRIANGLES, v,
                                    (size_t)(k * 6), tint);
      }
      n -= m;
   }
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

/* ---- GPU programs (vback.h vb_prog) --------------------------------------
 * The shaders in gpsp/vita/shaders/ are compiled HERE, at run time, by the
 * system's own Cg compiler: SceShaccCg, the module libshacccg.suprx that
 * vitaGL games need too (the test console has it in ur0:data/).  Nothing to ship
 * and no offline compiler: VitaSDK has none.  A compiled program is kept in
 * ux0:data/GBAdhoc/shaders/<name>-<hash of the text>.gxp, so the compiler
 * runs once per version of a shader, not at every start.
 *
 * Each program is a fragment program bound to libvita2d's own texture
 * vertex program (position + texture coordinate, the same vertices
 * vb_tquads makes), without blending: its colour is written as it is. */

extern const SceGxmProgram texture_v_gxp_start;          /* libvita2d */
extern float _vita2d_ortho_matrix[4 * 4];
extern SceGxmContext *_vita2d_context;
extern SceGxmVertexProgram *_vita2d_textureVertexProgram;
extern const SceGxmProgramParameter *_vita2d_textureWvpParam;

struct vb_prog
{
   SceGxmShaderPatcherId  id;
   SceGxmFragmentProgram *fp;
   SceGxmProgram         *gxp;
   const SceGxmProgramParameter *uni[VB_PROG_UNIFORMS];
   int tex[2];                /* the samplers' resource indices          */
};

static struct vb_prog g_prog[VB_PROGS];
static int  g_prog_state[VB_PROGS];          /* 0 untried, 1 ok, -1 not */
static char g_prog_why[VB_PROGS][384];
static int  g_shacc;                          /* 0 untried, 1 ok, -1 not */

static const char *const g_prog_name[VB_PROGS] = { "xbrz0", "xbrz1", "xbrz2" };
static const char *const g_prog_uni[VB_PROGS][VB_PROG_UNIFORMS] = {
   { "SourceSize", NULL, NULL, NULL },
   { "SourceSize", "CornerInv", NULL, NULL },
   { "RefSize", "InfoScale", "OutScale", NULL } };
static const char *const g_prog_tex[VB_PROGS][2] = {
   { "Source", NULL }, { "Source", "Corner" }, { "Ref", "Info" } };

/* The Cg half of the shaders' dialect (shaders/xbrz_common.inc). */
static const char g_prelude_cg[] =
   "#define V_XY(v) ((v).xy)\n"
   "#define V_ZW(v) ((v).zw)\n"
   "#define V_RGB(v) ((v).rgb)\n"
   "#define V_MOD(a, b) fmod((a), (b))\n"
   "#define V_EQ3(a, b) all((a) == (b))\n";
static const char g_main0[] =
   "float4 main(float2 vTexcoord : TEXCOORD0,\n"
   "            uniform sampler2D Source : TEXUNIT0,\n"
   "            uniform float4 SourceSize) : COLOR\n{\n";
static const char g_main1[] =
   "float4 main(float2 vTexcoord : TEXCOORD0,\n"
   "            uniform sampler2D Source : TEXUNIT0,\n"
   "            uniform sampler2D Corner : TEXUNIT1,\n"
   "            uniform float4 SourceSize,\n"
   "            uniform float4 CornerInv) : COLOR\n{\n";
static const char g_main2[] =
   "float4 main(float2 vTexcoord : TEXCOORD0,\n"
   "            uniform sampler2D Ref : TEXUNIT0,\n"
   "            uniform sampler2D Info : TEXUNIT1,\n"
   "            uniform float4 RefSize, uniform float4 InfoScale,\n"
   "            uniform float4 OutScale) : COLOR\n{\n";

static int shacc_load(void)
{
   static const char *const where[] = { "ur0:data/libshacccg.suprx",
                                        "ux0:data/libshacccg.suprx" };
   unsigned i;
   if (g_shacc)
      return g_shacc > 0;
   g_shacc = -1;
   for (i = 0; i < sizeof(where) / sizeof(where[0]); i++)
      if (sceKernelLoadStartModule(where[i], 0, NULL, 0, NULL, NULL) >= 0)
      {
         sceShaccCgSetDefaultAllocator(malloc, free);
         g_shacc = 1;
         break;
      }
   return g_shacc > 0;
}

static SceShaccCgSourceFile g_cg_src;

static SceShaccCgSourceFile *cg_open(const char *name,
                                     const SceShaccCgSourceLocation *from,
                                     const SceShaccCgCompileOptions *opt,
                                     const char **err)
{
   (void)name; (void)from; (void)opt; (void)err;
   return &g_cg_src;
}

static uint32_t fnv(const char *s, uint32_t h)
{
   while (*s)
      h = (h ^ (unsigned char)*s++) * 16777619u;
   return h;
}

static const char *g_gxp_reject;       /* why a cached program was not used */

static SceGxmProgram *gxp_load(const char *path)
{
   FILE *f = fopen(path, "rb");
   long n = 0;
   SceGxmProgram *p = NULL;
   g_gxp_reject = NULL;
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && n < (1 << 20) &&
       fseek(f, 0, SEEK_SET) == 0 && (p = (SceGxmProgram *)malloc((size_t)n)))
   {
      if (fread(p, 1, (size_t)n, f) != (size_t)n)
         g_gxp_reject = "cache_short_read";
      else if (sceGxmProgramCheck(p) != 0)
         g_gxp_reject = "cache_check";
      else if (!sceGxmProgramGetSize(p) || sceGxmProgramGetSize(p) > (unsigned)n)
         g_gxp_reject = "cache_size";
      if (g_gxp_reject)
      {
         free(p);
         p = NULL;
      }
   }
   else
      g_gxp_reject = "cache_unreadable";
   fclose(f);
   return p;
}

/* One compile.  variant 0: the options as sceShaccCgInitializeCompileOptions
 * leaves them, then ours; 1: every field set by hand, as vitaShaRK does
 * (Rinnegatamante's wrapper that vitaGL uses).  `why` gets "compiled" or
 * the compiler's own words: every diagnostic, "L<line>:" before each. */
static SceGxmProgram *cg_compile(const char *text, int variant, int opt,
                                 char *why, size_t whylen, unsigned *size)
{
   SceShaccCgCallbackList cb;
   SceShaccCgCompileOptions o;
   const SceShaccCgCompileOutput *out;
   SceGxmProgram *p = NULL;
   size_t at = 0;
   int i;
   *size = 0;
   if (!shacc_load())
   {
      snprintf(why, whylen, "no_compiler");      /* libshacccg.suprx */
      return NULL;
   }
   g_cg_src.fileName = "gbadhoc.cg";
   g_cg_src.text = text;
   g_cg_src.size = (SceUInt32)strlen(text);
   sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
   cb.openFile = cg_open;
   if (variant == 0)
      sceShaccCgInitializeCompileOptions(&o);
   else
      memset(&o, 0, sizeof(o));
   o.mainSourceFile = g_cg_src.fileName;
   o.targetProfile = SCE_SHACCCG_PROFILE_FP;
   o.entryFunctionName = "main";
   o.searchPathCount = 0;
   o.searchPaths = NULL;
   o.macroDefinitionCount = 0;
   o.macroDefinitions = NULL;
   o.includeFileCount = 0;
   o.includeFiles = NULL;
   o.suppressedWarningsCount = 0;
   o.suppressedWarnings = NULL;
   o.locale = SCE_SHACCCG_ENGLISH;
   o.useFx = 1;
   o.noStdlib = 0;
   o.optimizationLevel = opt;
   o.useFastmath = 1;
   o.useFastprecision = 0;
   o.useFastint = 1;
   o.warningsAsErrors = 0;
   o.performanceWarnings = 0;
   o.warningLevel = 1;
   o.pedantic = 0;
   o.pedanticError = 0;
   out = sceShaccCgCompileProgram(&o, &cb, 0);
   if (out && out->programData && out->programSize)
   {
      p = (SceGxmProgram *)malloc(out->programSize);
      if (p)
      {
         memcpy(p, out->programData, out->programSize);
         *size = out->programSize;
      }
      snprintf(why, whylen, p ? "compiled" : "no_memory");
   }
   else
      at = (size_t)snprintf(why, whylen, out ? "failed" : "no_output");
   for (i = 0; out && i < out->diagnosticCount && at + 8 < whylen; i++)
   {
      const SceShaccCgDiagnosticMessage *d = &out->diagnostics[i];
      int n = snprintf(why + at, whylen - at, "%s%c L%u:%s",
                       at ? ";" : "",
                       d->level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR ? 'E'
                       : d->level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING ? 'W'
                       : 'I',
                       d->location ? (unsigned)d->location->lineNumber : 0u,
                       d->message ? d->message : "?");
      if (n < 0)
         break;
      at += (size_t)n;
      if (at >= whylen)
         at = whylen - 1;
   }
   for (i = 0; why[i]; i++)                       /* one log line */
      if (why[i] == '\n' || why[i] == '\r' || why[i] == ' ')
         why[i] = '_';
   if (out)
      sceShaccCgDestroyCompileOutput(out);
   return p;
}

/* Program `which`'s whole text (malloc'd). */
static char *prog_text(int which)
{
   const char *body = which == VB_PROG_XBRZ0 ? gpu_src_xbrz_pass0
                    : which == VB_PROG_XBRZ1 ? gpu_src_xbrz_pass1
                                             : gpu_src_xbrz_pass2;
   const char *mainf = which == VB_PROG_XBRZ0 ? g_main0
                     : which == VB_PROG_XBRZ1 ? g_main1 : g_main2;
   size_t len = strlen(g_prelude_cg) + strlen(gpu_src_xbrz_common) +
                strlen(mainf) + strlen(body) + 4;
   char *text = (char *)malloc(len);
   if (text)
      snprintf(text, len, "%s%s%s%s}\n", g_prelude_cg, gpu_src_xbrz_common,
               mainf, body);
   return text;
}

/* Program `which`'s compiled form: from the cache, or the compiler. */
static SceGxmProgram *prog_gxp(int which, char *why, size_t whylen)
{
   char path[128];
   char *text;
   uint32_t h = 2166136261u;
   unsigned size;
   SceGxmProgram *p;

   text = prog_text(which);
   if (!text)
   {
      snprintf(why, whylen, "no_memory");
      return NULL;
   }
   h = fnv(text, h);
   h = fnv("o3 fastmath fastint v3", h);
   snprintf(path, sizeof(path), "ux0:data/GBAdhoc/shaders/%s-%08x.gxp",
            g_prog_name[which], (unsigned)h);
   if ((p = gxp_load(path)))
   {
      free(text);
      snprintf(why, whylen, "cached");
      return p;
   }
   p = cg_compile(text, 1, 3, why, whylen, &size);
   free(text);
   if (p && g_gxp_reject)
      snprintf(why, whylen, "compiled_%s", g_gxp_reject);
   if (p)
   {
      FILE *f;
      sceIoMkdir("ux0:data/GBAdhoc/shaders", 0777);
      if ((f = fopen(path, "wb")))
      {
         fwrite(p, 1, size, f);
         fclose(f);
      }
   }
   return p;
}

/* vita.ini gpu_probe: compile test programs and say what the compiler
 * said (main_vita.c logs each as `gpu_probe`); nothing is kept.  Returns
 * 0 when there is no probe `n`. */
int vb_prog_probe(int n, char *out, size_t outlen)
{
   static const char tiny[] =
      "float4 main(float2 vTexcoord : TEXCOORD0,\n"
      "            uniform sampler2D Source : TEXUNIT0) : COLOR\n"
      "{\n   return tex2D(Source, vTexcoord);\n}\n";
   static const struct { int text, variant, opt; } probe[] = {
      { -1, 1, 3 }, { VB_PROG_XBRZ0, 1, 3 }, { VB_PROG_XBRZ1, 1, 3 },
      { VB_PROG_XBRZ2, 1, 3 } };
   char why[384];
   char *text;
   unsigned size = 0, t0;
   SceGxmProgram *p;
   if (n < 0 || n >= (int)(sizeof(probe) / sizeof(probe[0])))
      return 0;
   text = probe[n].text < 0 ? strdup(tiny) : prog_text(probe[n].text);
   if (!text)
      return 0;
   t0 = sceKernelGetProcessTimeLow();
   p = cg_compile(text, probe[n].variant, probe[n].opt, why, sizeof(why),
                  &size);
   snprintf(out, outlen, "n=%d text=%s variant=%d opt=%d ok=%d size=%u "
            "us=%u why=%s", n, probe[n].text < 0 ? "tiny"
            : g_prog_name[probe[n].text], probe[n].variant, probe[n].opt,
            p ? 1 : 0, size, (unsigned)(sceKernelGetProcessTimeLow() - t0),
            why);
   free(p);
   free(text);
   return 1;
}

vb_prog *vb_prog_get(int which)
{
   struct vb_prog *P;
   SceGxmShaderPatcher *sp;
   int k;
   if (which < 0 || which >= VB_PROGS || !g_inited)
      return NULL;
   if (g_prog_state[which])
      return g_prog_state[which] > 0 ? &g_prog[which] : NULL;
   g_prog_state[which] = -1;
   P = &g_prog[which];
   P->gxp = prog_gxp(which, g_prog_why[which], sizeof(g_prog_why[which]));
   if (!P->gxp)
      return NULL;
   sp = vita2d_get_shader_patcher();
   if (sceGxmProgramCheck(P->gxp) != 0 ||
       sceGxmShaderPatcherRegisterProgram(sp, P->gxp, &P->id) < 0)
   {
      snprintf(g_prog_why[which], sizeof(g_prog_why[which]), "register_failed");
      free(P->gxp);
      P->gxp = NULL;
      return NULL;
   }
   if (sceGxmShaderPatcherCreateFragmentProgram(
          sp, P->id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
          SCE_GXM_MULTISAMPLE_NONE, NULL, &texture_v_gxp_start, &P->fp) < 0)
   {
      snprintf(g_prog_why[which], sizeof(g_prog_why[which]), "link_failed");
      sceGxmShaderPatcherUnregisterProgram(sp, P->id);
      free(P->gxp);
      P->gxp = NULL;
      return NULL;
   }
   for (k = 0; k < VB_PROG_UNIFORMS; k++)
      P->uni[k] = g_prog_uni[which][k]
                  ? sceGxmProgramFindParameterByName(P->gxp, g_prog_uni[which][k])
                  : NULL;
   for (k = 0; k < 2; k++)
   {
      const SceGxmProgramParameter *s = g_prog_tex[which][k]
         ? sceGxmProgramFindParameterByName(P->gxp, g_prog_tex[which][k]) : NULL;
      P->tex[k] = s ? (int)sceGxmProgramParameterGetResourceIndex(s) : k;
   }
   g_prog_state[which] = 1;
   return P;
}

/* vb_term: the patcher goes with libvita2d. */
static void prog_release(void)
{
   SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
   int i;
   vita2d_wait_rendering_done();
   for (i = 0; i < VB_PROGS; i++)
   {
      if (g_prog_state[i] > 0)
      {
         sceGxmShaderPatcherReleaseFragmentProgram(sp, g_prog[i].fp);
         sceGxmShaderPatcherUnregisterProgram(sp, g_prog[i].id);
         free(g_prog[i].gxp);
      }
      memset(&g_prog[i], 0, sizeof(g_prog[i]));
      g_prog_state[i] = 0;
   }
}

const char *vb_prog_status(int which)
{
   if (which < 0 || which >= VB_PROGS)
      return "none";
   return g_prog_state[which] ? g_prog_why[which] : "untried";
}

void vb_pquads(vb_prog *p, vb_tex *t0, vb_tex *t1, const vb_tquad *q, int n,
               const float uni[VB_PROG_UNIFORMS][4])
{
   float iw, ih;
   int clip = g_sc_on && !g_rt, k = 0, i;
   float cx0 = (float)g_sc_x, cy0 = (float)g_sc_y;
   float cx1 = (float)(g_sc_x + g_sc_w), cy1 = (float)(g_sc_y + g_sc_h);
   vita2d_texture_vertex *v;
   void *u;
   if (!p || !p->fp || !t0 || !q || n <= 0 || n > QUAD_BATCH || !uni ||
       (!g_open && !g_rt) || t0 == g_rt || (t1 && t1 == g_rt))
      return;
   iw = 1.0f / (float)t0->w;
   ih = 1.0f / (float)t0->h;
   v = (vita2d_texture_vertex *)vita2d_pool_memalign(
          (unsigned)(n * 6) * sizeof(vita2d_texture_vertex),
          sizeof(vita2d_texture_vertex));
   if (!v)
      return;
   for (i = 0; i < n; i++, q++)
   {
      float x0 = q->x0, y0 = q->y0, x1 = q->x1, y1 = q->y1;
      float u0 = q->u0, v0 = q->v0, u1 = q->u1, v1 = q->v1;
      vita2d_texture_vertex *e;
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
      e = v + k++ * 6;
      e[0].x = x0; e[0].y = y0; e[0].z = 0.5f; e[0].u = u0; e[0].v = v0;
      e[1].x = x1; e[1].y = y0; e[1].z = 0.5f; e[1].u = u1; e[1].v = v0;
      e[2].x = x0; e[2].y = y1; e[2].z = 0.5f; e[2].u = u0; e[2].v = v1;
      e[3] = e[2];
      e[4] = e[1];
      e[5].x = x1; e[5].y = y1; e[5].z = 0.5f; e[5].u = u1; e[5].v = v1;
   }
   if (!k)
      return;
   (void)vita2d_fragment_fence_record(&t0->readers);
   if (t1)
      (void)vita2d_fragment_fence_record(&t1->readers);
   sceGxmSetVertexProgram(_vita2d_context, _vita2d_textureVertexProgram);
   sceGxmSetFragmentProgram(_vita2d_context, p->fp);
   sceGxmReserveVertexDefaultUniformBuffer(_vita2d_context, &u);
   sceGxmSetUniformDataF(u, _vita2d_textureWvpParam, 0, 16,
                         _vita2d_ortho_matrix);
   if (sceGxmReserveFragmentDefaultUniformBuffer(_vita2d_context, &u) == 0)
      for (i = 0; i < VB_PROG_UNIFORMS; i++)
         if (p->uni[i])
            sceGxmSetUniformDataF(u, p->uni[i], 0, 4, uni[i]);
   sceGxmSetBackPolygonMode(_vita2d_context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
   vita2d_texture_set_filters(t0->t, SCE_GXM_TEXTURE_FILTER_POINT,
                              SCE_GXM_TEXTURE_FILTER_POINT);
   sceGxmSetFragmentTexture(_vita2d_context, (unsigned)p->tex[0],
                            &t0->t->gxm_tex);
   if (t1)
   {
      vita2d_texture_set_filters(t1->t, SCE_GXM_TEXTURE_FILTER_POINT,
                                 SCE_GXM_TEXTURE_FILTER_POINT);
      sceGxmSetFragmentTexture(_vita2d_context, (unsigned)p->tex[1],
                               &t1->t->gxm_tex);
   }
   sceGxmSetVertexStream(_vita2d_context, 0, v);
   sceGxmDraw(_vita2d_context, SCE_GXM_PRIMITIVE_TRIANGLES,
              SCE_GXM_INDEX_FORMAT_U16, vita2d_get_linear_indices(),
              (unsigned)(k * 6));
}
