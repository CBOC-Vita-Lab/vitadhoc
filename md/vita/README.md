# md.bin: the Sega player

One program for the Mega Drive / Genesis, Master System, Game Gear and SG-1000 (console ids `md`, `sms`, `gg`, `sg`), built on the
Genesis Plus GX core (`md/core`, through the `md_host_*` API) and playerkit. It follows the SNES player (`snes/vita`) piece by piece.
It links only `md/`, `playerkit/` and the vendored libvita2d (`snes/frontend/third_party/vita2d`, a third party library shared by the
players); nothing from `gpsp/` (`make -C md/vita check-links`).

Label of every number below: **host** = measured on the Linux twin (x86), **qemu** = armhf under qemu (correctness only, never time),
**model** = computed from a host instruction count with the cost model, **estimate** = a judgement. Nothing here was measured on a console.

## Files

| File | What |
|---|---|
| `player.{h,cpp}` | `mdp::MdPlayer`: launch protocol, playerkit menu and host rows, states, battery RAM writer thread work, cheats, patches, resume point, steady pacing, look governor, session report, log. Portable (no console calls) |
| `pipe.{h,cpp}` | the picture hand-over to the drawing thread (playerkit `pk_handoff`) |
| `pal_pace.h` | frames owed per refresh for a 49.70 Hz game on a 59.94 Hz screen (5 in 6) |
| `main.cpp` | the console side: vita2d pipeline, drawing thread, scaler helpers, audio thread, writer thread, pads, touch, keyboard, clocks, census lines |
| `../frontend/picture.{h,cpp}` | layout (Original / Zoom / Full / Pixel perfect), settings file, scaler ceilings, the Game Gear LCD table |
| `../frontend/cheats.{h,cpp}` | Game Genie / Pro Action Replay / Fusion decoders (ports of `tools/make_cheatdb.py`), the `.cht` list |
| `../frontend/savefile.{h,cpp}` | the safe `.srm` write with the verified backup |
| `../frontend/audio_pace.{h,cpp}` | the audio pacer (a copy of the SNES one, MIT) |
| `../twin/player_twin.cpp`, `handoff_proof.cpp`, `player_selftest.sh` | the tests, below |
| `../../playerkit/src/pk_scale.{h,cpp}` | MMPX 2x, xBR lv2 2x / 3x on the CPU (GCC vector extensions) |

## What it does

- **Launch protocol** the same as `snes.bin`: `launch/<console>.ini` in, `launch/session.txt` out. Four launch files can sit side by side;
  the player takes the one whose `game=` is its argument, else the newest. The console id decides how a file is run (a Game Gear game with an
  `.sms` name is started with `console=gg`); without a launch file the extension decides.
- **Save states**: 10 slots with a thumbnail and a scene picture, Continue (resume point, written when the player ends, kept per patch key),
  Undo load, atomic writes by the writer thread. A state is ~145 KB for a Mega Drive game (host).
- **Battery RAM** as a RetroArch `.srm` (the raw bytes). The writer thread writes it with the safe write (temp file read back, old file kept
  until the new one is in place) and, the first time a session replaces a file, a verified backup `backups/<console>/<game>.<crc>.<size>.bak`.
  A RetroArch save newer than ours is brought in (the old one is kept as `.before-retroarch.srm`). The RAM is hashed every 8th pass and
  `md_host_sram_dirty()` is read; the file is written once a change is more than 2 s old and has been quiet for 1 s (or every 30 s while the game keeps changing it), and after the menu is on screen; a failing card is retried after 10 s, doubling up to 60 s, and the old file is never touched.
- **Patches** (IPS / BPS / UPS, applied to the ROM in memory; a patch that does not fit stops the start and tells the app), **cheats** (a typed
  code through the system keyboard: Mega Drive Game Genie and Pro Action Replay / RAM `AAAAAA:VVVV`, Master System / Game Gear Game Genie
  `DDA-AAB[-RRr]`, `AAAA:VV[:CC]`; the `.cht` file of the game with each entry on or off, delete, up to 64) and the app's database entries (`cheats_db=`).
- **Settings rows** (the playerkit host rows): Pixel art scaler, Image look, Hide masked edge (Master System), LCD colours and LCD ghosting
  (Game Gear), Pad type (Mega Drive), Region, FM sound (Mega Drive), FM sound unit (Master System), Steady pacing, PAL games speed, Smooth hand-over.
  The two-player row is hidden (no `CAP_PLAY_TOGETHER`), the wide view row is greyed with its reason.
- **Controls**: Mega Drive A Square, B Cross, C Circle, X L, Y Triangle, Z R, Start, Mode a tap of Select (Start + Select held opens the menu;
  Select + L / R quick save / load). Master System / SG-1000: Button 1 Cross, Button 2 Circle, Start = Pause; Game Gear Start. Square and
  Triangle are the turbo buttons of the 8 bit consoles. The 6 button pad follows the ROM header, or is forced (Pad type row).
- **Picture**: the whole parity set: screen sizes Original (the display aspect ratio the core reports), Zoom (blank rows cropped), Full, Pixel
  perfect (the console's pixel shape or square); sharp bilinear; sharp / smooth; ambient glow; scanlines; LCD grid; bezel PNG
  (`overlays/<game>.png`, else `overlays/<console>.png`); Game Gear LCD colours (a 64 K entry table: lifted blacks, softer colours).
- **Timing**: PAL games run at true 50 Hz (5 of 6 refreshes, `pal_pace.h`), Smooth 50 Hz (the default: the sixth refresh's frame runs one refresh early and is shown mixed with the one before, `playerkit/src/pk_palsmooth.h`) or Fast 60 Hz; the region row changes the core's frame rate and
  the loop follows (mailbox frame time, speed meter, pacing); audio is made at 48 kHz by the core and paced by `audio_pace`.
- **Threads** (playerkit `pk_cores`, profile `CP_MD`, so the fourth core is used only when the CapUnlocker probe proves it):

  | thread | core (3 cores) | core (4 cores) | priority |
  |---|---|---|---|
  | emulation (the core is one thread) | 0 | 0 | 0x60 |
  | drawing: upload, scaler, bands, present | 1 | 1 | 0x60 |
  | audio out | 2 | 3 | 0x40 |
  | writer | 2 | 3 | 0xA0 |
  | scaler helper 1 | 2 | 2 | 0xA0 |
  | scaler helper 2 | - | 3 | 0xA0 |

  The helpers are work stealers: the drawing thread cuts the picture into 8 row chunks, takes chunks itself and the helpers take what they get
  to; a helper that is not scheduled costs nothing. `threads.ini` (`<root>/md/threads.ini`): `core4=auto|off`, `audio_prio`, `main_prio`,
  `draw_prio`, `helpers=0..3`, `audio_legacy`.

## The pixel art scaler and the look governor

The scalers (`playerkit/src/pk_scale.cpp`): **MMPX 2x** (MIT, McGuire and Gagiu; invents no colours), **xBR lv2 2x and 3x** (Hyllian's shader, MIT,
`licenses/xBR-MIT.txt`, ported from the shader's own code; `xbr_reference()` is the literal transcription and the fast path is bit identical to it
on the test pictures). xBRZ is GPL and is never linked.

Cost (instructions of one picture, **host** x86 callgrind, a sprite-like test picture at 50 % and 100 % busy; **model** milliseconds = instructions /
350 M per second, the single thread figure, with an error bar of 1.5x either way):

| picture | MMPX 2x | xBR 2x | xBR 3x |
|---|---|---|---|
| Game Gear 160x144 | 3.0 - 3.6 M = 8.5 - 10.4 ms | 2.6 - 3.5 M = 7.4 - 10.0 ms | 3.5 - 4.7 M = 10.0 - 13.4 ms |
| Master System 256x192 | 6.4 - 7.9 M = 18 - 22 ms | 5.7 - 7.7 M = 16 - 22 ms | 7.7 - 10.3 M = 22 - 30 ms |
| Mega Drive 256x224 (H32) | 7.5 - 9.2 M = 21 - 26 ms | 6.7 - 8.9 M = 19 - 25 ms | 9.0 - 12.0 M = 26 - 34 ms |
| Mega Drive 320x224 (H40) | 9.4 - 11.4 M = 27 - 33 ms | 8.3 - 11.1 M = 24 - 32 ms | 11.1 - 15.0 M = 32 - 43 ms |

(Kind order in `ScalerKind` is MMPX, xBR2, xBR3; xBR 2x comes out a little cheaper than MMPX because it skips flat pixels earlier.) A game that
has no scaler pays one memcpy of the picture. One thread cannot hold 16.7 ms on anything but the Game Gear; with the drawing thread and one helper
each carrying half the rows the model gives roughly half those figures, with the emulating thread free at the same time. That sets the **ceilings**
(`pic_scaler_ceiling`, the best tier Auto starts at):

| native picture | ceiling | output pixels |
|---|---|---|
| 160x144 (Game Gear; the wide view's 256x144 band too) | xBR 3x | 480x432 (768x432 wide) |
| 256x192 (Master System, SG-1000; the wide view's up to 338x192 too) | xBR 3x (owner decision, 2.0: was xBR 2x) | 768x576 (1014x576 wide) |
| 256x224 / 256x240 / 320x224 / 320x240 (Mega Drive H32 and H40, Master System games of 224 / 240 rows) | xBR 2x | 512x448 .. 640x480 (the wide Mega Drive picture, up to 512x224, is 1024x448) |
| interlaced (more than 256 rows) or wider than 352 (the normal picture's size is what counts) | none (sharp bilinear) | |

The ladder is xBR 3x -> xBR 2x -> sharp on the Master System, SG-1000 and Game Gear, xBR 2x -> sharp on the Mega Drive. 3x on a 192 row picture makes 576 rows
for a 544 row screen: a little more than the screen can show, so the fit averages the surplus away; the look governor steps it down when the frame budget needs it.
**Budget of xBR 3x with the wide view on the Master System** (`scale_proof --widen 41`, three homebrew games, 150 frames, the picture the player scales: 338x192 with the
soft extension): the player scales incrementally (`pk_scale` ScaleCache: only the 8 pixel blocks that changed), per picture **host** average 0.05 - 0.10 ms, p99 0.4 - 0.9 ms; **qemu**
(armhf, ratio only, not Vita time) average 0.20 - 0.43 ms, p99 1.3 - 4.0 ms; the full recompute at a scene cut costs host 0.7 - 2.5 ms, qemu 2.2 - 19 ms (p99), which the drawing
thread and the two scaler helpers share and which the emulating thread (a Master System frame is about 0.08 ms host, 0.6 ms qemu, 3 - 4 ms model on the Vita) never waits for. Within
the 16.7 ms budget on every measured frame; xBR 3x on the 256 wide picture without the wide view costs about 40 percent less (host average 0.03 - 0.06 ms).

3x was kept for the Game Gear only before 2.0: on 192..240 rows it makes 576..720 rows for a 544 row screen, computing pixels that the fit averages away
again. `md/twin/sidebyside.{cpp,py}` makes the 960x544 comparison (xBR 2x | xBR 3x | difference x8, each fitted with the player's bilinear draw);
over 8 Mega Drive and 7 Master System pictures the mean absolute difference is 0.05 .. 1.0 (Mega Drive) and 0.2 .. 3.8 (Master System) of 255,
and the differences are one screen pixel wide along edges. MMPX 2x is no longer a rung: since the integer NEON xBR (`pk_scale`) xBR 2x costs less than
MMPX 2x, so MMPX stays a look the player can pick by hand in the Pixel art scaler row, never one Auto chooses. The look governor
(playerkit `pk_look`) owns the **Auto** scaler row: it starts at the ceiling and takes one tier off (xBR 3x -> xBR 2x -> sharp) when
this game cannot hold full speed, remembers it per game (`look/<console>/..`), gives it back when there is room. A scaler chosen by hand in the row
is never eased. **Image look**: Auto (the governor), Full (the scaler as set, never eased), Light (no scaler). The scaler of a picture is the picture's
own ceiling plus the governor's steps, so a game that switches H32 to H40 follows without a new ladder.

**xBR result:** the port is correct (the bit-identity test against the literal shader transcription, `playerkit/tests/test_scale.cpp`) and a console
can afford it on the Game Gear and, split over two threads, on the 256 wide pictures; on the 320 wide Mega Drive picture the model says MMPX at best, and
a busy scene will make the governor step down to sharp bilinear. All of that is model, not measured: the first console log decides the ceilings.

## Reading the log (`<root>/<console>/log.txt`)

One line a second: `t=N fps= emu_fps= emu_ms= ... | scaler=<kind> pics= ms/pic= nonflat/pic= helper_chunks= waits= | per s: ... pace=...`, then `threads ms/s: main= audio=
draw= writer= scale1= scale2=`, `handoff: on= depth= mode= produced= shown= repeated= dropped= gate_waits= ... draw_ms=`, `look:` (the governor's census), and the audio line `A t=...`.
`census` lines of `pk_cores` say which cores are in use. `timing:` appears when the region changes the frame rate.

## Tests

`md/twin/player_selftest.sh` (needs `MD_TEST_ROMS`, `MD_TEST_SMS_ROMS`; they SKIP without; no ROM is in the repository). Counts are checks per test program run
(**host**, x86, ASAN-free build):

| test | checks |
|---|---|
| picture ceilings, layout, LCD table | 22 |
| Mega Drive walk: menu pages (25 screenshots), 10 slots with thumbnails, a write failure that keeps the slot, load and undo, Continue and the patch key, battery RAM write and verified backup, RetroArch import, IPS patch, a failed patch, typed cheats (bad code, GG / PAR, on / off, delete, `.cht` round trip, database entry), region rows, pad remap, session.txt | 248 |
| Master System walk (same) | 248 |
| Game Gear walk (an SMS ROM with `console=gg`) | 248 |
| pad models: Mega Drive 19, Master System 15, Game Gear 15 | 49 |
| look governor: Mega Drive 27, Master System 27, Game Gear 28 | 82 |
| region and PAL pacing (49.70 Hz over 3000 refreshes: 2487 frames, -0.023 %; NTSC untouched) | 10 |
| smooth hand-over on / off bit for bit (Mega Drive, Master System) | 608 each |

## For the app side (recipe)

1. Write `launch/<console>.ini` with `console=md|sms|gg|sg` and the common keys (`playerkit/FORMATS.md`), then `sceAppMgrLoadExec("app0:md.bin", {game path})`.
   Read `launch/session.txt` when `eboot.bin` starts again.
2. New key `cheats_db=<path to a .cht>`: export the database cheats for the game (RetroArch `.cht` format; Genesis codes are Game Genie, PAR or Fusion
   RAM / ROM text, 8 bit codes `DDA-AAB[-RRr]` or `AAAA:VV[:CC]`). The player merges entries it does not have yet, switched off.
3. Console ids and extensions: `md` = `.md .gen .smd .bin`, `sms` = `.sms`, `gg` = `.gg`, `sg` = `.sg .sc`.
4. `gpsp/vita/Makefile` packs `md/vita/build/md.bin` into the VPK next to `snes.bin` (see the line added there).
5. Per-game overrides `ovr.<key>`: `scaler`, `region`, `fm`, `pad_type`, `scanlines`, `glow`, `lcd_grid`, `gg_colors`, `look_mode`, `pal_speed`.

## Wide view (Picture page, Auto / On / Off: the same row and words as the Super NES and NES players)

The console's picture stays pixel exact in the middle and the rest of the screen's width (Original shape: 960 of 960 px at 544 rows) is filled with
more picture: **real** picture where it can be shown truthfully, and a **soft extension** of the picture's own edge colours where it cannot. On by
default; Off gives the 4:3 picture back. The emulation does not know: picture inside the original window, sound, state and RAM are bit for bit the
same with the view on or off, with and without the VDP worker thread (`selftest.sh` section 11 checks it on every test ROM, `arm_check.sh` on armhf).

| Console | Margin per side | What is beside the picture |
|---|---|---|
| Mega Drive / Genesis | 51 px (H40) at the Original shape, about 41 px (H32) (at most 96; `pic_wide_margin` of the player's 960x544 screen) | the VDP draws a narrow strip beside the picture for every line: the planes and sprites that exist beyond the edge (`core/vdp_render.c`, `wide_line()`, in the VDP worker's copy too). `md/frontend/wide.cpp` hands the name tables of planes A and B to widekit (`wk_layers`: what is the level, the backdrop, a HUD; which columns the game has shown or written) and `wk_compose` shows the strip where every plane in it is trusted and fades into the soft extension where it is not. A plane widekit does not trust (a fixed HUD plane) is left out of the strip, so the other plane, the sprites and the soft extension show there. **Learned picture**: cells the game showed earlier at the world columns now beside the screen (`wk_map`) are written into copies of the name table rows (`md_host_wide_row`) and the VDP draws the strips from those, with the other plane and the sprites as on the screen; a strip pixel counts as real only when the row it was drawn from is the row the map vouches for at analysis time |
| Master System | 41 px at the Original shape | the tile map is exactly the screen wide, so nothing exists beside the picture in the console: the strips are the **learned picture** only, drawn by `wide.cpp` from the cells the picture has shown (`wk_map`), with the tile graphics and palette of the frame, and only while a cell's tile graphics are what they were when it was seen. Where a game masks the first 8 columns, those show the learned picture too on the lines whose left strip is real (the border colour stays on the others), and the soft extension continues the picture, not the border |
| Game Gear | 25 px at the Original shape (at most 48) | the VDP always draws 256 x 192 and the LCD shows the middle 160 x 144; the wide view draws the 256 columns of the LCD's rows (`gg_band`: the same machine, nothing else changes) as far as the name table can be trusted, then learned cells, then the soft extension |
| SG-1000 | 41 px at the Original shape | the soft extension only (no scroll registers: there is no place to learn the picture by) |

Lines the Mega Drive strips cover (2.0): every mode 5 line that is not interlaced. Under a **window plane** the side the window takes gets a strip
without plane A (the window is never continued; plane B and the sprites are) where the window's two cells at that edge are empty, and no strip where a
panel reaches the edge. With **per 2-cell vertical scroll** a side gets its strip where the three columns next to that edge scroll alike (and the partly
shown left-most column too); the strip scrolls like the edge column. A linked two-player game never widens its picture.

**Who does what** (the point is that the emulating thread pays nothing):

| Step | Thread | Cost, model (350 M instructions a second; host callgrind counts; x1.5 either way; not measured on a Vita) |
|---|---|---|
| strips | the VDP worker, inside its line pass; a line's strips are copied from the last frame while nothing they are drawn from changed | 0.14 - 1.13 M instructions a frame (0.4 - 3.2 ms): 0.6 M on a platformer, 1.1 M with shadow / highlight on every line |
| analysis (name tables, `wk_layers`, `wk_map`, the learned rows, what is real) | the VDP worker, after the frame's last line, while the emulating thread goes on (`md_host_wide_run`); the name tables are reread only when the game wrote them, the map only when the screen shows other cells | 0.64 - 0.85 M (1.8 - 2.4 ms). Without the worker (the 8 bit consoles by default) it runs on the emulating thread: 0.31 - 0.87 M on a Master System game that takes 0.9 - 1.2 M itself |
| hand-over | the emulating thread | under 2 K instructions (it was the whole analysis, 0.63 - 0.85 M, 1.8 - 2.4 ms) |
| compose (`wk_compose`) | the drawing thread; the lines are shared with the scaler's helper threads (`compose_begin / compose_lines / compose_end`) | 0.69 - 0.86 M in all (it was 0.75 - 0.93 M), of which 0.47 M are lines any helper can take: 0.54 M (1.6 ms) on the drawing thread when two helpers take their share |

Heaviest samples, wide view on at full width, per thread (model): xgm-player emulating thread 7.4 ms, VDP worker 10.8 ms; h-int/wobble 7.0 / 13.4 ms;
starfield-donut 6.9 / 10.9 ms; platformer 8.3 / 8.6 ms; bad-apple 7.1 / 7.1 ms; against 16.7 ms. Before this work the frame was over when the worker
had finished and the emulating thread had analysed after it: 11.3, 16.1, 10.9, 10.2 and 9.4 ms for the same five. Table and method: `md/twin/baseline_results.md`.

**The ladder stays as the safety net**: `main.cpp`, `wide_cost_update` watches the smoothed time of the emulating thread's frame (emulation and the wait for
the worker, so a worker that is too busy shows) against the frame time: over 90 percent the real strips are halved (rung 1), then dropped (rung 2: soft
extension only); under 62 percent for 240 frames it steps back. "On" keeps rung 0. By the model no measured ROM reaches the line; at the model's upper error
bar (x1.5) wobble does and xgm-player comes close. A log line `wide: margin= strips= rung= handover_us/fr= compose_us/pic= real= soft_lines= map_cells= map_px= rows=
strips_copied= drawn= compose_chunks_by_helpers=` appears each second.

**Proof that the sides are true** (`selftest.sh` section 12, tools in `md/twin/tools`): test cartridges written for the purpose, whose worlds are known
functions (Master System with and without the masked columns, Game Gear, and a Mega Drive cartridge with a window plane and per 2-cell vertical scroll),
are played through a script of scrolling out and back and every margin pixel shown at full weight as real is compared with the world: 0 wrong of 1.7 M
(Master System), 1.7 M (the same without the mask), 1.8 M (Game Gear) and 1.1 M (Mega Drive) pixels. On the Mega Drive samples every learned cell shown
beside the screen is compared with the cell the game shows when the screen gets there (`md_twin --wide-check`): 0 different. The worker's strip cache
is compared with a fresh draw of every cached strip (`--wide-cache-check`): 0 different on the 96 test ROMs. What the analysis decides is the same on the worker
thread and on the emulating thread (`selftest.sh` 11b), on armhf and x86 (`arm_check.sh`), and through the picture pipe with the compose shared between three
threads (`wide_pipe_proof`, also under ThreadSanitizer).

**Not done, said plainly**: the learned picture lasts for the play session (no file; a state load, a rewind or a reset forgets it) and shows only cells
seen since the level was recognised (the Super NES player also shows older memories at half strength). SG-1000 has no learned picture. Master System
games in the 224 / 240 line modes, or with the right 8 columns scrolling apart, get the soft sides. The Mega Drive's interlaced modes get the soft sides.
On lines whose columns scroll apart vertically the learned rows are not used (the strip is the ring's own content or nothing). No commercial ROM and no real
Game Gear or SG-1000 ROM was available: the 8 bit learned picture was tested with Master System homebrew and the test cartridges. Nothing has run on a Vita.

## Hooks left in place

- **Rewind / fast forward**: `MdPlayer::set_extra_caps(CAP_REWIND | CAP_FF)` before `start()` shows the menu rows; the Mapper already reports `ACT_FF`
  and `ACT_REWIND` (`last_input().fired / held`); `main.cpp` has the loop pass where frames are skipped or run again (`md_host_set_render(0)`).
- **RetroAchievements**: `md_host_ra_read` and `md_host_ra_console` give the memory map; `MdPlayer::after_frame()` (once per emulated frame, game thread) and
  `notice()` are the places the app side module plugs into.
- **Two players / online**: done (2.0). The app forms the room (game list > Settings > Two players; not for the Game Gear) and writes `link.*`; `start()` reads it, `netplay.{h,cpp}` runs playerkit's `pk_link` (MIT) on the project's UDP transport and NAT layer (`gpsp/netdrv/transport_lan.c`, `nat_*.c`: MIT OR GPL-2.0-or-later), and `main.cpp` steps it instead of `md_host_run_frame()`. Nothing of the main app's Game Boy link (netdrv.c, fe_gblink.c, fe_evt.c, fe_util.c) is linked: `make -C md/vita` fails if it is (check-licence). The in-game card stays greyed with a reason (the game list starts it). Pinned while linked: region Auto, pad type Auto, FM discrete, FM unit Auto, no rewind/fast forward/cheats/states, every frame drawn. Test: `md/twin/link_test.py` (Makefile.link).
- **Wide view**: done (2.0), see "Wide view" below. `MdPlayer::wide_apply()` (player.cpp) decides the margin, `main.cpp` draws the strips and composes.
- **Adding a look rung**: a line in `kLookLadder` (player.cpp) and its effect in `look_apply()`.

## Known limits

- The scaler ceilings are a model. No console measurement exists yet. Master System and SG-1000 start at xBR 3x (the wide picture is up to 338 x 192, 1014 x 576 scaled; the governor steps it down to xBR 2x and sharp), the Game Gear at xBR 3x, the Mega Drive at xBR 2x.
- A Game Gear state picture carries the LCD colours (it is read back from the picture texture).
- A game that changes between H32 and H40 keeps the ladder it started with; its scaler follows the picture's own ceiling.
