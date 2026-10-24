/* video_vita_text.c -- the UI's text at the Vita's native resolution.
 *
 * The PSP bakes Inter on the host into two 8-bit atlases (tools/bake_font.py
 * -> psp/font_ui.h): `ui`, Inter Regular at 15 px, and `hd`, Inter SemiBold
 * at 19 px, and draws them 1:1.  Stretching those bitmaps 2x on a 960x544
 * panel would be exactly the blur this port exists to avoid, so here the two
 * faces are rasterised from the TTFs themselves (embedded in the binary,
 * video_vita_fonts.S) with stb_truetype at 30 px and 38 px -- twice the PSP
 * sizes -- once, at start-up, into two A8 atlases.
 *
 * LAYOUT IS THE PSP'S, EXACTLY.  ui_psp.c lays every screen out in the PSP's
 * 480x272 pixels by asking vid_text_w(), and several rows are full to within
 * a few pixels (the browser footer ends at x = 469 of 480).  So the advance
 * of every glyph is 2 x the PSP's baked integer advance, read from the very
 * table the PSP uses (font_ui.h, fu_*_g[].xadv): vid_text_w() returns the
 * same number on both consoles for every string, by construction.
 * The natural advance at 30 px is within 1 native pixel of that (half a PSP
 * pixel: the PSP rounded it); the difference is split between the glyph's two
 * side bearings, placed with stb's sub-pixel shift so no stem is resampled.
 * vv_text_worst_dev() reports the numbers.
 *
 * Vertical metrics are the PSP's too: the baseline sits FU_*_ASC logical
 * rows below the line top (15 / 19 -> 30 / 38 native), and the cap height of
 * the atlas 'H' that vid_band_y() centres on comes from the PSP table.
 *
 * THE SIX BUTTON GLYPHS (bytes 0x7F..0x84: triangle, circle, cross, square,
 * star, star outline).  The PSP takes five of them from Inter's geometric
 * shapes at a reduced size and synthesises the cross.  Here all six are
 * drawn as vector shapes straight into the atlas at native size, 8x8
 * super-sampled, in the PSP's cell (same advance, centred on the same row),
 * so they are as sharp as the letters and uniform in weight.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

#include "video_vita_priv.h"
#include "font_ui.h"          /* the PSP's glyph tables: advances, metrics */

extern const unsigned char vv_font_regular[], vv_font_regular_end[];
extern const unsigned char vv_font_semibold[], vv_font_semibold_end[];

#define ATLAS_W   512
#define ATLAS_H   512         /* working buffer; the texture is cut to fit */
#define N_EXTRA   6
#define N_ASCII   (FU_COUNT - N_EXTRA)

typedef struct
{
   unsigned short ax, ay;     /* position in the atlas                     */
   unsigned char  w, h;       /* bitmap size, native px (0 = no ink)       */
   signed char    ox, oy;     /* bitmap offset from the pen / the line top */
   unsigned char  adv;        /* advance, native px = 2 x the PSP's        */
} vv_glyph;

typedef struct
{
   vv_glyph g[FU_COUNT];
   vb_tex  *tex;
   float    worst, mean;      /* natural vs used advance, native px        */
   int      worst_ch;
} vv_face;

static vv_face g_face[2];     /* 0 = ui, 1 = hd                            */
static int     g_ready;

/* ---- a row packer over one 8-bit working atlas -------------------------- */

typedef struct
{
   unsigned char *px;
   int pen_x, pen_y, row_h, overflow;
} packer;

static unsigned char *pack(packer *p, int w, int h, int *ax, int *ay)
{
   if (p->pen_x + w + 1 > ATLAS_W)
   {
      p->pen_x = 0;
      p->pen_y += p->row_h + 1;
      p->row_h = 0;
   }
   if (p->pen_y + h > ATLAS_H)
   {
      p->overflow = 1;
      return NULL;
   }
   *ax = p->pen_x; *ay = p->pen_y;
   p->pen_x += w + 1;
   if (h > p->row_h)
      p->row_h = h;
   return p->px + (size_t)*ay * ATLAS_W + *ax;
}

/* ---- the button glyphs as vector shapes --------------------------------- */

typedef struct { float x, y; } v2;

static float seg_dist(v2 p, v2 a, v2 b)
{
   float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
   float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
   float dx, dy;
   if (t < 0.0f) t = 0.0f;
   if (t > 1.0f) t = 1.0f;
   dx = wx - t * vx; dy = wy - t * vy;
   return sqrtf(dx * dx + dy * dy);
}

/* Distance to a closed polygon's outline; *inside by even-odd crossing. */
static float poly_dist(v2 p, const v2 *v, int n, int *inside)
{
   float best = 1e9f;
   int i, j, in = 0;
   for (i = 0, j = n - 1; i < n; j = i++)
   {
      float d = seg_dist(p, v[j], v[i]);
      if (d < best)
         best = d;
      if (((v[i].y > p.y) != (v[j].y > p.y)) &&
          (p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) +
                 v[i].x))
         in = !in;
   }
   *inside = in;
   return best;
}

/* 1 when the unit-space point p (the shape spans about -0.5..0.5) is ink.
 * `sw` is the stroke width in the same units. */
static int shape_hit(int kind, v2 p, float sw)
{
   static const v2 TRI[3] = { {0.0f, -0.47f}, {0.5f, 0.40f}, {-0.5f, 0.40f} };
   static v2 star[10];
   static int star_ready;
   int in;
   float d;

   if (!star_ready)
   {
      int i;
      for (i = 0; i < 10; i++)
      {
         float r = (i & 1) ? 0.215f : 0.53f;
         float a = -1.5707963f + (float)i * 0.62831853f;
         star[i].x = r * cosf(a);
         star[i].y = r * sinf(a) + 0.045f;   /* optical centre of a star */
      }
      star_ready = 1;
   }
   switch (kind)
   {
   case 0:                                   /* triangle, outline         */
      d = poly_dist(p, TRI, 3, &in);
      return in && d <= sw;
   case 1:                                   /* circle, ring              */
      d = sqrtf(p.x * p.x + p.y * p.y);
      return d <= 0.48f && d >= 0.48f - sw;
   case 2:                                   /* cross                     */
   {
      static const v2 A = {-0.40f, -0.40f}, B = {0.40f, 0.40f};
      static const v2 C = {-0.40f, 0.40f}, D = {0.40f, -0.40f};
      float h = sw * 0.56f;
      return seg_dist(p, A, B) <= h || seg_dist(p, C, D) <= h;
   }
   case 3:                                   /* square, outline           */
   {
      float ax = fabsf(p.x), ay = fabsf(p.y), m = ax > ay ? ax : ay;
      return m <= 0.43f && m >= 0.43f - sw;
   }
   case 4:                                   /* star                      */
      (void)poly_dist(p, star, 10, &in);
      return in;
   default:                                  /* star, outline             */
      d = poly_dist(p, star, 10, &in);
      return in && d <= sw * 0.85f;
   }
}

/* One button glyph into the atlas: `size` native px across, centred in an
 * advance of `adv` on the row `mid` (native, from the line top). */
static void bake_extra(packer *pk, vv_glyph *g, int kind, float size,
                       float stroke, int adv, float mid)
{
   const int SS = 8;
   int w = (int)ceilf(size) + 2, h = w, ax = 0, ay = 0, x, y, sx, sy;
   unsigned char *d = pack(pk, w, h, &ax, &ay);
   float cx = (float)w * 0.5f, cy = (float)h * 0.5f;
   memset(g, 0, sizeof(*g));
   g->adv = (unsigned char)adv;
   if (!d)
      return;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         int hits = 0;
         for (sy = 0; sy < SS; sy++)
            for (sx = 0; sx < SS; sx++)
            {
               v2 p;
               p.x = ((float)x + ((float)sx + 0.5f) / SS - cx) / size;
               p.y = ((float)y + ((float)sy + 0.5f) / SS - cy) / size;
               hits += shape_hit(kind, p, stroke / size);
            }
         d[(size_t)y * ATLAS_W + x] = (unsigned char)(hits * 255 / (SS * SS));
      }
   g->ax = (unsigned short)ax; g->ay = (unsigned short)ay;
   g->w = (unsigned char)w; g->h = (unsigned char)h;
   g->ox = (signed char)((adv - w) / 2);
   g->oy = (signed char)lroundf(mid - (float)h * 0.5f);
}

/* ---- one face ----------------------------------------------------------- */

static int bake_face(vv_face *f, const unsigned char *ttf, float px_logical,
                     int asc_logical, const fu_glyph *psp, float extra_size,
                     float extra_star, float extra_stroke, float extra_mid)
{
   stbtt_fontinfo font;
   packer pk;
   float scale, sum = 0.0f;
   int c, used_h, n = 0;

   memset(f, 0, sizeof(*f));
   if (!stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)))
      return -1;
   pk.px = (unsigned char *)calloc(ATLAS_W, ATLAS_H);
   if (!pk.px)
      return -1;
   pk.pen_x = pk.pen_y = pk.row_h = pk.overflow = 0;

   /* PIL's truetype(path, 15) is "15 px to the em"; so is this. */
   scale = stbtt_ScaleForMappingEmToPixels(&font, px_logical * VV_S);

   for (c = FU_FIRST; c < FU_FIRST + N_ASCII; c++)
   {
      vv_glyph *g = &f->g[c - FU_FIRST];
      int adv_units, lsb, x0, y0, x1, y1, ax = 0, ay = 0, w, h;
      float nat, e, shift, dev;
      int target = VV_S * psp[c - FU_FIRST].xadv;
      unsigned char *d;

      stbtt_GetCodepointHMetrics(&font, c, &adv_units, &lsb);
      nat = (float)adv_units * scale;
      dev = nat - (float)target;
      sum += fabsf(dev);
      n++;
      if (fabsf(dev) > fabsf(f->worst))
      {
         f->worst = dev;
         f->worst_ch = c;
      }
      /* Split the rounding the PSP did between the two side bearings. */
      e = ((float)target - nat) * 0.5f;
      shift = e - floorf(e);
      g->adv = (unsigned char)target;
      stbtt_GetCodepointBitmapBoxSubpixel(&font, c, scale, scale, shift, 0.0f,
                                          &x0, &y0, &x1, &y1);
      w = x1 - x0; h = y1 - y0;
      if (w <= 0 || h <= 0 || w > 255 || h > 255)
         continue;                     /* a space: advance only */
      d = pack(&pk, w, h, &ax, &ay);
      if (!d)
         continue;
      stbtt_MakeCodepointBitmapSubpixel(&font, d, w, h, ATLAS_W, scale, scale,
                                        shift, 0.0f, c);
      g->ax = (unsigned short)ax; g->ay = (unsigned short)ay;
      g->w = (unsigned char)w; g->h = (unsigned char)h;
      g->ox = (signed char)((int)floorf(e) + x0);
      g->oy = (signed char)(VV_S * asc_logical + y0);
   }
   f->mean = n ? sum / (float)n : 0.0f;

   for (c = 0; c < N_EXTRA; c++)
   {
      int idx = N_ASCII + c;
      bake_extra(&pk, &f->g[idx], c, c >= 4 ? extra_star : extra_size,
                 extra_stroke, VV_S * psp[idx].xadv, extra_mid);
   }

   used_h = (pk.pen_y + pk.row_h + 8) & ~7;
   if (used_h > ATLAS_H)
      used_h = ATLAS_H;
   f->tex = vb_tex_create(ATLAS_W, used_h, VB_FMT_A8, 0);
   if (f->tex)
      vb_tex_update(f->tex, 0, 0, ATLAS_W, used_h, pk.px, ATLAS_W);
   free(pk.px);
   return (f->tex && !pk.overflow) ? 0 : -1;
}

int vv_text_init(void)
{
   int rc = 0;
   if (g_ready)
      return 0;
   /* Sizes and rows: tools/bake_font.py FACES / EXTRAS / EXTRA_MID, x2.
    * The shapes are a little larger than Inter's reduced geometric glyphs
    * were (10 px in a 12 px advance): 10.5 logical px across, the star
    * 11.5 -- about the cap height, the size the system's own prompts use. */
   rc |= bake_face(&g_face[0], vv_font_regular, 15.0f, FU_UI_ASC, fu_ui_g,
                   21.0f, 23.0f, 2.3f, 9.5f * VV_S);
   rc |= bake_face(&g_face[1], vv_font_semibold, 19.0f, FU_HD_ASC, fu_hd_g,
                   27.0f, 29.0f, 3.0f, 12.0f * VV_S);
   g_ready = 1;
   return rc;
}

void vv_text_term(void)
{
   int i;
   for (i = 0; i < 2; i++)
   {
      vb_tex_destroy(g_face[i].tex);
      g_face[i].tex = NULL;
   }
   g_ready = 0;
}

float vv_text_worst_dev(int hd, int *ch, float *mean_abs)
{
   const vv_face *f = &g_face[hd ? 1 : 0];
   if (ch) *ch = f->worst_ch;
   if (mean_abs) *mean_abs = f->mean;
   return f->worst;
}

/* The PSP's own measure (video_psp.c fu_measure), over the PSP's table. */
int vv_text_width(int hd, const char *s)
{
   const fu_glyph *g = hd ? fu_hd_g : fu_ui_g;
   int w = 0;
   for (; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      if (c < FU_FIRST || c > FU_LAST)
         c = '?';
      w += g[c - FU_FIRST].xadv;
   }
   return w;
}

void vv_text_cap(int *top, int *h)
{
   const fu_glyph *g = &fu_ui_g['H' - FU_FIRST];
   *top = g->yoff;
   *h   = g->h;
}

#define TEXT_BATCH 128

void vv_text_draw(int hd, int nx, int ny, const char *s, uint32_t tint)
{
   const vv_face *f = &g_face[hd ? 1 : 0];
   vb_tquad q[TEXT_BATCH];
   int n = 0, pen = nx;

   if (!f->tex || !s)
      return;
   for (; *s; s++)
   {
      unsigned char c = (unsigned char)*s;
      const vv_glyph *g;
      if (c < FU_FIRST || c > FU_LAST)
         c = '?';
      g = &f->g[c - FU_FIRST];
      if (g->w)
      {
         q[n].x0 = (float)(pen + g->ox);
         q[n].y0 = (float)(ny + g->oy);
         q[n].x1 = q[n].x0 + (float)g->w;
         q[n].y1 = q[n].y0 + (float)g->h;
         q[n].u0 = (float)g->ax;
         q[n].v0 = (float)g->ay;
         q[n].u1 = (float)(g->ax + g->w);
         q[n].v1 = (float)(g->ay + g->h);
         if (++n == TEXT_BATCH)
         {
            vb_tquads(f->tex, q, n, tint, VB_NEAREST);
            n = 0;
         }
      }
      pen += g->adv;
   }
   if (n)
      vb_tquads(f->tex, q, n, tint, VB_NEAREST);
}
