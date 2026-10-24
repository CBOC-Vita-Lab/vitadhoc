/* vstyle.h -- the Vita's own drawing: native pixels, rounded shapes, more
 * than two sizes of type, icons, RGBA pictures.
 *
 * psp/video_psp.h is the PSP's API -- 480x272 logical pixels, two faces,
 * square rects -- and every page of ui_psp.c is laid out in it.  The Vita's
 * own screens (the home screen, the in-game control centre, the restyled
 * page chrome) are drawn with this instead, on the same frame and through
 * the same rasteriser (vback.h), so both kinds of drawing mix freely:
 *
 *   - COORDINATES ARE NATIVE: 960 x 544, floats, origin top-left.  Shapes
 *     are snapped to whole pixels so their edges stay crisp.
 *   - COLOURS ARE 0xAABBGGRR (vback's), made with VS_HEX(0xRRGGBB, alpha).
 *   - TEXT is Inter at six sizes, natural advances with kerning (not the
 *     PSP's table: these screens are not laid out in PSP pixels).
 *   - ROUNDED SHAPES are nine pieces: four corners from a small coverage
 *     texture made once for that radius, the rest flat quads -- exact at
 *     any size, no shader.
 *   - ICONS come from an atlas baked from SVG (tools/bake_vstyle_icons.py ->
 *     vstyle_icons.h), drawn at any size in any colour.
 *
 * Everything is lazy: the first call that needs a face, a corner or the
 * icon atlas makes it.
 */
#ifndef VSTYLE_H
#define VSTYLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_W 960
#define VS_H 544

/* 0xRRGGBB and an alpha -> vback's 0xAABBGGRR. */
#define VS_HEX(rgb, a) \
   ((((uint32_t)(a) & 0xFFu) << 24) | (((uint32_t)(rgb) & 0xFFu) << 16) | \
    ((uint32_t)(rgb) & 0xFF00u) | (((uint32_t)(rgb) >> 16) & 0xFFu))
/* The same colour at another alpha. */
#define VS_ALPHA(c, a) (((c) & 0x00FFFFFFu) | (((uint32_t)(a) & 0xFFu) << 24))

/* ---- type -------------------------------------------------------------- */
enum
{
   VS_CAPTION = 0,   /* Inter Regular 20: small print, chips          */
   VS_BODY,          /* Inter Regular 24: rows, descriptions          */
   VS_LABEL,         /* Inter SemiBold 24: buttons, selected rows     */
   VS_SUB,           /* Inter SemiBold 30: a tile's title, headings   */
   VS_TITLE,         /* Inter SemiBold 40: page titles                */
   VS_HERO,          /* Inter SemiBold 54: the focused game's name    */
   VS_FACES
};

/* Draw `s` with its line box's top-left at x, y; returns the pen's x after
 * it.  Line height and ascent per face, for laying text out. */
float vs_text(int face, float x, float y, const char *s, uint32_t rgba);
float vs_text_w(int face, const char *s);
int   vs_line_h(int face);
int   vs_ascent(int face);
/* Text centred on cx / ending at xr. */
void  vs_text_c(int face, float cx, float y, const char *s, uint32_t rgba);
void  vs_text_r(int face, float xr, float y, const char *s, uint32_t rgba);
/* Cut `s` (in place) to fit max_w, ending in "..." when it was cut. */
void  vs_text_fit(int face, char *s, float max_w);
/* Words wrapped to max_w, at most max_lines; returns the lines drawn. */
int   vs_text_wrap(int face, float x, float y, float max_w, int max_lines,
                   float line_gap, const char *s, uint32_t rgba);

/* ---- shapes ----------------------------------------------------------- */
void vs_rect(float x, float y, float w, float h, uint32_t rgba);
/* Corner colours top-left, top-right, bottom-left, bottom-right. */
void vs_grad(float x, float y, float w, float h, uint32_t tl, uint32_t tr,
             uint32_t bl, uint32_t br);
void vs_grad_v(float x, float y, float w, float h, uint32_t top,
               uint32_t bottom);
void vs_grad_h(float x, float y, float w, float h, uint32_t left,
               uint32_t right);
void vs_rrect(float x, float y, float w, float h, float r, uint32_t rgba);
/* Filled, top colour to bottom colour. */
void vs_rrect_grad_v(float x, float y, float w, float h, float r,
                     uint32_t top, uint32_t bottom);
/* An outline `t` px thick, inside the rect. */
void vs_rrect_line(float x, float y, float w, float h, float r, float t,
                   uint32_t rgba);
/* A soft shadow or glow around the rect: `spread` px of falloff outside
 * (and as much inside, under the shape). */
void vs_shadow(float x, float y, float w, float h, float r, float spread,
               uint32_t rgba);
void vs_circle(float cx, float cy, float r, uint32_t rgba);
void vs_ring(float cx, float cy, float r, float t, uint32_t rgba);

/* ---- icons ------------------------------------------------------------ */
enum
{
   VS_I_PLAY = 0, VS_I_GEAR, VS_I_LINK, VS_I_SPARK, VS_I_LAYERS,
   VS_I_POWER, VS_I_GRID, VS_I_STAR, VS_I_STAR_O, VS_I_WIFI,
   VS_I_BACK, VS_I_MORE, VS_I_PAD, VS_I_SCREEN, VS_I_REWIND,
   VS_I_PEOPLE, VS_I_FOLDER, VS_I_CHECK, VS_I_SAVE, VS_I_FAST,
   VS_I_TRASH, VS_I_PLUS, VS_I_BTN_X, VS_I_BTN_O, VS_I_BTN_TRI,
   VS_I_BTN_SQ, VS_I_REFRESH, VS_I_CART, VS_I_HOME, VS_I_PAUSE,
   VS_I_CAMERA, VS_I_SIGNAL, VS_I_SEARCH, VS_I_CLOSE,
   VS_ICONS
};
void vs_icon(int id, float cx, float cy, float size, uint32_t rgba);
/* The wordmark, `h` px tall with its top-left at x, y; returns its width. */
float vs_wordmark(float x, float y, float h, uint32_t rgba);
float vs_wordmark_w(float h);
/* The mark (two handhelds, linked) in its own colours, size x size. */
void  vs_mark(float x, float y, float size, int alpha);

/* ---- pictures --------------------------------------------------------- */
typedef struct vs_img vs_img;
/* RGBA8 (bytes R,G,B,A); NULL when there is no memory. */
vs_img *vs_img_new(int w, int h);
void    vs_img_set(vs_img *im, const uint32_t *rgba, int stride_px);
int     vs_img_writable(const vs_img *im);
/* 16-bit, libretro RGB565 (R in the high bits): a game's own frames. */
vs_img *vs_img_new_565(int w, int h);
void    vs_img_set_565(vs_img *im, const uint16_t *pix, int stride_px);
void    vs_img_free(vs_img *im);
int     vs_img_w(const vs_img *im);
int     vs_img_h(const vs_img *im);
/* The whole picture, or a source rect of it (texels), onto x, y, w, h;
 * `tint` multiplies it (0xFFFFFFFF = as it is). */
void    vs_img_draw(vs_img *im, float x, float y, float w, float h,
                    uint32_t tint, int linear);
void    vs_img_draw_src(vs_img *im, float sx, float sy, float sw, float sh,
                        float x, float y, float w, float h, uint32_t tint,
                        int linear);

/* ---- clipping --------------------------------------------------------- */
void vs_clip(float x, float y, float w, float h);
void vs_clip_off(void);

/* Free what the module made (vid_term calls it). */
void vs_term(void);

#ifdef __cplusplus
}
#endif

#endif /* VSTYLE_H */
