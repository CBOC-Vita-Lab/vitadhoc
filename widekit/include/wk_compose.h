/* widekit - MIT licence.  wk_compose: the margin compositor (RGB565).
 *
 * Inputs per frame: the centre picture, per-layer CANDIDATE margin strips
 * (what the core drew from the ring / the learned map for that layer, at any
 * distance - wk_compose only uses the first real[] pixels of each) and the
 * wk_out decisions.  Output: the full wide picture.
 *
 *   base            flat backdrop colour per line when real backdrop layers
 *                   continue there, else a SOFT EDGE (edge colours extended,
 *                   vertically blurred, slightly darkened outwards)
 *   backdrop layers drawn over the base for their real extent
 *   level layers    drawn for their real extent; the outer fade_px pixels of
 *                   that extent dissolve into what lies below (alpha ramp
 *                   + ordered dither in RGB565), so unknown foreground reads
 *                   as more of the same scene; optionally a flat SMEAR of the
 *                   last real colours fades out for smear_px more pixels
 *                   (never claimed as real: it is a blur, not a tile)
 *   hysteresis      a region must be wanted for appear_frames in a row before
 *                   it grows (grow_px per frame); it shrinks at once (never a
 *                   wrong tile); a large shrink leaves a short ghost fade
 *   HUD lines       centre untouched, margin = soft edge only
 *
 * PERIODIC CONTINUATION (cfg.period, needs wk_compose_in.inner): where the last
 * three periods (8 or 16 px) of a level layer's own picture at a line, taken
 * from the visible columns next to the edge plus the real margin run, repeat
 * EXACTLY pixel for pixel and are opaque, and the same holds for every line
 * below it down to the bottom of the view (terrain standing on the screen
 * floor, e.g. a flat ground strip), the pattern is continued to the margin
 * edge.  It is a prediction, never a certified tile: it is marked
 * WK_PROV_PERIOD / WK_PROV_PFADE, appears only after being eligible for
 * appear_frames, vanishes at once when not eligible, and is checked in
 * hindsight: every predicted world column that later becomes real is compared
 * with the real pixels; one mismatch switches the feature off for that layer
 * for period_ban_frames.
 *
 * Strip layout: strip[layer][side] has `H` rows of `M` pixels, index d =
 * distance from the view edge (d = 0 touches the centre).  Not thread safe
 * per handle. */
#ifndef WK_COMPOSE_H
#define WK_COMPOSE_H

#include "wk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { WK_FADE_ALPHA = 0, WK_FADE_STIPPLE = 1 };

/* provenance byte written to wk_compose_in.prov: low nibble = what, high
 * nibble = layer index for the layer kinds */
enum { WK_PROV_CENTRE = 0, WK_PROV_BG = 1, WK_PROV_SOFT = 2, WK_PROV_BACKDROP = 3,
       WK_PROV_LEVEL = 4, WK_PROV_FADE = 5, WK_PROV_GHOST = 6, WK_PROV_SMEAR = 7,
       WK_PROV_PERIOD = 8,   /* provable-periodic continuation, full weight   */
       WK_PROV_PFADE = 9 };  /* ... partly faded                              */

/* Widest inner strip (wk_compose_in.inner_n) the periodic detector uses. */
#define WK_MAX_INNER 64

typedef struct wk_compose_cfg {
    int fade_px;        /* fade ramp length at the outer end of a real run (20) */
    int fade_mode;      /* WK_FADE_*                                           */
    int appear_frames;  /* frames a region must be stable before it grows (8)  */
    int grow_px;        /* growth per frame once it may appear (2)             */
    int ghost_frames;   /* length of the fade after a large shrink; 0 = off (3)*/
    int ghost_min_px;   /* shrink that starts a ghost (12)                     */
    int smear_px;       /* tail beyond a level layer's real run: its last real
                           colours, flat and fading out; 0 = none (0, off:
                           it reads as a smudge)                   */
    int smear_alpha;    /* opacity (0..32) the fade settles at where the smear
                           starts (14)                                        */
    int soft_blur;      /* vertical blur radius in lines for the soft edge (16): a wide
                           radius reads as an out-of-focus glow, a small one as
                           horizontal streaks of whatever touches the edge   */
    int soft_sample;    /* edge columns averaged for the soft edge (4)         */
    int soft_darken;    /* % the soft edge darkens towards the screen edge (30)*/
    uint16_t bg_default;/* flat backdrop colour when the caller gives none     */
    int period;         /* 1: continue provably periodic terrain beyond the
                           trusted edge (see below); needs inner strips      */
    int period_ban_frames; /* periodic continuation of a layer is switched off
                           this long after hindsight showed a miss (300)     */
} wk_compose_cfg;

typedef struct wk_compose_in {
    const wk_out *ana;
    const uint16_t *centre;      /* view_w x view_h                            */
    int centre_stride;           /* in pixels                                  */
    const uint16_t *bg;          /* per line flat backdrop colour, or NULL     */
    const uint16_t *cand[WK_MAX_LAYERS][2];  /* NULL = layer not provided      */
    const uint8_t *opaque[WK_MAX_LAYERS][2]; /* 0 = transparent; NULL = opaque */
    int norder;                  /* layers to draw, bottom to top              */
    int order[WK_MAX_LAYERS];
    uint16_t *out;               /* (view_w + 2M) x view_h                     */
    int out_stride;
    uint8_t *prov;               /* optional, same layout as out               */
    /* optional, for periodic continuation: the layer's OWN visible pixels next
     * to the view edge, `inner_n` (<= WK_MAX_INNER) per line, index e = 0 is the
     * pixel just inside the edge, growing inwards; same row layout as cand */
    const uint16_t *inner[WK_MAX_LAYERS][2];
    const uint8_t *inner_opaque[WK_MAX_LAYERS][2];
    int inner_n;
} wk_compose_in;

typedef struct wk_compose wk_compose;

void wk_compose_cfg_default(wk_compose_cfg *cfg);
wk_compose *wk_compose_create(const wk_compose_cfg *cfg, int view_w, int view_h, int margin);
void wk_compose_destroy(wk_compose *c);
void wk_compose_reset(wk_compose *c);   /* forget hysteresis (new game/session) */
void wk_compose_frame(wk_compose *c, const wk_compose_in *in);

/* The blend kernel, exposed for tests: for i < n,
 *   dst = (src * w + dst * (32 - w) + dither) >> 5   per 565 channel,
 * w in 0..32, dither in 0..31.  The _scalar version is the reference; the
 * plain one uses NEON when compiled with __ARM_NEON.  Results are identical. */
void wk_blend_row_scalar(uint16_t *dst, const uint16_t *src, const uint8_t *w,
                         const uint8_t *dither, int n);
void wk_blend_row(uint16_t *dst, const uint16_t *src, const uint8_t *w,
                  const uint8_t *dither, int n);
int wk_have_neon(void);

/* How often hindsight switched periodic continuation off (diagnostics). */
int wk_compose_period_bans(const wk_compose *c);

#ifdef __cplusplus
}
#endif
#endif
