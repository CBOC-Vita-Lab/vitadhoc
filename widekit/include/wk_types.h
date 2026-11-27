/* widekit - MIT licence (see ../LICENSE).  All-new code; nothing copied from
 * any emulator core.  Portable C interface (C99 / C++11), no platform headers.
 *
 * wk_types.h: the neutral description a core fills every frame, and the
 * decisions wk_layers produces from it. */
#ifndef WK_TYPES_H
#define WK_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WK_MAX_LAYERS 4    /* background layers a core can describe          */
#define WK_MAX_LINES 256   /* scanlines of the view (SNES 239, NES 240)      */
#define WK_MAX_MARGIN 128  /* margin pixels per side, hard cap               */
#define WK_LEFT 0
#define WK_RIGHT 1

/* What the core / profile knows about a layer (WK_ROLE_AUTO = find out). */
enum { WK_ROLE_AUTO = 0, WK_ROLE_BACKDROP, WK_ROLE_LEVEL, WK_ROLE_HUD,
       WK_ROLE_OFF };

/* What wk_layers concluded (wk_out.kind[]). */
enum { WK_KIND_UNKNOWN = 0, WK_KIND_BACKDROP, WK_KIND_LEVEL, WK_KIND_HUD,
       WK_KIND_OFF };

/* One background layer as the core sees it this frame.
 *
 * Coordinates are LAYER space: the layer is a torus of map_w x map_h tiles
 * (tile_px pixels each); (0,0) is the top-left of the tilemap ring.  The view
 * shows layer pixel columns scroll_x[y] .. scroll_x[y]+view_w-1 on scanline
 * y, at layer pixel row row_y[y].  Values may be any int32; wk takes them
 * modulo the ring where it needs to.
 *
 * Arrays hash/code/stamp are row-major, map_w * map_h entries, owned by the
 * core and only read during the wk_* call.  If the hardware mirrors
 * nametables, fold the mirroring first (map_w = 32 for horizontal mirroring
 * or single screen) so the ring size is what the picture really wraps at. */
/* wk_layer.code value of an entry the learned map must not record (what the ring holds there is not what the picture shows
 * at that world column: a column hidden under a left-edge mask that the game is already rewriting). Added for the NES adapter. */
#define WK_CODE_NOREC 0xFFFFFFFFu

typedef struct wk_layer {
    int active;           /* layer visible this frame (enabled, not forced
                             blank).  0 -> everything below ignored        */
    int role;             /* WK_ROLE_*                                      */
    int priority;         /* 0 = furthest back; used for compositing order  */
    int map_w, map_h;     /* tilemap ring size in tiles (wrap size)         */
    int tile_px;          /* 8 or 16 (any 4..32 works)                      */
    const uint32_t *hash; /* content hash per entry: tile index + attributes
                             + a hash of that tile's graphics.  REQUIRED    */
    const uint32_t *code; /* optional redraw token per entry (tile index +
                             attributes) stored in the learned map          */
    const uint32_t *stamp;/* optional: frame number the game last wrote the
                             entry.  Without it only history ("seen")
                             columns can be trusted, never the lead side    */
    uint32_t gfx_id;      /* identity of the tileset/CHR bank set this layer
                             draws from (cosmetic animation excluded)       */
    const uint8_t *line_on;/* optional per scanline: layer enabled on that
                             line (windows, IRQ splits).  NULL = all        */
    int32_t scroll_x[WK_MAX_LINES]; /* layer px column at view column 0     */
    int32_t row_y[WK_MAX_LINES];    /* layer px row shown on that scanline  */
    int map_by_cell;      /* opt-in (0 = off, the old behaviour): the learned map
                             ignores gfx_id; each cell is identified by its
                             content hash alone (hash[] must cover the tile's
                             graphics) and the adapter re-checks it when serving */
} wk_layer;

/* One frame of input.  Only the first nlayers entries are read. */
typedef struct wk_frame {
    uint32_t frame;       /* monotonically increasing frame number          */
    int view_w, view_h;   /* the original picture (256 x 224/240)           */
    int margin;           /* wanted px per side, <= WK_MAX_MARGIN           */
    int brightness;       /* 0..255 master brightness, 0 = blank/forced blank*/
    int palette_delta;    /* palette entries that changed since last frame  */
    int palette_size;     /* palette entries in total (SNES 256, NES 32)    */
    int nlayers;
    wk_layer layer[WK_MAX_LAYERS];
} wk_frame;

/* Per scanline decision.  Index 0 = left side, 1 = right side; margin pixel
 * counts are measured from the view edge outwards. */
typedef struct wk_line_out {
    uint16_t real[2][WK_MAX_LAYERS]; /* margin px of the layer that are REAL:
                                        ring-certified, learned or profile
                                        (backdrop layers: the full margin)  */
    uint16_t ring[2][WK_MAX_LAYERS]; /* the part of real[] backed by the ring;
                                        [ring, real) comes from the map     */
    uint16_t level_real[2];          /* min of real[] over level layers (the
                                        margin where ALL level layers are
                                        real); equals margin if none        */
    uint8_t hud;           /* 1: fixed line (status bar): do not widen it   */
    uint8_t backdrop_mask; /* bit l: layer l continued as backdrop here     */
    uint8_t level_mask;    /* bit l: layer l is a level layer here          */
    uint8_t group;         /* line group index modulo 256                   */
} wk_line_out;

typedef struct wk_out {
    int nlines;
    int ngroups;
    int margin;                  /* margin used (frame.margin clipped)     */
    uint16_t cap[2];             /* margin cap per side (profile limits);
                                    wk_out_raise never exceeds it          */
    int scene_event;             /* 1 on a frame that started/continued a
                                    scene change (jump, blank, tileset swap)*/
    uint32_t epoch;              /* increments at every scene change       */
    int main_layer;              /* layer taken as the camera, or -1       */
    int kind[WK_MAX_LAYERS];     /* WK_KIND_*                              */
    int conf[WK_MAX_LAYERS];     /* 0..255 confidence of kind              */
    int dir[WK_MAX_LAYERS];      /* last horizontal direction: -1, 0, +1   */
    int hud_lines;               /* number of fixed (hud) lines            */
    /* world coordinates, valid where coherent[l][y] != 0 (unwrapped within
     * the epoch): pixel column of view column 0 and pixel row of the line */
    int32_t world_x[WK_MAX_LAYERS][WK_MAX_LINES];
    int32_t world_row[WK_MAX_LAYERS][WK_MAX_LINES];
    uint8_t coherent[WK_MAX_LAYERS][WK_MAX_LINES];
    wk_line_out line[WK_MAX_LINES];
    /* diagnostics */
    int hs_checked, hs_missed;   /* hindsight: predictions checked / wrong  */
    int streaming[WK_MAX_LAYERS];/* layer saw writes beyond its edges now   */
} wk_out;

#ifdef __cplusplus
}
#endif
#endif
