/* widekit - MIT licence.  wk_map: the learned map.
 *
 * As the camera moves, every tile the layer shows is recorded at its WORLD
 * tile coordinate (scroll deltas accumulated with wrap handling, supplied by
 * wk_layers in wk_out.world_x / world_row).  Cells are grouped into rooms; a
 * scene change (wk_out.epoch) starts a new room unless the picture matches a
 * known room at the same world position.  While a room is bound the map can
 * serve margin tiles the ring no longer (or not yet) holds, but only when
 *   - the tileset identity stored with the cell equals the current one,
 *   - the visible picture still matches the stored room (else revoked).
 * Rooms persist per game (ROM checksum) in a compact verified file.
 *
 * Fixed memory after wk_map_create(); lookups are one hash probe; nothing is
 * allocated per frame.  Not thread safe per handle. */
#ifndef WK_MAP_H
#define WK_MAP_H

#include "wk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wk_map_cfg {
    int max_cells;      /* cells held in memory (65536 -> ~2 MB)               */
    int max_rooms;      /* rooms held (64)                                     */
    int settle_frames;  /* quiet frames after a scene change before matching (2) */
    int rematch_tries;  /* match attempts before a new room is opened (4)      */
    int rematch_gap;    /* frames between attempts (4)                         */
    int match_pct;      /* % of existing cells that must equal the picture (85)*/
    int found_pct;      /* % of visible cells that must exist in the room (40) */
    int verify_pct;     /* bound room: % that must still match (90): sparse rooms differ by 10-20%  */
    int revoke_frames;  /* mismatching frames in a row that revoke a room (3)  */
    int min_conf;       /* cell confidence needed to serve it (1)              */
    int file_cap;       /* largest persisted file in bytes (262144)            */
    int min_room_cells; /* rooms smaller than this are not persisted (48)      */
} wk_map_cfg;

typedef struct wk_cell {
    uint32_t code;      /* the redraw token the core supplied                  */
    uint16_t h16;       /* folded content hash it was recorded with (wk_layer.hash)  */
    uint16_t gfx;       /* folded tileset identity it was recorded with        */
    uint8_t conf;       /* times seen unchanged                                */
    uint8_t scout;      /* 1: recorded by the look-ahead and not yet seen live (a guess that proved nothing so far) */
} wk_cell;

typedef struct wk_map_stats {
    int cells, rooms, cur_room, bound, fresh;
    int found, matched;          /* last frame's overlap with the room         */
    uint32_t binds, revokes, new_rooms;
    uint32_t served_tiles;       /* tiles raised by the last wk_map_serve()    */
    /* look-ahead (wk_map_record_scout): cells recorded, and of those later
     * seen live the same / different (hindsight) */
    uint32_t scout_cells, scout_confirmed, scout_contradicted;
} wk_map_stats;

/* Geometry of a line, shared by map and adapters: columns are unwrapped tile
 * columns in the epoch's world space.  part[] = margin pixels at that side
 * that already belong to a visible tile (always real); the first fully
 * outside tile is index 1. */
typedef struct wk_geom {
    int32_t tile_first;   /* unwrapped column of the first visible tile        */
    int nvis;             /* visible tile columns                              */
    int part[2];
    int tp;
} wk_geom;

typedef struct wk_map wk_map;

void wk_map_cfg_default(wk_map_cfg *cfg);
wk_map *wk_map_create(const wk_map_cfg *cfg, uint32_t game_crc);
void wk_map_destroy(wk_map *m);
void wk_map_clear(wk_map *m);

/* Call in this order each frame, after wk_layers_update() with the same
 * frame: observe (records, binds, verifies), then serve (raises real[]). */
void wk_map_update(wk_map *m, const wk_frame *f, const wk_out *out);
void wk_map_serve(wk_map *m, const wk_frame *f, wk_out *out);

/* Look-ahead: record what a second emulator instance running ahead of the
 * game (a scout) showed, described by f/out in the LIVE epoch's world
 * coordinates (out->epoch = the live epoch, no scene event).  Only cells the
 * map does not hold are added (the live picture always wins); they are served
 * like learned cells until the live picture shows one of them differently:
 * then every scouted cell not yet seen live stops being served (hindsight,
 * stats.scout_contradicted).  Scouted cells are not persisted until seen
 * live.  Call between wk_map_update and wk_map_serve of a live frame, or
 * after them; the map must be bound (the live frame recognised its room). */
void wk_map_record_scout(wk_map *m, const wk_frame *f, const wk_out *out);

void wk_line_geom(const wk_frame *f, const wk_out *out, int layer, int y,
                  wk_geom *g);

/* The learned tile at outside tile `index` (1 = first fully outside) of a
 * side of a line.  Returns 1 and fills *cell if it is held, in the current
 * room, current tileset and confident enough. */
int wk_map_margin_cell(const wk_map *m, const wk_frame *f, const wk_out *out,
                       int layer, int y, int side, int index, wk_cell *cell);

void wk_map_stats_get(const wk_map *m, wk_map_stats *s);

/* Persistence.  Files are keyed by the ROM checksum given to create(). */
void wk_map_path(const char *dir, uint32_t game_crc, char *out, size_t cap);
size_t wk_map_serialize_max(const wk_map *m);        /* buffer size needed   */
size_t wk_map_serialize(const wk_map *m, uint8_t *buf, size_t cap);
/* 0 = loaded.  Any error leaves the map EMPTY (never half loaded). */
int wk_map_deserialize(wk_map *m, const uint8_t *buf, size_t len);
/* Atomic and verified: temp file, size + CRC read back, then rename.
 * Returns 0 or a negative step number.  The previous file is untouched
 * unless every step succeeded. */
int wk_map_save(const wk_map *m, const char *path);
/* A missing, truncated, corrupt, wrong-version or wrong-game file starts
 * empty and returns negative; 0 when loaded. */
int wk_map_load(wk_map *m, const char *path);

#ifdef __cplusplus
}
#endif
#endif
