# widekit integration guide

widekit makes SNES and NES games fill a 960x544 screen without stretching or
cropping: the original picture stays pixel exact in the centre and the side
margins are filled with real picture where that is provable, continued
backdrop where it is safe, and a fade or soft edge otherwise. MIT licensed,
all-new code, C++11 with a C API, no platform or emulator headers.

## What the library is and is not

widekit decides, per scanline, per side and per layer, how many margin pixels
are REAL (`wk_out.line[y].real[side][layer]`). It never draws tiles itself:
the core adapter renders candidate margin strips (the tiles the layer would
show beyond the view edge) and `wk_compose` blends them. Anything not proven
real is not drawn from the layer.

Rules the library enforces (and tests check against a synthetic game):

1. Real picture where provable (the ring slot was displayed earlier in this
   scene at this world column with unchanged content; or the learned map has it
   for the current room and tileset).
2. Backdrop layers (wrapping or far parallax layers) get the full margin.
3. Level layers only where trusted: seen history, learned map, a verified
   profile level map, or a lead the engine demonstrably wrote for this scroll
   direction (write stamps).
4. Unknown foreground fades out over the continued backdrop (about 20 px).
5. Soft edge only when nothing exists to continue.
6. HUD lines keep original width, centred.
7. Never a wrong tile: a region shrinks at once, grows only after it has been
   wanted for `appear_frames` frames, and a hindsight check bans lead guessing
   after any miss.

## Files

| file | job |
| --- | --- |
| `include/wk_types.h` | `wk_frame` (input), `wk_out` (decisions) |
| `include/wk_layers.h` | scene events, per-line coherence, HUD, backdrop and lead decisions |
| `include/wk_map.h` | learned map, rooms, persistence |
| `include/wk_compose.h` | margin compositor (hysteresis, fade, soft edge, NEON blend) |
| `include/wk_profile.h` | per-game data file parser and RAM level map lookup |
| `profiles/wide_profiles.ini` | profile table (ships empty; see below) |

Link `src/*.cpp` (five files). Build flags: `-std=c++11`; for the Vita add
`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard`. `make vita` compiles all of it
warning free with the Vita SDK compiler. Do not link anything else from here.

## Per-frame call order

```
fill wk_frame            (adapter)
wk_layers_update(L, &fr, &out)
wk_map_update(M, &fr, &out)      // records, binds rooms, verifies
wk_map_serve(M, &fr, &out)       // raises out.line[].real beyond the ring
render candidate strips          // adapter; use wk_line_geom + wk_map_margin_cell
wk_compose_frame(C, &in)         // writes the wide picture (+ optional provenance)
```

`wk_map` is optional (pass no map and skip both calls; the ring-only behaviour
is the S4 test). `wk_compose` can also be used without the map.

### Filling wk_frame

* `frame`: monotonically increasing 32 bit counter, always the same clock as
  the stamps. Do not wrap it in a running game; if you must, rebase every stamp
  and the counter together (the library compares stamps with subtraction
  tolerance, never absolute thresholds, but it does not understand a wrap).
* `brightness`: 0..255 master brightness (0 = forced blank). A blank frame is a
  scene-change trigger. `palette_delta` / `palette_size`: entries changed since
  last frame / total (SNES 256, NES 32); half the palette changing is a
  trigger.
* per layer: `map_w`/`map_h` is the ring in tiles, `tile_px`, `hash` (REQUIRED:
  tile index + attributes + a hash of that tile's graphics, so a tile whose
  graphics were swapped changes), `code` (redraw token the learned map stores:
  enough to redraw the tile), `stamp` (the frame each entry was last written;
  without it the lead side can never be trusted, history still works),
  `gfx_id` (identity of the tileset/CHR bank set, excluding cosmetic animation),
  `scroll_x[y]`, `row_y[y]` per scanline, optional `line_on[y]`.
* Hash granularity matters: a hash that ignores the graphics makes a swapped
  tileset look unchanged and the map would serve wrong tiles; the gfx gate in
  `wk_map_margin_cell` catches the case where `gfx_id` changed but the epoch
  did not (test S5b).

### Snes9x

* Layers: BG1..BG4 as `wk_layer`. Map ring = the tilemap size (32/64 x 32/64
  tiles) after mirroring is applied. Tile hash: tilemap word (index,
  palette, flips) mixed with a hash of the 8x8/16x16 tile's character data in
  VRAM. To keep it cheap, keep a 16 bit "VRAM version" per character block
  that the VRAM write handler bumps and fold that in; recompute only blocks
  that changed.
* Stamps: write the current frame number into a parallel `uint32` array in the
  VRAM write handler for the tilemap area (one store per word written, also for
  DMA: stamp the DMA'd range when the DMA runs). Mid-frame writes count.
* `scroll_x[y]`, `row_y[y]`: sample the BGnHOFS/VOFS at each scanline (the
  core already latches per line for HDMA effects). Mode 7: do not describe it
  (set `active = 0`); the core's Mode 7 layer is a single full-field layer that
  needs its own handling, widekit does not attempt it.
* `gfx_id`: hash of BG character base + the DMA'd graphics generation counter.
* `brightness`: INIDISP; blank = forced blank bit or brightness 0.
* Windows/colour math: pass `line_on[y]` for window cut-outs only if the window
  is a layer mask; otherwise leave NULL and rely on `hud` detection.
* Sprites: the adapter draws margin sprites itself; `profile->sprites_off` tells
  it not to.

### FCEUmm (NES)

* One tilemap layer (the PPU background) plus optional status-bar split lines.
  The ring is the NAMETABLE space after folding mirroring: for horizontal
  mirroring (vertical scrolling games) `map_w = 32`, for vertical mirroring
  (horizontal scrollers) `map_w = 64`, single screen 32, four screen 64x60.
  Fold it into one contiguous array per frame (cheap: 2 KB).
* Tile hash: nametable byte + attribute-derived palette + hash of the CHR tile
  at the current bank (CHR banking: version counter per bank swap). Mapper
  CHR-RAM games: version counter bumped by the CHR write handler.
* Stamps: nametable writes via `$2007` and mapper-controlled nametable RAM
  (MMC5 fill/ExRAM) get the frame number. Attribute table writes stamp the 2x2
  tile group they affect.
* Scroll per line: latch `PPU_hook` / the loopy scroll registers at the start
  of every scanline (mid-frame `$2005/$2006` writes and sprite-0 splits then
  show up as a per-line change and widekit finds the status bar by itself,
  `hud_lines=` in a profile forces it).
* Exclusions: MMC5 split-screen and ExGrafix mode, and games that rewrite
  the nametable every frame (full-screen redraws), should be skipped via
  `enable = off` in the profile.

## Profiles

`wide_profiles.ini` is a data file (see the header of `include/wk_profile.h`).
It ships EMPTY on purpose: entries must be verified on the real ROM.
Absent profile = full automatic behaviour. Lookup: crc32 first, title second.
Keys: `enable` (auto|off|force), `bg1..bg4` (auto|level|backdrop|hud|off),
`margin`, `margin_left`, `margin_right`, `lead_tiles`, `fade_px`, `sprites`,
`hud_lines`, `levelmap`.

Load once at game load, keep the `wk_profile_db` alive while the profile
pointer is in use, pass the profile to `wk_layers_set_profile`. Parse errors
are returned as text and count; show them in a log. A bad line never stops
the rest of the file.

### Level maps (`levelmap=`)

Only for games where the RAM level layout is known. The library gives you
`wk_levelmap_tile()` (what tile belongs at a world pixel). The ADAPTER is
responsible for the self-check: compare the predicted tiles of all visible
columns against the ring every frame, and use the level map for the margin
only while at least ~95% match; otherwise drop it for the rest of the scene.
widekit does not read RAM itself and does not do this check.

## Compositing

Allocate `wk_compose_create(cfg, view_w, view_h, margin)`; the output is
`(view_w + 2*margin) x view_h` RGB565 and is scaled to the screen by the GPU
with one uniform factor (no stretch). For 960x544 the margin per side is
`(960 * view_h / 544 - view_w) / 2`: about 70 px for SNES 256x224 and about 84
px for NES 256x240 (the limit is `WK_MAX_MARGIN` = 128). Candidate
strips: `H` rows x `M` px per layer and side, index d = distance from the view
edge; `opaque` marks transparent candidate pixels so lower layers show
through. Draw order is the order array (bottom to top). Pass `prov` if you
want the provenance image (debug builds and the test screenshots).

Candidate strips only matter up to `real[side][layer]` px; draw only that
many to save time (the test harness draws them all to check the claim).

### Periodic continuation (optional, `cfg.period`, on by default)

Where a level layer's own picture repeats EXACTLY, pixel for pixel and opaque,
over its last three periods (8 or 16 px) at the trusted edge, and the same holds
on every line below down to the bottom of the view (terrain standing on the
floor of the picture, for example a flat ground strip), the pattern is continued
to the margin edge instead of fading. This is a prediction, not a certified
tile: it is drawn with provenance `WK_PROV_PERIOD` / `WK_PROV_PFADE`, appears
only after `appear_frames` of eligibility, vanishes at once when not eligible,
and every predicted world column that later becomes real is compared with the
real pixels. One mismatch switches the feature off for that layer for
`period_ban_frames` (300, longer after repeated misses, reset on a scene change).
To use it supply `wk_compose_in.inner[layer][side]` (+ `inner_opaque`,
`inner_n` <= `WK_MAX_INNER` = 64): the layer's OWN visible pixels next to the
view edge (index 0 = the pixel just inside the edge), same row layout as the
candidate strips; leave `inner_n` = 0 to disable. A level that is periodic by
design (flat ground) gets a solid continuation; anything else fades as before.
A pit or wall that starts inside the predicted stretch is shown wrongly for the
few frames until hindsight sees it: this is the price of the feature and the
reason it is optional.

### Smear

`cfg.smear_px` (default 0, off) extends a level run with a flat colour tail.
It was built and judged: it reads as a murky smudge, the fade over the
continued backdrop looks better. Leave it off.

## Threading and memory

* Handles are not thread safe. If the compose runs on another thread, give it
  a copy of `wk_out` (about 7.6 KB) and the strips; the library has no
  globals.
* No allocation after create. Memory (host, 64 bit): `wk_layers` about 130 KB
  per 4 layers with max map 4096 entries, `wk_map` about 2 MB at the default
  65536 cells, `wk_compose` ~ view_h * margin * 8 bytes plus one row buffer. On
  32 bit ARM the footprint is smaller.
* Persistence: `wk_map_path(dir, game_crc, ...)`, `wk_map_load` at game load,
  `wk_map_save` on exit and every few minutes of play (it is atomic and
  verified, ~50 ms for a typical file). A file is at most `file_cap` bytes
  (256 KB default). Any error leaves the map empty; a damaged file can never
  produce a wrong tile because served cells are still checked against the
  visible picture before and while they are used.

## Known limits (be honest in the UI)

* On a streaming engine without map, profile or scout, the lead side is only
  about 8 to 24 px.
* The lead (chain) trust is given only on the side the camera moves towards;
  tile/page loaders also overwrite slots behind the camera (test S7).
* A streaming layer needs a ring wider than the view (>= 34 tiles at 8 px);
  the library cannot certify anything on a ring that overwrites visible tiles.
* Layers that the engine writes at the view edge are never classed as
  backdrop for the rest of the scene (a streaming "sky" is level content).
* World edits while the tile is off screen (destroyed blocks, doors) can serve
  the old tile until the camera sees the real one again; it is removed at
  once and the room revoked when the verification drops below 75%.
* The learned map assumes two scenes at the same position share the same
  scroll origin.
* The adapter must fold NES mirroring, supply 32 bit stamps and respect
  `profile.sprites_off`.
* Mode 7 and mosaic effects are not described and are not widened.
