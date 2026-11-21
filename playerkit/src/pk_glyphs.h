/*
 * Glyphs on demand: the characters the baked ASCII atlases do not hold (the
 * accented Latin letters, Greek, Cyrillic, Japanese kana and kanji, fullwidth
 * forms, Korean) are rasterised from outline fonts the first time a screen
 * needs them and kept in a small atlas of fixed cells, least recently used
 * out first.
 *
 *   - nothing costs anything until a character outside printable ASCII is
 *     measured: the big font file is read then, not at start-up;
 *   - the atlas of a face has a fixed size (a budget in bytes): there is no
 *     growth, a screen that needs more distinct glyphs than the cells hold
 *     shows '?' for the surplus instead of stalling;
 *   - a cell is only reused once it has not been drawn for several frames
 *     (the GPU still reads the last frames' pictures), so the atlas can be
 *     rewritten in place;
 *   - the module draws nothing and owns no texture: the owner makes the
 *     atlas texture of pk_gc_atlas_size() and receives each new glyph's
 *     pixels through the upload callback (one cell at a time).
 *
 * Portable C (host twin, the console's two players, the main app). stb_truetype
 * (public domain) does the rasterising; the font files are SIL OFL
 * (licenses/NotoSansCJK-OFL.txt, licenses/Inter-OFL.txt).
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_GLYPHS_H
#define PK_GLYPHS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the fonts ---------------------------------------------------------- */

typedef struct pk_gfont pk_gfont;

/* The directory the font files are read from ("app0:" on the console, the
 * assets directory on the twin; PK_FONT_DIR in the environment when none is set). Copied. Without one, only fonts given in
 * memory work and every other character shows as '?'. */
void pk_glyphs_set_dir(const char *dir);

/* The files of the package (data/ in the repository: gpsp/vita/assets). */
#define PK_FONT_FILE_CJK     "cjkfont.otf"        /* Japanese kana + kanji, fullwidth, Hangul, Latin/Greek/Cyrillic */
#define PK_FONT_FILE_LATIN   "latinfont.ttf"      /* Inter Regular, Latin-1 / Extended / punctuation (the players) */
#define PK_FONT_FILE_LATIN_B "latinfont-sb.ttf"   /* Inter SemiBold of the same */

/* A font file read on first use (NULL name: none). The same handle comes back
 * for the same name. */
pk_gfont *pk_gfont_file(const char *name);
/* A font already in memory (an embedded TTF); the data must outlive it. */
pk_gfont *pk_gfont_mem(const unsigned char *ttf);
/* 1 once the font is loaded and usable (tries to load it). */
int pk_gfont_ready(pk_gfont *f);
/* 1 when the font has a glyph for the code point (loads the font). */
int pk_gfont_has(pk_gfont *f, uint32_t cp);
/* The bytes the loaded files occupy now (for the notes and the tests). */
size_t pk_glyphs_font_bytes(void);

/* ---- a cache of one face ------------------------------------------------ */

typedef struct pk_gcache pk_gcache;

typedef struct
{
   int px;                  /* the em size in pixels, as the ASCII face's  */
   int ascent;              /* line top to baseline in pixels              */
   int embolden;            /* 0, or the pixels x 100 the outline is smeared
                             * to the right: the synthetic semibold given to
                             * glyphs of the CJK file (one weight only)    */
   const pk_gfont *chain[3];/* tried in order; NULL ends the list          */
   int budget_bytes;        /* the atlas's size, about                     */
   int min_slots;           /* at least this many cells whatever the budget */
} pk_gc_spec;

typedef struct
{
   uint16_t x, y;           /* the cell's place in the atlas               */
   uint8_t  w, h;           /* the bitmap's size (0 = nothing to draw)     */
   int8_t   ox, oy;         /* from the pen / from the line top            */
   float    adv;            /* the advance, pixels                         */
} pk_gglyph;

/* Called once for every glyph rasterised: copy the w x h block (rows
 * `pitch` bytes apart) to x,y of the owner's atlas texture. */
typedef void (*pk_gc_upload_fn)(void *user, int x, int y, int w, int h,
                                const uint8_t *pix, int pitch);

/* NULL when out of memory. The atlas is not touched until the first glyph. */
pk_gcache *pk_gc_new(const pk_gc_spec *spec, pk_gc_upload_fn up, void *user);
void       pk_gc_free(pk_gcache *c);
/* The atlas texture's size the owner has to make (A8, zero filled). */
void       pk_gc_atlas_size(const pk_gcache *c, int *w, int *h);
/* One frame has passed (for every cache): cells idle for a few frames may be
 * reused. Call once per presented frame while text is drawn. */
void       pk_gc_tick(void);
/* The advance of a character in pixels. 1 when some font has it, else 0
 * (the caller then uses '?'). Does not rasterise and does not use a cell. */
int        pk_gc_adv(pk_gcache *c, uint32_t cp, float *adv);
/* The glyph, rasterised and uploaded if it is not in the atlas; marks the
 * cell as used this frame. 1 = ok (w == 0: a character with nothing to draw);
 * 0 = no font has it, or no cell was free. */
int        pk_gc_get(pk_gcache *c, uint32_t cp, pk_gglyph *g);
/* Counters for the notes and the tests. */
void       pk_gc_stats(const pk_gcache *c, unsigned *slots, unsigned *used,
                       unsigned *raster, unsigned *evict, unsigned *miss);

/* 1 for a character that is drawn as nothing and takes no room (controls,
 * zero width joiners, variation selectors). */
int        pk_glyph_invisible(uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif
