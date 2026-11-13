# Wide view (SNES): design, threading, integration

The wide view shows the 4:3 SNES picture with real picture of the same game beside it, so that a 16:9 screen is filled
without stretching or cropping. The 256 columns in the middle are the normal picture, pixel for pixel; the margins are real
picture where the game's own data proves it, and soft edges where not. Emulation, audio and saved states do not change
(proof: `snes/twin/wide_compare.py`).

## Parts

| Part | Where | Licence |
|---|---|---|
| Capture + margin rendering | `snes/core/gfx.cpp` (`S9xWideBatch`, `DrawOBJSWide`), `core/widecap.h`, `core/ppu_mt.cpp`, `core/ppu.h`, `core/tileimpl.h`; every change in `core/PROVENANCE.md` | Snes9x |
| Adapter | `snes/frontend/wide.{h,cpp}` (`snes_wide_*`) | MIT |
| Layer analysis, learned map, composition | `widekit/` (unchanged; `make -C widekit test` stays green) | MIT |
| Player row, picture geometry, map file | `snes/vita/player.{h,cpp}` (`wide_apply`), `snes/frontend/picture.{h,cpp}` (`pic_wide_margin`, `pic_geometry_wide`) | MIT |
| Vita presentation | `snes/vita/main.cpp` (`video_fetch`, `frame_draw`) | MIT |

`snes.bin` links only `snes/`, `playerkit/` and `widekit/`. Nothing from `gpsp/` or `nes/` is copied.

## What the core records (cheap, emulation neutral)

Per render batch (the lines drawn between two register changes) the renderer keeps a `SWideLine`: mode, tilemap base and size,
tile size, scroll (HDMA included, one value per line), mosaic, windows, layer enables, colour math, Mode 7 matrix per line,
sprite settings, backdrop, palette checksum. One 16-bit write stamp per VRAM word and one age per 16-byte unit are stored at the
six VRAM write sites (`S9xWideStampVRAM`, only while the view is on); they say when the game wrote each tile or map word, which is
how widekit tells columns the camera showed from columns the game streamed ahead.

Margin rendering reuses the renderer: the batch is drawn again with the BG scroll shifted by the margin and per-side clip
tables into separate buffers: a composite (all layers, windows, colour math, exactly what the PPU would show), one pass per BG
(mode 0, 1, 3) and a sprite pass (`DrawOBJSWide`: left-margin sprites, right overhang, real draw rules, no extra budget).
Mode 7 is extended directly (start column shifted, EXTBG included); the whole margin of a Mode 7 line is real. Lines the
renderer cannot widen are marked "no margin" (hi-res, interlace, modes 2/4/6, mosaic, forced blank).
The normal picture is drawn by the unchanged code first.

## Threading

* Capture and margin drawing happen inside the renderer's own pass, from the same state snapshot as the normal picture: on
  the PPU thread when `ppu_thread=1` (the slot ring carries the margin; `K_WIDE_END` ends the frame's capture), else on the
  emulation thread at the end of the frame.
* The adapter is two halves (13 Nov 2026), `capture()` and `run()` ("compose") in `snes/frontend/wide.cpp`:
  - **capture** runs in the frame hook (the thread that drew the margins). It copies into a fixed-size snapshot (`Snap`, about 1.1 MB, a
    pool of 5) exactly what compose reads: the line records, CGRAM and the palette as drawn, VRAM + the write stamps (not on a Mode 7
    frame), the 256 centre columns, and for every line and pass the margin pixels the core drew (only the passes and rows the line says
    it drew, only the `margin` nearest columns per side; Z bytes for the sprite and layer passes). It touches no compose state.
  - **compose** (describe, widekit, learned map, candidates, exact composite, ambient, Auto, per-layer fade, soft edge, census) reads only
    the snapshot and owns widekit, the learned map and the output ring.
  - Single call path (`snes_wide_split(0)`, the default, used by the twin and when the hand-over is off): the hook is capture + compose
    in one call on the hook's thread, as before. Output is identical (see Checks).
  - Split path (`snes_wide_split(1)`; the player sets it while the smooth hand-over is on and the wide view is active): the hook only
    captures; the emulating thread takes snapshots in order with `snes_wide_snap_take` (once per frame, in a loop: a frame in which the
    core's main loop returned late has two captures and the next none) and puts the handle into the hand-over mailbox
    (`PicturePipe::put_snap`: a 24 byte descriptor, plus the core's normal picture only for a frame the capture says will not be widened);
    the drawing thread calls `snes_wide_compose` and uploads the result at once. One compose at a time, in the order taken.
  - Slots: FREE, FILLING (capture), FILLED, TAKEN (handed on), COMPOSING. Capture never waits: it takes a free slot, else the oldest
    FILLED one (counted), else the oldest TAKEN one (its descriptor then fails the serial check: `snes_wide_compose` returns -1 and does
    nothing). A snapshot the mailbox dropped (steady pacing's half mode, fast forward) is simply never composed; compose then sees
    the frames that were not dropped. That is not proven: only the full, no-drop sequence is (see Checks).
* The consumer of the single path (`video_fetch`) takes the newest finished picture with `snes_wide_frame()` (a 4-slot ring; the slot is
  valid for three further frames). In split mode the drawing thread uses the pointer that `snes_wide_compose` returns (valid until the third
  next compose; the player copies it into a texture at once). Control calls (`snes_wide_set`, `snes_wide_split`, options, game, map) are
  for the thread that runs the frames, between frames; `snes_wide_set` / `_split` wait for the PPU thread and for a running compose.
* The learned map belongs to compose. All access to it (compose, `snes_wide_map_save/load/dirty/mark_saved`, census, options) is
  under the adapter's one spin lock, which compose holds for the whole frame: a map save requested while compose runs waits for that frame
  (at most one compose time), and a compose that wants to start while the map is serialised waits for the serialisation (measured below,
  one 30 s event). `WriterJob::WIDEMAP` (writer thread) calls `snes_wide_map_save`, writes the bytes and calls `mark_saved`; the game thread
  only posts the job every 30 s while the map has new cells, and at the end of the session. `snes_wide_twin --map-poll` calls
  save / dirty / census from the frame thread every 37 frames while compose runs on its own thread (ThreadSanitizer, below).

## Player integration (done)

* Settings > Picture "Wide view: Auto / On / Off" is playerkit's own row `ROW_PIC_WIDE` (`CAP_WIDE` set), default Auto, stored in
  `picture.ini` / the game's file as `wide_view=0|1|2` (per game with "Apply to: This game"), launch override `ovr.wide_view`.
* `SnesPlayer::wide_apply(scr_w, scr_h)` (called by the platform every frame) takes the margin from the picture shape
  (`pic_wide_margin`: Original 41 on 960x544, Pixel perfect 81 (8:7) or the core's 96 limit (square), Zoom as Original, Full screen: none)
  and switches the core view on/off/to another margin. Auto is off while the steady pacer thins frames; On is always (where the
  shape has room); Off never.
* Presentation: the wide frame (256 + 2M columns, 224 or 239 rows) is uploaded like the normal picture;
  `pic_geometry_wide` gives it the same scale per column as the 4:3 picture (trimmed symmetrically if wider than the screen).
  Hi-res and interlaced pictures, and frames the view cannot widen, use the normal picture. State pictures are the 256 middle columns.
* Learned map: `snes_dir/wide/<game>.wmap`, loaded at start, written by the writer thread. Cells are keyed by a CRC of the ROM.
* `snes_wide_set_game` is called at start from `snes_host_rom_image()`; reset and state load start a new scene by themselves (epoch).
* Per-game profile: `snes_wide_profiles_load` takes widekit's profile text; the built-in table ships empty.

## Checks

`sh snes/twin/selftest.sh` has a wide section: truth tests on synthetic cartridges (`twin/tests/make_wide_roms.py`, `wide_truth.py`),
identity (frames, audio, state; single thread and workers) and the player test (`player_twin wide ROM WORK`).
Larger runs: `twin/wide_compare.py` (the 15 owner games and the test-ROM suites).

## Known harness note (Super FX test ROMs)

On four PeterLemon Super FX ROMs (GSU 256x192) the twin check "wide centre differs from the normal picture" reports 9 to 17
rows per frame. Cause: `S9xMainLoop` returns some lines after `S9xSyncSpeed` for these ROMs, so `snes_host_video()` read
after `run_frame` already holds lines of the next frame, while the wide view's centre is the snapshot taken at the frame
end (screen hash at the hook, at `S9xControlEOF` and at `S9xSyncSpeed` are equal to the wide centre). The same read-time
picture is what the unmodified build returns (frame, audio and state hashes are identical to the reference).
The wide centre is the cleaner frame. Not a wide-view error.

## Auto decision and census

`snes_wide_option("auto", 1)` (the player's "Auto") lets the adapter decide per scene whether the margins show the wide picture or
an ambient frame. Coverage is the share of margin pixels (over the drawn lines) that come from a real tilemap, the learned map or a
trusted continued backdrop; it is smoothed (EMA over about 60 frames, snapped at scene events). Wide turns on at 85 % and off at 75 %
(hysteresis); a change cross-fades over 32 frames (about 0.5 s). `snes_wide_option("auto", 0)` ("On") always shows the wide picture.
The ambient frame is a dimmed, heavily blurred colour field taken from the picture's own edge columns (no structure), the same
look as the app's ambient backdrop. Pixels not trusted fade over 24 px into it.

`snes_wide_census_get()` returns running totals since the game was set (`snes_wide_census_reset()` restarts them): frames, frames
wide / ambient / cross-fading, fallback reasons (nothing drawn, profile off, setup), Auto switches, scene events, margin pixels by
provenance (tilemap, learned, backdrop, unknown), map cells, current coverage and the Auto state. The player writes one log line per
second from it while the wide view is on. `snes_wide_twin --summary` prints the same (`census:` and `censuspx:` lines).

## Learned map and sub screen layers (13 Nov 2026)

- The learned map identifies a cell by its content hash (tilemap word + the tile's graphics) instead of a global tile set identity
  (`wk_layer.map_by_cell`, opt-in; the NES adapter does not set it and its golden output is unchanged). The adapter re-checks that hash
  against the tile's graphics today before showing a served cell, so a changed or animated tile is never shown stale.
- Colour math on the backdrop alone (`cgadsub` 0x20, no window, no clip) no longer disables the layered continuation. A BG that is only on
  the sub screen (Yoshi's Island's far mountains) is drawn by the core as backdrop + layer (`SWideLine.tsx`), and map cells of such a layer
  get the same "add" maths (only for `cgadsub == 0x20`; other maths keep the ring-only extent).
- Not done: backdrop pixels not covered by any layer are still the soft edge (the colour is not guessed); a foreground that is unknown
  beside a known backdrop ends with a hard edge (no per layer fade beyond the real extent yet).

## Learned map safety and the foreground fade (13 Nov 2026, release 1.4)

No file under snes/core changed for this (PROVENANCE.md needs no new row); everything is in widekit (opt-in fields, defaults = old
behaviour, so the NES adapter and its golden hashes are unchanged) and snes/frontend/wide.cpp. Adapter option `map_safe` (bit mask,
1 = all, default on): 2 strict bind, 4 match by tilemap word, 8 drop a revoked room, 16 seam check, 64 strict verify, 128 live flags.

- Binding: a room binds only when 98% of the found cells match (`bind_pct`) for 8 frames in a row (`bind_frames`, probation); matching and
  verifying compare the tilemap word (`match_by_code`), so animation does not break a match; a revoked room is dropped (`drop_on_revoke`);
  verify 98%, revoked after one bad frame. Proof: two levels with identical scroll coordinates and different tiles, and an 85% shared
  variant (synthetic ROMs `wide_alias_a/b/c`): 0 wrong real pixels in pass 1 and pass 2 (the old rules showed 42742 wrong on `alias_b`).
- A learned cell is served only if the 2 visible tiles next to the seam, in the same tile row, equal the room's cells (`seam_tiles`).
- Changed terrain: a cell has `live`. Cells loaded from a file, or not seen on screen since the room was bound, are not live and are drawn
  "soft" (50% toward the ambient colour, provenance 12), never as real (provenance 4 is claimed only for live cells). Soft pixels count half
  in the Auto coverage. Proof: `wide_destr` (terrain changed in the ring by game code while the camera is away): 0 wrong in both passes.
- Residual risk, not provable by this design: a game that changes level tiles through RAM that was never on screen and is beyond the ring
  (`wide_destr_far`: 43600 wrong of 3.98M checked pixels in pass 2, reported as INFO by selftest, not as a failure); a tilemap ring of
  32 tiles cannot be told from a 64 tile one.
- Foreground fade: an unknown foreground column fades out over 24 px (smoothstep alpha, no dither, `cfg.fg_fade_px`; the layer's own last
  known colour continues, provenance 7) and a lower layer shows through beyond it, labelled `WK_PROV_REVEAL` (11, not a claim, counted in
  the coverage). Sprites use the same ramp. The Auto cross-fade is 32 frames (measured on Super Mario World: largest per frame change of
  the margin 1.7 colour units at both switches).

## Cost (13 Nov 2026; x86_64 callgrind, instructions per frame in millions, 300 frames of gameplay, margin 41 = the default "Original" shape, 96 = the core's limit)

Measured with `snes_wide_twin --bench --cg A,B` under `valgrind --tool=callgrind --instr-atstart=no` (the window starts 100 frames after the
input script reaches play; no harness hashing), games with the owner's scripts: Super Mario Kart (Mode 7 racer), Super Mario World
(3 BG layers + sprites + colour maths), Yoshi's Island (Super FX, 3 BG layers, a per-line HDMA gradient: every line is its own render batch).
"old" is the tree before this work (adapter in the frame hook), "new" the tree with the capture / compose split and the merged margin clip tables.

| game, margin | off | core margin passes + stamps | capture | compose | emulation side (off + core + capture) | total |
|---|---|---|---|---|---|---|
| SMK 41 | 7.20 | 1.15 | 0.21 | 1.85 | +19 % | +45 % |
| SMK 96 | 7.20 | 2.06 | 0.22 | 3.41 | +32 % | +79 % |
| SMW 41 | 7.36 | 2.50 | 0.31 | 7.07 | +38 % | +134 % |
| SMW 96 | 7.36 | 5.27 | 0.35 | 11.85 | +76 % | +237 % |
| YI 41 | 10.09 | 5.93 | 0.29 | 6.59 | +62 % | +127 % |
| YI 96 | 10.09 | 9.76 | 0.32 | 10.82 | +100 % | +207 % |

(old tree, same columns, emulation side: SMK 41 +16 %, SMK 96 +29 %, SMW 41 +35 %, SMW 96 +72 %, YI 41 +71 %, YI 96 +109 %; total 42 / 78 / 132 / 236 / 137 / 218 %.)
Targets for the emulation side (<= +35 % at 41, <= +75 % at 96): met by SMK; missed by SMW 41 (+38 %) and 96 (+76 %), and by YI (+62 % / +100 %).
What was done: window tables cut once per batch and merged (YI -1.3 M, SMW -0.04 M, SMK 0); a Mode 7 frame does not copy VRAM or the stamps.
What the rest is: the margin passes draw the layers again (one pass per BG, one composite, one for sprites) for a strip a third as wide;
a tile draw costs the same per call whatever the strip width, and Yoshi's Island has one render batch per line (HDMA), so a tile row is
one call per line and layer (about 3 K instructions per layer and line). Compose is 2.3 M of widekit's `wk_compose_frame` (soft edge base
and blending) and about 4 M of the adapter's own per-pixel loops and widekit's layer and map analysis, spread thin (no line over 1 M); the ring walk of `describe_layer` is 0.85 M.
Not done (ideas, none proven): build the composite from the layer passes by Z (lines without colour maths), merge the batches of
lines whose layer registers do not change, fill only the columns up to a bound of the ring extent, skip the soft base under proven pixels.

Per thread on the Vita (estimate: x86 instructions counted as ARM instructions, 444 MHz, IPC 1.0 / 0.7; milliseconds per frame; CPU thread =
CPU, DMA, PPU registers, stamps; PPU thread = the renderer and the margin passes and the capture; draw thread = compose, before the texture copy and the display):

| | CPU | PPU | APU | draw (compose) | single thread, all but compose |
|---|---|---|---|---|---|
| SMK off | 3.7 / 5.3 | 8.0 / 11.5 | 4.4 / 6.4 | - | 16.2 / 23.2 |
| SMK 41 | 4.0 / 5.7 | 10.9 / 15.5 | 4.4 / 6.4 | 4.2 / 6.0 | 19.3 / 27.6 |
| SMK 96 | 4.0 / 5.8 | 12.9 / 18.4 | 4.4 / 6.4 | 7.7 / 11.0 | 21.4 / 30.5 |
| SMW off | 4.2 / 6.0 | 7.3 / 10.4 | 5.1 / 7.3 | - | 16.6 / 23.7 |
| SMW 41 | 4.4 / 6.3 | 13.3 / 19.1 | 5.1 / 7.3 | 15.9 / 22.7 | 22.9 / 32.7 |
| SMW 96 | 4.5 / 6.4 | 19.6 / 28.0 | 5.1 / 7.3 | 26.7 / 38.1 | 29.2 / 41.7 |
| YI off | 6.9 / 9.8 | 11.4 / 16.3 | 4.4 / 6.3 | - | 22.7 / 32.5 |
| YI 41 | 7.1 / 10.2 | 25.2 / 36.0 | 4.4 / 6.3 | 14.8 / 21.2 | 36.7 / 52.5 |
| YI 96 | 7.2 / 10.2 | 33.8 / 48.4 | 4.4 / 6.3 | 24.4 / 34.8 | 45.5 / 64.9 |

Reading it: the CPU (emulating) thread, which runs `snes_host_run_frame`, is not touched (+0.1 to +0.2 ms for the stamps) and fits 16.7 ms in
all three games with the wide view on. The thread that renders (with `ppu_thread=1` the PPU thread) fits at margin 41 only for SMK, and for
SMW only at IPC 1.0; Yoshi's Island does not fit at either margin (the player's Auto mode turns the wide view off while the pacer thins
frames). The drawing thread's compose does not fit 16.7 ms for SMW at margin 41 at IPC 0.7 (22.7 ms) or at margin 96, nor YI at 96: the
wide pictures then arrive at a lower rate than the game (the mailbox drops the older ones in its half mode; the game itself is not slowed).
Nothing here ran on a Vita.
