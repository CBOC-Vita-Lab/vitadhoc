/* vstyle.c -- see vstyle.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

#include "vstyle.h"
#include "video_vita_priv.h"
#include "vstyle_icons.h"

extern const unsigned char vv_font_regular[], vv_font_semibold[];
extern const unsigned char vv_font_display_md[], vv_font_display_sb[];

/* ---- type -------------------------------------------------------------- */

#define VS_ATLAS_W 512
#define VS_ATLAS_H 512
#define VS_FIRST   32
#define VS_LAST    126
#define VS_GLYPHS  (VS_LAST - VS_FIRST + 1)

typedef struct
{
   unsigned short ax, ay;
   unsigned char  w, h;
   signed char    ox, oy;      /* bitmap offset from the pen / line top */
   float          adv;
} vs_glyph;

typedef struct
{
   int       tried, ok;
   vs_glyph  g[VS_GLYPHS];
   vb_tex   *tex;
   int       line_h, ascent;
   stbtt_fontinfo font;
   float     scale;
} vs_face;

static const struct
{
   const unsigned char *ttf;
   float px;
} FACE_SRC[VS_FACES] = {
   { vv_font_regular,     20.0f },   /* VS_CAPTION */
   { vv_font_regular,     24.0f },   /* VS_BODY    */
   { vv_font_semibold,    24.0f },   /* VS_LABEL   */
   { vv_font_display_md,  30.0f },   /* VS_SUB     */
   { vv_font_display_sb,  40.0f },   /* VS_TITLE   */
   { vv_font_display_sb,  54.0f },   /* VS_HERO    */
};

static vs_face g_vf[VS_FACES];

static int face_bake(int i)
{
   vs_face *f = &g_vf[i];
   unsigned char *px;
   int pen_x = 0, pen_y = 0, row_h = 0, c, asc, desc, gap, used_h;
   const unsigned char *ttf = FACE_SRC[i].ttf;

   f->tried = 1;
   if (!stbtt_InitFont(&f->font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)))
      return -1;
   f->scale = stbtt_ScaleForMappingEmToPixels(&f->font, FACE_SRC[i].px);
   stbtt_GetFontVMetrics(&f->font, &asc, &desc, &gap);
   f->ascent = (int)ceilf((float)asc * f->scale);
   f->line_h = (int)ceilf((float)(asc - desc) * f->scale);
   px = (unsigned char *)calloc(VS_ATLAS_W, VS_ATLAS_H);
   if (!px)
      return -1;
   for (c = VS_FIRST; c <= VS_LAST; c++)
   {
      vs_glyph *g = &f->g[c - VS_FIRST];
      int adv, lsb, x0, y0, x1, y1, w, h;
      stbtt_GetCodepointHMetrics(&f->font, c, &adv, &lsb);
      g->adv = (float)adv * f->scale;
      stbtt_GetCodepointBitmapBox(&f->font, c, f->scale, f->scale,
                                  &x0, &y0, &x1, &y1);
      w = x1 - x0; h = y1 - y0;
      if (w <= 0 || h <= 0 || w > 255 || h > 255)
         continue;
      if (pen_x + w + 1 > VS_ATLAS_W)
      {
         pen_x = 0;
         pen_y += row_h + 1;
         row_h = 0;
      }
      if (pen_y + h > VS_ATLAS_H)
         break;
      stbtt_MakeCodepointBitmap(&f->font, px + (size_t)pen_y * VS_ATLAS_W +
                                pen_x, w, h, VS_ATLAS_W, f->scale, f->scale, c);
      g->ax = (unsigned short)pen_x; g->ay = (unsigned short)pen_y;
      g->w = (unsigned char)w; g->h = (unsigned char)h;
      g->ox = (signed char)x0;
      g->oy = (signed char)(f->ascent + y0);
      pen_x += w + 1;
      if (h > row_h)
         row_h = h;
   }
   used_h = (pen_y + row_h + 8) & ~7;
   if (used_h > VS_ATLAS_H)
      used_h = VS_ATLAS_H;
   f->tex = vb_tex_create(VS_ATLAS_W, used_h, VB_FMT_A8, 0);
   if (f->tex)
      vb_tex_update(f->tex, 0, 0, VS_ATLAS_W, used_h, px, VS_ATLAS_W);
   free(px);
   f->ok = f->tex != NULL;
   return f->ok ? 0 : -1;
}

static vs_face *face(int i)
{
   if (i < 0 || i >= VS_FACES)
      i = VS_BODY;
   if (!g_vf[i].tried)
      face_bake(i);
   return g_vf[i].ok ? &g_vf[i] : NULL;
}

static float kern(vs_face *f, int a, int b)
{
   return (float)stbtt_GetCodepointKernAdvance(&f->font, a, b) * f->scale;
}

static int glyph_index(unsigned char c)
{
   if (c < VS_FIRST || c > VS_LAST)
      c = '?';
   return c - VS_FIRST;
}

float vs_text_w(int fi, const char *s)
{
   vs_face *f = face(fi);
   float w = 0.0f;
   const unsigned char *p = (const unsigned char *)s;
   if (!f || !s)
      return 0.0f;
   for (; *p; p++)
   {
      w += f->g[glyph_index(*p)].adv;
      if (p[1])
         w += kern(f, *p, p[1]);
   }
   return w;
}

int vs_line_h(int fi)
{
   vs_face *f = face(fi);
   return f ? f->line_h : 0;
}

int vs_ascent(int fi)
{
   vs_face *f = face(fi);
   return f ? f->ascent : 0;
}

#define VS_BATCH 96

float vs_text(int fi, float x, float y, const char *s, uint32_t rgba)
{
   vs_face *f = face(fi);
   vb_tquad q[VS_BATCH];
   const unsigned char *p = (const unsigned char *)s;
   float pen = x;
   int n = 0, iy = (int)lroundf(y);

   if (!f || !s)
      return x;
   vv_frame_need();
   for (; *p; p++)
   {
      const vs_glyph *g = &f->g[glyph_index(*p)];
      if (g->w)
      {
         float gx = floorf(pen + 0.5f) + (float)g->ox;
         q[n].x0 = gx;
         q[n].y0 = (float)(iy + g->oy);
         q[n].x1 = gx + (float)g->w;
         q[n].y1 = q[n].y0 + (float)g->h;
         q[n].u0 = (float)g->ax;
         q[n].v0 = (float)g->ay;
         q[n].u1 = (float)(g->ax + g->w);
         q[n].v1 = (float)(g->ay + g->h);
         if (++n == VS_BATCH)
         {
            vb_tquads(f->tex, q, n, rgba, VB_NEAREST);
            n = 0;
         }
      }
      pen += g->adv;
      if (p[1])
         pen += kern(f, *p, p[1]);
   }
   if (n)
      vb_tquads(f->tex, q, n, rgba, VB_NEAREST);
   return pen;
}

void vs_text_c(int fi, float cx, float y, const char *s, uint32_t rgba)
{
   vs_text(fi, floorf(cx - vs_text_w(fi, s) * 0.5f + 0.5f), y, s, rgba);
}

void vs_text_r(int fi, float xr, float y, const char *s, uint32_t rgba)
{
   vs_text(fi, floorf(xr - vs_text_w(fi, s) + 0.5f), y, s, rgba);
}

void vs_text_fit(int fi, char *s, float max_w)
{
   size_t n;
   if (!s || vs_text_w(fi, s) <= max_w)
      return;
   n = strlen(s);
   while (n > 0)
   {
      char keep;
      n--;
      while (n > 0 && s[n - 1] == ' ')
         n--;
      keep = s[n];
      s[n] = '\0';
      if (vs_text_w(fi, s) + vs_text_w(fi, "...") <= max_w)
      {
         strcat(s, "...");
         return;
      }
      s[n] = keep;
   }
   s[0] = '\0';
}

int vs_text_wrap(int fi, float x, float y, float max_w, int max_lines,
                 float line_gap, const char *s, uint32_t rgba)
{
   char line[160];
   int lines = 0;
   const char *p = s;
   float lh = (float)vs_line_h(fi) + line_gap;
   while (p && *p && lines < max_lines)
   {
      size_t take = 0, best = 0, len = strlen(p);
      while (take < len && take < sizeof(line) - 4)
      {
         size_t next = take;
         while (next < len && p[next] != ' ')
            next++;
         memcpy(line, p, next);
         line[next] = '\0';
         if (vs_text_w(fi, line) > max_w && best > 0)
            break;
         best = next;
         take = next;
         while (take < len && p[take] == ' ')
            take++;
         if (next >= len)
            break;
      }
      if (best == 0)
         best = len < sizeof(line) - 4 ? len : sizeof(line) - 4;
      memcpy(line, p, best);
      line[best] = '\0';
      if (lines == max_lines - 1 && best < len)
      {
         /* the last line takes the rest, cut to fit */
         size_t rest = len < sizeof(line) - 4 ? len : sizeof(line) - 4;
         memcpy(line, p, rest);
         line[rest] = '\0';
         vs_text_fit(fi, line, max_w);
      }
      vs_text(fi, x, y + (float)lines * lh, line, rgba);
      lines++;
      p += best;
      while (*p == ' ')
         p++;
   }
   return lines;
}

/* ---- shapes ----------------------------------------------------------- */

static float snap(float v) { return floorf(v + 0.5f); }

void vs_rect(float x, float y, float w, float h, uint32_t rgba)
{
   if (w <= 0 || h <= 0)
      return;
   vv_frame_need();
   x = snap(x); y = snap(y); w = snap(w); h = snap(h);
   vb_quad(x, y, x + w, y + h, rgba, rgba, rgba, rgba);
}

void vs_grad(float x, float y, float w, float h, uint32_t tl, uint32_t tr,
             uint32_t bl, uint32_t br)
{
   if (w <= 0 || h <= 0)
      return;
   vv_frame_need();
   x = snap(x); y = snap(y); w = snap(w); h = snap(h);
   vb_quad(x, y, x + w, y + h, tl, tr, bl, br);
}

void vs_grad_v(float x, float y, float w, float h, uint32_t top,
               uint32_t bottom)
{
   vs_grad(x, y, w, h, top, top, bottom, bottom);
}

void vs_grad_h(float x, float y, float w, float h, uint32_t left,
               uint32_t right)
{
   vs_grad(x, y, w, h, left, right, left, right);
}

/* A disc's coverage, (2R)^2 texels: a corner is one quadrant of it.  Kind
 * 0 = filled, 1 = a ring `t` thick, 2 = a soft disc (shadow) whose edge
 * falls off over `t` px each side of R.  Made once per (kind, R, t). */
typedef struct
{
   int kind, r, t;
   int size;                  /* texture edge, px                          */
   vb_tex *tex;
} vs_disc;

#define VS_DISCS 64
static vs_disc g_disc[VS_DISCS];
static int g_disc_next;

static vs_disc *disc_get(int kind, int r, int t)
{
   int i, size, x, y;
   unsigned char *px;
   vs_disc *d;
   if (r < 1)
      r = 1;
   if (r > 120)
      r = 120;
   for (i = 0; i < VS_DISCS; i++)
      if (g_disc[i].tex && g_disc[i].kind == kind && g_disc[i].r == r &&
          g_disc[i].t == t)
         return &g_disc[i];
   d = &g_disc[g_disc_next];
   g_disc_next = (g_disc_next + 1) % VS_DISCS;
   if (d->tex)
      vb_tex_destroy(d->tex);
   memset(d, 0, sizeof(*d));
   size = kind == 2 ? 2 * (r + t) : 2 * r;
   px = (unsigned char *)malloc((size_t)size * size);
   if (!px)
      return NULL;
   for (y = 0; y < size; y++)
      for (x = 0; x < size; x++)
      {
         float c = (float)size * 0.5f, cov = 0.0f;
         if (kind == 2)
         {
            float dx = (float)x + 0.5f - c, dy = (float)y + 0.5f - c;
            float dist = sqrtf(dx * dx + dy * dy);
            /* linear, as the straight edges' gradients are */
            cov = 1.0f - ((dist - (float)(r - t)) / (float)(2 * t));
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
         }
         else
         {
            int sx, sy, hits = 0;
            for (sy = 0; sy < 4; sy++)
               for (sx = 0; sx < 4; sx++)
               {
                  float dx = (float)x + ((float)sx + 0.5f) / 4.0f - c;
                  float dy = (float)y + ((float)sy + 0.5f) / 4.0f - c;
                  float dist = sqrtf(dx * dx + dy * dy);
                  if (dist <= (float)r &&
                      (kind == 0 || dist >= (float)(r - t)))
                     hits++;
               }
            cov = (float)hits / 16.0f;
         }
         px[(size_t)y * size + x] = (unsigned char)lroundf(cov * 255.0f);
      }
   d->tex = vb_tex_create(size, size, VB_FMT_A8, 0);
   if (d->tex)
      vb_tex_update(d->tex, 0, 0, size, size, px, (size_t)size);
   free(px);
   if (!d->tex)
      return NULL;
   d->kind = kind; d->r = r; d->t = t; d->size = size;
   return d;
}

/* The four corners of a w x h rect at x, y, each c x c, from disc d. */
static void corners(vs_disc *d, float x, float y, float w, float h, float c,
                    uint32_t rgba)
{
   vb_tquad q[4];
   float s = (float)d->size, m = s * 0.5f;
   int i;
   for (i = 0; i < 4; i++)
   {
      int right = i & 1, bottom = i >> 1;
      q[i].x0 = right ? x + w - c : x;
      q[i].y0 = bottom ? y + h - c : y;
      q[i].x1 = q[i].x0 + c;
      q[i].y1 = q[i].y0 + c;
      q[i].u0 = right ? m : 0.0f;
      q[i].u1 = right ? s : m;
      q[i].v0 = bottom ? m : 0.0f;
      q[i].v1 = bottom ? s : m;
   }
   vb_tquads(d->tex, q, 4, rgba, VB_NEAREST);
}

void vs_rrect(float x, float y, float w, float h, float r, uint32_t rgba)
{
   vs_disc *d;
   int ri;
   if (w <= 0 || h <= 0)
      return;
   x = snap(x); y = snap(y); w = snap(w); h = snap(h);
   if (r > w * 0.5f) r = w * 0.5f;
   if (r > h * 0.5f) r = h * 0.5f;
   ri = (int)floorf(r);
   if (ri < 1)
   {
      vs_rect(x, y, w, h, rgba);
      return;
   }
   d = disc_get(0, ri, 0);
   vv_frame_need();
   if (!d)
   {
      vb_quad(x, y, x + w, y + h, rgba, rgba, rgba, rgba);
      return;
   }
   corners(d, x, y, w, h, (float)ri, rgba);
   /* the middle column, full height; the two side columns between the
    * corners */
   if (w > 2 * ri)
      vb_quad(x + ri, y, x + w - ri, y + h, rgba, rgba, rgba, rgba);
   if (h > 2 * ri)
   {
      vb_quad(x, y + ri, x + ri, y + h - ri, rgba, rgba, rgba, rgba);
      vb_quad(x + w - ri, y + ri, x + w, y + h - ri, rgba, rgba, rgba, rgba);
   }
}

static uint32_t lerp_rgba(uint32_t a, uint32_t b, float t)
{
   uint32_t out = 0;
   int k;
   for (k = 0; k < 32; k += 8)
   {
      float ca = (float)((a >> k) & 0xFF), cb = (float)((b >> k) & 0xFF);
      out |= (uint32_t)lroundf(ca + (cb - ca) * t) << k;
   }
   return out;
}

void vs_rrect_grad_v(float x, float y, float w, float h, float r,
                     uint32_t top, uint32_t bottom)
{
   vs_disc *d;
   int ri;
   uint32_t c_r, c_hr, c_top_corner, c_bot_corner;
   vb_tquad q[2];
   float s, m;
   if (w <= 0 || h <= 0)
      return;
   x = snap(x); y = snap(y); w = snap(w); h = snap(h);
   if (r > w * 0.5f) r = w * 0.5f;
   if (r > h * 0.5f) r = h * 0.5f;
   ri = (int)floorf(r);
   if (ri < 1 || !(d = disc_get(0, ri, 0)))
   {
      vs_grad_v(x, y, w, h, top, bottom);
      return;
   }
   vv_frame_need();
   c_r = lerp_rgba(top, bottom, (float)ri / h);
   c_hr = lerp_rgba(top, bottom, (h - (float)ri) / h);
   c_top_corner = lerp_rgba(top, bottom, (float)ri * 0.5f / h);
   c_bot_corner = lerp_rgba(top, bottom, (h - (float)ri * 0.5f) / h);
   s = (float)d->size; m = s * 0.5f;
   /* the corners, two at a time, each pair in its own colour */
   q[0].x0 = x; q[0].y0 = y; q[0].x1 = x + ri; q[0].y1 = y + ri;
   q[0].u0 = 0; q[0].v0 = 0; q[0].u1 = m; q[0].v1 = m;
   q[1].x0 = x + w - ri; q[1].y0 = y; q[1].x1 = x + w; q[1].y1 = y + ri;
   q[1].u0 = m; q[1].v0 = 0; q[1].u1 = s; q[1].v1 = m;
   vb_tquads(d->tex, q, 2, c_top_corner, VB_NEAREST);
   q[0].y0 = y + h - ri; q[0].y1 = y + h; q[0].v0 = m; q[0].v1 = s;
   q[1].y0 = y + h - ri; q[1].y1 = y + h; q[1].v0 = m; q[1].v1 = s;
   vb_tquads(d->tex, q, 2, c_bot_corner, VB_NEAREST);
   if (w > 2 * ri)
      vb_quad(x + ri, y, x + w - ri, y + h, top, top, bottom, bottom);
   if (h > 2 * ri)
   {
      vb_quad(x, y + ri, x + ri, y + h - ri, c_r, c_r, c_hr, c_hr);
      vb_quad(x + w - ri, y + ri, x + w, y + h - ri, c_r, c_r, c_hr, c_hr);
   }
}

void vs_rrect_line(float x, float y, float w, float h, float r, float t,
                   uint32_t rgba)
{
   vs_disc *d;
   int ri, ti;
   if (w <= 0 || h <= 0 || t <= 0)
      return;
   x = snap(x); y = snap(y); w = snap(w); h = snap(h);
   if (r > w * 0.5f) r = w * 0.5f;
   if (r > h * 0.5f) r = h * 0.5f;
   ri = (int)floorf(r);
   ti = (int)lroundf(t);
   if (ti < 1)
      ti = 1;
   vv_frame_need();
   if (ri < ti)
   {
      /* square corners: four bars */
      vb_quad(x, y, x + w, y + ti, rgba, rgba, rgba, rgba);
      vb_quad(x, y + h - ti, x + w, y + h, rgba, rgba, rgba, rgba);
      vb_quad(x, y + ti, x + ti, y + h - ti, rgba, rgba, rgba, rgba);
      vb_quad(x + w - ti, y + ti, x + w, y + h - ti, rgba, rgba, rgba, rgba);
      return;
   }
   d = disc_get(1, ri, ti);
   if (!d)
      return;
   corners(d, x, y, w, h, (float)ri, rgba);
   if (w > 2 * ri)
   {
      vb_quad(x + ri, y, x + w - ri, y + ti, rgba, rgba, rgba, rgba);
      vb_quad(x + ri, y + h - ti, x + w - ri, y + h, rgba, rgba, rgba, rgba);
   }
   if (h > 2 * ri)
   {
      vb_quad(x, y + ri, x + ti, y + h - ri, rgba, rgba, rgba, rgba);
      vb_quad(x + w - ti, y + ri, x + w, y + h - ri, rgba, rgba, rgba, rgba);
   }
}

void vs_shadow(float x, float y, float w, float h, float r, float spread,
               uint32_t rgba)
{
   vs_disc *d;
   int ri, si;
   float c, ox, oy, ow, oh;
   uint32_t clear = rgba & 0x00FFFFFFu;
   if (w <= 0 || h <= 0)
      return;
   x = snap(x); y = snap(y); w = snap(w); h = snap(h);
   if (r > w * 0.5f) r = w * 0.5f;
   if (r > h * 0.5f) r = h * 0.5f;
   ri = (int)floorf(r);
   si = (int)lroundf(spread);
   if (si < 1)
      si = 1;
   if (ri < si)
      ri = si;                 /* the falloff needs a radius at least as
                                * wide as itself */
   d = disc_get(2, ri, si);
   if (!d)
      return;
   vv_frame_need();
   /* The soft shape spans the rect grown by `spread` on every side; its
    * corners are (ri + si) squares of the soft disc. */
   c = (float)(ri + si);
   ox = x - si; oy = y - si; ow = w + 2 * si; oh = h + 2 * si;
   if (ow < 2 * c) ow = 2 * c;
   if (oh < 2 * c) oh = 2 * c;
   corners(d, ox, oy, ow, oh, c, rgba);
   /* Edges: the falloff as a gradient across 2*si, with the solid band
    * inside it.  The profile across an edge is the soft disc's radial one,
    * so a straight edge and a corner meet without a seam. */
   if (ow > 2 * c)
   {
      float a0 = ox + c, a1 = ox + ow - c;
      /* top: from y-si (clear) to y+si (solid), then solid to the corner */
      vb_quad(a0, oy, a1, oy + 2 * si, clear, clear, rgba, rgba);
      vb_quad(a0, oy + 2 * si, a1, oy + c, rgba, rgba, rgba, rgba);
      vb_quad(a0, oy + oh - c, a1, oy + oh - 2 * si, rgba, rgba, rgba, rgba);
      vb_quad(a0, oy + oh - 2 * si, a1, oy + oh, rgba, rgba, clear, clear);
   }
   if (oh > 2 * c)
   {
      float b0 = oy + c, b1 = oy + oh - c;
      vb_quad(ox, b0, ox + 2 * si, b1, clear, rgba, clear, rgba);
      vb_quad(ox + 2 * si, b0, ox + c, b1, rgba, rgba, rgba, rgba);
      vb_quad(ox + ow - c, b0, ox + ow - 2 * si, b1, rgba, rgba, rgba, rgba);
      vb_quad(ox + ow - 2 * si, b0, ox + ow, b1, rgba, clear, rgba, clear);
   }
   if (ow > 2 * c && oh > 2 * c)
      vb_quad(ox + c, oy + c, ox + ow - c, oy + oh - c, rgba, rgba, rgba,
              rgba);
}

void vs_circle(float cx, float cy, float r, uint32_t rgba)
{
   vs_rrect(cx - r, cy - r, 2 * r, 2 * r, r, rgba);
}

void vs_ring(float cx, float cy, float r, float t, uint32_t rgba)
{
   vs_rrect_line(cx - r, cy - r, 2 * r, 2 * r, r, t, rgba);
}

/* ---- icons and the wordmark ------------------------------------------ */

static vb_tex *g_icon_tex[2];          /* small, large atlas */
static vb_tex *g_mark_tex;

static vb_tex *atlas(int big)
{
   if (!g_icon_tex[big])
   {
      int cell = big ? VSI_BIG : VSI_SMALL;
      int w = cell * VSI_COLS, h = cell * VSI_ROWS;
      g_icon_tex[big] = vb_tex_create(w, h, VB_FMT_A8, 0);
      if (g_icon_tex[big])
         vb_tex_update(g_icon_tex[big], 0, 0, w, h,
                       big ? vsi_big : vsi_small, (size_t)w);
   }
   return g_icon_tex[big];
}

void vs_icon(int id, float cx, float cy, float size, uint32_t rgba)
{
   int big = size > (float)VSI_SMALL * 1.2f, cell;
   vb_tex *t;
   vb_tquad q;
   if (id < 0 || id >= VS_ICONS || size <= 0)
      return;
   t = atlas(big);
   if (!t)
      return;
   cell = big ? VSI_BIG : VSI_SMALL;
   vv_frame_need();
   q.x0 = snap(cx - size * 0.5f);
   q.y0 = snap(cy - size * 0.5f);
   q.x1 = q.x0 + snap(size);
   q.y1 = q.y0 + snap(size);
   q.u0 = (float)((id % VSI_COLS) * cell);
   q.v0 = (float)((id / VSI_COLS) * cell);
   q.u1 = q.u0 + (float)cell;
   q.v1 = q.v0 + (float)cell;
   vb_tquads(t, &q, 1, rgba,
             (int)lroundf(size) == cell ? VB_NEAREST : VB_LINEAR);
}

float vs_wordmark_w(float h)
{
   return h * (float)VSI_MARK_W / (float)VSI_MARK_H;
}

float vs_wordmark(float x, float y, float h, uint32_t rgba)
{
   vb_tquad q;
   float w = vs_wordmark_w(h);
   if (!g_mark_tex)
   {
      g_mark_tex = vb_tex_create(VSI_MARK_W, VSI_MARK_H, VB_FMT_A8, 0);
      if (!g_mark_tex)
         return 0.0f;
      vb_tex_update(g_mark_tex, 0, 0, VSI_MARK_W, VSI_MARK_H, vsi_mark,
                    VSI_MARK_W);
   }
   vv_frame_need();
   q.x0 = snap(x); q.y0 = snap(y); q.x1 = q.x0 + snap(w); q.y1 = q.y0 + snap(h);
   q.u0 = 0; q.v0 = 0; q.u1 = VSI_MARK_W; q.v1 = VSI_MARK_H;
   vb_tquads(g_mark_tex, &q, 1, rgba,
             (int)lroundf(h) == VSI_MARK_H ? VB_NEAREST : VB_LINEAR);
   return w;
}

static vb_tex *g_markc_tex;

void vs_mark(float x, float y, float size, int alpha)
{
   vb_tquad q;
   if (!g_markc_tex)
   {
      g_markc_tex = vb_tex_create(VSI_MARKC, VSI_MARKC, VB_FMT_RGBA8, 0);
      if (!g_markc_tex)
         return;
      vb_tex_update(g_markc_tex, 0, 0, VSI_MARKC, VSI_MARKC, vsi_markc,
                    VSI_MARKC * 4);
   }
   vv_frame_need();
   q.x0 = snap(x); q.y0 = snap(y);
   q.x1 = q.x0 + snap(size); q.y1 = q.y0 + snap(size);
   q.u0 = 0; q.v0 = 0; q.u1 = VSI_MARKC; q.v1 = VSI_MARKC;
   vb_tquads(g_markc_tex, &q, 1, VB_RGBA(255, 255, 255, alpha),
             (int)lroundf(size) == VSI_MARKC ? VB_NEAREST : VB_LINEAR);
}

/* ---- pictures --------------------------------------------------------- */

struct vs_img
{
   vb_tex *tex;
   int w, h;
   int bpp;            /* 4 = RGBA8, 2 = RGB565 */
};

static vs_img *img_new(int w, int h, int fmt, int bpp)
{
   vs_img *im;
   if (w <= 0 || h <= 0)
      return NULL;
   im = (vs_img *)calloc(1, sizeof(*im));
   if (!im)
      return NULL;
   im->tex = vb_tex_create(w, h, fmt, 0);
   if (!im->tex)
   {
      free(im);
      return NULL;
   }
   im->w = w; im->h = h;
   im->bpp = bpp;
   return im;
}

vs_img *vs_img_new(int w, int h)
{
   return img_new(w, h, VB_FMT_RGBA8, 4);
}

vs_img *vs_img_new_565(int w, int h)
{
   return img_new(w, h, VB_FMT_RGB565, 2);
}

void vs_img_set(vs_img *im, const uint32_t *rgba, int stride_px)
{
   if (im && rgba && im->bpp == 4)
      vb_tex_update(im->tex, 0, 0, im->w, im->h, rgba,
                    (size_t)stride_px * 4);
}

void vs_img_set_565(vs_img *im, const uint16_t *pix, int stride_px)
{
   if (im && pix && im->bpp == 2)
      vb_tex_update(im->tex, 0, 0, im->w, im->h, pix,
                    (size_t)stride_px * 2);
}

void vs_img_free(vs_img *im)
{
   if (!im)
      return;
   vb_tex_destroy(im->tex);
   free(im);
}

int vs_img_w(const vs_img *im) { return im ? im->w : 0; }
int vs_img_h(const vs_img *im) { return im ? im->h : 0; }

void vs_img_draw_src(vs_img *im, float sx, float sy, float sw, float sh,
                     float x, float y, float w, float h, uint32_t tint,
                     int linear)
{
   vb_tquad q;
   if (!im || w <= 0 || h <= 0)
      return;
   vv_frame_need();
   q.x0 = snap(x); q.y0 = snap(y);
   q.x1 = q.x0 + snap(w); q.y1 = q.y0 + snap(h);
   q.u0 = sx; q.v0 = sy; q.u1 = sx + sw; q.v1 = sy + sh;
   vb_tquads(im->tex, &q, 1, tint, linear ? VB_LINEAR : VB_NEAREST);
}

void vs_img_draw(vs_img *im, float x, float y, float w, float h,
                 uint32_t tint, int linear)
{
   if (im)
      vs_img_draw_src(im, 0, 0, (float)im->w, (float)im->h, x, y, w, h, tint,
                      linear);
}

/* ---- clipping --------------------------------------------------------- */

void vs_clip(float x, float y, float w, float h)
{
   vv_frame_need();
   vb_scissor(1, (int)snap(x), (int)snap(y), (int)snap(w), (int)snap(h));
}

void vs_clip_off(void)
{
   vb_scissor(0, 0, 0, 0, 0);
}

void vs_term(void)
{
   int i;
   for (i = 0; i < VS_FACES; i++)
   {
      if (g_vf[i].tex)
         vb_tex_destroy(g_vf[i].tex);
      memset(&g_vf[i], 0, sizeof(g_vf[i]));
   }
   for (i = 0; i < VS_DISCS; i++)
   {
      if (g_disc[i].tex)
         vb_tex_destroy(g_disc[i].tex);
      memset(&g_disc[i], 0, sizeof(g_disc[i]));
   }
   for (i = 0; i < 2; i++)
   {
      if (g_icon_tex[i])
         vb_tex_destroy(g_icon_tex[i]);
      g_icon_tex[i] = NULL;
   }
   if (g_mark_tex)
      vb_tex_destroy(g_mark_tex);
   g_mark_tex = NULL;
   if (g_markc_tex)
      vb_tex_destroy(g_markc_tex);
   g_markc_tex = NULL;
}
