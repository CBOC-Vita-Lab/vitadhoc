/* vglyph.h -- the UI's glyphs on demand: the characters outside printable
 * ASCII (accents, Greek, Cyrillic, kana, kanji, fullwidth forms, Hangul),
 * drawn from the outline fonts at the size of the face asking.
 *
 * playerkit's pk_glyphs does the work (cell cache, font files read on first
 * use); this file gives it a GPU atlas to live in (one A8 texture per face)
 * and the UTF-8 walk the two text layers (vstyle.c, video_vita_text.c) share.
 * Printable ASCII never comes here: both layers draw it from their baked atlases.
 */
#ifndef VGLYPH_H
#define VGLYPH_H

#include <stddef.h>
#include <stdint.h>

#include "vback.h"
#include "pk_glyphs.h"
#include "pk_utf8.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vglyph_set vglyph_set;

/* Where the font files are (plat_app_dir(); PK_FONT_DIR in the environment
 * when this was never called, for the host tools). */
void vglyph_dir(const char *dir);
/* One frame has been presented. */
void vglyph_tick(void);

/* A set for a face of `px` pixels whose baseline is `ascent` rows below the
 * line top.  `primary` is the TTF the face's ASCII comes from (Inter or
 * Outfit; NULL for none), `latin` the Inter of the same weight to try next
 * (NULL when primary is Inter already), `semibold` the face is heavier than
 * regular (the CJK glyphs get a synthetic extra weight).  Made on first use,
 * nothing is read or allocated before. */
vglyph_set *vglyph_new(const unsigned char *primary, const unsigned char *latin,
                       int px, int ascent, int semibold);
/* The glyph of a code point >= 0x80 (rasterised into the atlas when new),
 * and the atlas texture to draw it from; 0 when no font has it. */
int     vglyph_get(vglyph_set *s, uint32_t cp, pk_gglyph *g, vb_tex **tex);
/* Its advance, pixels (no rasterising); 0 when no font has it. */
int     vglyph_adv(vglyph_set *s, uint32_t cp, float *adv);
/* Gives the set's texture and cache back (NULL is fine). */
void    vglyph_free(vglyph_set *s);
/* Bytes the atlases of all sets hold (GPU), and the font files (RAM). */
size_t  vglyph_atlas_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
