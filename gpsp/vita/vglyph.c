/* vglyph.c -- see vglyph.h. */
#include <stdlib.h>
#include <string.h>

#include "vglyph.h"

#define VG_BUDGET (160 * 1024)     /* a face's atlas, bytes, at most about */

struct vglyph_set
{
   pk_gcache *gc;
   vb_tex    *tex;
   int        failed;
   int        px, ascent, semibold;
   const unsigned char *primary, *latin;
};

static size_t g_atlas_bytes;

void vglyph_dir(const char *dir)
{
   pk_glyphs_set_dir(dir);
}

void vglyph_tick(void)
{
   pk_gc_tick();
}

size_t vglyph_atlas_bytes(void)
{
   return g_atlas_bytes;
}

vglyph_set *vglyph_new(const unsigned char *primary, const unsigned char *latin,
                       int px, int ascent, int semibold)
{
   vglyph_set *s = (vglyph_set *)calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->px = px;
   s->ascent = ascent;
   s->semibold = semibold;
   s->primary = primary;
   s->latin = latin;
   return s;
}

static void vg_upload(void *user, int x, int y, int w, int h,
                      const uint8_t *pix, int pitch)
{
   vglyph_set *s = (vglyph_set *)user;
   if (s->tex)
      vb_tex_update(s->tex, x, y, w, h, pix, (size_t)pitch);
}

/* The cache and its texture, made when the first character needs them. */
static int vg_ready(vglyph_set *s)
{
   pk_gc_spec sp;
   int w, h;
   unsigned char *zero;
   if (s->gc)
      return 1;
   if (s->failed)
      return 0;
   s->failed = 1;
   memset(&sp, 0, sizeof(sp));
   sp.px = s->px;
   sp.ascent = s->ascent;
   /* the semibold faces: Inter SemiBold has them, the CJK file one weight
    * only, so its glyphs are smeared a little: about 1/40 em */
   sp.embolden = s->semibold ? (s->px * 100 + 20) / 40 : 0;
   sp.chain[0] = s->primary ? (const pk_gfont *)pk_gfont_mem(s->primary) : NULL;
   if (s->latin)
      sp.chain[sp.chain[0] ? 1 : 0] = (const pk_gfont *)pk_gfont_mem(s->latin);
   {
      int i = 0;
      while (i < 3 && sp.chain[i])
         i++;
      if (i < 3)
         sp.chain[i] = (const pk_gfont *)pk_gfont_file(PK_FONT_FILE_CJK);
   }
   sp.budget_bytes = VG_BUDGET;
   sp.min_slots = 48;
   s->gc = pk_gc_new(&sp, vg_upload, s);
   if (!s->gc)
      return 0;
   pk_gc_atlas_size(s->gc, &w, &h);
   s->tex = vb_tex_create(w, h, VB_FMT_A8, 0);
   if (!s->tex)
   {
      pk_gc_free(s->gc);
      s->gc = NULL;
      return 0;
   }
   zero = (unsigned char *)calloc((size_t)w, (size_t)h);
   if (zero)
   {
      vb_tex_update(s->tex, 0, 0, w, h, zero, (size_t)w);
      free(zero);
   }
   g_atlas_bytes += (size_t)w * (size_t)h;
   s->failed = 0;
   return 1;
}

void vglyph_free(vglyph_set *s)
{
   if (!s)
      return;
   if (s->tex)
   {
      int w = vb_tex_w(s->tex), h = vb_tex_h(s->tex);
      g_atlas_bytes -= (size_t)w * (size_t)h;
      vb_tex_destroy(s->tex);
   }
   pk_gc_free(s->gc);
   free(s);
}

int vglyph_get(vglyph_set *s, uint32_t cp, pk_gglyph *g, vb_tex **tex)
{
   if (!s || !vg_ready(s))
      return 0;
   if (!pk_gc_get(s->gc, cp, g))
      return 0;
   *tex = s->tex;
   return 1;
}

int vglyph_adv(vglyph_set *s, uint32_t cp, float *adv)
{
   if (!s || !vg_ready(s))
      return 0;
   return pk_gc_adv(s->gc, cp, adv);
}
