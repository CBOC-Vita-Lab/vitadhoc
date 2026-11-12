/* widekit - MIT licence.  wk_profile: per-game data-driven profile.
 *
 * Text format (ini-like, '#' starts a comment, a line starting with ';' is one):
 *
 *   [crc32=1A2B3C4D title="EXAMPLE GAME"]      section header: crc32 and/or title
 *   enable      = auto | off | force            auto = trust the analysis
 *   bg1         = backdrop | level | hud | off | auto     (also layer1..layer4)
 *   margin      = 48                            max px per side
 *   margin_left = 24     margin_right = 48      per side overrides
 *   lead_tiles  = 2                             cap on lead-side tiles trusted
 *                                               from write stamps (default: none)
 *   fade_px     = 20                            fade ramp length
 *   hud_lines   = 0-31,208-223                  scanlines never widened
 *   sprites     = existing | off                (advice for the adapter)
 *   levelmap    = layer=0 addr=0x1000 w=64 h=16 cell=16 bytes=1 endian=little
 *                 stride=64 origin=0,0 meta=2x2 tiles=linear:0x40,4
 *   levelmap    = layer=0 addr=0x2000 w=32 h=8 cell=16 bytes=1 meta=2x2
 *                 tiles=table:0=0x10,0x11,0x12,0x13|1=0x20,0x21,0x22,0x23
 *
 * A line that does not parse is reported and skipped; a bad section header
 * skips its section; the rest of the file is still used.  Unknown games get
 * wk_profile_default(): everything automatic, no limits. */
#ifndef WK_PROFILE_H
#define WK_PROFILE_H

#include "wk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WK_LM_MAX 2            /* level maps per profile        */
#define WK_LM_TABLE 64         /* table entries per level map   */
#define WK_LM_META 16          /* on-screen tiles per cell      */
#define WK_HUD_RANGES 4

enum { WK_ENABLE_AUTO = 0, WK_ENABLE_OFF, WK_ENABLE_FORCE };

/* "Level map in RAM": the game keeps its level as a grid of cells (often
 * 16x16 metatiles) at a fixed address.  With this a core adapter can answer
 * "what tile is at world pixel (x,y)" for places the tilemap ring does not
 * hold yet, i.e. real data ahead of the camera.  The adapter must verify the
 * model against what is on screen before trusting it (see INTEGRATION.md). */
typedef struct wk_levelmap {
    int layer;               /* layer the cells draw into (0..3)            */
    uint32_t addr;           /* offset into the adapter's RAM window        */
    int w, h;                /* cells                                        */
    int cell_px;             /* pixels per cell (e.g. 16)                    */
    int bytes;               /* 1 or 2 bytes per cell                        */
    int big_endian;
    int stride;              /* bytes between rows (0 = w * bytes)           */
    int column_major;        /* 1: cells stored column by column             */
    int origin_x, origin_y;  /* world px of cell (0,0)                       */
    int meta_w, meta_h;      /* on-screen tiles per cell                     */
    int map_kind;            /* 0 = linear, 1 = table                        */
    uint32_t tile_base, tile_mul;      /* linear: tile = base + v*mul + i    */
    int table_n;                       /* table: entries                     */
    uint16_t table_key[WK_LM_TABLE];   /* cell value                         */
    uint32_t table_tile[WK_LM_TABLE][WK_LM_META];
} wk_levelmap;

typedef struct wk_profile {
    int has_crc;
    uint32_t crc;
    char title[48];
    int enable;                       /* WK_ENABLE_*                        */
    int role[WK_MAX_LAYERS];          /* WK_ROLE_*                          */
    int margin_left, margin_right;    /* -1 = no limit                      */
    int lead_tiles;                   /* -1 = no cap                        */
    int fade_px;                      /* -1 = default                       */
    int sprites_off;                  /* 1 = adapter should not draw margin
                                         sprites                            */
    int nhud;
    int hud_lo[WK_HUD_RANGES], hud_hi[WK_HUD_RANGES];
    int nlevelmaps;
    wk_levelmap levelmap[WK_LM_MAX];
} wk_profile;

typedef struct wk_profile_db wk_profile_db;

void wk_profile_default(wk_profile *p);

/* Parse a whole file held in memory.  Problems are appended to msgs (one
 * line each, truncated to msgcap) and counted in *nproblems; neither may be
 * NULL-checked by the caller: both are optional.  Never returns NULL for
 * well-formed memory (an empty/garbage file gives an empty database). */
wk_profile_db *wk_profile_parse(const char *text, size_t len, char *msgs,
                                size_t msgcap, int *nproblems);
void wk_profile_free(wk_profile_db *db);
int wk_profile_count(const wk_profile_db *db);
const wk_profile *wk_profile_at(const wk_profile_db *db, int i);

/* crc32 match first, then title match (case-insensitive); NULL if none. */
const wk_profile *wk_profile_find(const wk_profile_db *db, uint32_t crc,
                                  const char *title);

/* Look up the tile code a level map says belongs at WORLD pixel (wx, wy)
 * (the top-left pixel of an on-screen tile).  ram/len is the adapter's RAM
 * window.  Returns 1 and sets *tile, or 0 if out of the level, out of the
 * RAM window or not in the table. */
int wk_levelmap_tile(const wk_levelmap *lm, const uint8_t *ram, size_t len,
                     int32_t wx, int32_t wy, uint32_t *tile);

#ifdef __cplusplus
}
#endif
#endif
