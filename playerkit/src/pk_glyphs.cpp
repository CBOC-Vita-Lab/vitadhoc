/* Glyphs on demand: see pk_glyphs.h (C-style C++: the header's API is extern "C"). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The main app already builds stb_truetype's implementation (stb_impl_vita.c; it
 * defines PK_GLYPHS_EXTERNAL_STB and puts the header on the include path);
 * everywhere else this file carries its own copy, private to it. */
#ifdef PK_GLYPHS_EXTERNAL_STB
extern "C" {
#include "stb_truetype.h"
}
#else
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../../snes/frontend/third_party/stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

/* ---- fonts -------------------------------------------------------------- */

struct pk_gfont
{
   const unsigned char *data;
   unsigned char       *owned;     /* the file's bytes, when read from a file */
   size_t               size;
   char                 name[40];  /* "" for a font in memory                 */
   int                  state;     /* 0 not tried, 1 ready, 2 unusable        */
   int                  cjk;       /* the CJK file: regular weight only       */
   stbtt_fontinfo       info;
};

#define MAX_FONTS 12
static pk_gfont g_fonts[MAX_FONTS];
static int      g_nfonts;
#ifdef __vita__
static char     g_dir[256] = "app0:";        /* the package's own files */
#else
static char     g_dir[256];
#endif
static size_t   g_font_bytes;

void pk_glyphs_set_dir(const char *dir)
{
   size_t n;
   g_dir[0] = '\0';
   if (!dir)
      return;
   n = strlen(dir);
   if (n >= sizeof(g_dir) - 1)
      return;
   memcpy(g_dir, dir, n + 1);
}

static int      g_region;                    /* PK_REGION_*                             */
static unsigned g_region_gen = 1;            /* bumped when it changes: caches follow   */

void pk_glyphs_set_region(int region)
{
   region = region == PK_REGION_SC ? PK_REGION_SC : PK_REGION_JP;
   if (region != g_region)
   {
      g_region = region;
      g_region_gen++;
   }
}

int pk_glyphs_region(void)
{
   return g_region;
}

pk_gfont *pk_gfont_file(const char *name)
{
   int i;
   if (!name || !*name || strlen(name) >= sizeof(g_fonts[0].name))
      return NULL;
   for (i = 0; i < g_nfonts; i++)
      if (!strcmp(g_fonts[i].name, name))
         return &g_fonts[i];
   if (g_nfonts == MAX_FONTS)
      return NULL;
   memset(&g_fonts[g_nfonts], 0, sizeof(g_fonts[0]));
   strcpy(g_fonts[g_nfonts].name, name);
   g_fonts[g_nfonts].cjk = !strcmp(name, PK_FONT_FILE_CJK) || !strcmp(name, PK_FONT_FILE_CJK_SC);
   return &g_fonts[g_nfonts++];
}

pk_gfont *pk_gfont_mem(const unsigned char *ttf)
{
   int i;
   if (!ttf)
      return NULL;
   for (i = 0; i < g_nfonts; i++)
      if (g_fonts[i].data == ttf && !g_fonts[i].name[0])
         return &g_fonts[i];
   if (g_nfonts == MAX_FONTS)
      return NULL;
   memset(&g_fonts[g_nfonts], 0, sizeof(g_fonts[0]));
   g_fonts[g_nfonts].data = ttf;
   return &g_fonts[g_nfonts++];
}

static int font_load(pk_gfont *f)
{
   if (f->state)
      return f->state == 1;
   f->state = 2;
   if (!f->data)
   {
      char path[320];
      FILE *fp;
      long n;
      size_t got;
      if (!g_dir[0])
         pk_glyphs_set_dir(getenv("PK_FONT_DIR"));     /* the host twins: no package directory */
      if (!f->name[0] || !g_dir[0])
         return 0;
      snprintf(path, sizeof(path), "%s%s%s", g_dir,
               (g_dir[strlen(g_dir) - 1] == '/' || g_dir[strlen(g_dir) - 1] == ':') ? "" : "/",
               f->name);
      fp = fopen(path, "rb");
      if (!fp)
         return 0;
      if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) <= 0 || n > 16 * 1024 * 1024 ||
          fseek(fp, 0, SEEK_SET) != 0)
      {
         fclose(fp);
         return 0;
      }
      f->owned = (unsigned char *)malloc((size_t)n);
      if (!f->owned)
      {
         fclose(fp);
         return 0;
      }
      got = fread(f->owned, 1, (size_t)n, fp);
      fclose(fp);
      if (got != (size_t)n)
      {
         free(f->owned);
         f->owned = NULL;
         return 0;
      }
      f->data = f->owned;
      f->size = (size_t)n;
      g_font_bytes += (size_t)n;
   }
   if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0)))
   {
      if (f->owned)
      {
         g_font_bytes -= f->size;
         free(f->owned);
         f->owned = NULL;
      }
      f->data = NULL;
      return 0;
   }
   f->state = 1;
   return 1;
}

int pk_gfont_ready(pk_gfont *f)
{
   return f && font_load(f);
}

int pk_gfont_has(pk_gfont *f, uint32_t cp)
{
   return f && font_load(f) && stbtt_FindGlyphIndex(&f->info, (int)cp) != 0;
}

size_t pk_glyphs_font_bytes(void)
{
   return g_font_bytes;
}

int pk_glyph_invisible(uint32_t cp)
{
   return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD || cp == 0x34F ||
          (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
          (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
          cp == 0xFEFF || (cp >= 0xE0100 && cp <= 0xE01EF);
}

/* ---- the cache ---------------------------------------------------------- */

#define HASH_BITS 11
#define HASH_SIZE (1 << HASH_BITS)
#define IDLE_FRAMES 4              /* a cell is reused after this many frames unused */
#define ADV_Q 32.0f                /* advances kept in 1/32 pixel            */

typedef struct
{
   uint32_t cp;
   int16_t  slot;                  /* -1: no cell (advance only / no font)    */
   int16_t  advq;                  /* -1: no font has the character           */
   uint8_t  used;
} hentry;

typedef struct
{
   uint32_t cp;                    /* 0: free                                 */
   uint32_t last;
   uint8_t  w, h;
   int8_t   ox, oy;
   int16_t  advq;
   uint8_t  stale;                 /* made under another region: not found, reused when idle */
} cslot;

struct pk_gcache
{
   pk_gc_spec spec;
   pk_gc_upload_fn up;
   void *user;
   int cw, ch, cols, rows, nslots, aw, ah;
   cslot *slot;
   hentry tab[HASH_SIZE];
   int nent;
   unsigned char *cell, *cell2;    /* scratch: one cell, and the bold pass    */
   unsigned st_raster, st_evict, st_miss;
   unsigned nused;
   unsigned gen;                   /* g_region_gen the cells were made under   */
};

static uint32_t g_frame = 8;

void pk_gc_tick(void)
{
   g_frame++;
}

pk_gcache *pk_gc_new(const pk_gc_spec *spec, pk_gc_upload_fn up, void *user)
{
   pk_gcache *c;
   int slots, i;
   if (!spec || spec->px < 6 || spec->px > 120 || !up)
      return NULL;
   c = (pk_gcache *)calloc(1, sizeof(*c));
   if (!c)
      return NULL;
   c->spec = *spec;
   c->gen = g_region_gen;
   c->up = up;
   c->user = user;
   c->cw = c->ch = spec->px + 4;           /* a glyph's box plus overshoot */
   slots = spec->budget_bytes / (c->cw * c->ch);
   if (slots < spec->min_slots)
      slots = spec->min_slots;
   if (slots < 16)
      slots = 16;
   if (slots > 1000)
      slots = 1000;
   c->aw = 512;
   c->cols = c->aw / c->cw;
   c->rows = (slots + c->cols - 1) / c->cols;
   c->nslots = c->rows * c->cols;
   c->ah = (c->rows * c->ch + 7) & ~7;
   c->slot = (cslot *)calloc((size_t)c->nslots, sizeof(cslot));
   c->cell = (unsigned char *)calloc((size_t)c->cw * c->ch, 1);
   c->cell2 = (unsigned char *)calloc((size_t)c->cw * c->ch, 1);
   if (!c->slot || !c->cell || !c->cell2)
   {
      pk_gc_free(c);
      return NULL;
   }
   for (i = 0; i < HASH_SIZE; i++)
      c->tab[i].used = 0;
   return c;
}

void pk_gc_free(pk_gcache *c)
{
   if (!c)
      return;
   free(c->slot);
   free(c->cell);
   free(c->cell2);
   free(c);
}

void pk_gc_atlas_size(const pk_gcache *c, int *w, int *h)
{
   if (w)
      *w = c ? c->aw : 0;
   if (h)
      *h = c ? c->ah : 0;
}

void pk_gc_stats(const pk_gcache *c, unsigned *slots, unsigned *used,
                 unsigned *raster, unsigned *evict, unsigned *miss)
{
   if (slots) *slots = c ? (unsigned)c->nslots : 0;
   if (used) *used = c ? c->nused : 0;
   if (raster) *raster = c ? c->st_raster : 0;
   if (evict) *evict = c ? c->st_evict : 0;
   if (miss) *miss = c ? c->st_miss : 0;
}

static unsigned hash_of(uint32_t cp)
{
   return (unsigned)((cp * 2654435761u) >> (32 - HASH_BITS));
}

static hentry *find(pk_gcache *c, uint32_t cp)
{
   unsigned h = hash_of(cp);
   int n;
   for (n = 0; n < HASH_SIZE; n++, h = (h + 1) & (HASH_SIZE - 1))
   {
      if (!c->tab[h].used)
         return NULL;
      if (c->tab[h].cp == cp)
         return &c->tab[h];
   }
   return NULL;
}

static hentry *insert(pk_gcache *c, uint32_t cp);

/* The table is a cache of advances too: when it is nearly full it is rebuilt
 * from the cells' own records (an advance is recomputed when asked again). */
static void rebuild(pk_gcache *c)
{
   int i;
   memset(c->tab, 0, sizeof(c->tab));
   c->nent = 0;
   for (i = 0; i < c->nslots; i++)
      if (c->slot[i].cp && !c->slot[i].stale)
      {
         hentry *e = insert(c, c->slot[i].cp);
         e->slot = (int16_t)i;
         e->advq = c->slot[i].advq;
      }
}

static hentry *insert(pk_gcache *c, uint32_t cp)
{
   unsigned h;
   if (c->nent > HASH_SIZE * 6 / 10)
      rebuild(c);
   h = hash_of(cp);
   while (c->tab[h].used)
      h = (h + 1) & (HASH_SIZE - 1);
   c->tab[h].used = 1;
   c->tab[h].cp = cp;
   c->tab[h].slot = -1;
   c->tab[h].advq = -1;
   c->nent++;
   return &c->tab[h];
}

/* The font that has the character: the chain in order, then (cjk_fallback)
 * the CJK files of the region. */
static pk_gfont *pick(pk_gcache *c, uint32_t cp, int *gi, float *scale)
{
   pk_gfont *list[7];
   int i, n = 0;
   if (cp >= 0x110000)
      return NULL;
   for (i = 0; i < 4 && c->spec.chain[i]; i++)
      list[n++] = (pk_gfont *)c->spec.chain[i];
   if (c->spec.cjk_fallback)
   {
      if (g_region == PK_REGION_SC)
         list[n++] = pk_gfont_file(PK_FONT_FILE_CJK_SC);
      list[n++] = pk_gfont_file(PK_FONT_FILE_CJK);
   }
   for (i = 0; i < n; i++)
   {
      pk_gfont *f = list[i];
      int g;
      if (!f || !font_load(f))
         continue;
      g = stbtt_FindGlyphIndex(&f->info, (int)cp);
      if (g)
      {
         *gi = g;
         *scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)c->spec.px);
         return f;
      }
   }
   return NULL;
}

/* The region changed since the cells were made: nothing in the cache is found
 * any more.  A cell may still be read by a frame in flight, so each one stays
 * as it is (stale) and is only reused after the idle frames, like any other. */
static void follow_region(pk_gcache *c)
{
   int i;
   if (c->gen == g_region_gen)
      return;
   c->gen = g_region_gen;
   if (!c->spec.cjk_fallback)
      return;
   memset(c->tab, 0, sizeof(c->tab));
   c->nent = 0;
   for (i = 0; i < c->nslots; i++)
      if (c->slot[i].cp)
         c->slot[i].stale = 1;
   c->nused = 0;
}

static int advq_of(pk_gfont *f, int gi, float scale)
{
   int adv, lsb;
   float a;
   stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
   a = (float)adv * scale * ADV_Q + 0.5f;
   if (a > 32000.0f)
      a = 32000.0f;
   return (int)a;
}

int pk_gc_adv(pk_gcache *c, uint32_t cp, float *adv)
{
   hentry *e;
   pk_gfont *f;
   int gi;
   float sc;
   if (!c)
      return 0;
   follow_region(c);
   if (pk_glyph_invisible(cp))
   {
      *adv = 0.0f;
      return 1;
   }
   e = find(c, cp);
   if (!e)
   {
      e = insert(c, cp);
      f = pick(c, cp, &gi, &sc);
      e->advq = f ? (int16_t)advq_of(f, gi, sc) : (int16_t)-1;
   }
   if (e->advq < 0)
      return 0;
   *adv = (float)e->advq / ADV_Q;
   return 1;
}

/* Rasterise into c->cell (cw x ch, zero filled first): the glyph's bitmap
 * placed at the cell's top-left; returns its size and offsets. */
static int raster(pk_gcache *c, pk_gfont *f, int gi, float sc, int *w, int *h,
                  int *ox, int *oy)
{
   int x0, y0, x1, y1;
   memset(c->cell, 0, (size_t)c->cw * c->ch);
   stbtt_GetGlyphBitmapBoxSubpixel(&f->info, gi, sc, sc, 0.0f, 0.0f, &x0, &y0, &x1, &y1);
   if (f->cjk && c->spec.embolden > 0)
   {
      /* synthetic semibold: the outline drawn again a fraction of a pixel to
       * the right, the two coverages merged by max */
      float e = (float)c->spec.embolden * 0.01f;
      int bx0, by0, bx1, by1, ux0, ux1, W, H, x, y;
      if (e < 0.35f)
         e = 0.35f;
      if (e > 1.2f)
         e = 1.2f;
      stbtt_GetGlyphBitmapBoxSubpixel(&f->info, gi, sc, sc, e, 0.0f, &bx0, &by0, &bx1, &by1);
      ux0 = x0 < bx0 ? x0 : bx0;
      ux1 = x1 > bx1 ? x1 : bx1;
      W = ux1 - ux0;
      H = y1 - y0;
      if (W <= 0 || H <= 0)
      {
         *w = *h = 0;
         return 1;
      }
      if (W > c->cw) W = c->cw;
      if (H > c->ch) H = c->ch;
      if (x1 - x0 > 0 && y1 - y0 > 0)
      {
         int w0 = x1 - x0 < c->cw - (x0 - ux0) ? x1 - x0 : c->cw - (x0 - ux0);
         int h0 = H;
         if (w0 > 0)
            stbtt_MakeGlyphBitmapSubpixel(&f->info, c->cell + (x0 - ux0), w0, h0, c->cw,
                                          sc, sc, 0.0f, 0.0f, gi);
      }
      if (bx1 - bx0 > 0 && by1 - by0 > 0)
      {
         int w1 = bx1 - bx0 < c->cw - (bx0 - ux0) ? bx1 - bx0 : c->cw - (bx0 - ux0);
         int h1 = H;
         memset(c->cell2, 0, (size_t)c->cw * c->ch);
         if (w1 > 0)
            stbtt_MakeGlyphBitmapSubpixel(&f->info, c->cell2 + (bx0 - ux0), w1, h1, c->cw,
                                          sc, sc, e, 0.0f, gi);
         for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
            {
               unsigned char *d = &c->cell[(size_t)y * c->cw + x];
               unsigned char s = c->cell2[(size_t)y * c->cw + x];
               if (s > *d)
                  *d = s;
            }
      }
      *w = W; *h = H; *ox = ux0; *oy = y0;
      return 1;
   }
   {
      int W = x1 - x0, H = y1 - y0;
      if (W <= 0 || H <= 0)
      {
         *w = *h = 0;
         return 1;
      }
      if (W > c->cw) W = c->cw;
      if (H > c->ch) H = c->ch;
      stbtt_MakeGlyphBitmapSubpixel(&f->info, c->cell, W, H, c->cw, sc, sc, 0.0f, 0.0f, gi);
      *w = W; *h = H; *ox = x0; *oy = y0;
   }
   return 1;
}

static int take_slot(pk_gcache *c)
{
   int i, best = -1;
   uint32_t best_last = 0;
   for (i = 0; i < c->nslots; i++)
      if (!c->slot[i].cp)
         return i;
   for (i = 0; i < c->nslots; i++)
   {
      uint32_t last = c->slot[i].last;
      if (g_frame - last < IDLE_FRAMES)
         continue;
      if (best < 0 || last < best_last)
      {
         best = i;
         best_last = last;
      }
   }
   if (best >= 0)
   {
      if (!c->slot[best].stale)
      {
         hentry *e = find(c, c->slot[best].cp);
         if (e)
            e->slot = -1;
         c->nused--;
      }
      c->slot[best].cp = 0;
      c->slot[best].stale = 0;
      c->st_evict++;
   }
   return best;
}

int pk_gc_get(pk_gcache *c, uint32_t cp, pk_gglyph *g)
{
   hentry *e;
   cslot *s;
   pk_gfont *f;
   int gi, w, h, ox, oy, si;
   float sc;
   if (!c || !g)
      return 0;
   follow_region(c);
   memset(g, 0, sizeof(*g));
   if (pk_glyph_invisible(cp))
      return 1;
   e = find(c, cp);
   if (e && e->advq < 0)
      return 0;                    /* no font has it */
   if (e && e->slot >= 0)
   {
      s = &c->slot[e->slot];
      s->last = g_frame;
      g->x = (uint16_t)((e->slot % c->cols) * c->cw);
      g->y = (uint16_t)((e->slot / c->cols) * c->ch);
      g->w = s->w; g->h = s->h; g->ox = s->ox; g->oy = s->oy;
      g->adv = (float)s->advq / ADV_Q;
      return 1;
   }
   f = pick(c, cp, &gi, &sc);
   if (!f)
   {
      if (!e)
         e = insert(c, cp);
      e->advq = -1;
      return 0;
   }
   si = take_slot(c);
   if (si < 0)
   {
      c->st_miss++;
      return 0;
   }
   raster(c, f, gi, sc, &w, &h, &ox, &oy);
   c->st_raster++;
   c->nused++;
   e = find(c, cp);                /* take_slot may have rebuilt nothing, but the table moves on insert */
   if (!e)
      e = insert(c, cp);
   s = &c->slot[si];
   s->cp = cp;
   s->stale = 0;
   s->last = g_frame;
   s->w = (uint8_t)w; s->h = (uint8_t)h;
   s->ox = (int8_t)ox;
   {
      int top = c->spec.ascent + oy;
      if (top < -128) top = -128;
      if (top > 127) top = 127;
      s->oy = (int8_t)top;
   }
   s->advq = (int16_t)advq_of(f, gi, sc);
   e->slot = (int16_t)si;
   e->advq = s->advq;
   {
      int cx = (si % c->cols) * c->cw, cy = (si / c->cols) * c->ch;
      c->up(c->user, cx, cy, c->cw, c->ch, c->cell, c->cw);
      g->x = (uint16_t)cx; g->y = (uint16_t)cy;
   }
   g->w = s->w; g->h = s->h; g->ox = s->ox; g->oy = s->oy;
   g->adv = (float)s->advq / ADV_Q;
   return 1;
}
