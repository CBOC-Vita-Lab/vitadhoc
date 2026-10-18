/* vback_soft.c -- the software backend of vback.h.
 *
 * Plain C, no dependencies: a 960x544 RGBA frame in memory and a rectangle
 * rasteriser that holds exactly the semantics vback.h states (pixel centres,
 * texel-space coordinates, clamp addressing, nearest / bilinear, src-over
 * blending, tint, scissor, render targets).  It is the REFERENCE for those
 * semantics and the way the Vita UI is verified off the console: the host
 * twin (gpsp/vita/host) links ui_psp.c + video_vita.c + this file and writes
 * PNGs of every screen.
 *
 * Every texture is kept as 0xAABBGGRR words whatever format it was created
 * in (565 expands by bit replication, as a GPU does; A8 becomes white with
 * the byte as alpha), so one sampler serves all four formats.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vback.h"

struct vb_tex
{
   int       w, h, fmt, rt;
   uint32_t *px;
};

static uint32_t  g_fb[VB_W * VB_H];
static uint32_t *g_tgt = g_fb;         /* current target                   */
static int       g_tw = VB_W, g_th = VB_H;
static int       g_open;               /* a frame is open                  */
static int       g_rt_on;
/* Scissor: one for the frame, none inside a render target (video_vita.c
 * never clips a prescale pass). */
static int g_sc_on, g_sc_x0, g_sc_y0, g_sc_x1, g_sc_y1;

const char *vb_name(void) { return "soft"; }

int vb_init(void)
{
   memset(g_fb, 0, sizeof(g_fb));
   g_tgt = g_fb; g_tw = VB_W; g_th = VB_H;
   g_open = g_rt_on = g_sc_on = 0;
   return 0;
}

void vb_term(void) { }

/* ---- textures ---------------------------------------------------------- */

vb_tex *vb_tex_create(int w, int h, int fmt, int rt)
{
   vb_tex *t;
   if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
      return NULL;
   t = (vb_tex *)calloc(1, sizeof(*t));
   if (!t)
      return NULL;
   t->px = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
   if (!t->px)
   {
      free(t);
      return NULL;
   }
   t->w = w; t->h = h; t->fmt = fmt; t->rt = rt;
   return t;
}

void vb_tex_destroy(vb_tex *t)
{
   if (!t)
      return;
   free(t->px);
   free(t);
}

int vb_tex_w(const vb_tex *t) { return t ? t->w : 0; }
int vb_tex_h(const vb_tex *t) { return t ? t->h : 0; }

static uint32_t from565(unsigned c, int bgr)
{
   unsigned hi = (c >> 11) & 31, g = (c >> 5) & 63, lo = c & 31;
   unsigned r = bgr ? lo : hi, b = bgr ? hi : lo;
   r = (r << 3) | (r >> 2);
   g = (g << 2) | (g >> 4);
   b = (b << 3) | (b >> 2);
   return VB_RGBA(r, g, b, 255);
}

void *vb_tex_lock(vb_tex *t, size_t *pitch_bytes)
{
   /* Every format is kept as RGBA8 words here; only an RGBA8 texture's
    * storage IS its format. */
   if (!t || t->fmt != VB_FMT_RGBA8)
      return NULL;
   if (pitch_bytes)
      *pitch_bytes = (size_t)t->w * 4;
   return t->px;
}

int vb_tex_read(vb_tex *t, uint32_t *dst)
{
   if (!t || !dst || t->fmt != VB_FMT_RGBA8)
      return -1;
   memcpy(dst, t->px, (size_t)t->w * t->h * 4);
   return 0;
}

void vb_tex_update(vb_tex *t, int x, int y, int w, int h,
                   const void *pix, size_t pitch_bytes)
{
   int i, j;
   if (!t || !pix || x < 0 || y < 0 || x + w > t->w || y + h > t->h)
      return;
   for (j = 0; j < h; j++)
   {
      const uint8_t *s = (const uint8_t *)pix + (size_t)j * pitch_bytes;
      uint32_t *d = t->px + (size_t)(y + j) * t->w + x;
      switch (t->fmt)
      {
      case VB_FMT_RGB565:
      case VB_FMT_BGR565:
         for (i = 0; i < w; i++)
         {
            unsigned c = s[i * 2] | ((unsigned)s[i * 2 + 1] << 8);
            d[i] = from565(c, t->fmt == VB_FMT_BGR565);
         }
         break;
      case VB_FMT_A8:
         for (i = 0; i < w; i++)
            d[i] = VB_RGBA(255, 255, 255, s[i]);
         break;
      default:
         for (i = 0; i < w; i++)
            d[i] = VB_RGBA(s[i * 4], s[i * 4 + 1], s[i * 4 + 2], s[i * 4 + 3]);
         break;
      }
   }
}

/* ---- frame / targets --------------------------------------------------- */

void vb_frame_begin(int clear)
{
   if (clear)
   {
      int i;
      for (i = 0; i < VB_W * VB_H; i++)
         g_fb[i] = 0xFF000000u;
   }
   g_open = 1;
}

void vb_frame_end(void)  { g_open = 0; }
int  vb_frame_open(void) { return g_open; }
static void gpu_timed_present(void);
void vb_present(void)    { g_open = 0; g_sc_on = 0; gpu_timed_present(); }
void vb_set_vsync(int on) { (void)on; }
void vb_set_common_dialog(int on) { (void)on; }

void vb_tex_mem(int cpu_written) { (void)cpu_written; }

/* The twin's display queue is plat_host.c's (--flip-queue). */
int plat_host_flip_state(void);
int vb_present_state(void) { return plat_host_flip_state(); }

const char *vb_tex_where(const vb_tex *t) { return t ? "ram" : "?"; }

void vb_rt_begin(vb_tex *rt)
{
   if (!rt || !rt->rt)
      return;
   g_tgt = rt->px; g_tw = rt->w; g_th = rt->h;
   g_rt_on = 1;
}

void vb_rt_end(void)
{
   g_tgt = g_fb; g_tw = VB_W; g_th = VB_H;
   g_rt_on = 0;
}

void vb_scissor(int on, int x, int y, int w, int h)
{
   g_sc_on = on;
   g_sc_x0 = x; g_sc_y0 = y; g_sc_x1 = x + w; g_sc_y1 = y + h;
}

/* The integer pixel range a float rect covers (centre rule), clipped. */
static int span(float a, float b, int lim, int sc0, int sc1, int *i0, int *i1)
{
   int lo = (int)ceilf(a - 0.5f), hi = (int)ceilf(b - 0.5f);
   if (lo < 0) lo = 0;
   if (hi > lim) hi = lim;
   if (g_sc_on && !g_rt_on)
   {
      if (lo < sc0) lo = sc0;
      if (hi > sc1) hi = sc1;
   }
   *i0 = lo; *i1 = hi;
   return hi > lo;
}

static inline uint32_t blend(uint32_t dst, unsigned r, unsigned g, unsigned b,
                             unsigned a)
{
   unsigned dr, dg, db, ia;
   if (a >= 255)
      return VB_RGBA(r, g, b, 255);
   if (!a)
      return dst;
   ia = 255 - a;
   dr = dst & 0xFF; dg = (dst >> 8) & 0xFF; db = (dst >> 16) & 0xFF;
   r = (r * a + dr * ia + 127) / 255;
   g = (g * a + dg * ia + 127) / 255;
   b = (b * a + db * ia + 127) / 255;
   return VB_RGBA(r, g, b, 255);
}

void vb_quad(float x0, float y0, float x1, float y1,
             uint32_t c_tl, uint32_t c_tr, uint32_t c_bl, uint32_t c_br)
{
   int ix0, ix1, iy0, iy1, x, y, k;
   int flat = (c_tl == c_tr && c_tl == c_bl && c_tl == c_br);
   if (!(x1 > x0) || !(y1 > y0))
      return;
   if (!span(x0, x1, g_tw, g_sc_x0, g_sc_x1, &ix0, &ix1) ||
       !span(y0, y1, g_th, g_sc_y0, g_sc_y1, &iy0, &iy1))
      return;
   for (y = iy0; y < iy1; y++)
   {
      uint32_t *d = g_tgt + (size_t)y * g_tw;
      float ty = ((float)y + 0.5f - y0) / (y1 - y0);
      for (x = ix0; x < ix1; x++)
      {
         unsigned c[4];
         if (flat)
         {
            for (k = 0; k < 4; k++)
               c[k] = (c_tl >> (8 * k)) & 0xFF;
         }
         else
         {
            float tx = ((float)x + 0.5f - x0) / (x1 - x0);
            for (k = 0; k < 4; k++)
            {
               float a = (float)((c_tl >> (8 * k)) & 0xFF);
               float b = (float)((c_tr >> (8 * k)) & 0xFF);
               float e = (float)((c_bl >> (8 * k)) & 0xFF);
               float f = (float)((c_br >> (8 * k)) & 0xFF);
               float top = a + (b - a) * tx, bot = e + (f - e) * tx;
               c[k] = (unsigned)(top + (bot - top) * ty + 0.5f);
            }
         }
         d[x] = blend(d[x], c[0], c[1], c[2], c[3]);
      }
   }
}

static inline int clampi(int v, int lo, int hi)
{
   return v < lo ? lo : (v > hi ? hi : v);
}

void vb_tquads(vb_tex *t, const vb_tquad *q, int n, uint32_t tint, int filter)
{
   unsigned tr = tint & 0xFF, tg = (tint >> 8) & 0xFF;
   unsigned tb = (tint >> 16) & 0xFF, ta = (tint >> 24) & 0xFF;
   int i;
   if (!t || !q || n <= 0)
      return;
   /* A target cannot sample itself. */
   if (t->px == g_tgt)
      return;
   for (i = 0; i < n; i++)
   {
      float x0 = q[i].x0, y0 = q[i].y0, x1 = q[i].x1, y1 = q[i].y1;
      float du, dv;
      int ix0, ix1, iy0, iy1, x, y;
      if (!(x1 > x0) || !(y1 > y0))
         continue;
      if (!span(x0, x1, g_tw, g_sc_x0, g_sc_x1, &ix0, &ix1) ||
          !span(y0, y1, g_th, g_sc_y0, g_sc_y1, &iy0, &iy1))
         continue;
      du = (q[i].u1 - q[i].u0) / (x1 - x0);
      dv = (q[i].v1 - q[i].v0) / (y1 - y0);
      for (y = iy0; y < iy1; y++)
      {
         uint32_t *d = g_tgt + (size_t)y * g_tw;
         float v = q[i].v0 + ((float)y + 0.5f - y0) * dv;
         for (x = ix0; x < ix1; x++)
         {
            float u = q[i].u0 + ((float)x + 0.5f - x0) * du;
            unsigned r, g, b, a;
            if (filter == VB_NEAREST)
            {
               uint32_t c = t->px[(size_t)clampi((int)floorf(v), 0, t->h - 1) *
                                  t->w + clampi((int)floorf(u), 0, t->w - 1)];
               r = c & 0xFF; g = (c >> 8) & 0xFF;
               b = (c >> 16) & 0xFF; a = c >> 24;
            }
            else
            {
               float fu = u - 0.5f, fv = v - 0.5f;
               int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
               float wx = fu - (float)u0, wy = fv - (float)v0;
               int ua = clampi(u0, 0, t->w - 1), ub = clampi(u0 + 1, 0, t->w - 1);
               int va = clampi(v0, 0, t->h - 1), vb = clampi(v0 + 1, 0, t->h - 1);
               uint32_t c00 = t->px[(size_t)va * t->w + ua];
               uint32_t c10 = t->px[(size_t)va * t->w + ub];
               uint32_t c01 = t->px[(size_t)vb * t->w + ua];
               uint32_t c11 = t->px[(size_t)vb * t->w + ub];
               float ch[4];
               int k;
               for (k = 0; k < 4; k++)
               {
                  float p00 = (float)((c00 >> (8 * k)) & 0xFF);
                  float p10 = (float)((c10 >> (8 * k)) & 0xFF);
                  float p01 = (float)((c01 >> (8 * k)) & 0xFF);
                  float p11 = (float)((c11 >> (8 * k)) & 0xFF);
                  float top = p00 + (p10 - p00) * wx;
                  float bot = p01 + (p11 - p01) * wx;
                  ch[k] = top + (bot - top) * wy;
               }
               r = (unsigned)(ch[0] + 0.5f); g = (unsigned)(ch[1] + 0.5f);
               b = (unsigned)(ch[2] + 0.5f); a = (unsigned)(ch[3] + 0.5f);
            }
            r = (r * tr + 127) / 255; g = (g * tg + 127) / 255;
            b = (b * tb + 127) / 255; a = (a * ta + 127) / 255;
            d[x] = blend(d[x], r, g, b, a);
         }
      }
   }
}

int vb_readback(uint32_t *dst)
{
   if (!dst)
      return -1;
   memcpy(dst, g_fb, sizeof(g_fb));
   g_open = 0;
   return 0;
}

/* ---- GPU programs (vback.h vb_prog) ------------------------------------
 * The shader text itself, compiled as C++ (vback_soft_prog.cpp) and run
 * once per output pixel: slow (a fit-sized xBRZ frame is ~0.4 M programs),
 * so only when the twin is asked for it (--gpu-progs sets
 * GBADHOC_SOFT_PROGS); without it the twin answers as a console without
 * the shader compiler does, and every picture takes the CPU's path. */
/* No GPU here: a timed present takes GBADHOC_SOFT_GPU_US (--gpu-us). */
static int g_gpu_time;
static unsigned g_gpu_n;

void vb_gpu_time(int frames) { g_gpu_time = frames < 0 ? -1 : frames; }
int  vb_gpu_timing(void)     { return g_gpu_time; }

static void gpu_timed_present(void)
{
   if (!g_gpu_time)
      return;
   if (g_gpu_time > 0)
      g_gpu_time--;
   g_gpu_n++;
}

void vb_gpu_stats(unsigned *frames, unsigned *avg_us, unsigned *max_us)
{
   const char *e = getenv("GBADHOC_SOFT_GPU_US");
   unsigned us = e ? (unsigned)atoi(e) : 0;
   if (frames) *frames = g_gpu_n;
   if (avg_us) *avg_us = g_gpu_n ? us : 0;
   if (max_us) *max_us = g_gpu_n ? us : 0;
   g_gpu_n = 0;
}

struct vb_prog { int which; };
static struct vb_prog g_progs[VB_PROGS] = { { VB_PROG_XBRZ0 }, { VB_PROG_XBRZ1 },
                                             { VB_PROG_XBRZ2 } };

uint32_t vbsp_run(int which, const uint32_t *t0, int w0, int h0,
                  const uint32_t *t1, int w1, int h1, float u, float v,
                  const float uni[4][4]);

static int progs_on(void)
{
   static int on = -1;
   if (on < 0)
   {
      const char *e = getenv("GBADHOC_SOFT_PROGS");
      on = e && *e && *e != '0';
   }
   return on;
}

vb_prog *vb_prog_get(int which)
{
   if (which < 0 || which >= VB_PROGS || !progs_on())
      return NULL;
   return &g_progs[which];
}

int vb_prog_probe(int n, char *out, size_t outlen)
{
   (void)n; (void)out; (void)outlen;
   return 0;
}

const char *vb_prog_status(int which)
{
   if (which < 0 || which >= VB_PROGS)
      return "none";
   return progs_on() ? "soft" : "off";
}

void vb_pquads(vb_prog *p, vb_tex *t0, vb_tex *t1, const vb_tquad *q, int n,
               const float uni[VB_PROG_UNIFORMS][4])
{
   int i;
   if (!p || !t0 || !q || n <= 0 || !uni)
      return;
   if (t0->px == g_tgt || (t1 && t1->px == g_tgt))
      return;
   for (i = 0; i < n; i++)
   {
      float x0 = q[i].x0, y0 = q[i].y0, x1 = q[i].x1, y1 = q[i].y1;
      float du, dv;
      int ix0, ix1, iy0, iy1, x, y;
      if (!(x1 > x0) || !(y1 > y0))
         continue;
      if (!span(x0, x1, g_tw, g_sc_x0, g_sc_x1, &ix0, &ix1) ||
          !span(y0, y1, g_th, g_sc_y0, g_sc_y1, &iy0, &iy1))
         continue;
      du = (q[i].u1 - q[i].u0) / (x1 - x0);
      dv = (q[i].v1 - q[i].v0) / (y1 - y0);
      for (y = iy0; y < iy1; y++)
      {
         uint32_t *d = g_tgt + (size_t)y * g_tw;
         float v = (q[i].v0 + ((float)y + 0.5f - y0) * dv) / (float)t0->h;
         for (x = ix0; x < ix1; x++)
         {
            float u = (q[i].u0 + ((float)x + 0.5f - x0) * du) / (float)t0->w;
            d[x] = vbsp_run(p->which, t0->px, t0->w, t0->h,
                            t1 ? t1->px : NULL, t1 ? t1->w : 0,
                            t1 ? t1->h : 0, u, v, uni);
         }
      }
   }
}
