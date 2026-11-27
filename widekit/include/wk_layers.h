/* widekit - MIT licence.  wk_layers: which margin pixels of which layer are
 * real.
 *
 * Per frame the core fills a wk_frame; wk_layers_update() returns, for every
 * scanline and side, how many margin pixels of every layer are
 *   - REAL by ring provenance: columns the camera already showed (history,
 *     verified by content), or lead columns the game wrote after the one at
 *     the view edge (strictly newer write stamps, monotone, same scene);
 *   - BACKDROP-continued: layers that are designed to wrap (periodic across
 *     the ring, slow parallax, or quiet while the camera travelled a full
 *     wrap), which are real over the whole margin;
 * and which scanlines are fixed HUD lines that must not be widened.
 * Everything else is "not real": the compositor fades it out.
 *
 * Not thread safe per handle.  No allocation after wk_layers_create(). */
#ifndef WK_LAYERS_H
#define WK_LAYERS_H

#include "wk_types.h"
#include "wk_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wk_layers_cfg {
    int jump_px;           /* |scroll step| above this is a scene change (64)   */
    int quiet_px;          /* camera travel with no writes before a layer may be
                              called backdrop (96)                              */
    int periodic_frames;   /* a row must stay periodic this long (30)           */
    int par_px;            /* own travel for the parallax rule (48)             */
    int par_frames;        /* frames the parallax ratio must be stable (24)     */
    int hud_still;         /* camera-moving frames a line must stay put (24)    */
    int hud_main_min_q8;   /* camera speed (px/frame * 256) that counts as
                              moving for HUD evidence (96)                      */
    int stamp_tol;         /* frames of slack between write stamps (2)          */
    int resident_px;       /* travel with no writes after which equal stamps
                              are accepted (a level resident in the ring).
                              0 = never (default): at the edges of a finite
                              resident level the ring wraps to the wrong end */
    int cluster_gap;       /* scene events closer than this are one change (8)  */
    int blank_thr;         /* brightness at or below this is a blank frame (2)  */
    int max_cells;         /* largest map_w*map_h supported per layer (4096)    */
    int chain_ban_frames;  /* lead trust is switched off this long after a
                              hindsight miss (300)                              */
} wk_layers_cfg;

typedef struct wk_layers wk_layers;

void wk_layers_cfg_default(wk_layers_cfg *cfg);
wk_layers *wk_layers_create(const wk_layers_cfg *cfg); /* NULL on OOM */
void wk_layers_destroy(wk_layers *w);
void wk_layers_reset(wk_layers *w);                    /* forget everything   */
/* Profile roles / limits / hud lines override the frame's hints.  The profile
 * must stay valid until replaced.  NULL clears. */
void wk_layers_set_profile(wk_layers *w, const wk_profile *p);
void wk_layers_update(wk_layers *w, const wk_frame *f, wk_out *out);

/* Helpers for adapters / the map that add real pixels from another source. */
void wk_out_raise(wk_out *out, int layer, int y, int side, int px);
void wk_out_refresh_line(wk_out *out, int y); /* recompute level_real       */

#ifdef __cplusplus
}
#endif
#endif
