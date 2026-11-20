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
* Compose is one part that a single thread does (description, widekit's layer and map analysis, the Auto decision) and one part per picture
  line (candidates, widekit's composition, exact composite, ambient frame). `snes_wide_parallel(fn, user, n)` lets the frontend share the lines
  between up to 4 threads: the adapter calls `fn(user, job, arg, n)`, which must run `job(arg, w)` for w = 0 .. n - 1 (w = 0 on the caller) and
  return when all have returned; the workers take lines in chunks of 8 until none are left. A line is done by one worker and writes only its
  own rows, and what a worker keeps (scratch rows, the soft row cache, the memo of map cells, the sums for the census) is its own, so the
  picture does not depend on who did which line (`snes_wide_twin --wide-workers N`: the hashes of one thread). The Vita player gives its
  scaler helper threads (one on three cores, two with the fourth core) as workers 1 and 2; a helper that wakes late finds no line left and does nothing.
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
Last full run (13/14 Nov 2026, final tree): `selftest.sh` all passed (includes the ladder floor and per-side Auto truth runs on scrolling, window
and Mode 7 cartridges: 0 wrong real pixels); `make -C widekit test` 1095 checks, 0 failed; NES `wide_golden.sh` with `WG_VERIFY=1` 37 files
match; `wide_compare.py` against the pre-wide reference tree on the 15 owner games, 3000 frames: margin 70 (single thread and workers) 15
identical, margin 41 and 96 (single thread) 15 identical.

## Known harness note (Super FX test ROMs)

On four PeterLemon Super FX ROMs (GSU 256x192) the twin check "wide centre differs from the normal picture" reports 9 to 17
rows per frame. Cause: `S9xMainLoop` returns some lines after `S9xSyncSpeed` for these ROMs, so `snes_host_video()` read
after `run_frame` already holds lines of the next frame, while the wide view's centre is the snapshot taken at the frame
end (screen hash at the hook, at `S9xControlEOF` and at `S9xSyncSpeed` are equal to the wide centre). The same read-time
picture is what the unmodified build returns (frame, audio and state hashes are identical to the reference).
The wide centre is the cleaner frame. Not a wide-view error.

## Auto decision (per side) and census

`snes_wide_option("auto", 1)` (the player's "Auto") lets the adapter decide, per side and scene, whether a margin shows the composed wide
picture or the ambient frame. Coverage is the share of that side's margin pixels (over the drawn lines, status bar lines excluded:
`hud_excl`, default on) that come from a real tilemap, the learned map or a trusted continued backdrop (soft learned pixels count half);
it is smoothed (EMA over about 60 frames, snapped at scene events). A side turns real at 85 % and back to ambient at 75 % (hysteresis);
a change cross-fades over 32 frames (about 0.5 s). The two sides decide alone (13 Nov 2026): a scrolling game has real picture behind the
camera (the tilemap keeps up to about 240 px of history behind the scroll direction and only 0 to 16 px ahead) and the side the camera came
from can show it while the other side shows the ambient frame, instead of both falling back together. `snes_wide_option("auto", 0)` ("On")
always shows the composed picture. A side that is ambient or cross-fading claims no real pixel (`snes_wide_real()` is 0 there and the
provenance is soft / fade), so the truth tools see exactly what is claimed. The ambient frame is a dimmed, heavily blurred colour field
taken from the picture's own edge columns (no structure), the same look as the app's ambient backdrop. Pixels not trusted fade over 24 px
into it. Option calls with "auto" are lock free when the value does not change (the player calls it every frame; compose holds the adapter
lock for a whole frame).

`snes_wide_census_get()` returns running totals since the game was set (`snes_wide_census_reset()` restarts them): frames, frames wide /
ambient (both sides) / ambient per side / cross-fading, fallback reasons (nothing drawn, profile off, setup), per line reasons
(`line_reason[]`, `SNES_WIDE_LR_*`), Auto switches, scene events, margin pixels by provenance (tilemap, learned, backdrop, unknown, soft,
reveal, pad), map cells, the ladder rung and the smoothed compose time. The player writes one log line per second from it while the wide
view is on. `snes_wide_twin --summary` prints the same (`census:`, `censuspx:`, `sides:`, `ladder:`, `reasons:` lines).

## Cost ladder: wide stays on, what degrades is how much is composed (13 Nov 2026)

Owner rule: wide on by default for every game, never a missed 60 Hz refresh for it. The adapter therefore never switches the view off for
cost. Option `policy` (default 1; the twin's proofs run with 0 = always the full margin) measures the compose time per frame (the wall time
of `run()`, smoothed over about 32 frames; on the split path that is the drawing thread's time) and steps a rung:

| rung | work margin (columns per side that the passes, widekit and the map are asked for) | beyond it |
|---|---|---|
| 0 | the picture's margin (41 / 70 / up to 96) | - |
| 1 | 5/8 of it | ambient frame |
| 2 | 3/8 of it | ambient frame |
| 3 | the floor: centre only (no margin passes, no capture) | ambient frame |

The picture keeps its full width: the columns beyond the work margin are the ambient frame (a calm colour field, never structure, so never a
wrong tile) and the last 12 work columns fade into it (a faded pixel claims nothing). At the floor the core is told `S9xWideCentreOnly(1)`
(a relaxed atomic byte, latched at the start of the next frame, so the compose thread may set it): the renderer draws no margin passes and takes
no capture of the frame (`S9xWideRenderFrameStart(0)`, no VRAM / stamp copy in the PPU thread's `K_WIDE_END`; the VRAM write stamps stay on so
the way back up is cheap), the hook still gets the centre picture, and compose (`run_co`) only copies it between two ambient columns
strips. Hi-res and interlaced frames are told by the rendered size and fall back to the normal picture as before. The step back up is judged
by the compose time the wider rung had when it was given up (`last_us`), trusted for one minute, after which one probe is made. Steps down: smoothed compose time above `cost_us`
(default 9000 us, about half of a 60 Hz frame so that the draw thread also has room for the texture copy) for `cost_frames` (60) frames.
Steps up (hysteresis): only after `hold_frames` (600 frames = 10 s; doubled up to 7200 each time a step up was followed by a step down within
a minute) at the rung, and only if the time scaled to the wider margin (linear in the margin) is at most 75 % of the budget. Super FX and SA-1
cartridges (header type byte) start at rung 1; the measurement corrects either way. Proof that the rung never shows a wrong tile: it only
shrinks the region that is composed and fills the rest with the ambient frame; `selftest.sh` runs the truth tools at the floor
(`policy=1 cost_us=1`) on scrolling, window and Mode 7 cartridges (0 wrong real pixels, centre identical).
The twin can stand in for a slower machine with `--wopt cost_scale=PERCENT` (the ladder sees the measured time times this).

Menu semantics (owner, 14 Nov 2026): Wide view **Auto** = per side Auto plus the cost ladder (`snes_wide_option("mode", 0)`), **On** = the full margin
always, no ladder (`"mode", 1`), **Off** = `snes_wide_set(0, 0)`. The player calls `"mode"` every frame (lock free unless it changes). The core's own
margin follows the rung (`S9xWideCoreMargin`, latched at the start of the next frame like the centre only flag): the passes are drawn with the
work margin, so the PPU thread's cost moves with the rung, not only compose; a frame drawn just before the core followed a step up is captured at
the narrower margin it has. `snes_wide_option("pressure", 1|2)`: the player reports trouble (pacer not at full rate, a governor "bad second"); the
ladder steps down at once on the next composed frame whatever compose took (1 = one rung, 2 = to the floor), and a step up needs `hold_frames` quiet
frames after the last report, then the normal rule; ignored in On. This replaces the old valve that switched the wide view off in half pacing.
Ladder details: the hold time doubles only after a step down that follows a step UP too soon; the average is only fed by frames composed at the
margin that is asked for now (a frame from before a step does not reseed it).

Cost of the rungs (x86 callgrind, Yoshi's Island, frames 1500 to 1560 of the owner's script, margin 41, instructions per frame in millions): wide off
8.12; wide on at the full margin (rung 0) 15.24 (+88 %); rung 1 (26 columns) 14.5; rung 2 (16 columns) 10.5; floor (centre only) 8.85 (+9 %): the PPU
thread's passes and capture, about 6.4 M a frame, are not done. Rung 1 saves little (a tile draw costs the same per call, one call per tile row, line and
layer, and Yoshi's Island has one render batch per line); rung 2 and the floor are the ones that give frames back.

## The 15 games (13 Nov 2026; `snes/twin/wide_table.py`, margin 41, 3000 frames of the owner's script, x86 twin)

Columns: real share = margin pixels that are real picture (tilemap, learned map, continued backdrop, revealed lower layer) over all margin
pixels, with `hud_excl` on (default) and off; soft = learned cells shown softly; Auto ambient frames L / R = percent of frames on which that
side showed the ambient frame (Auto decides per side; 0 = always the composed picture); main limits = per picture line reasons
(`SNES_WIDE_LR_*`; "ring shorter than margin" = the layers' known extent is shorter than the margin, "layer not described" = a layer on
the line is not a mode 0/1/3 tilemap widekit can describe, "bitmap tilemap" = framebuffer style tilemap); compose = wall microseconds of
compose per frame on this x86 box, full margin (a contended 2 core box: indicative only); ladder rung = where the cost ladder was after
3000 frames with `--wopt policy=1 --wopt cost_scale=800` (the measured time times 8, a guess at the Vita's speed from the instruction
counts below; the box was shared with other jobs, so the steps are noisy), steps in brackets; hud_excl pixels = the composed frames with
Auto off (`auto=0`) with `hud_excl` 1 and 0 compared byte for byte.

| game | chip | real share % (hud_excl 1 / 0) | soft % | Auto ambient frames L / R % | main limits of the margin (lines) | compose us/frame (x86, full margin) | ladder rung at x800 (steps) | hud_excl pixels |
|---|---|---|---|---|---|---|---|---|
| ChronoTrigger | - | 42.8 / 42.8 | 0.0 | 86 / 86 | ring shorter than margin 53.7%  exact 40.2%  forced blank 6.0% | 440 | 0 (0) | identical |
| Donkey Kong 1 - Country | - | 11.7 / 11.7 | 0.0 | 99 / 99 | ring shorter than margin 77.2%  exact 11.3%  bitmap tilemap 7.8% | 289 | 0 (0) | identical |
| Doom | Super FX | 0.0 / 0.0 | 0.0 | 99 / 99 | ring shorter than margin 67.7%  forced blank 32.3% | 272 | 0 (0) | identical |
| F-Zero | - | 2.9 / 2.9 | 0.0 | 99 / 99 | ring shorter than margin 92.0%  forced blank 4.6%  exact 2.7% | 309 | 0 (0) | identical |
| Kirby Superstar | SA-1 | 23.9 / 23.9 | 0.0 | 91 / 91 | ring shorter than margin 53.0%  exact 24.9%  forced blank 13.2% | 397 | 0 (0) | identical |
| Mega Man X 2 | - | 29.5 / 29.5 | 0.0 | 69 / 69 | ring shorter than margin 47.7%  exact 42.8%  forced blank 6.6% | 411 | 0 (0) | identical |
| Pilotwings | - | 60.2 / 60.2 | 0.0 | 91 / 91 | exact 55.8%  layer not described 36.9%  forced blank 7.3% | 193 | 0 (0) | identical |
| Street Fighter Alpha 2 | - | 44.2 / 44.2 | 0.2 | 56 / 52 | ring shorter than margin 46.4%  exact 39.6%  forced blank 14.0% | 433 | 0 (2) | identical |
| Stunt Race FX | Super FX | 4.6 / 4.6 | 0.0 | 91 / 91 | ring shorter than margin 84.5%  forced blank 11.4%  exact 4.1% | 352 | 0 (0) | identical |
| Super Mario Kart | - | 62.3 / 62.3 | 0.0 | 43 / 92 | ring shorter than margin 56.0%  exact 30.8%  layer not described 7.0% | 313 | 0 (0) | identical |
| Super Mario RPG | SA-1 | 34.1 / 34.1 | 0.0 | 71 / 72 | ring shorter than margin 55.3%  exact 33.8%  forced blank 10.9% | 328 | 0 (2) | identical |
| Super Mario World | - | 53.6 / 53.6 | 4.2 | 60 / 84 | exact 63.8%  ring shorter than margin 30.6%  forced blank 5.6% | 613 | 2 (2) | identical |
| Super Metroid | - | 99.0 / 99.0 | 0.0 | 0 / 0 | exact 88.6%  forced blank 10.5%  ring shorter than margin 0.9% | 118 | 0 (0) | identical |
| Yoshi's Island | Super FX | 17.6 / 16.2 | 7.1 | 90 / 90 | ring shorter than margin 59.3%  forced blank 18.6%  exact 15.5% | 463 | 0 (0) | identical |
| Zelda - A Link to the Past | - | 38.1 / 38.1 | 0.0 | 67 / 67 | ring shorter than margin 59.1%  exact 36.4%  forced blank 4.5% | 332 | 0 (0) | identical |

Reading it: `hud_excl` changes no pixel in any of the 15 games (it only changes which lines count towards Auto's decision; the real share
differs only on Yoshi's Island, 17.6 against 16.2 percent). Real picture is rare outside Mode 7 and the few games with long tilemap history:
Super Metroid (99 %), Super Mario Kart (62 %), Pilotwings (60 %), Super Mario World (54 %), Street Fighter Alpha 2, Chrono Trigger, Zelda
(38 to 44 %). The side decision shows its effect where the camera scrolls: Super Mario Kart 43 / 92 and Super Mario World 60 / 84 percent
ambient on the left / right, against both sides together before. For every other game the limit is the ring: the line is real only as far
as the tilemap extent known from the write stamps, which is shorter than the 41 px margin on 46 to 92 percent of the lines.

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

## Compose cost, upscaler, rewind capture (20 Nov 2026, 2.0; host x86_64 callgrind, nothing here ran on a Vita)

What changed in the adapter (`snes/frontend/wide.cpp`; `snes/core` and the margin passes are untouched in this step):
- **Ambient rows** are built from a table per (phase, distance, channel value) instead of the closed form per pixel, kept in a 1024 row direct mapped cache,
  and the lookup is hoisted out of the soft pixel loop. `cell_row()` decodes a tile cell row once for the whole margin row.
- **`describe_layer` ring walk skip.** The walk of a layer's tile ring is not repeated when the key (scroll, size, settle count) is unchanged, no cell was
  unsettled, the tilemap words and their write stamps are unchanged per block and the graphics hash of every used tile is unchanged. Result and
  counters are those of the walk (it is a pure function of exactly those inputs).
- widekit `wk_compose.cpp`: the soft row cache is 1024 rows (was 128) with a better hash. Pure cache: the NES wide golden (37 files) is unchanged.
- Census: `capture_us` (the capture's own cost), `cache_checks` / `cache_mismatches`. The cost ladder's default budget is 13 ms (was 9) and
  chip carts (Super FX, SA-1) start at rung 0: the ladder is the last net, not the first resort. It still measures the real compose time on the
  device and only then narrows.
- Option `verify_cache=1` (twin proof): every kept result (ring walk, ambient row) is also made fresh and compared; a difference is counted in the census.

Proofs (x86 twin; 15 private games, 3000 frames of the owner's pad script): centre picture, audio and state hashes identical to the reference
tree before the wide view, single thread and with the workers, margin 41 (`wide_compare.py`, 15 identical); wide output hashes identical to the
tree before this step on all 15 games at margins 41 and 96 (all 3000 frames); `verify_cache=1`: 26.2 M checks, 0 mismatches (41 and 96);
selftest (truth ROMs: 0 wrong real px) all passed; TSan over the capture/compose split on Super Mario World, Yoshi's Island (Super FX) and Kirby Super Star
(SA-1), single / split / split with workers: same hashes, 0 reports.

Cost after (Minstr per frame; frames 1500..1560, owner's script; before in brackets from the table above, same method):

| game | off | margin 41 total | compose 41 | margin 96 total | compose 96 | emulation side extra at 41 |
|---|---|---|---|---|---|---|
| Yoshi's Island (Super FX) | 7.84 | 13.71 (14.96) | 4.46 (5.70) | 18.27 (20.72) | 8.02 (10.46) | 1.42 |
| Super Mario World | 6.95 | 13.95 (15.78) | 4.77 (6.61) | 19.24 (22.00) | 7.97 (10.73) | 2.23 |
| Super Mario Kart | 5.00 | 10.75 (11.56) | 3.82 (4.63) | 14.26 (15.58) | 5.93 (7.26) | 1.92 |
| Super Metroid | 6.97 | 9.97 (10.12) | 1.41 (1.56) | 12.57 (12.91) | 2.40 (2.74) | 1.59 |
| Kirby Super Star (SA-1) | 6.25 | 12.76 (14.13) | 5.13 (6.50) | 16.15 (18.07) | 7.71 (9.63) | 1.38 |

"Emulation side extra" is the core's margin passes plus the capture (total minus off minus compose); it is not changed by this step. Compose is
-10 to -28 % at margin 41. The per-margin-pixel target (under 100 instructions) was not reached by this step (about 240 to 260); the next section
("Full width") is the step after it and has the numbers that hold now.

Per thread on the Vita (model: x86 instructions counted as ARM instructions at 444 MHz, IPC 1.0 / 0.7; ms per frame; core (off) is the sum of
CPU, PPU and APU work of the whole game and is shared between threads there; the draw thread also runs the scaler and the upload):

| | emulation extra (41) | compose 41 | compose 96 | scaler, xBR 2x (typical / worst) | upload | draw thread 41, typical |
|---|---|---|---|---|---|---|
| Yoshi's Island | 3.2 / 4.6 | 10.0 / 14.4 | 18.1 / 25.8 | 3 / 8.5 | 1.5 | 14.5 / 18.9 |
| Super Mario World | 5.0 / 7.2 | 10.7 / 15.4 | 17.9 / 25.6 | 3 / 8.5 | 1.5 | 15.2 / 19.9 |
| Super Mario Kart | 4.3 / 6.2 | 8.6 / 12.3 | 13.4 / 19.1 | 3 / 8.5 | 1.5 | 13.1 / 16.8 |
| Super Metroid | 3.6 / 5.1 | 3.2 / 4.5 | 5.4 / 7.7 | 3 / 8.5 | 1.5 | 7.7 / 9.0 |
| Kirby Super Star (SA-1) | 3.1 / 4.4 | 11.6 / 16.5 | 17.4 / 24.8 | 3 / 8.5 | 1.5 | 16.1 / 21.0 |

(scaler: about 50 instructions per output source pixel over the 338 x 224 picture with the ScaleCache reusing unchanged blocks, helper threads
share it; an estimate, not measured on a Vita.) Reading it: at IPC 1.0 the draw thread fits 16.7 ms at margin 41 for every game here except the worst
scaler seconds; at IPC 0.7 it does not for the three heaviest, and then the cost ladder narrows the margin from the measured time. Full width (96)
does not fit the draw thread of the Super FX / SA-1 / SMW games at either IPC on this model: it is held only by the ladder or by a lower rate of
wide pictures (the game itself is not slowed).

## Pixel art scaler (20 Nov 2026)

`snes.bin` links `pk_scale` as `md.bin` does: integer xBR 2x (the definition), MMPX 2x as a look, xBR 3x, with the ScaleCache reusing unchanged
blocks and the helper threads taking chunks. Ceiling: xBR 2x for 256 x 224/239 including the wide width; none for hi-res 512 and interlace. The
look governor's first rung is "xBR 2x -> sharp", then the interlace and hi-res blends. Menu row "Pixel art scaler" and the Image quality wording are
md.bin's. Proof (`snes/twin/scale_proof`): ScaleCache output equals `scale_rows` and, every 100 frames, `xbr_reference_int`, on 15 games with the wide
view off, at margin 41 (xBR 2x and MMPX 2x) and at 96 (xBR 2x), 1500 frames each, 75 runs, 0 mismatches. The census prints a `scaler=` line
(pictures, ms per picture, non-flat share, helper chunks, computed % and moved %).

## Rewind capture (20 Nov 2026)

`snes_host_rewind_state_save_buf` writes the same bytes as `snes_host_state_save_buf` (checked byte for byte on 15 games, 300 captures each) but does
not copy the part of Snes9x's 512 KB cartridge RAM block that cannot hold anything (a plain cartridge: beyond `SRAMMask`; Super FX beyond 128 KB;
SA-1 beyond 264 KB, each checked at every capture, and the whole block is written from then on if a byte is found there) and skips the sizing pass.
Bytes copied per capture: about 300 KB (plain), 430 KB (Super FX), 580 KB (SA-1) instead of 823 KB. Host instructions per capture 1.16 M -> 0.53 M
(callgrind, Super Mario World). On the Vita this is an estimate: about 0.5 to 1.2 ms of the game thread for the heavy games. See "Rewind capture, second
step" at the end of this file for where that time now goes and for the bound of the SA-1's RAM.

## Full width: compose by lines, fewer margin passes (20 Nov 2026, second step; host x86_64 callgrind and a model, nothing here ran on a Vita)

Goal: the full Vita width (margin 96) on the heaviest games without the cost ladder having to narrow it. What was done, each proven to leave the
wide picture bit for bit as it was (the proofs are listed below):

1. **Compose is per line and can be shared between threads** (`cand_line`, `out_line`, `fin_line` in `snes/frontend/wide.cpp`;
   `wk_compose_begin / _lines / _end` in widekit; `snes_wide_parallel`, see Threading). Inside a line both margins are held by distance from the
   picture and the left one is turned round once at the end.
2. **Row kernels** in widekit (`wk_soft_row`, `wk_copy_masked`, `wk_fill_masked8`, `wk_or_nz8`, `wk_nz8`, `wk_rev16/8`, `wk_mix565_row`, and the
   blend): 8 pixels a step, written once with GCC vector types, so the Vita build is NEON and the host build SSE2 (the host count is then like
   the Vita's); the plain C versions are the definitions (`make -C widekit test`, and `arm-check` under qemu: 40,000 runs, 0 different).
3. **Less repeated work**: the learned map's answer for a cell is asked once per tile row, layer and side instead of once per line (adapter memo;
   the same in `wk_map_serve`); a tile row is decoded from the bit planes through a table; the ambient row is made by the soft row kernel instead
   of a cache that missed every frame; the soft learned pixels are blended a run at a time; the census counts through a histogram.
4. **A side that shows the ambient frame is not composed on the frames whose coverage is not counted** (option `lazy`, default on). Auto counts
   the coverage on every second frame and can only change its mind there, so on the frames between a side that is ambient and not fading shows
   none of its composed pixels and nothing reads them later. The extents, the trimming by the learned map and widekit's hysteresis still advance
   for that side (`wk_compose_in.side_skip`), so the frame on which Auto turns the side back on is composed from the same state. One thing is not
   kept: the "previous row" a ghost fade (3 frames after a layer's extent shrank by 12 px or more) blends from; such a ghost in the first frames
   of a cross-fade out of the ambient frame is marked in the provenance as before but not blended (its weight in the picture would be at most
   3/32 of 24/32). On the 15 games (2 x 45,000 frames) this never changed a pixel: the hashes with `lazy` on, off and of the tree before are equal.
   The heavy games spend most of their time there: Yoshi's Island and Kirby Super Star show the ambient frame on 81 to 82 % of the first 1560 frames
   of the script at margin 96, Super Mario World on 53 to 62 % (left / right), and that is also where compose cost most (many lines whose tilemap ring is shorter than the margin).
5. **Margin passes the adapter never reads are not drawn** (`snes/core/gfx.cpp`, `SWideLine.pflags`; every edit is in `core/PROVENANCE.md`). The
   core applies the adapter's own rules to the line record it has just written:
   - windows, colour math on a layer or clip-to-black (the adapter shows only the composite and the sprite pass there): the BG passes are left out
     (composite + sprites: 2 passes instead of up to 6);
   - no window and no colour math: the composite is left out; the adapter builds the columns it shows from the sprite and BG passes by the depth
     byte each pass keeps per pixel (`derive_composite`);
   - colour math on the backdrop alone (Super Mario World, Yoshi's Island: a sky gradient with a far layer on the sub screen): the composite pass
     is drawn with the sub screen and the backdrop only, the main screen's layers are laid over it by depth, and with one sub screen layer that
     picture is also that layer's pass (4 passes instead of 5, and the heaviest one without its main layers).
   `verify_cache=1` makes the core draw the whole composite as well and the capture compares it with what would be built, over the whole margin
   of every such line.
6. Small: the palette hash is kept across batches; the margin strips of the pass depth buffers are cleared as one run per row at wide margins.

W4 of the performance design, honestly: step 4 (compose cost) is continued by 1 to 4 above. Step 1 (one margin pass with a tag byte) is done in
another form, 5 above: fewer passes, the depth byte as the tag; a single pass is not possible without changing the picture because compose needs
each layer's own pixels under the layers above it (the fade of a foreground whose extent is shorter). Step 2 (merging margin batches across
lines) is **not done**: in the measured scenes the heavy games have 1 to 9 batches a frame, so it would gain nothing there; a game with a batch
per line (a line by line HDMA effect) still pays a call per line and pass. Step 3 (caching the strip between frames) is **not done**. Also **not
done**, and the one thing that would take the margin passes off the PPU thread: a third copy of the renderer on its own thread that replays the
same slots and draws only the margins (the slot ring would need a second reader, `rcopy.sh` a second rename map).

Cost now (Minstr per frame; frames 1500 to 1560 of the owner's script; in brackets the start of this step, same method). "Counted frame" is a frame
whose coverage is counted (`lazy=0`: every frame is composed whole), the dearest kind:

| game | off | margin 41: total | compose, average / counted frame | core's passes + capture | margin 96: total | compose, average / counted frame | core's passes + capture |
|---|---|---|---|---|---|---|---|
| Yoshi's Island (Super FX) | 7.84 | 11.34 (13.71) | 2.51 / 3.01 (4.46) | 0.99 (1.41) | 12.68 (18.27) | 3.31 / 4.19 (8.02) | 1.53 (2.41) |
| Super Mario World | 6.95 | 11.31 (13.95) | 2.79 / 3.31 (4.77) | 1.57 (2.23) | 13.14 (19.24) | 3.33 / 4.02 (7.97) | 2.86 (4.32) |
| Super Mario Kart | 5.00 | 8.96 (10.75) | 2.67 / 2.94 (3.82) | 1.29 (1.93) | 10.36 (14.26) | 3.20 / 3.50 (5.93) | 2.16 (3.33) |
| Super Metroid | 6.96 | 9.51 (9.97) | 0.95 / 0.95 (1.41) | 1.60 (1.59) | 11.35 (12.57) | 1.19 / 1.19 (2.40) | 3.20 (3.21) |
| Kirby Super Star (SA-1) | 6.25 | 10.03 (12.76) | 2.85 / 3.58 (5.13) | 0.93 (1.38) | 10.94 (16.15) | 3.32 / 4.19 (7.71) | 1.37 (2.19) |

Per margin pixel (2 x margin x 224 pixels), compose: at margin 96 about 74 to 77 instructions on average and 81 to 97 on a counted frame (was 138 to
186; Super Metroid 28, was 56); at margin 41 about 137 to 155 on average and up to 195 on a counted frame (was 208 to 279; Super Metroid 52). The
target of the design (under 100) is met at the full width and **not at margin 41**: about 1.1 to 1.4 M of compose is work per frame, not per pixel
(layer description, widekit's analysis, the learned map, the ambient colours). Of a counted frame at margin 96, 2.4 to 3.0 M is the per line part
that the workers share.

Per thread on the Vita (model: x86 instructions counted as ARM instructions at 444 MHz, IPC 1.0 / 0.7; ms per frame, 16.7 is a 60 Hz frame; CPU =
65816, cartridge chip, DMA, PPU registers; PPU = the renderer, the margin passes and the capture; draw = compose of a counted frame at margin 96,
alone and with one helper sharing the lines; the draw thread also has the scaler, about 3 ms typically, and the upload, 1.5 ms):

| | CPU | PPU, wide off | PPU, margin 41 | PPU, margin 96 | APU | compose 96, alone | compose 96, one helper |
|---|---|---|---|---|---|---|---|
| Yoshi's Island | 7.7 / 11.0 | 5.4 / 7.8 | 7.5 / 10.6 | 8.7 / 12.4 | 4.5 / 6.5 | 9.4 / 13.5 | 6.0 / 8.6 |
| Super Mario World | 4.3 / 6.2 | 5.9 / 8.5 | 9.2 / 13.2 | 12.2 / 17.4 | 5.4 / 7.7 | 9.1 / 12.9 | 6.1 / 8.7 |
| Super Mario Kart | 3.2 / 4.6 | 3.4 / 4.8 | 6.1 / 8.7 | 8.0 / 11.4 | 4.7 / 6.7 | 7.9 / 11.3 | 5.2 / 7.4 |
| Super Metroid | 3.0 / 4.3 | 8.3 / 11.8 | 11.7 / 16.7 | 15.3 / 21.9 | 4.4 / 6.3 | 2.7 / 3.8 | 1.7 / 2.4 |
| Kirby Super Star | 6.9 / 9.8 | 2.4 / 3.5 | 4.3 / 6.2 | 5.3 / 7.6 | 4.8 / 6.8 | 9.4 / 13.5 | 6.2 / 8.8 |

Reading it. At IPC 1.0 every thread of the five games fits a 60 Hz frame at the full width (Super Metroid's PPU thread with 1.4 ms to spare). At
IPC 0.7 the draw thread fits with a helper (about 13 ms with the scaler and the upload; alone it does not for Yoshi's Island, Super Mario World and Kirby), the
CPU and APU threads fit, and the PPU thread of Yoshi's Island, Mario Kart and Kirby fits; **the PPU thread of Super Mario World (17.4 ms) and of
Super Metroid (21.9 ms) does not fit at margin 96**, and Super Metroid's is at the limit at margin 41 (16.7 ms): there the player's pacing reports
pressure and the ladder narrows the margin, as before. Which IPC is the Vita's is not known; the first console run decides. What would remove the
PPU thread's share is the renderer copy for the margins named above. The helper is the scaler's (one on three cores, on the core the plan gives it,
two with the fourth core); how much of a frame it really gets there is also for the console run to show (the player's log has `wide_lines jobs= helped=`).

Proofs of this step (x86 twin, final tree; the owner's 15 games, 3000 frames of the pad script each):
- wide picture: the per frame hashes of the whole wide picture, its centre and the claimed real extents, and of the normal picture, are equal to
  those of the tree before this step, margins 41 and 96, all 15 games (30 runs, 90,000 frames); the same with 3 line workers and with `lazy=0`
  on five games at margin 96 (10 runs);
- `verify_cache=1`, margins 41 and 96, 15 games: 35.1 M checks (ring walk, ambient rows, and the built composite against the drawn one), 0 different;
- `wide_compare.py` against the tree before the wide view: frames, audio and state identical, single thread and with the PPU and APU threads,
  margin 41, and margin 96 with 3 line workers: 15 identical each;
- ThreadSanitizer, 900 frames at margin 96 on Super Mario World, Yoshi's Island and Kirby Super Star and 600 at margin 41 on Super Metroid and Super
  Mario Kart, each as single call, 3 line workers, capture / compose split with 3 workers, and split with the PPU and APU threads and 4 workers
  (with map save / census calls from the frame thread): same hashes in all four, 0 reports;
- `selftest.sh` 58 passed, 0 failed (truth ROMs: 0 wrong real pixels; ladder floor; pressure; player test); `make -C widekit test` 1101 checks,
  0 failed; `arm-check` (NEON kernels and whole frames against the host) equal; NES `wide_golden.sh` with `WG_VERIFY=1`: 37 files match;
  `make -C playerkit test` 252 tests, 0 failed; `scale_proof` (xBR 2x of the wide picture at margin 96, 900 frames, three heavy games): 0 mismatches;
- the armhf build (NEON kernels) of `snes_wide_twin` under qemu-arm, margin 96, 2 line workers, 3000 frames, on Super Mario World, Kirby Super Star,
  Yoshi's Island, Super Metroid and Super Mario Kart: wide and normal hashes equal to the x86 reference tree.

Not proven: anything on a Vita. The player's side of the line workers (`wide_pfor` in `snes/vita/main.cpp`, the scaler's helpers) is compiled for
the Vita and follows the twin's tested scheme, but has not run; its log line `wide_lines jobs= helped=` shows per second how many line jobs there
were (three a frame) and how many a helper joined. Memory: each of the four workers keeps about 105 KB (rows and a 512 row soft row cache).

## Rewind capture, second step (20 Nov 2026)

- **Where it runs.** The player no longer writes rewind's state between the end of the frame and the hand-over of its picture. `pk::Speed` has an
  opt-in (`set_defer_capture`): `run_frame()` only notes that a frame ran, and the platform calls `capture_idle(late)` once per refresh at the
  point where the game thread has handed the picture and the sound on and would only wait for the next refresh. A pass that has already missed
  its refresh puts the capture off, at most 8 passes in a row. The state written is the machine's at that moment on the same thread (nothing
  emulates in between), and the frames count towards the interval as before (playerkit tests: the same states are captured as inline; put off
  but not for ever). So the copy (still about 300 KB plain, 430 KB Super FX, 570 KB SA-1; estimated 0.5 to 1.2 ms) is no longer on the frame's
  path: it costs the frame nothing while the game thread has that much time left before the refresh (by the model it has 5 ms or more in the heavy
  games), and at most one late pass in nine otherwise. A dirty page capture is **not done** (the SNES RAM and the chips' RAM have no write hook).
- **The SA-1 bound, decided.** 264 KB was a hint with a scan of the rest of the 512 KB block at every capture (reading 250 KB to save writing
  it). The bound is now derived (the comment above `g_rw_ext` in `snes/frontend/host.cpp` lists every access path): the SA-1 and the 65816 reach
  the block below 256 KB except through the SA-1's bitmap view of the $6000 window, whose highest byte is 0x407FF, and through the SA-1's DMA into
  BW-RAM, which ends below twice the header's RAM size. A capture writes max(0x41000, 2 x RAM size) bytes of the block (260 KB for the usual
  8 to 64 KB cartridges; the whole block when the header says 256 KB). The scan is kept as a net, not as the proof: at the first capture after
  rewind opens and after every state load, and every 32nd capture for the chip cartridges; a byte found there makes every later capture write the
  whole block. Checked on Kirby Super Star, Super Mario RPG (SA-1), Yoshi's Island, Stunt Race FX, Doom (Super FX), Super Mario World and Super
  Metroid: 300 captures each equal the whole state byte for byte, the part left out stays zero, and a byte put there by hand with a state load is
  found by the next capture (`player_twin costs`, 621 checks each, 0 failed).
