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
* widekit, the learned map and the composition run in the frame hook (`on_frame`) on the same thread that ended the frame
  (the PPU thread with workers), so the emulation (CPU) thread never waits for them.
* The consumer (the Vita's game thread in `video_fetch`) takes the newest finished picture with `snes_wide_frame()`
  (a 4-slot ring; the slot is valid for three further frames). Control calls (`snes_wide_set`, options, game, map)
  are for the thread that runs the frames, between frames. `snes_wide_set` waits for the PPU thread.
* The learned map is serialised and written by the writer thread (`WriterJob::WIDEMAP`, safe write); the game thread only
  posts the job every 30 s while the map has new cells, and at the end of the session.

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
