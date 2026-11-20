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
  perfect (the console's pixel shape or square); sharp bilinear; sharp / smooth; side glow; scanlines; LCD grid; bezel PNG
  (`overlays/<game>.png`, else `overlays/<console>.png`); Game Gear LCD colours (a 64 K entry table: lifted blacks, softer colours).
- **Timing**: PAL games run at true 50 Hz (5 of 6 refreshes, `pal_pace.h`) or Fast 60 Hz; the region row changes the core's frame rate and
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
| 160x144 (Game Gear) | xBR 3x | 480x432 |
| 256x192 / 256x224 / 256x240 / 320x224 / 320x240 (Master System, SG-1000, Mega Drive H32 and H40) | xBR 2x | 512x384 .. 640x480 |
| interlaced (more than 256 rows) or wider than 352 | none (sharp bilinear) | |

3x is kept for the Game Gear only: on 192..240 rows it would make 576..720 rows for a 544 row screen, computing pixels that the fit averages away
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

## Hooks left in place

- **Rewind / fast forward**: `MdPlayer::set_extra_caps(CAP_REWIND | CAP_FF)` before `start()` shows the menu rows; the Mapper already reports `ACT_FF`
  and `ACT_REWIND` (`last_input().fired / held`); `main.cpp` has the loop pass where frames are skipped or run again (`md_host_set_render(0)`).
- **RetroAchievements**: `md_host_ra_read` and `md_host_ra_console` give the memory map; `MdPlayer::after_frame()` (once per emulated frame, game thread) and
  `notice()` are the places the app side module plugs into.
- **Two players / online**: `set_extra_caps(CAP_PLAY_TOGETHER)`; `start()` already reads `req_.link`. Until then the card is not shown.
- **Wide view**: not implemented for Sega in this version. The Picture page shows the row greyed with the reason. `on_change(ROW_PIC_WIDE)` and `pic_geometry()` are the place.
- **Adding a look rung**: a line in `kLookLadder` (player.cpp) and its effect in `look_apply()`.

## Known limits

- The scaler ceilings are a model. No console measurement exists yet.
- A Game Gear state picture carries the LCD colours (it is read back from the picture texture).
- A game that changes between H32 and H40 keeps the ladder it started with; its scaler follows the picture's own ceiling.
