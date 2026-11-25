# Technical Notes

## Architecture

`gpsp/vita/Makefile` builds the native Vita app. gpSP supplies the GBA core
and ARM dynamic recompiler; TGB Dual supplies GB/GBC. The common frontend
owns cartridges, saves and link state. The Vita session loop is
`gpsp/vita/main_vita.c`, with separate network, local-pair, cheat, audio and
rendering modules. Host twins compile much of the same code for tests.

The Vita frontend reuses the PSP menu/settings logic through a compatibility
layer, but uses GXM/libvita2d, embedded Inter/Outfit fonts, Vita audio/input,
front touch and Vita networking. It does not run a PSP executable inside an
emulator. Original gpSP, GBAdhoc and dependency notices remain in the source.

## Display And Timing

- Per-console settings include 2x/3x, fit, stretch and 4x crop; nearest,
  bilinear, sharp bilinear, LCD, scanline, Scale3x and CPU xBRZ filters;
  colour correction and motion blending.
- Scale3x/xBRZ use worker cores, changed regions and a scrolling image cache.
  During heavy fades, smoothing can fall back to 2x inside existing textures.
- A GBA render worker draws from recorded scanline state. The main thread
  joins it before presentation. Two-player paths retain their own timing.
- Keep full speed can run up to two catch-up frames without drawing before
  a presented frame. Emulated frames, submitted pictures and actual displayed
  FPS are different measurements; a log counter alone is not a benchmark.
- Presentation waits before input sampling to avoid filling the display queue.
  Audio starts with a 60 ms cushion and can adapt up to 120 ms after underruns.
- Experimental GPU xBRZ is off by default: earlier Vita measurements were
  slower than CPU xBRZ. It needs the user's own shader compiler module and has
  a slow-frame fallback. No proprietary compiler is distributed.

GBA Wide view can draw beyond the original viewport using game profiles,
Mode 7 support or an experimental background cache/scout. Emerald has prior
testing; other profiles and generic/scout behaviour are not universally
accepted. Where information is unavailable, margins use the selected fallback.
It does not turn all games into native widescreen games or modify their saves.

## Version 1.3: battery saves

- **Nothing after the image is dropped.** `fe_host.c` `sram_load` keeps up to
  64 KiB the file carries after the core's 128 KiB image (`sram_tail`, e.g.
  mGBA's 16-byte clock block: 131072 + 16) and writes it back after every
  full rewrite; delta block writes never touch it. A file of that shape is
  now "exact", so it is not rewritten at the first scan. `gb_save_load` keeps
  the bytes past the layout the core writes (cart RAM + 44/48-byte clock
  block) in `gb_extra` and `gb_save_write` appends them. A file longer than
  64 KiB past the image is refused (never rewritten), as an unreadable one is.
  The GBA save the app writes is still the 128 KiB image (a 32 KiB SRAM or
  8 KiB EEPROM file comes out padded with 0xFF), the same bytes RetroArch's
  gpSP writes.
- **`vita/save_sync.c`** (`<data>/saves-sync.txt`, one tab-separated line per
  save: save path, RetroArch source path, source size / mtime / CRC as last
  synced, CRC of the save as last written, CRC when the source last got it).
  At game start, a save the app has not written itself (no record, or its CRC
  differs from the record) is copied once to `backups/<name>.<crc>.bak`.
  `save_import` links a save taken from RetroArch's folder (`ux0:data/
  retroarch/savefiles[/<core>]`, a folder under the data root on the twin) or
  `Game.srm` beside the game. If the linked source changed since the last
  sync, the newer file (mtime) is used and the other is backed up. After each
  complete battery write fe_host calls the weak `app_battery_written`, which
  streams the save into the source (`.import` temp, then replace): the
  source's size, the save's bytes as far as both reach, the source's own bytes
  (its trailer) beyond, and only if the source still has the recorded size and
  CRC, and no data of the save would be cut off. Off with `sync_ra_saves = 0`
  (Settings > Library; `ui_host_sync_ra_on/_set`).
- 1.4: `save_sync_heal_game` (called by game_boot after `save_sync_game_start`)
  gives every save without a linked RetroArch file a plain `Game.srm` where
  RetroArch looks, once per game (record flag), never over a different or
  newer file; see docs/SAVES.md for the rules, layouts and the log lines.
- Tests: `app_tests.py --only save_sync,save_heal,save_gb_keep,save_import` and
  `vita/tests/savesync/test_save_sync.c`, `test_backup_failure.c`,
  `test_legacy_heal.c` (the compile line is in each header).

## Version 1.3 Changes

**Smoothing is never lowered for speed** (issue #7: with full-screen CPU xBRZ
the game ran at about 60 emulated frames but fewer shown). A governor that
stepped xBRZ down (4x, 3x, 2x, GPU filter) when frames kept missing the
screen was written and then switched off: the picture the player chose is not
traded for frame rate. `upscale_gov_step` and its rules stay in
`vita/upscale_vita.c` (tested in `tests/upscale`), but `main_vita.c` calls
`upscale_gov_enable(0)`, so it never moves. The answer to #7 is cheaper
smoothing (the profiling pass below: word-wise changed-pixel mask, xBRZ
distance fast path, bit-identical) and Keep full speed's catch-up frames,
which skip drawing only; emulation and sound stay at 60 Hz.
`app_tests.py --only smooth_gov` checks that slow frames under xBRZ never
step the smoothing, with Keep full speed on or off.

## Version 1.3 Changes

### Audio investigation (issue #4: "distorted in every game")

Measured on the Linux twin with a simulated clock (bit-for-bit repeatable),
the output of the whole path (core mix, 32768 Hz ring, 48 kHz resampler) is
clean; no defect that explains "distorted in every game" was found there, so
that report is **not** confirmed fixed. What was measured:

- **Level and scaling** (`vita/hostcore/testrom/audio.c`, analysed by
  `tools/audio_phases.py`): Direct Sound at 100 % with a +-100 sample sine
  peaks at exactly 25600, at 50 % exactly 12800 (x256, then the 50 % shift);
  a full-volume PSG square peaks at 8192; two Direct Sound channels at
  100 % that sum past full scale clip hard at +-32767, as the hardware mixer
  does. Clipping was zero in a Pokemon Emerald run (title, intro and the
  first menu screens, 40 s: peak 20420 = 62 % of full scale, 0 clipped, 0 wrap
  jumps, 0 steps above 12000, mean -1246).
- **Wrap-around:** every stage saturates (`sound_read_samples`, the TGB Dual
  mix, and the resampler cannot overshoot: linear interpolation of int16
  neighbours). A full-scale +-32767 square through the resampler gives 0
  wrapped steps (`test_audio_sim.c`).
- **Resampler:** a sine through the real ring + resampler (no rate follow) has
  everything-but-the-sine at -57 / -38 / -25 / -16 dB for 1 / 3 / 6 / 9 kHz.
  A Catmull-Rom cubic improves that to -78 / -52 / -33 / -21 dB but overshoots
  square edges (PSG peak 8192 -> 9331, +1.1 dB; more steps over 12000 in the
  clipped test: 40 -> 68), which only adds clipping on loud music. Rejected;
  the linear path stays and a regression guard keeps it from getting worse.
- **Vita thread and buffer:** the audio thread runs at priority 0x50, above
  the render thread (0x60) and the worker (0xB0); `sceAudioOutOutput` on the
  MAIN port at 48 kHz, 512-frame grains, 64-byte-aligned double buffers; the
  mGBA-Vita "raise the audio thread priority" lesson was already met. Grain
  size and ring cushion were left as they are: a bigger grain would add
  latency for a gain that can only be judged on a console.
- Fast forward and rewind already drop audio instead of overwriting it
  (`sink_audio`), and a full ring drops the newest frames.
- 1.3: 2x / 3x fast forward "with sound" (`ff_audio = 1`, the Fast-forward
  row's values) averages each group of 2 or 3 emulated samples into one
  (`ff_audio_push`), so the port gets one refresh of sound per refresh at its
  usual rate and the cushion logic runs once per batch; the queue is not
  flushed when such a fast forward starts or ends. 2x is `ff_mult_x10 = 20`
  (Vita only; a PSP build reads any speed as 3x).

### Fixes

- **Latency creep after intentional silence** (`vita/audio_vita.c`,
  `vita/main_vita.c`): the menu, fast forward and rewind stop the producer,
  the ring runs dry, and that was counted as an underrun, which grows the
  cushion by 20 ms (up to 120 ms) until 30 s of calm. A dry spell longer than
  250 ms (`g_gap_len`, output frames of the latest gap) is now not counted;
  fast forward start/end and the in-game menu closing also flush the queue.
  `vita/tests/audio/test_audio_sim.c` drives the real ring with a virtual
  clock: five 3 s pauses ended with a 120 ms target (was 60 ms at start);
  now 60 ms. A short stall (45 ms of missing frames) still adds 20 ms, as
  designed, and is audible as a gap of about 40 ms; steady 60.00 / 59.94 Hz
  and +-4 ms jitter runs have 0 underruns, 0 clicks.
- **Game Boy parked channel** (`gbcore/tgbdual/apu.c`): a square channel
  with rate above 65 kHz returned a constant +15000 (output +12304 at full
  volume, 150 % of the channel's own peak); a wave channel there returned
  the first nibble scaled differently from normal playback. Both now return
  the pattern's average, which is what an ultrasonic tone leaves
  (`gbcore/tests/gbaudio.c`: parked 50 % duty peak 12304 -> 0; parked 12.5 %
  duty equals the 440 Hz tone's own average).

### Tools

- `tools/audio_measure.py file.wav`: peak, clipped samples, wrap jumps,
  clicks, DC offset, largest step and silent gaps of a `gbadhoc_host --wav`
  capture. `tools/audio_phases.py` analyses the audio test cartridge
  (`make -C vita/hostcore/testrom audio`) phase by phase.
- `tools/run_audio_regressions.py` also runs `sim` (the real-time simulation
  and resampler/saturation checks) and `gb` (`gbcore/tests/run_gbaudio.py`).

### Not verified / left alone

- Not heard on a Vita. The real-console underrun rate under heavy picture
  settings (see PERFORMANCE.md) is a CPU-time problem; the sound path only
  decides how it sounds.
- GBA PSG master volume uses level/7 (0 = mute); the hardware (like the Game
  Boy's NR50) plays level 0 at 1/8 and 7 at 8/8, so levels 1-6 are 4-12 %
  quieter than hardware. Not changed: no reference to measure against here.
- PSG squares are point-sampled at 32768 Hz, so notes above about 4 kHz
  alias (a 3 kHz square: about 4.6 % of its energy off the harmonics).

## Version 1.3 Changes

### CPU speed (`cpuclock.c`)

At start the app calls `scePowerSetArmClockFrequency(500)` with the bus and GPU
at 222 MHz and the crossbar at 166 (what the other Vita ports do). A console
without an overclock plugin refuses; the app then asks for 444. Both requests
and results are in the log (`clocks: asked for ...`).

PSVshellPlus ignores such a request and applies a per-title profile instead,
read when the app becomes the active app, so a profile written now applies
from the next launch. `ur0:data/PSVshell/profiles/GBAD00001`, exactly 28
bytes, little-endian:

| offset | size | content |
|---|---|---|
| 0 | 8 | ASCII `PSVS0300` |
| 8 | u32 | lock flags `0x0b` (bit 0 CPU, bit 1 GPU, bit 3 crossbar) |
| 12 | i32 | CPU MHz, 500 |
| 16 | i32 | GPU MHz, 222 |
| 20 | i32 | bus MHz, 222 |
| 24 | i32 | crossbar MHz, 166 |

`50535653 30333030 0b000000 f4010000 de000000 de000000 a6000000`

The plugin is found by `ur0:tai/PSVshellPlus*.skprx` or the profiles folder.
With the setting on 500 the file is written only when none exists. A file that
differs is never replaced at start. Choosing 500 in Settings saves the
player's file as `psvshell_profile_backup.bin` in the data folder (and records
`psvshell_profile.state`), then writes ours; choosing 444 removes our file or
puts the backup back (if the file is still ours). Writes go to a `.tmp` file,
are read back, then renamed. If the first write fails, `psvshell_nowrite.stamp`
stops further tries. Under the plugin `scePowerGetArmClockFrequency` still
answers 444, so the app times a `subs/bne` loop (`plat_arm_mhz_measured`) at
start, 8 s later and after a change: `clocks: the ARM core measures N MHz (the
system reports M)`. The setting is `cpu_mhz = 500|444` in `vita.ini`.
The twin's stand-ins: `GBADHOC_FAKE_REFUSE500`, `GBADHOC_FAKE_UR0=<dir>`,
`GBADHOC_FAKE_MHZ=<n>`.

## Version 1.3 Changes

**Your own cover pictures** (`vita/cover_custom.[ch]`, `vita/ui_vita_cover.h`)
- **One source of truth.** A custom cover is the file every consumer already
  reads: `boxart/<stem>.png` (the home tile worker `menu_files.c`, the shelf
  and hero through `art_load_any`, `art_thumb_load`). So nothing else needed a
  precedence rule. `cover_set_file` decodes a PNG/JPG (stb_image, at most 8
  megapixels, 24 MiB) or a `.shot`, box-averages it to at most
  `COVER_MAX_EDGE` (512) px on the long side (never enlarged, shape kept),
  writes `boxart/<stem>.png` through a `.tmp` and a rename, then the tile
  thumbnail, then the record. A failed step puts everything back.
- **Files beside the cover** (all `boxart/<stem>.*`): `.own` = the player's
  (`src=` and `seen=` lines); `.dl` = the replaced download or hand-placed
  picture, renamed aside, not copied; `.auto` = Automatic was chosen
  (the inbox file last seen is recorded). `artfetch.c` needs only the file
  `.own`: it makes that stem's thumbnail the whole picture on a 160x160 square
  (`thumb_rgb(..., square=1)`, bars of the picture's average colour at 40%)
  instead of the plain fit that the tile centre-crops. A fetch never writes
  over any art on the card (`save()` checks `have_art_disk` before and after
  the transfer) and does not ask for a game whose art exists, so a cover set
  while a download is in flight still wins.
- **Cache invalidation, in one place each.** Disk: `cover_custom.c`
  `purge_caches` removes `boxart/<stem>.v565` and `.vh565`, and the
  thumbnail is rewritten (`cover_thumb_path`, the only place cover code
  builds the thumbnail path from `ART_THUMB_SUFFIX`). Memory:
  `ui_vita_cover.h` `cv_changed` calls `hm_art_arrived(base, n)` (the shelf
  slots, the hero, and the Home's artwork epoch for the tile).
- **Automatic** (`cover_auto`): the `.auto` note is written first, then the
  cover is removed and `.dl` renamed back (1), or, with none, `artfetch_rescan()`
  makes the worker read `boxart/` again and download (2). Nothing in
  `covers/` is touched.
- **The inbox** (`cover_inbox_scan` / `_step`): `covers/` is listed once per
  library scan (list opening, Refresh library, a console switch); a PNG/JPG
  matches a game when its name without the image extension, lower-cased,
  equals the game's stem lower-cased, or does once a ROM extension
  (.gba .gb .gbc .sgb .sfc .smc .swc .fig .nes .zip .7z) is cut off. An
  exact name beats one with the ROM extension, then png, jpg, jpeg. The
  file's `name|size|mtime` is compared with the `seen=` of the game's `.own` /
  `.auto`; a difference is a wish, taken one file per frame while the list
  rests (`ui_cover_pump`). Console-agnostic: only stems.
- **Where it is in the UI.** Home: a Cover button (`BROWSER_COVER_BUTTON`,
  bit 29 of the browser's virtual edges); Settings (browser only):
  LIBRARY > Cover picture (`SET_COVERPIC`, placed before Game folders so the
  walk-up-from-the-top of the existing tests keeps its rows). The page
  (`ui_cover_run`) sources: `ui_host_pictures_dir()` (`ux0:picture`;
  vita.ini `pictures` for the twin's tests), the screenshot folder
  (`ui_host_shot_dir()` or `screenshots/`) and `covers/`, three levels deep,
  at most 1500 files, newest first.
- **Tests:** `app_tests.py --only cover_menu,cover_inbox`.

## Version 1.3 Changes: Controls And First Impressions

- **Default picture** (`gpsp/wide.c` wide_apply, `psp/config_psp.h`): the
  "Known games" Wide view no longer opens margins in a game without a profile
  (it used to hand the renderer a 282 pixel picture and fill the 21 pixel
  margins with soft edges, which looked like a stretch). Without a profile
  the picture is 240 x 160 at the Fit size, 816 x 544, centred. `wide_fill`
  defaults to 0 (bars) instead of 1 (soft edges); keys already in
  `config.ini` are untouched.
- **Ambient fill**: `PCFG_AMBIENT_DEF` is `PCFG_AMB_ART_GAME` on the Vita (hero
  art, else a blurred game frame) so the aspect-correct picture fills the
  screen; `overlay_apply()` turns it off while a screen overlay is shown.
  `ambient_<console>` keys already in `config.ini` are kept. `app_tests.py`
  pins `ambient_* = 0` in its base config.
- **A and B buttons** (`ab_buttons` in `config.ini`, none = follow the Vita):
  `ab_circle()` in `vita/main_vita.c` takes the key, else
  `pspcompat_enter_is_circle()` (`sceAppUtilSystemParamGetInt` with
  `SCE_SYSTEM_PARAM_ID_ENTER_BUTTON`, after `sceAppUtilInit`), and calls
  `plat_pad_face_swap()` / `pspcompat_face_swap()`: the pad words exchange
  Cross and Circle once, so menus, game buttons, shortcuts and Button mapping
  all follow, and `vid_set_face_glyphs()` draws the hints' circle and cross
  (text glyphs 0x80/0x81 and `VS_I_BTN_X/O`) exchanged. A player who already
  moved A or B in Button mapping is never swapped by the Vita's setting
  (only by choosing). The twin reads `GBADHOC_ENTER_BUTTON=circle`.
  Row hooks: `ui_host_ab_circle`, `ui_host_buttons_changed`.
- **Right stick rewind** is `stick_rewind` (default 0); the Rewind button
  (Select + Square) is unchanged. **Screenshot combo**: `bind_screenshot` now
  defaults to unbound on the Vita; the Settings row sets or clears
  L + R + Select. Keys already in `config.ini` win.
- **Popups**: the two first-games toasts are one line, shown once in the first
  game ever (`hints` defaults to 1 and is set to 0 and saved when shown).
- Tests: `app_tests.py` pins `stick_rewind = 1` and the screenshot chord in
  its base config; `t_controls_defaults` runs the real defaults.

## Version 1.3 Changes

**Core: what was taken from GBAdhoc 4.x.** The core in the 1.2 tree is
GBAdhoc 4.1.1 (`d54feb6` / `318dd01`) with the Vita changes on top, plus
libretro gpSP's September 2026 fixes (already in). It reports itself as
`4.1.1+4.5.0fixes` (`CORE_VERSION`, `gpsp/vita/Makefile`). From 4.1.1 to 4.5.0
upstream changed mostly the MIPS dynarec; each change was checked for a
counterpart in the ARM backend (`gpsp/arm/`).

Taken (ARM backend and shared code):
- `TIMER_RELOAD_FIX` (`main.c` update_timers): a timer that just overflowed
  keeps its reload value in TMxCNT_L until the next update, instead of 0. Need
  for Speed: Underground's mixer computes a length from TM1 and cleared IWRAM
  with `0xFFFF0120`. Always on.
- `BX_ALIGN_FIX` (`cpu_threaded.c` block_lookup_address_dual, `arm/arm_stub.S`
  both dual branches): BX into ARM state with address bit 1 set fetches the
  word below (the ARM7TDMI ignores A1); the old `+2` rounded up. Camelot's IRQ
  veneer `bx pc` sits on a halfword (Golden Sun 2, Mario Golf, Mario Tennis).
- `COND_ENTRY_FIX` (`cpu_threaded.c` translate_block_arm): an internal branch
  target inside a run of same-condition instructions is marked (`0x40`) so the
  run is closed and a new condition header opened there.
- `FIQ_BANK_FIX` (`cpu.cc` set_cpu_mode): FIQ's r8-r12 are banked against the
  shared ones parked in the user row; leaving FIQ no longer loads r8-r12 from
  a never-written row (OpenLara).
- Interpreter (code in VRAM, palette RAM, OAM): Thumb register shifts use
  Rs[7:0]; ROR by register no longer shifts by 32 or more (undefined in C);
  LDM with the S bit switches to the user bank only when the PC is not in the
  list (it tested the base register).
- `LDM {..., pc}^` in the ARM dynarec (`arm/arm_emit.h`
  arm_block_memory_adjust_pc_load): loads the CPSR from the SPSR like MOVS pc
  (it ignored the S bit). The user-bank form without the PC is still not
  implemented.
- `MSR CPSR_c` in the ARM stub (`arm_stub.S` execute_store_cpsr): the live
  N, Z, C, V are kept when the mask does not cover them (the old merge used the
  stale flags of `reg[REG_CPSR]`).
- `SCHED_FAST` (`main.c`, now on in `vita/coreflags.mk`): update_gba skips the
  interrupt check when `IE & IF == 0`. Exact; run-time kill switch `sched_fast`.

Not taken, and why:
- `VRAM_EXEC` (Blue Roses engine): the tree already runs code from VRAM,
  palette RAM and OAM through the interpreter bridge (`HAVE_VRAM_INTERPRETER`).
- Live timer counters, ARM7 prefetch window, taken-branch refill, zero-byte
  cycle position (Pokemon MonoMyth): they need the cycle count at each I/O
  access from the emitter (`REG_IO_CYCLES`, stored by the MIPS stubs) and a
  pipeline step in the SMC trap; the ARM stub has neither, and the change
  would touch every I/O load and store. Not needed for the listed games.
- `SMC_*`, `XLAT_FAST`, `JIT_COST`, `ENTRY_GATES`, `SPIN_SKIP`, DMA retire,
  `DISPATCH_*`, `IRQ_IO_FAST`, `MSR_FAST`, `HOT_DATA_GUARD`, layout pinning:
  MIPS emitter/stub features (see the header of `vita/coreflags.mk`); the ARM
  backend flushes the RAM cache whole on a code write and would need its own
  trap. The largest remaining speed item for self-modifying games.
- 4.0.0's mid-frame Media Engine state (NFS Underground 2 flicker): the PSP's
  second renderer only. Vita renders each line from recorded state.
- The MIPS-only shift, `BACKUP_BUS_FIX` and MSR-IRQ link-register fixes; the
  ARM emitter uses the host's own shifts, and the MSR-raised IRQ's return
  address is not observable in practice. `LR_irq` for an IRQ unmasked by MSR
  is left as it was.
- libretro gpSP: everything up to its 2026-09-19 head was already in (VRAM
  execution, I/O read semantics, flag-preserving I/O load stub, BX PC). Not
  taken: 70707ce (iterative exit resolution, a stack-depth bound) and the
  sound-rate and rounding commits (sound is the audio work's area).
- lr-gpsp-amcc OAM order changes: replaced in 1.4 by the full rule (OBJ_EXACT, below).

**Tests:** `cpufix` tests 12 to 21 (`testrom/cpufix.c`): reload read after
overflow, register-shift conformance from ROM and VRAM, Rm = PC in a shift,
BX to a halfword address, a branch into a conditional run (ROM and IWRAM), FIQ
banking, `LDM {pc}^`, `MSR CPSR_c` flags. The cart takes `-DRUN_MASK=bits` to
run some of them.

## Version 1.3 Changes: Split Screen (two players on one Vita)

- **Player 2's port** (`plat_vita.c` `pad2_port`, the rule in
  `pad_port_pick.h`, tested by `hostcore/padports_test.c`): sceCtrl port 0 is
  the console's controls (on a PS TV merged with the first pad), ports 1..4
  are the paired controllers in order (`SceCtrlPortInfo.port[]`, 0 =
  unpaired; types PHY 1, VIRT 2, DS3 4, DS4 8 all count). On a PS TV port 1
  is the first pad again, so player 2 is the first paired of 2, 3, 4; on a
  handheld, the same, else port 1 (one external pad). Sources: vita-headers
  `ctrl.h`, SDL's Vita joystick driver ("on Vita TV port 0 and 1 are the same
  controller ... start at port 2"), RetroArch's psp_joypad (PS TV: pad n is
  port n+1 for n = 0). Not run on a console. The port info is re-read every 20
  polls; a failed read of the chosen port forces a re-read at once.
- **Reading it** (`plat_pad_read2`): `sceCtrlPeekBufferPositiveExt2`, falling
  back to `sceCtrlPeekBufferPositive2`. The extended call reports L1/R1 as
  0x400/0x800 and L2/R2 as 0x100/0x200; both pairs fold into `PLAT_BTN_L` /
  `PLAT_BTN_R`; L3/R3 and the PS button are dropped. Player 1 still uses
  `sceCtrlPeekBufferPositive(0)`.
- **Layout.** `layout_now()` in `local_vita.c`: a second controller or the
  touch pad forces `VID_PAIR_SIDE` whatever `local_layout` says (the config
  value is kept for the one-pad case).
- **Menus.** `main_vita.c` `game_loop`: Start + Select held on a real second
  pad for `MENU_CHORD_FRAMES` opens the menu (`menu_open("chord2")`);
  `pspcompat_pad2_menu(1)` while the menu is open and the pair runs makes
  `pspcompat_pad()` OR in the second pad, so every page answers either pad.
  `link_local_block_held()` blocks both players' held buttons when the menu
  closes (`held_block`, `held_block2`).
- **Touch pad** (`local_touch2` in `config.ini`, Settings row in Two players
  on this Vita, default 0): `plat_touch_points()` (Vita: `sceTouchPeek`
  front, up to six fingers, 1920x1088 halved; twin: every `touch=` script
  entry that is down) -> `link_local_touch_keys()` in `local_vita.c` hit-tests
  logical 480x272 shapes on the right half (`TP_*`) into a libretro mask passed
  as player 2's keys, with `have2 = 1`; drawn in `link_local_present`
  (`touch_draw`). Taps on that half do not switch the view; a tap on the
  player's picture still opens the menu.
- **Tests** (`hostcore/local_tests.py`): `ports` (the port table), `twopads`
  (layout, each pad its own game, either menu), `touch2` (both inputs at once,
  L/R/Start, nothing leaking, off, a pad overriding the touch pad). The twin's
  `pad2=on|off` and `p2.` script entries already simulated the second pad.
- **GB and GBC are one link family.** `same_console()` in `local_vita.c` used
  to pair a `.gb` only with `.gb` and a `.gbc` only with `.gbc`; the picker
  and `partner_set()` now accept either for a Game Boy game (GBA only with
  `.gba`). The partner's save keeps the name its own extension gives
  (`save_suffix_of()`). `fe_host_gb_local_start()` runs the partner's machine
  as its own hardware (`gbloc_partner_console()`: `.gbc` = CGB, `.gb` = DMG,
  a colour-only header byte 0x143 = 0xC0 forces CGB) instead of copying the
  player's `host.console`; the `gb_local_start` event prints both consoles
  (`console=0/1`). The network link already carried each side's console in
  HELLO and made each slot's machine from it (`fe_gblink.c`), and its
  compatibility word is one value for the whole GB family (`link_vita.c`,
  `GBAdhoc GB link v2`), so a `.gb` host and a `.gbc` guest were accepted;
  only GBA rooms check the game code. The cable is byte based
  (`gbcore_dual.c`: the byte crosses at the SC write, the sender's own
  `seri_occer` completes it in CPU clocks), so a Color in double speed against
  a Game Boy at normal speed needs no special case; the tests prove it.
  Over the network the GBA cable now takes two different games too (next
  section).
- **Tests:** `local_tests.py gb_mixed` (a `.gb` with a colour-flag `.gbc`, with
  a `.gbc` without the flag, with a Color in double speed, each both ways, and
  the picker's list) and `link_tests.py gb_mixed` (the same over the loopback
  network link). The cartridges are `testrom/gblocal*.gbc` and
  `gblink*.gbc` (`make -C testrom gb`; the `-cgbfast` ones switch to double
  speed at power on, `rgbasm -DDOUBLE`).

### GBA cable link between two different games (1.3)

Before, each Vita mirrored BOTH machines of a GBA cable session from one ROM
(`fe_gblink` with the `fe_gbadual` machines, instance B reading instance A's
ROM pool), so both Vitas needed the same game. Now machine B can run the
partner's own cartridge, the way the local link's "partner's game" does
(`b_recart()`, B with its own ROM cache).

- **Room check.** `join_decide()` (`ui_vita_play.h`) no longer compares game
  codes for a GBA cable room; any two GBA games may join, as hardware allows.
  The list row shows "their game <-> yours" (`join_other_game()`), the
  linking screen "yours <-> theirs". `LINK_ABI` is `vita-link-2`.
- **Identity.** HELLO carries each cartridge's size, header and the 20-byte
  identity (a whole-file SHA-1 under 256 KiB, else the SHA-1 of the size and 64
  slices of 4 KiB, `SAMPLE_N`). A different identity means a different game:
  `partner_lookup()` asks the cache (`fe_gblink_platform.rom_cache`, op 0), and
  an entry is used only if its size and identity match (`ident_of_file()`),
  otherwise NEED 1 and the partner streams its ROM. A pair with no cache (the
  PSP) behaves as before: ROM_MISSING.
- **Transfer.** Same lanes as the GB path: the bulk lane (1024-byte chunks,
  CRC-32 per chunk, selective acknowledgement, adaptive rate) or the ordered
  channel. The sender has no copy in memory (the machine owns it), so
  `bk_send_chunk()` and `stream_rom()` read the chunks from the cartridge file
  (`own_rom_read()`, kept open). The receiver collects the ROM in one malloc
  buffer (16 MiB at most 32), checks `ident_of_buf()` against the HELLO
  identity (M_ROM_RESULT), then writes it to `<root>/cache/link/<identity>.gbarom`
  in steps (`store_step()`, `.part` then rename), frees the buffer, and only
  then loads it. `fe_gblink_progress` carries `rate_bps` and `eta_s` (needs
  the clock) for the screen's time estimate; Cancel is the LINKING screen's
  usual one (aborts the setup, the partner is told).
- **Machine B.** `fe_gblink_machines.partner_rom()` = `m_partner_rom()` in
  `fe_gbadual.c`: `b_recart(cache file, own_rom = 1)` before READY (so READY
  means B is loaded), and `partner_rom(NULL)` at `fe_gblink_destroy()` puts B
  back on the player's cartridge (shared ROM pool). A failed load fails the
  session with `E_START` ("memory") and restores B.
- **Saves and states.** Unchanged: each side streams its battery image and its
  running state, both are loaded into the slot's machine (B's state loads
  only after B runs that partner's cartridge, so `retro_serialize_size`
  agrees). Only the local side's battery is committed at the end; the partner's
  stays in memory. Hashes are compared in lockstep as before; the machines'
  roles are symmetric, so who holds which cartridge does not matter.
- **Cache.** `gba_rom_cache()` in `fe_host.c`: one file per cartridge named by
  its identity, in `host.gba_rom_cache_dir` (Vita: `<root>/cache/link`, not a
  game folder, so never listed). After each store or use,
  `romc_trim()` keeps at most 4 files and 96 MB (`gba_rom_cache_files/mb`),
  removing the oldest files other than the one in use and any `.part` left by
  a crash. FIFO by modification time (no `utime` on the Vita libc).
- **Memory (Vita).** Instance B is loaded at boot, sharing A's pool. For a
  session with another game: A holds the cartridge (cap = its size in 1 MiB
  blocks, `gamepak_buffer_cap`), the receive buffer holds the partner's
  (freed after the cache write), then B's own pool takes the partner's size
  in blocks. Peak with two 16 MiB games = 32 MiB of ROM cache plus 16 MiB for
  the buffer only while receiving. Not measured on a Vita (see KNOWN-ISSUES).
- **Tests.** `link_tests.py gba_two_games` (two test cartridges of
  `tools/linktest`, `make VARIANT_B=1` builds the second with another game
  code): both ways with 9 KB cartridges (whole-file identity) and 1 MiB padded
  ones (sliced identity, many chunks), then a second session that finds both
  in the cache and moves nothing. It checks the room list, the transfer, the
  cache file's name and bytes, nothing extra in `roms/`, the matching
  hashes, the cartridges' own verdicts, the agreed end.

**Open BIOS: decompression from a halfword-aligned source.** The ReGBA/VBA open
BIOS read the 4-byte header of LZ77UnComp, HuffUnComp, RLUnCompWram and the Diff
filters with a plain `ldr`, which rotates an unaligned address; only
RLUnCompVram masked it. Mortal Kombat: Deadly Alliance calls RLUnCompWram with
sources two bytes into a word, got sizes in the millions and spent 40+ seconds
(and trashed EWRAM and the menus' tiles). The header is now read from
`source & ~3` and the data starts at `source + 4`, as in VBA-M and mGBA
(`gpsp/bios/source/softwareinterrupts.c`). The binary is not rebuilt: it is
patched by `gpsp/bios/patch_unaligned_decomp.py` (seven jumps into stubs in the
unused tail of the image; flags and other registers untouched, an aligned
source behaves as before, 3 more instructions per call). `cpufix` tests 22 and
23 (RLUnCompWram, LZ77UnCompWram from `buffer + 2`) fail on the old image and
pass on the new one. Huffman, the Diff filters and the Vram variant of LZ77 use
the same stub but have no test of their own. Whether a real BIOS aligns down or
rotates is taken from those two emulators, not measured on a console.

## Version 1.2 Changes

Engineering detail behind the [What's New](../CHANGELOG.md) list.

**Features**
- **Save import** (`vita/main_vita.c` save_import): before the core loads a
  game with no battery file, and no `.bak` / `.tmp` / `.old` beside it, the
  first of `import/<rom>.sav`, `import/<stem>.sav`, `import/<stem>.srm`,
  `<stem>.srm` beside the ROM, `<stem>.sav` beside a GB/GBC ROM (not when
  `<stem>.gba` exists) and `ux0:data/retroarch/savefiles[/<core>]/<stem>.srm`
  is copied (through `.import`, then renamed) to the game's save name. Files
  of up to 256 KiB; the loaders keep what they can of other layouts.
  Imported `import/` files are renamed `.imported`.
- **Launch parameter** (`plat_launch_param`; `sceAppMgrGetAppParam` on the
  Vita, `--launch-param` on the twin): the text after the first `param=`
  that starts a field, taken raw. Read once at start, before `autoboot.txt`.
- **Game folders** (`ui_host_extra_root_toggle`, ui_psp.c `SET_FOLDERS`):
  the keyboard's folder toggles in the list; the list is written as
  `extra_roms` through the bound `vita.ini` document (refused past 480
  characters, the length an ini line is read back whole; the list is restored
  when it cannot be written). `extra_roms` is also read from `config.ini`;
  `gba_roms` / `gb_roms` / `gbc_roms` add one folder each unless `vita.ini`'s
  `extra_roms` is the list.
- **Zipped games** (`vita/zip_rom.c`): End Of Central Directory, the first
  central entry named `.gba` / `.gb` / `.gbc`, method 0 or 8 (stb_image's
  inflater), CRC-32 checked. No ZIP64, encryption or multi-disk. The scan asks
  `ui_host_zip_console` (cached by path and size); the game is unpacked into
  `unzipped/` (one at a time, keyed by the zip's size, time and entry CRC) and
  booted from there, while saves, states, cheats and settings keep the zip's
  name.
- **Overlays** (`vid_set_overlay`): `overlays/<stem>.png`, else
  `overlays/<console>.png`, decoded once per game and drawn over the screen
  after the picture. Settings rows (1.2): Display > Grid strength (1-10,
  `grid_strength` in `vita.ini`, scales the LCD/scanline texture by 20% a
  step) and Screen overlay (`overlays`), Interface > Screenshot folder
  (`screenshots`), all written through the bound `vita.ini` document and
  applied at once.
- **Ten state slots** (`PSP_STATE_SLOT_COUNT` 10, `.st0` to `.st9`): the in-game page, the shelf and the home screen's
  cards show five at a time and follow the selection
  (`psp_state_slot_top`); the menu loader's metadata and task masks are
  sized from the count. State sharing (`FE_SHARE_SLOTS`) stays at five, so
  1.1 peers understand it.
- **Display names** (`ui_ascii_fold`): UTF-8 Latin letters fold to ASCII for
  the UI's fonts; Find still compares the file's own name.

**Graphics fixes** (`vita/video_vita.c`, `vita/vback_vita2d.c`, vita2d)
- The CPU upscalers no longer submit into a texture the GPU may still read
  (`vb_tex_writable` before `upscale_submit`; a skip counts as dropped).
- Live, snapshot and soft staging textures are a ring of three chosen by GPU
  fence, not by frame parity; with none free the last upload is redrawn.
- Destroyed textures are freed when their fragment fence retires
  (`defer_sweep`); `sceGxmFinish` only when the queue is full or at exit.
- A latched EndScene or display-queue error is cleared by `vita2d_recover`
  (up to five times in a row), instead of every later frame being skipped.
- The tint uniform no longer comes from the vertex pool (a NULL write when
  the pool was nearly full); destroying the bound render target ends its
  scene first; failed texture allocations retry every 60 frames and fall
  back to the last good picture; unchanged menu snapshots are not uploaded
  again.

**Speed**
- `gba_save_state` zeroes only its padding, so serialisers no longer clear
  the 416 KiB image first (`retro_serialize` about half the instructions).
- Rewind's encoder and decoder compare 64 bytes at a time with NEON and XOR
  8 bytes at a time (bit-identical streams; differential tests on x86 and
  ARM).
- Rewind landings keep the ROM translation cache (`state_load_keeps_rom`,
  as the wide-view scout already did).
- `upscale_submit` compares each row right after copying it;
  `upscale_vita.c` is built with `-O3 -ftree-vectorize`.

**Fixes from review**
- `fe_ini_doc_save`'s fallback (when the card refuses a rename) keeps the old
  file as `.bak` (copied there when it could not be moved) and the complete
  `.tmp` until the in-place write succeeds, and puts the old file back if
  that write fails.
- An independent review of the 1.2 features (before release) also led to:
  cheats and the link room name of a zipped game read the unpacked file; a
  refused launch is asked once and does not fall back to `autoboot.txt`; a
  zip's entry sizes are checked against the file before anything is
  allocated; an overlay PNG's size is checked before it is decoded.

## Version 1.1 Changes

Audio output changes apply to GBA, GB and GBC; the mixer fixes below are in
the GBA core.

- **Persistent output storage** (`vita/audio_vita.c`): two 64-byte-aligned
  PCM buffers alternate submissions. Each remains owned until the audio
  port has finished with it.
- **Checked shutdown and recovery** (`vita/audio_vita.c`, `vita/plat_vita.c`):
  failed writes retire the old port before rendering again. Port drain,
  release and thread termination are checked. Failed or timed-out cleanup
  retains buffer and thread ownership before another session can start.
- **Resampler range:** the step uses 2.30 fixed point, supporting input
  rates from 8,000 to 65,536 Hz and pacing from 0.5x to 2x without fractional
  overflow. Invalid inputs leave an existing session alone.
- **Smoother transitions:** flushes and underruns fade from the last sample
  actually emitted. Backlog trims blend into the new timeline over 64
  output samples.
- **GBA tone-channel stereo** (`gba_memory.c`): `SOUNDCNT_L` routing extracts
  each square, wave and noise channel's left/right bits independently.
- **GBA tone-channel timing** (`sound.c`, `gba_memory.c`): the 256 Hz
  sequencer step follows the output sample rate and is refreshed at
  initialization, rate changes and state loads, correcting length,
  volume-envelope and sweep timing.
- **GBA DirectSound FIFO reset** (`sound.c`): resetting either channel
  clears sample storage and read/write indices together.
- **Regression coverage** (`tools/run_audio_regressions.py`): a consolidated
  runner covers mixer, output and native/host lifecycle suites, plus the
  existing malformed-state safety checks, with optional ASan/UBSan runs.

## Version 1.0 Changes

Engineering detail behind the [What's New](../CHANGELOG.md) list.

**Features**
- **Sensor cartridges** (`gpsp/cart_sensor.h`, `gba_memory.c`, `gba_over.h`,
  `frontend-common/fe_sensor.h`, `gbcore/tgbdual/mbc.c`, `vita/plat_vita.c`
  SceMotion): Boktai's solar sensor, WarioWare: Twisted!'s gyro and the
  Yoshi / Koro Koro tilt sensor follow GBATEK's protocols; MBC7 latches the
  full 16-bit sample. One reading is published per frame and recorded in the
  rewind context, so rewound frames replay it. The sensor state rides in save
  states as an optional key; other games' states are unchanged. In a link the
  partner machine sees a still console in the dark.
- **Sidecar files** (`psp/sidecar.h`, 1.3): a ROM's folder holds the ROM and
  its battery save only. States (`.stN`, `.thumb`, `.shot`, link `.p2`),
  Continue (`.rsm`, `.rsm.pt`) go to `<root>/states/<gba|gb|gbc>/<stem><suffix>`,
  where the suffix is the old one (`.st0`, `.gb.st0`). The stem is the
  ROM's own, wherever it lives (no folder hash), as RetroArch names states:
  moving a ROM keeps its states; two different games with one file name for
  one console in two folders would share them, as in RetroArch. Tile
  thumbnails (`ART_THUMB_DIR`) and the app-made `.v565`/`.vh565` copies live in
  `<root>/cache/...`. `g_game.state_path` is the new slot-1 path and every
  derived path follows it (`psp_state_path_for_slot`, `psp_state_resume_path`,
  delete paths); `ui_psp.c` `browser_rom_state_path` and `menu_files.c` use the
  same function. Migration (`sidecar_migrate`): per file, only when the old
  exists and the new does not: `rename`, else copy to `.mv`, read both back,
  rename, remove the old (never overwrites; a failure leaves the old file,
  which `sidecar_read_path` serves for loads). It runs at game boot (logs
  `sidecar_migrate`), in the shelf's per-game state scan, and in the menu-load
  worker's metadata job (silent: the log has one producer). Battery saves and
  `save_import` are untouched. Patches (`Game.ips`) are the player's input and
  stay beside the ROM.
- **Continue** (`vita/main_vita.c` resume_*): a state taken in memory on the
  frame thread when a game is left, every two minutes of play and 30 frames
  after an in-game save, written by the I/O worker as `states/<console>/Game.rsm` (temp, old
  aside, new in). `Game.rsm.pt` records the patch the point was made with, and
  a different patch refuses it.
- **Play log** (`vita/play_log.c`): `playlog.txt`, static tables, atomic
  saves; full at 2,048 games, the game played longest ago is forgotten.
- **Patches** (`vita/rom_patch.c`): IPS (with the truncation record), BPS
  (source, target and patch CRC checks) and UPS (either direction). The
  patched copy is cached in `patched/` with a key of sizes, times and the
  patch's CRC. `Game.gba.ips` names one cartridge; `Game.ips` is refused when
  another cartridge shares the name.
- **RetroAchievements** (`vita/ra_vita.c`, `vita/ra_http.c`, vendored
  rcheevos v12.5.0 plus five backported fixes, with `RC_NO_THREADS`): the rc_client runs on the game
  thread; requests go one at a time to a network thread (HTTPS through
  SceHttp/SceSsl; plain HTTP over sockets for tests). The TLS here is the
  console's own, NOT the pinned stack of the room client: `tls_up` loads the
  roots of `ra_roots.h` with `sceHttpsLoadCert` BESIDE the firmware's
  certificate store (they are added, they do not replace it), and no check is
  switched off, so the firmware's defaults decide (certificate chain, host
  name, dates, known authority). Any authority in the console's store is
  therefore accepted for retroachievements.org, and the protocol versions and
  cipher suites are the firmware's. The room service is different: it uses the
  vendored Mbed TLS with twelve pinned roots (eight for the room service, four
  Sectigo ones for the update check), a clock floor and mandatory
  verification (`nethttps/`, below). The RetroAchievements password is sent
  once per sign-in over the console's connection; afterwards a token. Moving
  `ra_http.c` onto `nethttps/` is a possible later change (not done in 2.0). The game's hash is the MD5 of the whole file (patched
  copy if patched), computed on that thread. Memory maps match rcheevos's
  console tables (`fe_host_ra_read`, `gbcore_ra_read`). Hardcore blocks every
  state load, rewind and cheats, and lifts for games with neither achievements
  nor leaderboards; Continue waits for that answer (20 s at most). Processing
  pauses during link play and progress resets after a state load, rewind or
  link. A sign-in that couldn't reach the server is retried. Unsent unlocks get
  three seconds at exit. The password goes through the IME in password mode,
  is sent once, and its buffers are wiped.
- **Per-game settings** (`psp/config_psp.c` pcfg_game_*): `games/<rom>.ini`.
- **Super Game Boy borders** (`gbcore/tgbdual/sgb.c` under `TGB_SGB_BORDER`):
  the border is composed into one 256x224 picture and kept in
  `borders/<rom>.sgb`, since GB states don't carry it.

**Fixes from review**
- TGB Dual's MBC7 EEPROM write-all stored both bytes of each word at one
  address (`LOCAL-PATCHES.md`).
- A reset Game Boy no longer saves a blank SGB border.
- The resume writer reports to the frame thread instead of logging from the
  I/O thread (the log has one producer).
- Library, artwork and save-safety work: one folder
  walk for all three consoles, kerning and text measuring cached, cover
  downloads resolved once per pass, Game Boy saves checked every second,
  temp-old-new writes for favourites and partner saves.

**Fixes from Vita testing**
- An MSR that sets the T bit now continues in Thumb at the next instruction,
  as on the ARM7TDMI (`cpu.cc` `cpsr_masks` and the interpreter; each
  backend's `execute_store_cpsr_body` returns the Thumb address and its stub
  resumes there). Packed start-up code depends on it: Pokémon Pinball: Ruby &
  Sapphire with an added intro ran off into data with the screen
  blanked.
- Code in palette RAM and OAM runs through the same uncached interpreter
  bridge as VRAM code (`cpu_threaded.c`, `GBA_PC_UNCACHED` in `cpu.h`; the
  interpreter fetches it with `read_memory`). The same intro's last
  trampoline runs at `0x050003F0`; before, it was treated as a bad jump and
  the game reset.
- The x86 dynarec gives an NV-condition instruction a jump to patch, as the
  ARM backend does (an ARM scan that met one first patched a NULL address).
- A game picked before its backdrop art was decoded used to decode it on the
  press: on a Vita, 7 s with the screen frozen, most of it 513 row writes of
  the 1 MiB cache file. The launch now takes the ambient colours from the
  cover's thumbnail (`launch_art_quick`, `psp/ui_psp.c`), and the cache file
  is written in one call.

**Tests:** host-twin app tests `sensors`, `resume`, `recent`, `patch`, `ra`
(against a stand-in server), `game_look` and `sgb`, and `cpufix` tests 9-11 (MSR into Thumb, code in
palette RAM and OAM, `testrom/cpufix.c`); unit tests
`tools/tests/test_cart_sensors.c`, `tools/tests/test_rom_patch.c`,
`gbcore/tests/mbc7.c` and `vita/tests/playlog/test_play_log.c` under ASan and
UBSan.

## Cartridge clock and save type (1.3)

The GBA cartridge clock used to be "time at load + emulated frames", which
steps back at every Continue or state load and runs ahead under fast-forward
(libretro/gpsp #269, #275; upstream added an "RTC time source" option for
this, here the system clock is simply the default). `fe_host.c`
(`rtc_wall_publish`) now publishes `gpsp_wallclock()` (the Vita's local
clock, `plat_wallclock`) to `gpsp_rtc_wall` before the cartridge loads and at
most five times a second; `gba_memory.c` / `rtc_clock.h` show it to the game,
never earlier than the last value shown. The save-state format is unchanged:
the base still round-trips, is re-anchored on read, and old states load.
Where determinism matters the frame clock stays: a link session forces it on
both consoles (`gpsp_rtc_frames_only`, `fe_gbadual.c`); rewind records the
second shown each frame and replays it exactly (`rw_frame_context`), so
rewinding never makes the clock jump; the second link machine never has a
wall clock. RetroAchievements needs no repeatable clock (rcheevos reads
memory only), so Hardcore uses the real one. `CONFIG.INI rtc_realtime = 0` is
a kill switch back to the old clock. Tests: `VITADHOC_FAKE_TIME=<seconds>`
sets the twin's start-up time.

Save type: `games/<ROM>.ini` keys `save_type` (0 Auto, 1 SRAM, 2 Flash 64 KB,
3 Flash 128 KB, 4 EEPROM 512 B, 5 EEPROM 8 KB) and `rtc` (0 Auto, 1 On, 2 Off),
shown in Settings > This game and applied by `apply_save_type_choice()` in
`load_gamepak`, before the battery file is read. Auto also reads a 131072- or
131088-byte save file as 128 KB flash when the ROM is not in the override
database and its labels say 64 KB flash, nothing, or EEPROM together with
FLASH1M (an explicit SRAM label, or a 32 KB or 64 KB file, is left alone).
Tests: `tools/tests/test_rtc_save_type.c` (in the `rtc_clock` host suite) and
app test `save_clock`.

## Diagnostics

Switches include `drc = 0`, `render_thread = 2` for comparison,
`gpu_xbrz = 1` for experiments, `clips = 0` / `art = 0` to turn off optional
work, and `ra_host = http://host:port` for a test RetroAchievements server.
Logs under `ux0:data/GBAdhoc/log/` may contain ROM paths and network
addresses; review them before sharing. The package does not install a clock
or plugin profile. [Known Issues](../KNOWN-ISSUES.md) lists what still needs
a Vita.

## Version 1.3: profiling pass (GBA)

Callgrind on the Linux twin (x86, 1800 frames each) with seven of the owner's
cartridges (Emerald, Golden Sun 2, F-Zero GP Legend, Aria of Sorrow, Minish
Cap, Mario Kart Super Circuit, NFS Underground 2), smoothing off and xBRZ 3x,
plus a PC sampler on the armhf twin under qemu (the shipped ARM dynarec).
No Snes9x-style waste was found: the emulation thread (dynarec code and its
helpers) and the scanline renderer dominate; with xBRZ on, the xBRZ scaler is
the largest single function. Two bit-identical savings were taken:

- `upscale_vita.c`: the changed-pixel mask is built 32 bits at a time
  (`cm_mark_diff`, two pixels per 32-bit load; `cm_set_range` whole words;
  `cm_next` with `__builtin_ctz`) instead of per pixel and per bit. This runs
  on the emulation thread before the helpers scale.
- `xbrz.cpp`: `DistYCbCrBuffer::dist` is forced inline and returns 0 at once
  for equal pixels (every distance mode already gave exactly 0 there). An
  isolated xBRZ benchmark: 296.9 M -> 265.5 M instructions (about 10 % fewer),
  same output.

Checked: frame hashes and audio WAVs identical before/after for all seven games
with smoothing off and with xBRZ 3x (1800 frames; longer runs into gameplay
13 of 14 frame files and 14 of 14 WAVs confirmed); upscale tests, audio
regressions (8/8), rewind_tests (373 checks), app_tests look/wide/ambient/play/
session. Twin-only profiling switches (inert unless set): GBADHOC_FRAME_HASH,
GBADHOC_SOFT_NORASTER, GBADHOC_CG_FROM, GBADHOC_PCPROF(_FROM). None of this is
a frame-rate measurement on a Vita.

## Version 1.4 Changes: Game Names And The SNES Hint

- **Game names** (`lib_names` in `config.ini`, `SET_NAMES` in ui_psp.c, first row
  under LIBRARY so the walkers that wrap up to Cover picture, Game folders and
  Game order keep their counts). 0 = Game title, the default: `rom_display_name`
  cuts everything from the first `(` and puts a trailing ", The" back in front.
  1 = File name: the stem as it is. Find still matches both. Written only once
  changed. The names on screen come from the file name, not from gamedb
  (gamedb only feeds covers and the blurb), so hacks collide because the
  bracketed tag is cut.
- **Same name, two games** (`names_prepare` once a frame, `names_subtitle`): the
  list being shown (tab, search, order) is hashed by display name; every game
  sharing a name shows its file name under it on the home page (`hm_focus`). The
  Shelf and Marquee lists, one line a row, show the file name in place of the
  name for those games. Under File name equal stems in two folders show the
  folder instead. Display only: nothing keyed on the file changes.
  `ui_game_name` is the test's event.
- **SNES hint**: the home page's footer and both loading screens say "hold
  L + R + START for the menu" when the console is SNES (the loading screen said
  START + SELECT for every console). 2.0: every console's hint says START +
  SELECT (`ui_menu_chord_text`), which opens the menu on every console; L + R +
  Start is an extra on the Super NES and Sega players.
- **Settings fix**: the Cover picture and Sync RetroArch saves labels were in
  the other order from `SET_COVERPIC` / `SET_SYNCRA`, so each row wore the
  other's name.
- **Tests:** `app_tests.py --only names_filename,names_duplicate_subtitle`.

## Version 1.4 Changes: Core Fixes From GBAdhoc, libretro gpSP And lr-gpsp-amcc

What was taken into the gpSP core after the 1.3 review of GBAdhoc 4.5.0/4.6.0-beta,
libretro gpSP `70707ce` and lr-gpsp-amcc's `patch-3` branch, what was left, and
how each part was checked. Every change has a `-DGPSP_NO_...` switch that
restores the old code (the tests below were run against builds with all four
off, to prove they fail without the fix; NV_COND_FIX is the exception, see its entry). All of it is shared C: the emitters
(`arm/`, `x86/`) are untouched.

Measured on the host twins (700 frames, scripted input, the six private test ROMs
that are never copied into the repo): the picture hashes (every 200 frames) and the
sound CRC of all six are identical with all four switches off and with all four
on, on x86 and on the armhf twin under qemu; x86 and armhf also agree with each
other. User CPU time, off vs on: x86 within noise (-5% to +2%, load 20+ on the
box), armhf under qemu 43.6-52.7 s vs 43.3-50.7 s (no slowdown). A 3000-frame
Golden Sun run with a screenshot every 10 frames differs in exactly one frame
(an 8x12 px patch beside a character sprite, the expected OBJ-over-BG change);
no other frame of any ROM differs. `x86` cpufix tests 13, 20, 21 fail on the x86
twin by design (x86 emitter not fixed for them, `app_tests.py` enforces them on
ARM only), so run_tests.sh shows the cpufix picture as DIFFER between twins.

- **NV_COND_FIX** (GBAdhoc `a8b9f52`; `cpu_threaded.c` `arm_scan_nv_nop`,
  `arm_emit_case`). An ARM word with condition 0xF is never executed: the scan
  turns it into an AL no-op (no exit point, no `block_exits[]` entry, it closes
  an open condition run) and the emit switch gives it no code. Its 1S stays.
  Unlike GBAdhoc's MIPS backend, VitAdhoc's emitters were already safe: the
  x86 `generate_condition` has an explicit 0xF case (a jump over the word) and
  the ARM header's `condition ^= 1` turns NV into AL, i.e. an always-taken skip.
  So this is not a crash fix here; it makes NV words emit no code and no skip
  branch, and the test (`cpufix` 24-26) passes with and without the fix.
- **RAM_MIRROR_FIX** (GBAdhoc `d9052db`, first half; `block_lookup_translate_*`,
  `RAM_MIRROR_CANON`). IWRAM repeats every 32 KiB and EWRAM every 256 KiB; the
  lookup gave all aliases one tag but translated at whichever alias asked
  first, so a canonical caller could run code carrying the alias's pc (`mov r0,
  pc`, `mov lr, pc`, its exits). The lookup now translates the canonical pc
  for every alias (one AND). Code that really runs at an alias sees canonical pc
  values where the GBA shows the alias; upstream took the same trade. The second
  half (an exit to an aliased pc linked lazily through a dispatch island) is
  MIPS emitter work and is not ported: the eager link resolves to the canonical
  block. Test: `cpufix` 27-28 (an alias runs first, then the home address must
  see its own pc); fails without the fix (saw 0x300e408, 0x2071008).
- **SMC_STM_FIXED** (GBAdhoc `45bc8d1`) is **not ported**, and neither is the
  partial RAM retirement it is a refinement of. It needs, and the ARM and x86
  backends have none of: (1) the written address and size at the trap (the ARM
  stub's `smc_write` passes none, the x86 one jumps with it in a register that
  differs per region); (2) a source extent per block (`ramtag_note_extent`,
  MIPS only); (3) no direct RAM-to-RAM links, or a way to unlink: here every
  external exit is patched straight to the target's translation, so clearing one
  block's tags would leave live blocks jumping into retired code (GBAdhoc routes
  RAM exits through dispatch islands, `SMC_PARTIAL_SAFE`, and `coreflags.mk`
  records that it does not compile for ARM); (4) patching already-published
  code, which on the Vita needs `sceKernelSyncVMDomain` for words outside the
  `[last, ptr)` range `translate_icache_sync` covers, and can only be proven on
  the console. Doing it correctly is a project of its own (a trap that carries
  the address in both stubs, extents per tag, inbound-link tracking or islands,
  a tag rebuild for overlapping live blocks, then the Vita). The ARM backend
  still flushes the whole RAM cache on a code write.
- **XLAT_ITERATIVE** (libretro/gpsp `70707ce`; `defer_external_exit`,
  `process_deferred_exits`, `translate_depth`). A block's external exits used to
  be resolved by recursing: one `translate_block_*` frame (the exits array alone
  is 256-384 bytes) plus one lookup frame per not-yet-translated block of a
  branch chain. A chain of a few thousand first-visit blocks overflowed the 1 MiB
  main thread stack (`plat_vita.c`). Now the exits are queued and the outermost
  lookup drains the queue; the queue is filled in reverse and popped from the
  end, so the blocks are visited in the same order as the recursion visited them
  (same cache layout, tag numbers and cache-full timing) and the host stack depth
  is constant. The failure contract is the old one (NULL, queue dropped,
  `block_lookup_address` retries), the unmappable-target backstop moved into the
  drain, and the pre-publication island loops are unchanged. Test: `chain`
  (`testrom/chain.c`, 6000 ARM and 6000 Thumb blocks, 400 in IWRAM) with the host
  stack limited to 1 MiB: segfault (-11) without the change, passes with it.
- **OBJ_EXACT** (lr-gpsp-amcc `ea8e4a9`/`9faf6f9`/`d8db010`, replaced by the full
  rule; `video.cc`). Among sprites the hardware keeps one colour and one priority
  per pixel and walks the OAM in order: a later sprite takes a pixel over only
  with a strictly better priority, and a TRANSPARENT pixel of such a sprite keeps
  the colour but takes over the priority (and the semi-transparent flag). So a
  low-priority sprite that a later, better-priority sprite overlaps is drawn
  in front of a BG that painting by priority hides it behind: the hole in Golden
  Sun 1 and 2, Mario Golf, The Minish Cap. This is mGBA's rule
  (`software-obj.c`, the `else if (current != FLAG_UNWRITTEN)` arm); amcc's patch
  does the same for whole-tile transparency only and per object. Note the
  task text described the rule as "lowest OAM wins among OBJs regardless of
  priority"; that is not what mGBA or gpSP's existing common path do, and the
  implementation follows mGBA.
  `order_obj` marks a row (`obj_inv_row`) only when a sprite out-ranks an
  EARLIER, worse-priority sprite whose x extent it overlaps (four min/max pairs
  per row). Unmarked rows run the old code unchanged behind one predictable
  branch in `render_scanline_objs`. A marked row is built once per pass
  (`obj_exact_build`, cache reset in `order_layers`): every sprite of the row in
  OAM order is drawn into a scratch line by the ordinary `render_sprite`
  (flip, 4/8 bpp, mosaic, affine unchanged), its covered area is known exactly
  (`obj_cover`, the affine texture walked as `render_affine_object` walks it),
  and the pixels are merged by the rule; `render_scanline_objs_exact` then paints
  the pixels of the priority being asked for in FULLCOLOR, INDXCOLOR or
  STCKCOLOR form. OBJ-window sprites are not part of it. `video.cc` is also
  CORE_R (the render thread) and gets the same code. Test: `objprio`
  (`testrom/objprio.c`: out-of-order pair with a transparent half, in-order
  control, an affine transparent texture, a non-overlapping pair), core,
  render thread, and the thread-vs-core check on every frame of 1300; the
  `-DGPSP_NO_OBJ_EXACT` build fails exactly the two defect cases.
  `-DOBJ_EXACT_ALL` (test builds) sends every row down the exact path.
- **Tests:** `testrom/cpufix.c` tests 24-28, `testrom/chain.c`,
  `testrom/objprio.c` (`make -C testrom cpufix chain objprio`);
  `app_tests.py --only cpufix,chain,objprio`.
- **OBJ_EXACT, 2.0 recheck.** `testrom/objrand.c` (`app_tests.py --only
  objrand`): 64 random sprites (8x8 and 16x16, every priority, a third affine,
  four patterns with holes) over three BGs with holes, every one of the 38400
  pixels compared with the rules worked out in Python, on the core and the
  render thread; 16 pixels differ with `-DGPSP_NO_OBJ_EXACT`, none without.
  The sprites of the OBJ window (list 4) are still outside the exact path: in
  mGBA and NanoBoyAdvance their transparent pixels also hand a priority to the
  sprite below. Per-line replay against mGBA's renderer and the instruction
  count (+0.02% of a whole 200-frame run on x86 under callgrind, exact vs
  `GPSP_NO_OBJ_EXACT`) are in the COMPATIBILITY.md section "Sprite order".

- **WP0 counters** (`cpu_threaded.c`, `gpsp_jit_counters(u32 *out, u32 n)`,
  declared in `cpu.h`; each core instance exports its own copy under its prefix).
  Monotonic, 8 slots: ARM blocks translated, Thumb blocks translated, icache/VM
  domain sync calls, bytes they covered, RAM cache flushes, ROM cache flushes,
  RAM flushes from a code write. A few increments per block or flush, nothing on
  an execution path; hashes unchanged. `-DJCTR_DUMP` prints the totals at exit on
  the twins. 700 frames, six real ROMs (syncs / blocks translated / RAM flushes /
  of which SMC): 681/1564/4/3, 12768/26186/1163/761, 554/1420/1/0,
  19236/42348/2299/2266, 689/1490/48/47, 1351/2944/86/85.
- **WP1 (batch the icache sync): not done, finding.** Syncs are already one per
  top-level lookup, not per block (2.0-2.6 blocks per sync above, the chain is
  drained first, `XLAT_ITERATIVE`), and the code a lookup returns is executed
  at once, so the sync cannot move after it. Fewer syscalls needs translating
  ahead of execution (a different cache layout) and a Vita measurement.
- **WP2-WP4 not done in this pass** (WP2 is the SMC_STM_FIXED project above;
  the counters show why it matters: two of the six ROMs flush the RAM cache 1-3
  times a frame). WP2 and WP3 were done afterwards: see the next section.

## Version 1.4: SMC Without Whole-Cache Flushes (SMC_RANGE, smc_partial)

On the ARM and x86 back ends a store over translated RAM code used to throw the
whole RAM translation cache away (and so did a DMA onto code, and a full RAM
cache), and everything that ran next was translated again. WP0's counters put
two of the six test games at 1-3 such flushes a frame; Unbound-class ROM hacks
show 65-84 ms worst frames on a Vita. `SMC_RANGE` keeps the translated code.

**The constraint that shaped it: bit-identical to the full-flush build.** A trap
is not free for the guest: the store leaves its block at the next instruction
and the cycles the block counted since its last update are dropped (upstream
gpSP behaviour). So which stores trap must not change, and which stores trap is
decided by the code tags (one u16 per RAM halfword) that the full-flush build
sets when it translates and clears when it flushes. A scheme that retired only
the blocks a write touches would keep the other blocks' tags, trap on stores the
full-flush build lets through, and drift. `SMC_RANGE` therefore keeps the
full-flush build's tag array, tag numbers and RAM-cache fill pointer *exactly*
and keeps the code separately:

- **Descriptors.** Every RAM translation is recorded (`smcr_desc`): start pc and
  mode, end, a copy of the guest bytes `[start, end)` it was made from (a RAM
  translation reads nothing else: the Thumb literal folding is ROM-only), its
  size and the size before its tail, and its external exits (patch site,
  target). Index: a hash of pc|mode with up to 8 kept versions each.
- **Flushes re-enacted on the tags only.** A code write (`flush_translation_cache_ram_smc`,
  `_dma`) and a full cache (the full-flush build's limit test, with its fill
  pointer kept as `smcr_vptr` and its tag numbers as `ram_block_tag`) clear the
  tags and the code range exactly as `flush_translation_cache_ram` does
  (`smcr_flush_tags`, line for line) and start a new generation. The code stays.
- **Translation re-enacted, code revived.** Where the full-flush build would
  translate a RAM block (the lookup finds no offset in the block's tag entry --
  at the dispatcher or while resolving a block's exits eagerly), `smcr_lookup`
  does what that translation does to the tags, `code_min/max`, the tag entry
  (including the upstream quirk that the first block after a flush sits at
  offset 0 and so is translated again at its next lookup) and the fill pointer,
  runs the same cache-full test at the same block, and queues the block's exits
  in the same order on the `XLAT_ITERATIVE` queue -- but takes the code from a
  descriptor whose source copy is byte-for-byte the current memory, and
  translates only when there is none. Same bytes, pc and mode: same translation
  (checked, see GPSP_SMC_VERIFY). The re-queued exits re-link the kept block to
  the targets' current code (a link whose word changes is synced, below), so a
  kept block can never jump into a translation whose source changed.
- **Two pieces of history the full-flush build's translations depend on**,
  both found by the run-time checks below on Mario Golf and re-enacted:
  (1) a block whose scan stops at a translation gate (`game_config`) also
  tags the word AT the gate (`smc_tag_gate_*_yes`): kept per descriptor and
  replayed; (2) `scan_block` resets `block_data[].update_cycles` for every
  instruction but the LAST one, so the last instruction's flag is whatever the
  previous translation left there (upstream behaviour), and it decides whether
  a cycle update is emitted before that instruction. `smcr_vb` tracks the
  full-flush build's flags; it is loaded into `block_data` before every
  translation the full-flush build also makes (ROM ones included) and read
  back after; a kept translation is only revived when its recorded incoming
  flag equals the current one, and a re-translation reproduces its own.
- **Why a source compare and not a write index.** The compare is exact and needs
  no write tracking, so it also covers the writes that never trap: STM's earlier
  words (only an STM's last word is checked), DMA onto untagged code, C writers.
  A page index of "code present" would have to see every one of those. The
  trap itself is unchanged (the stubs are not touched): it needs neither the
  address nor the size, because what to keep is decided at revival.
- **The executing block.** A trapping store still completes, writes REG_PC =
  next instruction and resumes through the dispatcher (`arm_stub.S` label 3,
  `x86_stub.S` `smc_write`): the next instruction is fetched from a fresh lookup,
  i.e. an instruction the store rewrote right after itself runs in its NEW form
  (gpSP's semantics; an ARM7TDMI would run the prefetched old one). Kept code is
  never patched while it runs: links are re-pointed only inside a lookup, and
  every lookup leaves the block that called it.
- **Space.** Kept code lives in a region that starts `SMC_RANGE_CARVE` (1 MiB)
  below the RAM cache, taken from the end of the ROM cache (ROM: 10 -> 9 MiB on
  the Linux twins, 7 -> 6 MiB per instance on the Vita link build), and ends at
  the RAM cache's tag table. Before every top-level lookup at least one full RAM
  cache plus 256 KiB is kept free (everything one lookup can make the
  full-flush build translate fits its cache); otherwise `smcr_reclaim` throws
  away the code of everything not live in the current generation and keeps the
  live blocks' descriptors without code; they are translated again from their
  own source copy (put back in guest memory for the translator if memory has
  changed: a live block is the translation of what was there) when entered.
  This is the hysteresis: a reclaim leaves the whole region minus the live set.
- **Syncs.** New code is published by the existing `[last, ptr)` sync. A link
  re-pointed in already-published code is noted (`smcr_patch_note`, 64-byte
  lines, up to 16 ranges, else one min..max range) and synced once before the
  lookup returns (`smcr_patch_sync`). On the Vita the VM domain stays open for
  the emulation thread (jit_vita.c), so a patch needs only
  `sceKernelSyncVMDomain`; jctr[11] counts these syncs (also in [2]).
- **Full flushes.** `flush_translation_cache_ram()` from anywhere else (state
  load, cheats, reset, emitter rebuild, `flush_dynarec_caches*`) drops all kept
  code and descriptors.
- **Both instances.** All state is per object, so the link partner (`gbab_*`)
  has its own; the render thread's copy (CORE_R) has no CPU.

**Switches.** Build: `SMC_RANGE=1` in `vita/coreflags.mk` (`SMC_RANGE=0` builds
without it). Run time: `smc_partial` (`u32 smc_partial`,
`gpsp_smc_partial_set(u32)`, `gpsp_smc_partial_active()`; instance B:
`gbab_gpsp_smc_partial_set`), default `SMC_PARTIAL_DEFAULT`=1; it takes effect
at the next `init_dynarec_caches`/`flush_dynarec_caches` (game load or reset),
so the frontend sets it before loading. With it off every line of the old code
runs. Twins: `GPSP_SMC_PARTIAL=0/1`. Things that cannot happen (a translation
that does not fit the reserve, an allocation failure, a re-translation that
comes out different) are counted in `smc_partial_emergencies` and handled by a
full flush (safe, but not the full-flush build's behaviour).

**The app (main_vita.c).** `vita.ini` `smc_partial = 0|1` and
`dispatch_cache = 0|1` (both 1 by default; engineering switches for an A/B on
the console, no Settings row) are given to both machines before a GBA game
loads (`EVT jit_switches`). Each window (600 passes) logs
`EVT vita_jit smc_partial= ram_flush= smc_traps= tag_flush= blocks= vm_syncs=
revived= reclaims= dcache_hits= emergencies=` (counters [4] [6] [7] [0]+[1] [2]
[8] [13] [14] of `gpsp_jit_counters`, deltas over the window) and once, loudly,
`EVT JIT EMERGENCY` if `smc_partial_emergencies` ever leaves 0. The app test
`census` checks both switches are on, blocks are translated and emergencies
stay 0. Memory: the ROM translation cache is 6 MiB a machine now (1 MiB holds
kept code): the two-machine budget only shrank.

**Run-time proof obligations (twins).** `GPSP_SMC_VERIFY=1`: at every tag-only
flush, every reclaim and every 256th top-level lookup the tag array must be
exactly the union of the live blocks' extents, each live block tagged and named
by its tag; and every revival first translates the block AGAIN, in place, from
current memory and compares it byte for byte with the kept code (exit words
aside), plus extent, sizes and exits. Any difference aborts. `GPSP_SMC_TRACE=file`
writes one line per full-flush-build flush (`S` code write, `D` DMA, `F` cache
full, `X` external), with frame, REG_PC and the count of RAM translations the
full-flush build has made: the files of an off and an on run must be identical.
`GPSP_JCTR_LOG=file` writes the 16 counters at the first lookup of each frame.

**Worst-frame counters (P2, 13 Nov 2026).** Each window the app also logs
`EVT vita_jit_worst frame= demand= spec= syncs= sync_us= sync_kb= xlat_us=
xlat_pages= | win frames= demand= spec= syncs= sync_us= xlat_us= xlat_pages=
pages_total= [cap=<gamepak_cap_reason>, first line only]`: for the frame of the
window that spent most time translating plus syncing, the blocks translated
because the guest asked (demand, `translate_depth` 1) vs because an exit
pointed at them (spec, depth >= 2), the publish syncs and their time, the time
of the outermost translations and the ROM pages they pulled in; then the
window's totals. Touched only on the translating path. Finding: syncs are one
per top-level lookup, so in the worst frames syncs == demand blocks (Mario Golf
63/63, Golden Sun 138/138); merging ranges (tried as `vm_sync_batch`, removed)
changed no count. Twin worst frame demand/spec: Mario Golf 63/266, Golden Sun
138/152, Minish Cap 52/60. Vita3K first window: sync_us 13.9 ms over 836 syncs
vs xlat_us 110 ms.

**Lazy exits (`jit_lazy_exits`, `jit_lazy_budget`; ARM back end; default OFF).**
vita.ini `jit_lazy_exits = 0|1`, `jit_lazy_budget = N` (twins `GPSP_LAZY_EXITS`,
`GPSP_LAZY_BUDGET`). With it on, an exit from a ROM block to an untranslated
ROM target is, once the outermost lookup has already translated N speculative
blocks, pointed at a 20-byte island at the end of the ROM cache
(`generate_lazy_exit_island`); the first time the guest takes it the island
enters `arm_lazy_exit_*` (arm_stub.S) -> `block_lookup_address_lazy_*`, which
translates the target like an indirect branch and re-points the exit's `b` at
it (one extra `sceKernelSyncVMDomain` for that word; a link is never written
when the ROM cache was reset meanwhile, `rom_flush_gen`). RAM exits stay eager.
Function-table slots REG_USERDEF+17/18. `vita_jit_worst` shows `lazy= islands=
resolved=`. Armhf twin under qemu, 800-1300 frames, six ROMs (Golden Sun,
Minish Cap, Fire Red, Mario Kart SC, Aria of Sorrow, Mario Golf): picture and
sound identical to off with budget 0 (and Mario Golf at 1300 frames also with
budget 16). Cost: Mario Golf 1300 frames blocks 2233 -> 1681 (-25%) but VM
syncs 1849 -> 3208 (+73%); first-window worst frame 114 -> 80 blocks, syncs
53 -> 105. Budget 16 changes almost nothing (the graph below one cold lookup is
far bigger). Left OFF: whether fewer blocks beat more syncs is only known on a
Vita (Vita3K's sync_us rose sharply with it).

**RAM mirror in the SMC source copy (fix, 14 Nov 2026).** A RAM block that
starts near the end of IWRAM (or EWRAM) continues at the region's start (the
RAM mirrors), and the translator reads it that way. `smcr_translate` copied,
compared and swapped the block's source bytes with `smcr_mem(pc) + len`, which
ran off the end of the array: a fortify abort on the twin (Driver 2 Advance,
about 30 s in), silent corruption on the Vita. They now go through two-piece
helpers (`smcr_mem_read/write/cmp`). Regression: `testrom/smc.c` test 19
(`app_tests.py --only smc`) fails on the old code (GPSP_SMC_VERIFY: "different
code 03007ff8") and passes now on x86 and armhf.

**WP3: indirect-branch dispatch cache (`GPSP_DCACHE`, ARM back end).**
`arm_stub.S`'s indirect-branch stubs (`arm_indirect_branch_arm/_thumb` and the
same-mode paths of the `_dual_` ones: `bx lr`, `pop {pc}`, `ldr pc`, `mov pc`)
probe a direct-mapped table of {key, entry} pairs (`adc_table_arm/_thumb`,
1024 entries each; key = the pc masked as the lookup masks it) before calling
`block_lookup_address_*`: a hit costs ~14 instructions and no C call.
`adc_put` enters only lookups that have no side effects -- a ROM/BIOS/VRAM-stub
entry found in the hash, or a RAM block whose tag entry already holds its offset
(the full-flush build's offset-0 block is translated again at every lookup, so
it is never entered) -- and the table is emptied by every flush of either
cache, tag-only flushes and reclaims included. A hit therefore returns exactly
what the lookup would have, and skips nothing the lookup would have done.
Run-time switch `dispatch_cache` (`gpsp_dispatch_cache_set(u32)`, default 1;
twins: `GPSP_DISPATCH_CACHE`); with it off the table stays empty and every probe
misses. The x86 stubs do not probe (the C side fills a table nothing reads).
Return-address prediction was not changed: the ARM stubs are entered by `ldr pc`
and leave by `bx r0`, with the C call in between a balanced `bl`/`bx lr`, so the
host's return stack is not disturbed; a guest-level return stack (predicting
`bx lr` from the matching `bl`) was not attempted.

**Results (host twins, 13 Nov 2026).** Six private test ROMs, 3000 refreshes,
scripted input (`scratchpad` harness: never copied into the repo), smc_partial
off vs on in the same binary. Identical in every case on x86 (picture hash of
every presented frame), armhf under qemu and armhf VITASIM=1 (no raster;
machine-state hash every 10 frames and at every 4th code-write event): the
GPSP_SMC_TRACE files (every flush of the full-flush build with frame, REG_PC
and its translation count), sound, battery saves and the state hashes.
GPSP_SMC_VERIFY=1 runs (every revival re-translated and compared, tag-array
invariant) pass on Golden Sun and Mario Golf (800-1000 frames) and on the
`smc` cartridge. Counters over the 3000 frames (armhf; x86 the same):

| game | RAM flushes off -> on | blocks translated off -> on (worst frame) | icache/VM syncs off -> on | revived |
|---|---|---|---|---|
| Golden Sun | 3929 -> 1 (+3928 tag-only) | 105036 -> 3409 (295 -> 290) | 59446 -> 1955 | 101627 |
| Mario Golf | 12021 -> 1 (+12020 tag-only) | 247750 -> 4217 (373 -> 329) | 105340 -> 4997 | 243533 |
| Fire Red | 86 -> 1 | 3516 -> 3061 | 1736 -> 1543 | 455 |
| Mario Kart SC | 48 -> 1 | 1402 -> 1225 | 693 -> 613 | 177 |
| Aria of Sorrow | 6 -> 1 | 3635 -> 3499 | 1578 -> 1530 | 136 |
| Minish Cap | 1 -> 1 | 1442 -> 1441 | 562 -> 561 | 1 |

No reclaims and no emergencies in any run. callgrind (x86, frames 300-900,
no raster, whole process): Golden Sun 2184.2M -> 2043.3M Ir (-6.5%), Mario
Golf 2323.3M -> 2153.3M (-7.3%; `translate_block_*` 238M -> `smcr_lookup` 41M),
Fire Red 2289.5M -> 2298.1M (+0.4%: the per-lookup reserve test). WP3 on
armhf: lookups avoided (jctr[14]) per 3000 frames Fire Red 448,818, Aria
785,317, Mario Kart 555,902, Minish Cap 1,138,440, Golden Sun 300,844, Mario
Golf 1,233,739; identical with smc_partial off and on, and the six games give
the same trace, sound, saves and states with `dispatch_cache` off.
Test cartridges (`app_tests.py --only smc,cpufix,chain,objprio`): pass on x86,
armhf and armhf VITASIM=1 (VITASIM needed `hostcore/Makefile` to give
`jit_vita.c` -DARM_FAR_STUBS as the Vita app does: the grown twin image put the
VM block out of plain branch reach and the twin had silently been running the
interpreter). **Not verified: anything on a Vita** -- the VM domain staying
open for patches of published code, `sceKernelSyncVMDomain` on re-pointed
links, the I-cache, the 1 MiB taken from the ROM cache, and real timings.

**Counters** (`gpsp_jit_counters`, now 16 slots; the first 7 as WP0, [4] with
smc_partial on counts only flushes that discard RAM code): [7] tag-only
flushes, [8] revived blocks, [9] RAM translations the full-flush build makes
(identical in both modes), [10] links re-pointed, [11] their syncs, [12] kept
translations passed over because the source changed, [13] reclaims, [14] WP3
dispatch-cache hits, [15] dispatch-cache fills.

## Renderer colour effects and 4bpp tile kernels (gpsp/video.cc)

**Twin difference on testrom/objprio.gba.** The x86 and armhf (qemu-arm) twins
once showed different pictures. The cause of that observation was a stale armhf
binary, built before OBJ_EXACT. Both twins built from one tree give the same
picture (0 pixels differ with the lowest green bit masked, as run_tests.sh
does). `gpsp/tools/tests/check_objprio_twins.py` guards it: it refuses a build
without the `obj_exact_build` symbol (the stale-binary detector), checks the
colour classes at 11 positions on rows 20/24/27, and compares both pictures.

**Undefined behaviour in the OBJ X decode (fixed).** UBSan reports `left shift
of 16484 by 23 places cannot be represented in type 'int'` at three sites in
video.cc. `(s32)(attr1 << 23)` shifts an int past its sign bit whenever attr1
bit 8 (the sign of X) is set. The decode is now `obj_attr1_x()` in
`video_obj.h`, which shifts an unsigned value. The original expression gives
the same result on x86 and armhf at -O0 and -O2 for all 65536 attr1 values, so
the UB was latent and did not cause the picture difference above.

**Sanitizer runs.** A verification build of the x86 twin has UBSan on the
renderer objects (core `video.o` and the render thread's `video_r.o`), with
the rest of the core uninstrumented. The twelve testrom cartridges run with
scripted input and report no runtime errors. The ASan build of the app objects
reports a global-buffer-overflow in fe_gbadual_boot's memcpy of instance B's
.data image, a redzone false positive outside video.cc; it does not appear in
this build.

**Kernels.** `video_merge.h`: merge_blend / merge_brightness. NEON on ARM, SSE2
on x86, original loops elsewhere (MIPS/PSP ME) and for tails. Output is exactly
the original math (per channel floor, clamp to channel max, coefficients
clamped to 16). `video_tile.h`: NEON 4bpp BG tile row and OBJ row kernels
(vtbl2 16-entry palette lookup). Both are in the core and CORE_R builds.
Switches: VIDEO_MERGE_ORIG (old loops), VIDEO_MERGE_NO_VEC, VIDEO_MERGE_SCALAR_BF
(branch-free scalar, slower on x86), VIDEO_MERGE_CHECK / VIDEO_TILE_CHECK
(compare against the reference per row, abort on difference), VIDEO_TILE_NO_NEON.

**Tests.** `gpsp/tools/tests/run_video_kernel_tests.sh [quick]` builds and runs
test_video_merge.cc (all 5-bit channel pairs x 17x17 coefficients, all 65536
colours x BLDY 0..31, random palettes, starts 0..9 x lengths 0..40, guard words)
and test_video_tile.cc (every nibble in every lane, hflip, all rendtypes) on x86,
armhf NEON (qemu-arm) and armhf scalar.
The OBJ X decode is tested by test_video_obj.c: all 65536 attr1 values under
UBSan, with a negative control that the original expression must fail.

**Vita build note.** gpsp/vita/Makefile builds with -mfpu=neon (NEON kernels
active). The root gpsp/Makefile `vita` platform has -mcpu=cortex-a9 but no
-mfpu=neon, so that build silently uses the original loops.

## Version 1.4: Smooth Hand-over And The Look Governor (main app)

Recipe: `playerkit/INTEGRATION.md` sections 7 and 8. What the main app does:

- **Mailbox** (`gpsp/vita/handoff_glue.{h,cpp}`): `pk_handoff` compiled into
  one C++ unit with sceKernel semaphores and a drawing thread
  (`gbadhoc_draw`, `vita.ini` `draw_core` default 1, `draw_prio` default
  0x60), started per game. `sink_video` copies the picture to show (or the
  Super Game Boy frame) into a mailbox buffer; `frame_present` commits it with
  the OSD snapshot (`osd_take`, so the drawing thread never touches the live
  toast queue) and the emulating thread's cost (`now - t_drawn`). The drawing
  thread draws (`vid_draw_frame`), the toasts (`osd_draw_snap`), presents
  (`vid_swap`) and releases with its own time; Steady pacing gets the larger of
  the two costs (`ho_poll_cost`). The drain wait of `pace_frame` and the
  upscale help are off while a drawing thread runs.
- **One owner of the display.** Every drawing entry point of
  `video_vita.c` calls `vid_enter_hook` first; while a picture is in flight
  that waits for the drawing thread to go idle (`ho_guard`, counted as
  `guard_waits` in the `handoff:` line). So the menus, the pause screen,
  rewind's standing picture, the LINKING screen and every Settings change
  cannot draw beside the drawing thread. A state load and a rewind step
  discard what is in flight; a menu or pause resets the game-time baseline.
- **Off** in every link session, local two-player play, the bench pair and
  while the LINKING screen owns the picture (`set_enabled(false)` on a change).
- **The twin** runs the same mailbox; its drawing side is the emulating thread
  itself, at the old present point, so every screenshot is byte for byte the
  old one (app test `handoff`). `GBADHOC_HANDOFF_THREAD=1` runs a real thread
  (`GBADHOC_HANDOFF_SLOW_US` makes it slow) and `GBADHOC_PIC_TRACE=file`
  writes the frame number and CRC of every picture drawn: with a slow thread
  each picture shown is the old path's for its frame, in order, through a
  menu and the Settings row turned off and on.
- **No C++ library.** The app links with the C driver: `handoff_glue.cpp`
  gives `operator new/delete` (malloc/free); `look_glue.cpp` instantiates
  `std::string` and gives the four `std::__throw_*` helpers as `abort()`.
  The weak-symbol check after the link still passes.
- **Look governor** (`gpsp/vita/look_glue.{h,cpp}`, `pk_look`): ladder of
  `docs/DEFAULTS-1.4.md` section 5; fed once a second of play from the drawn
  frame's own work (`emu`), the drawing thread's average (`draw`), refreshes
  and misses (refreshes without a new picture in full mode; `late` without
  hand-over), parallel = hand-over active, half = Steady pacing's half mode.
  Applied in `app_look_apply`: the Auto xBRZ factor cap
  (`upscale_look_cap`), the ambient mode, the blend, the GBA wide mode
  (`app_wide_apply`) and the NES wide mode. Rows set by hand
  (`app_look_user_rows`) are told to the governor whenever the look rows
  change. Memory: `look/<console>/<xx>/<name>.look`, written at the menu and
  at the end of the game (`.tmp`, then renamed).
- **Defaults** (Vita): Image quality Auto everywhere, the base filter sharp
  bilinear, Auto = xBRZ over sharp (`up_kind`; `vita.ini` `auto_xbrz = 0`
  for the 1.3 behaviour), wide fill = soft edges.

## Version 1.4: Lessons-audit fixes (main app)

- **Battery writes off the game thread for every console.** `io_start` hands
  the writer (`fe_host_set_io`) the GB, GBC, NES and WonderSwan battery files
  too: the game thread only snapshots the cartridge RAM
  (`gb_save_flush_periodic`, a memcpy), the temporary file, renames and removes
  run on the io worker. Twin test `worker` compares the bytes with and without
  the worker.
- **Log wakes.** In a game the io worker is woken by the log only with 4 KiB
  waiting (`game_log_wake`); it writes every 250 ms anyway.
- **Link keep-alive.** A silent partner is given up after 20 s, not 8
  (`ND_PEER_DEAD_US` in `netdrv.h`; the join handshake keeps its own 8 s,
  `ND_OFFER_US`). And a partner whose MAIN thread is busy (a state save, card
  I/O: seconds on a Vita) is not silent to this console: with
  `LAN_F_IO_KEEPALIVE` (set by `infra_open`) the transport's I/O thread, which
  owns the socket, sends a small keepalive datagram (`GBLD` type 6, 18 bytes)
  to every session peer once a second and records when anything last came
  from each; netdrv asks `nd_transport.heard_age_us` before it declares a
  peer dead, and gives up on a console its pump has not heard for 60 s
  (three times the limit) whatever its I/O thread says. Tests: `test_lan
  stall12 | stall25 | peerdead` (a pump thread idle 12 s / 25 s with and
  without the keepalive; a peer whose transport vanishes without a BYE ends
  in 19-23 s through HOST_LOST / peer disconnect) and `run_sesslan.py stall
  kill` (the real link session: 12 s stall, 600+ stalled frames, every
  periodic hash still matches; the killed host's partner ends through the
  session's `peer_lost` after ~19.95 s). Gate steps `link-liveness`
  (default) and `link-liveness-long` (full).
- **Link census.** While linked, `link_vita.c` logs once a second
  `EVT link_census in= out= keep_in= keep_out= worst_gap_ms= retx= path=`
  (datagrams from/to the partner, the transport's keepalives among them, the
  longest silence from the partner inside that second, netdrv's
  retransmissions, `lan` / `direct` / `adhoc`). Counted by the I/O thread, so
  a stalled main thread cannot hide a gap. `link_is_active()` (`net_vita.h`)
  says whether a session is linked, for the fetchers: see "Pausing the
  fetchers while linked" below.
- **Pausing the fetchers while linked** (recipe; `artfetch.c`, `ra_vita.c` and
  `main_vita.c` are not `link_vita.c`'s): `artfetch` is already paused for the
  whole game (`artfetch_pause(1)` at boot), so the only web traffic left in a
  session is RetroAchievements. Hand `ra_http_config` / `ra_vita` an `online`
  callback of `net_up() && !link_is_active()` (`#include "net_vita.h"`); the
  achievements' own "will be sent when back online" path then holds the
  requests until the session ends. If the cover fetcher is ever allowed
  during a game, the one line is `artfetch_pause(link_is_active())` once a
  frame in `main_vita.c`.
- **"Save present" without stats.** The library scan keeps 64-bit hashes of the
  `.sav`/`.srm` names and of the folders it read (`savset`, `ui_psp.c`); a row
  asks the set instead of `sceIoGetstat` (folders the scan did not read still
  get the stat). The app adds the save of a game it just ran
  (`ui_savset_add`). Log: `savset names= dirs= full= hits= stats=`.
- **No failed opens for missing covers.** The menu loader reads the names of
  `boxart/`, `hero/`, `scenes/`, `titles/` and `cache/...` once and answers
  "not there" from them (`pix_absent`, `menu_files.c`); every writer of those
  folders calls `menu_files_art_changed`, after which the names are not trusted
  until read again (at most every 5 s).
- **`-fno-math-errno` instead of `-ffast-math`** for the cores (Vita and twin).
  Proof: the twin built both ways from the same sources gave byte-identical
  screenshots and sound for GBA, GB, GBC, NES (NROM, MMC3) and WonderSwan
  test cartridges; the Vita objects were compared instruction by instruction
  (see the 1.4 handoff notes for the list of objects whose code changed).

## Version 2.0: Online

Player-facing description: [Online Play](ONLINE.md). The wire protocol of the
room service is `online/service/PROTOCOL.md`; this section is how the app and
the service fit together and why they are shaped this way.

**Pieces.** `online/service/` (Worker + Durable Object), `gpsp/vita/online_http.{h,cpp}`
(room client), `gpsp/vita/nethttps/` (HTTPS client), `gpsp/vita/online_vita.{h,c}`
(one online connection), `gpsp/netdrv/transport_lan.{h,c}` (STUN, punch, TURN on the
link socket), `gpsp/vita/online_lib.{h,c}` (room card to library file),
`gpsp/vita/ui_vita_online.h` (menus). `link_vita.c` joins them to the link
session: the session above the transport (`fe_gblink`, netdrv, the Wireless
Adapter path) is unchanged and does not know a link is online.

**Service.** One Worker (`vitadhoc-online`, custom domain `vitadhoc.psvita.dev`,
no workers.dev address) in front of one SQLite-backed Durable Object. A room is
one row (state `open`, `joined`, `relay`, `closed`; both members' nickname,
addresses and token). The object keeps no room in memory, sets no alarm and no
timer; expiry is lazy (every request first deletes what has run out), so a
sleeping object costs nothing. Polls are short; there are no held requests,
because Durable Object duration is shared with the other services on the
account. The day's request budget (8000, new rooms from 80 %) is Galaxy's
`budget.js` with a save step of 20 instead of 200 and a save at the first request
after the object starts: this object is idle most of the day and would forget the
count. Per-address limits live in memory only. Relay credentials come from
Cloudflare Realtime TURN (`turn.js`: REST scheme, UDP URLs only), guarded per
day (800 minted) and per month (a used credential counts 50 MB against 50,000).
The zone's one shared rate-limiting rule (`psvita-shared-ratelimit`, 100
requests / 10 s per address on `/v1/*`) is not changed by this service; one
host-scoped WAF rule blocks every other path on the host.

**HTTPS client.** Not `ra_http`/`sceSsl`: Cloudflare's free-plan edge
certificates are ECDSA and `sceSsl` gives no control over the TLS version or
the ciphers. The project's own client (vendored from Galaxy and Strikers) uses
Mbed TLS 3.6.8 (TLS 1.2 and 1.3, certificate verification always on, no switch to
turn it off), twelve compiled-in roots (Google Trust Services R1 and R4, ISRG X1
and X2, four SSL.com, and for the update check USERTrust ECC and RSA and Sectigo E46 and R46), its own worker thread, per-stage time limits and
answers of at most 64 KiB. A console clock earlier than the build's floor is
reported as "date is wrong" and not as a certificate fault (a reset Vita reads
2011). It sends the app's own User-Agent and no custom headers; `?v=<version>`
is on every request and the member token is in POST bodies only. The twin has a
plain-HTTP client for loopback tests (`np_http_plain.cpp`): the Vita talks TLS or
nothing. `EVT online_tls` lines name the stage, the TLS version and, for a refusal,
each certificate.

**Connection (`online_vita.c`).** A non-blocking stage machine stepped once a
frame: STUN (Cloudflare, up to two servers, retransmit 250 ms to 2 s, 5.25 s
in all; two different mappings mean a symmetric NAT) -> create or join the room
-> poll (2 s for 30 s, 5 s to 2 min, 10 s after; 1.5 s while connecting; stretched up to
1.4 times from half of the new-room budget) -> the punch race (4 s) with the relay
credential fetched in parallel -> relay fallback -> measure. The relay is chosen
at once when either player set Always use the relay, STUN failed, or either NAT is
symmetric. Failures map to `UI_FAIL_ONLINE_*` sentences that say who has to act.

**NAT layer (`transport_lan.c`).** Everything runs on the link's own UDP socket
and I/O thread. A datagram from a STUN or TURN server address is taken first
(STUN/TURN message, or ChannelData `0x40..`); anything else goes to the existing
router (`"GBLD"` discovery family, bulk, netdrv `'GPN1'`). Routing by source
first keeps the families apart. Punch is two new discovery types (7 PUNCH,
8 PUNCH_ACK, 26 bytes, CRC16, the room's 64-bit punch key; an older build ignores
them). The joiner's declared-to-observed address alias (`nat_rewrite`) then works as on
the old Internet connection. Relay: an allocation per console, a permission and
channel for the peer's RELAYED address, data as ChannelData; both consoles post
their relayed address, which is what hides both public addresses. Keep-alives
(`DISC_KEEP` once a second, netdrv PING) come from the I/O thread and the pump,
not the game loop, so an open menu does not drop a mapping. Online rooms are not
announced on the LAN (`LAN_F_NO_DISCOVERY`); the same-router case uses the `lan`
candidate the room carries.

**Input delay.** `delay_pick()` clamps Auto at 12 frames for Online and 8 for every
other connection; a fixed setting stays 1 to 8. The session's own limit is
`CONFIG_DELAY_MAX` (60), so no protocol change is needed. The quality badge uses
the measured round trip: Great under 80 ms, Good under 120 ms, Slow above. Both
numbers are guesses until real paths have been read (`EVT link_rtt`, `gblink_win`,
`online_quality`).

**Two players (NES, WonderSwan, Super NES).** A room of the pad kind. Both
consoles run the host's machine: the host's pad is port 1, the joiner's is port
2 (a WonderSwan has one pad, which takes both), and only buttons cross, in
lockstep, over the same `fe_gblink` session as the cable modes. For NES and
WonderSwan `fe_padpair` is the machine side: the host's state and battery start
both, the joiner's own game and save are set aside and put back, and only the
host commits a save. It has no `partner_rom`, so a different file is refused:
the room's `game` is the 8 hex digits of the whole file's CRC-32 and the game
list shows the first four. The compatibility word is the version, link ABI and
build only. A Super NES game is played by the separate player (`snes.bin`), so
the app forms the link first (`link_snes_begin`: the same room, code, punch and
relay as for a NES game), then hands the player the peer, the punch key and any
relay credentials in the launch file (`link.*` keys; `playerkit/src/pk_launch.cpp`);
the player makes the link again on the same local port. The game list's
Settings > Two players page (`ui_vita_snes.h`) is the way in for it; Nearby is
not offered, because the system's ad hoc connection does not outlive the
change of program.

**Library match.** A room card carries the console, the game code (GBA header code,
GB/GBC title code) and `rom`, a CRC-32 of the file size and 64 slices of 4 KiB
taken from the image the core runs (after any patch). The joiner's step is: code,
then file fingerprint; a cable game is never opened on its code alone, and the
session's own SHA-1 exchange (`fe_gblink` HELLO/NEED) stays the final word.

**Settings and keys.** Menu: Connection (Same Wi-Fi, Online, Nearby; Direct address
only with `vita.ini link_direct = 1`), Always use the relay (`online_relay`), Link
delay. A 1.4 config with Internet becomes Online unless `link_direct = 1`.

**Logging.** `EVT online_*` lines for every stage (`online_stage`, `online_stun`,
`online_room`, `online_peer`, `online_punch`, `online_relay`, `online_quality`,
`online_tls`, `online_http`, `online_fail`, `online_lookup`, `online_lib_*`), beside the
existing `link_*` lines. Addresses are in the log, as with Wi-Fi: review it before
sharing.

**Build.** The Vita build needs an Mbed TLS prefix (`MBEDTLS=`, from
`tools/online/build-mbedtls.py`): see [Building](BUILDING.md). Without it everything
compiles, the client has no TLS and `make vpk` stops, because a package without
TLS would be an Online that cannot connect. Mbed TLS adds roughly 300 to 400 KB
of code and a few hundred KB of heap while a request runs; the numbers are
checked at the Vita build.

**Not verified.** Everything here is proven on the Linux twin against local
servers and the NAT/TURN simulator (RFC 4787 mapping and filtering models, a test
TURN server, loss, delay, jitter), not on a console and not against the live
service. Only two Vitas can show: TLS on the console (its sockets, resolver,
clock and thread), the extra traffic on one socket beside netdrv, how real home
routers and phone hotspots behave (the share that needs the relay), Cloudflare's
TURN details (including whether a relayed session survives its credential's
hour), and whether the input-delay cap of 12 and the 80/120 ms limits suit real
paths.

## Version 2.0: Fixes From GBAdhoc 5.0.0-beta.6, And Mbed TLS 3.6.8

Taken from the public GBAdhoc repository (the author of our GPL permission),
each diffed against this tree first: our core has moved on, so none was pasted.
Source commit ids are in [Source Record](SOURCE.md).
The two test cartridges are built by hand (`make -C gpsp/vita/hostcore/testrom cpufix biossound`)
and run with `python3 app_tests.py x86 --only cpufix,biossound` (and `armhf`).

- **No backup chip** (`4d94b62`). New `BACKUP_NONE` and a table flag
  `FLAGS_NO_BACKUP` (`gba_over.h`; the table's `flags` is now 32 bits, the 16
  were used up). `read_backup` reads 0xFF, `write_backup` and `write_eeprom`
  ignore writes, so the cart is never turned into EEPROM or flash by a probe;
  savestates accept the new value (`memory_check_savestate`). One row: Top Gun -
  Combat Zones (`A2YE`), whose copy check passes only when all three of its
  SRAM and flash probes find nothing. A player's save-type choice (Settings > This
  game) still wins. Test: `tools/tests/test_rtc_save_type.c`
  (`test_no_backup`), and the savestate preflight test's out-of-range value is now
  `BACKUP_NONE + 1`.
- **Direct boot on scanline 126** (`8db021f`): with the BIOS skipped, `init_cpu`
  writes VCOUNT = 0x7E, as mGBA's skip-BIOS start (a real BIOS run reaches the
  cartridge on line 127). Every direct-booted game's first-frame timing moves by 126
  lines, so boot-time hashes change and only those.
- **STM with R15** (`6eb61d6`): the interpreter stored the instruction address + 8; it
  is + 12 (ARM7TDMI), like STR. The x86 and ARM dynarec back ends already stored
  + 12 (`arm_block_memory_final_store`). Test: `testrom/cpufix.c` 29 (ROM, dynarec)
  and 30 (VRAM, interpreter), `stmia {r1, pc}` and `stmfd sp!, {pc}`, so the two
  must agree.
- **Open BIOS sound driver** (`abcd4ed`): SWI 19h, 1Ah, 1Bh, 1Ch, 1Dh, 1Eh, 28h and
  29h were `swi_Invalid` (a bare return; `swi_SoundDriverMain` in
  `softwareinterrupts.c` was never in the table). `bios/source/sound_driver.c` is
  the direct-sound driver; the image is `patch_unaligned_decomp.py` then
  `splice_sound_driver.sh`, with the driver at 0x2880 after our decompression stubs
  (upstream used 0x2800, which those occupy). The music player (20h-24h) and the
  sequencer jump table are still empty returns. Test: `testrom/biossound.c` (a
  cartridge that calls 1Ah, 1Bh, 1Dh, 1Ch and starts a voice) through `app_tests.py`
  `biossound`: the WAV is a steady tone, the same twice.
- **No cable on the serial port** (`8c7a93c`): multiplayer mode with no cable device
  (AUTO, DISABLED, GBP, RFU) reads SI = SD = 1, ID 0 (a child) and its start bit does
  nothing, instead of a cabled parent whose transfers complete with stale
  SIOMULTI. The netplay modes (POKE, AW1, AW2) and the link-cable session are
  unchanged. Test: `testrom/cpufix.c` 31.
- **Not taken:** `8c1b1e0` (a client's scheduled ticks keep it on the host roster)
  and the two netdrv commits after it. They fix the Wireless Adapter's scheduled
  ticks (`RFU_SCHED`, `rfu_sched`), which this tree does not have: the host
  already refreshes `clttl` on a client's keepalive ACK, and nothing in `rfu.c`
  stops that keepalive. `aeb96a6` (GB palettes) is another change.

Mbed TLS moves from 3.6.7 to 3.6.8 (commit `82e69999`). It carries the TLS 1.3
transform clean-up that was our one local patch (`afa648aa` is an ancestor of the
tag), so the patch, its overlay and the lock's patch entry are gone and
`build-mbedtls.py` builds the pinned checkout as it is (a lock entry that names a
patch is refused). 3.6.8 also rejects a TLS 1.3 version in a TLS 1.2 ServerHello's
legacy version field, which our client profile (TLS 1.2 and 1.3, client only) is
exposed to.

## Version 2.0: Community QA Fixes (start-up, space, taps, GB kind, FF pitch, covers)

- **Start-up scan.** `rom_scan_boot()` (ui_psp.c) draws the first frame before
  any folder is opened, then `rom_scan_dir` calls `scan_ui_tick()` per entry,
  which redraws "Finding games... N found" at most every 33 ms, reads the pad
  (Circle sets `g_scan_stop`) and stops at a 240 s budget. A stopped scan counts
  as an I/O error so the partial list is never cached. A single blocking
  `sceIoDopen` cannot be interrupted; a missing mount fails at once.
  Log: `boot_times first_frame_ms= input_ms= library_ready_ms= walked= stopped=
  frames=`. Test: `gpsp/vita/tests/library/library_test.c` (`test_cold_start`,
  600 entries at 100 ms each: same list CRC as the synchronous scan).
- **Free space.** `gpsp/vita/diskspace.h` (`ds_device_of`, `ds_free_for` via
  `sceIoDevctl(0x3001)` then `sceAppMgrGetDevInfo`, 64-bit, `ds_failed_line`).
  Gate step `disk-space`.
- **Tap screen for menu.** `config.ini tap_menu` (default 1), the launch file
  key `tap_menu`; the players read it in `input()`. The chords are in
  `playerkit/src/pk_input.cpp` (`legacy_chord` now also for SMS, GG, SG).
  Tests: `input_menu_chords_open_on_every_console`, `launch_tap_menu_key`,
  `app_tests.py --only tap_menu,touch,controls_defaults,snes_session,sega_session`.
- **GB kind.** `gpsp/vita/gbkind.h`: header flag 0x143 (0x80 / 0xC0 = Color),
  override file `gb_override.txt` (`gb|gbc <file name>` per line). Identity
  (save suffix, state folder) follows the extension (`sidecar_file_console`).
  Gate step `gb-kind`; `library_test.c` `test_gb_header_lists`.
- **FF normal pitch.** `playerkit/src/pk_ffnorm.h` (every `factor`th chunk, 128
  sample crossfade); `ff_audio` 0..2 in the main app (`g_ff.norm`), `ff_sound`
  0..2 in the launch file. Test: `ff_normal_pitch_is_smooth_and_keeps_the_pitch`.
- **Cover fallbacks.** `gpsp/vita/artalt.h` (pure) feeds `add_alts()` in
  artfetch.c once every normal candidate has missed. Tests: `test_artalt.py`
  (gate step `cover-alt`; set `AF_NOINTRO_DIR` to a folder with `gba.dat`,
  `gba.tree`, `snes.dat`, `snes.tree` for the real-list rates) and two cases
  in `test_artfetch.py`.
- **Players' left stick.** Already a Controls row (Off, D-pad, Buttons) with
  D-pad as the default, as in the main app, where the left stick is always the
  d-pad. Test `input_left_stick_is_the_dpad_by_default_on_every_console`.

## Version 2.0 Changes

The engineering list for 2.0 (the player-facing version is in the
[changelog](../CHANGELOG.md)). Every figure is from the PC builds, an emulator
or a model unless it says otherwise: nothing in 2.0 has run on a real Vita.
Sub-sections above (Online, GBAdhoc fixes, Community QA fixes) hold the longer
write-ups of single areas.

Also in 2.0, from the commit record:

- **Game Boy Camera and Game Boy Printer.** `gpsp/gbcore/tgbdual/gbcam.h` (cartridge `$FC`: the
  mapper, the camera registers, the M64282FP model, state in the core's reserved
  block; see LOCAL-PATCHES.md), `frontend-common/fe_gbcam_fit.h` (the camera's
  8:7 crop and shrink to 128x112) and `fe_gbprinter.h` (the printer protocol on
  the serial port, answered from `gb_serial_transfer` when no link partner is
  there and the game starts a packet with `$88`; strips are handed to the
  platform, which writes the PNG on the worker the clip export uses
  (`plat_job_*`), the strip copied first). `plat_camera_*` is SceCamera on the Vita
  (ABGR into a 256 KB CDRAM block, read without waiting; compiled with the
  SDK, never run) and a script feed on the twin. `gbcore_camera_requests`
  counts the captures a game starts; `main_vita.c gbcam_frame` opens the
  camera on the first one and closes it after 120 frames (about 2 s) without
  another, and calls it stopped after 60 frames (180 while it is warming up)
  with no picture. Tests: `gbcore/tests/gbcam.c`,
  `gbprinter.c`, `app_tests.py --only gbcam,gbprinter`.
- **Super FX.** `gsu_fast`, an exact block-cache interpreter for the chip
  (pre-decoded blocks, block chaining, fused prefixes, dead-flag elimination,
  folded delay slots, precomputed cycle costs, per-depth plot routines): about
  1.9x on the chip, results identical to the old interpreter. See
  docs/PERFORMANCE.md.
- **Super NES upscaling.** xBR in the SNES player, cheaper wide compose and a
  lighter rewind capture; NEON MMPX in the main app.
- **Two players for the Sega consoles.** A new MIT link layer (`pk_link` in
  playerkit, written from scratch) replaces the GPL-derived link files in the
  SNES and Sega players, which must not link them; the build checks it.
- **Room service hardening.** A stored room-code guess limit, a peer lock for
  online sessions, safer room text, unpredictable player session ids, STUN/TURN
  fuzzing; relay credentials counted when issued, per-address and per-room
  caps, relay only for two active members; the TURN key reply is accepted as
  Cloudflare actually sends it (HTTP 201, unwrapped).
- **State checks.** Every core (Genesis Plus GX, Snes9x, FCEUmm, Beetle
  WonderSwan, gpSP, TGB Dual) validates a loaded state; fuzz harnesses and a
  gate step cover them.

- **A state sent by the other player, or loaded from a file, can no longer crash or hang an emulator.** Starting a link loads the host's game
  state on the guest's console, and the same loader reads save state files and Share game data. A state made up on purpose (with its sizes
  and checksums repaired) used to be able to crash or freeze the Genesis core and others. Every core (Genesis / Master System / Game Gear /
  SG-1000, Super NES, GBA, Game Boy and Color, NES, WonderSwan) now checks the inside of a state before using it (indexes, banks, sizes,
  counts, timers) and refuses it, or brings impossible values back into range; and each was fuzzed on the PC build with a memory checker:
  no crash, no hang, no report. States the consoles themselves wrote, and older versions' states, still load and the game runs on exactly as
  if it had never been interrupted (all test games, 4 load points each). Not proven on a Vita; the GBA fuzzing ran the interpreter build.
- **The same looks on every console.** The main app's Settings > Display now has the rows the players have, with the same names and
  0..10 strengths (0 is off): **Ambient glow**, **Scanlines**, **LCD grid** (two rows now, each on its own; they used to be one Video filter
  with one strength) and **LCD ghosting** (was on/off) on the GBA, Game Boy, Game Boy Color and WonderSwan. **LCD colours** also for
  the WonderSwan, and **NTSC colours** for the NES. The GBA now starts with **GBA screen** colours, which look better on the Vita's screen
  than the raw ones (it is one row to change; judged on PC renders, not on a Vita). Old settings, per game too, are carried over once
  (the grid and scanline filters become their rows at the same strength). See docs/LOOKS.md.
- **NES: Ambient glow off now means black bars.** With the wide view on, the glow off still left blurred side bars.
- **Sega games no longer lose sprite collisions on frames that are not drawn.** Fast forward, Steady pacing's half mode and catching up
  after a long frame run frames without drawing them, and the Sega core took its sprite collision and overflow flags from the drawing:
  in 15 of 67 test games the game itself then ran differently. An undrawn frame now still runs the sprite pass of every line, and
  leaves the machine exactly as a drawn frame does (sound, state and memory equal on 105 test games, PC build).
- **Quick response for the Sega consoles.** Speed > Quick response (Off / Auto / On) shows a button press one frame sooner: the next frame is
  worked out ahead and shown, and the machine runs exactly as without it (sound, state, memory and battery save equal on 105 test games, PC
  build; a small test cartridge shows a press 1 frame after it plain and 0 frames after it with Quick response). Auto is the default on the
  Master System, Game Gear and SG-1000 and starts only while the game leaves the room; the Mega Drive is Off by default.
- **Quick response for the Super NES.** Speed > Quick response (Off / Auto / On, Auto by default) in the SNES player, the same run-ahead
  with a light snapshot of the machine in memory (`snes/core/ahead.cpp`) instead of Snes9x's state load, which resets the machine and
  makes the picture thread wait and take the whole video memory again: only what a frame can change is copied (the list was taken from
  the core's own static and global variables), video RAM goes back only where the look-ahead frame changed it (and only those tiles leave
  the tile cache), the sprite tables are copied aside when the look-ahead frame rebuilds them, the sound side is saved and put back in
  the APU thread's own command stream. Save and way back cost 0.02 to 0.08 ms on the PC build (Snes9x's save and load 0.3 to 2.4 ms).
  Proof: `snes/twin/ahead_check.sh` (gate `snes-ahead`): machine, sound, memory identical every frame, the next frame's picture, one
  frame less latency, on 164 public test programs and own SA-1, DSP-1 and wide view cartridges (snes/README.md, "Quick response").
- **Quick response in the app (Game Boy, Game Boy Color, NES, WonderSwan).** The same run-ahead for the cores the app runs itself
  (`fe_host.c` `gb_ahead_frame`, decision by `playerkit/src/pk_ahead.h` through `gpsp/vita/ahead_glue.cpp`, one frame at a time): real frame
  undrawn, `gbcore_ahead_save` (the machine's state plus a carry block), the next frame drawn and silent, `gbcore_ahead_load`. What the
  state formats miss is carried: Game Boy the frame counter, the clock's base and wall-time reading; NES the palette mirrors, the sound
  filter phase and history, the envelope reload flags, the sweep reload flags, the sample latch (and a mapper restore hook that zeroes a
  write timer, MMC1 and COOLGIRL, is told not to); WonderSwan the frame count and the orientation detector. Two state loaders were not
  exact and are fixed (they also make quick loads and rewind landings exact): the Game Boy noise generator's position equal to the output
  rate was folded to zero, and the NES envelope reload flags were not in the state. Proof: sound, quick save and battery file identical with
  it forced on and off (`ahead_check.sh`: 33 Game Boy and Game Boy Color, 51 NES, 5 WonderSwan test ROMs, PC build; NES also with the wide view
  on), the picture shown is the next frame's (`ahead_picture.sh`, middle of the screen: the ambient glow beside it blends over time), a press shows
  one refresh sooner (`ahead_latency.sh`). One difference remains and is left as it is: the Game Boy renderer's window line counter is counted
  only while lines are drawn, so after an undrawn real frame it holds another value until the next frame resets it; it is outside the machine's
  own hash. Off while linked, online, in local two-player play, in fast forward, in rewind, in a menu and in Steady pacing's half mode. The cost
  and the Game Boy Advance decision: docs/PERFORMANCE.md.
- **Sega states and rewind come back exactly.** A loaded state, a rewind landing and a quick load now continue exactly as the game that was
  never interrupted (first line of sprites, FIFO flag, PSG changes, EEPROM position, flash save chip included): 105 test games, 4 load
  points, with and without the VDP worker. The first game started after launch no longer differs from the same game started later. Old
  states still load.
- **No sprite flicker (Master System, Game Gear, SG-1000).** These consoles drew 8 sprites on a line (4 on the SG-1000) and games blinked
  the rest. Picture > No sprite flicker (on by default) draws them all. Only the picture changes: the console's overflow and collision
  flags are still set by the hardware's limit, so the game runs exactly the same (73 test games, PC build). Turn it off for a game that
  hides something behind the limit on purpose. The Mega Drive keeps its own limit.
- **Sega save states no longer carry stray memory.** A few unused bytes of a state file held whatever was in memory.
- **Sega CD, second pass** (all PC twin; nothing has run on a Vita).
  - **Saves.** The RAM cartridge defaults to 512 KB (what the games and the real RAM cartridge expect); Off is an empty slot. The save file is
    8 KB backup RAM, the fitted cartridge, then the cartridges of the other sizes with their length and a mark, so changing the size never
    loses a cartridge (a test writes markers into 128 KB, reloads with Off, 256 KB and 128 KB again and compares byte for byte).
  - **Region and BIOS.** A disc of a region with no BIOS falls back to the BIOS found and says so on screen and to the app.
  - **State loaders.** The disc hardware loaders (graphics ASIC, CD drive, CD controller, PCM, sub CPU) now check what they read: the stamp
    map and dot masks against the registers, the line cycle count, the drive's scan offset and latency, the interrupt bits, the sub CPU's cycle
    count (a negative one made the scheduler spin) and a program counter in an unmapped page. The ASan + UBSan state fuzz has a disc case
    (`statefuzz --disc DISC --bios BIOS`, run by the state-fuzz gate step): host runs of 150-200 s with five seeds loaded about 2800-2900 of
    about 3000 damaged states each without a crash or hang after the fixes (the first run found four faults: a division by zero, a hang,
    an index out of range and a crash), and a loaded state runs on as the uninterrupted machine.
  - **State loaders, second pass.** The counters that feed the sub CPU's cycle count and so bypassed its clamp are now checked on load:
    `gfx.cycles`, `cdc.cycles[0..1]`, `scd.cycles`, `scd.timer` and `scd.stopwatch` (a value out of the range a running machine reaches is put
    at 0; a finished graphics operation's or an idle chip's stale negative value is left as it is, because genuine states hold those and the next
    start overwrites them), and a negative `cdd.latency` (an unsigned field: a negative one reads as a huge one) becomes 0. A program counter
    in a page with no base address is still put back at 0; the comment now says what can reach it (in this fork `genesis.c` gives pages
    $80-$DF `work_ram`, so only the MegaSD pages $38-$3F, the CD cartridge pages $40-$7F and the sub CPU's backup RAM, register and
    1M-mode dot image pages), and `state_load_notes` makes the host log it ("state load: ..."). Host test: the load proof and
    three 20 s ASan + UBSan fuzz runs (seeds 1-3) on the synthetic disc and stand-in BIOS, about 400 states each, about 385 loaded and
    ran, the rest refused, no crash, no hang.
  - **The helper's waits are bounded.** The emulating thread waits at most about 2 ms for a block the helper is on (`take_ready`); then it
    marks the slot ABANDON (compare-and-swap BUSY to ABANDON), reads the bytes itself (the file's own descriptor, or the emulating thread's CHD
    decoder) and counts a timeout (census field `[9]`, `timeouts=` in the `disc:` log line). The helper finishes its block into the slot,
    sees the state is no longer BUSY and frees it; nobody reuses the slot meanwhile.
  - **A helper that stalls longer than a close waits (reviewed fixes).** A close of one file waits for the helper at most about 250 ms
    (`wait_idle`, no lock held); `cdio_end` (the first step of `md_host_unload`, so also of every disc swap) waits the same bound for the
    helper's `done` flag instead of an unbounded join. If the helper is still in a read: the cache is ORPHANED (unregistered, put on a list
    under the lock, never changed again; the CHD's helper context is heap allocated so an orphan stays exactly as the helper read it at its
    claim, and a later `cdio_chd_open` cannot touch it). The helper, the only thread that can be on it, frees the orphans (cache memory,
    file, path, helper descriptor and its budget count, decoder and the file under it) right after the block it is on and once more as its last
    act; a closer that comes after the helper stopped taking blocks frees its orphan itself. A `cdio_end` that gives the helper up leaves the
    semaphores, the thread and every cache it may be on alone, switches the helper off for the session (no helper for the next discs; it is
    back at the next `md_host_set_threads` once the stalled helper has left; the Sega player calls it once per launch, so on a Vita it stays off until the player is
    started again) and counts it in census `[9]`; a leaked thread handle is the
    price. The victim descriptor of the helper's budget is closed after the lock is released (a slow `sceIoClose` no longer stalls the
    emulating thread's `register_cache`). `cdio_gets` (and any read of a file whose cache is registered) never uses the one-block path that
    writes slot 0 with plain stores: it reads the exact bytes directly. Host test (twin, x86): `cd_test` stalls the helper's next block for
    500 ms (`md_host_cd_stall`), closes (and, as a second case, swaps the disc) while it stalls on a raw stream and on a CHD: the close comes
    back in under 450 ms, `[9]` counts it, the next disc loads and reads byte for byte, and a later session has its helper again; clean under
    ASan + UBSan (leak check on) and ThreadSanitizer (only the test's own never-joined stalled thread is reported). Not run: armhf / qemu (no
    ARM toolchain in this workspace), and nothing here has run on a Vita. The helper no longer calls `sceIoOpen` with the cache lock held (the open is made outside the lock),
    so `register_cache` on the emulating thread never waits for a slow open. On the Vita the helper is kept off the system core (core 3): when
    the plan puts the writer there the helper takes core 0's gaps below the emulating thread, so no thread of ours that nothing watches
    can be held there. Host test (twin, x86, not a Vita): `cd_test` runs the helper with a stall of several ms now and then; about 14-100
    waits per CHD, 2-21 of them given up, every byte still identical (193,000 checks). How often the 2 ms bound is met on a Vita is not known.
  - **Disc id of a CHD with no SHA-1.** A v5 CHD whose raw SHA-1 field is all zero used to give every such disc the same Two players id.
    Now the id is the CRC-32 of the file size, the 124 byte header and the first 512 bytes of the hunk map; a file with no map there is
    refused. Such an id is not the id the same disc would have with a SHA-1 (both players must hold the same file anyway).
  - **Rewind.** A scd state is about 1 MB (host measure: capture 0.11 ms, pack 0.17 ms on the stand-in BIOS, which is lighter than a game), so
    rewind captures every 4 frames on a disc (2 on a cartridge); the ring then holds about the same seconds.
  - **Disc thread.** `md/vita/main.cpp` now passes `MD_THREAD_DISC`; the helper takes the writer slot of the core plan (its priority, 0xA0 when there is no plan, below the
    game's 0x60) and has its own 128 KB stack. `md_thread_ops` has a new `nap` hook: `wait_idle` and `take_ready` sleep 100 us (Vita) or 50 us
    (twin) instead of spinning on a yield that a lower priority thread cannot use. The numbers are guesses until measured on a Vita; the
    per-second log line prints the helper's CPU time and a `disc:` census (reads, from helper, direct, waits, lost ms).
  - **Fixtures.** `tools/mkscd.py` writes compressed CHDs itself (no chdman): LZMA, zlib, zstd (the zstandard module when present, else stored
    blocks), FLAC (fixed predictors), a mix with stored hunks, 4 and 32 frame hunks and self references, with the real ECC and EDC so the
    reader's ECC rebuild runs. The reader checks every hunk's CRC, so a wrong byte fails; `cd_test` reads 17 images byte for byte, each again
    on the helper thread. The Sega CD player tests (walk, pads, look, rewind, run-ahead, RetroAchievements, costs) run on the synthetic disc in
    the new `md-player` gate step. What a real chdman file adds (its hunk size and codec choice) is not covered.
- **Frames that are not drawn leave every console's machine as it was.** Fast forward, Steady pacing's half mode and catching up run frames
  without drawing them. Checked, state, memory and sound compared between drawn and undrawn runs (PC build, one frame in two and one in three
  undrawn): Super NES 15 of 15 games (3000 frames; the one byte that differs is a drawing cache the next drawn frame refills), NES 263 of 263
  test cartridges (900 frames), Game Boy and Game Boy Color, GBA and WonderSwan games in the app (see docs/TESTING.md). No core needed a change.
- **Smooth 50 Hz for European games.** PAL games on the 60 Hz screen showed one picture twice every tenth of a second, a hitch in every scroll.
  Settings > PAL games speed (Super NES, Sega and, in the app, NES) has a new first choice, **Smooth 50 Hz**, on by default: the game, its sound
  and its input run exactly as before, but the frame the sixth refresh waited for is run one refresh early and that refresh shows an even mix
  of the two pictures; the next refresh shows the new one. The picture step per refresh is .5 .5 1 1 1 1 instead of 0 1 1 1 1 1 (the squared
  deviation from an even step falls from 0.83 to 0.33). Only that refresh differs, and only where something moves. True 50 Hz and Fast 60 Hz
  are as before. It is not used in a linked game.

- **Online Two players on a NES or WonderSwan game compares the games.** A room made from inside a NES or WonderSwan game carried an
  all-zero game number, so a joiner running another game was let in and only turned back when the link started, and the game list's
  "Join online" had no number to look for among its games. The room now carries the game's own number (its CRC) and the joiner's card
  says "Your friend is playing a different game. Add it to this Vita to join." at once.
- **Online relay: a joiner whose first relay request is refused asks again.** The service gives relay credentials only once both players have polled the room (403 `relay_not_allowed` before that). The app treated that first refusal as the end of the join ("could not reach your friend"); it now asks again every 3 seconds, up to 12 times, so the relay fallback works when the joiner is quicker than the host's next poll.
- **Wide view for the Sega consoles.** The Mega Drive / Genesis, Master System, Game Gear and SG-1000 fill the Vita's whole width like the Super NES
  and NES do: the console's picture stays pixel exact in the middle and the sides show more of the game's world where that can be shown truthfully, and a soft
  continuation of the picture's edge where it cannot. Picture > Wide view: Auto (on, and it narrows the real sides on a busy game before it would cost a
  frame), On, Off (the 4:3 picture back). Nothing the game does changes, with the view on or off.
  - **It remembers what you have seen.** On the Master System, the Game Gear and the Mega Drive the sides show the level you have already walked
    through (the part that scrolled off, and the part ahead when you turn back), not only a soft edge. Master System games have nothing beside their
    picture in the console itself, so there this is all of the real picture at the sides. Never a guessed tile: a remembered cell is shown only
    while its graphics are unchanged and the level is recognised; otherwise the soft side. (SG-1000 games keep the soft sides.)
  - **More of the Mega Drive picture is covered**: lines under a status window (the playfield behind it continues), and lines of games that scroll
    columns of the picture up and down, where the columns at that edge move together.
  - **It costs the game nothing**: the extra picture is drawn and judged on the picture thread while the game's own thread goes on, strips of a
    picture that stands still are reused, and the last step is shared between the helper threads. By the model the heaviest test programs keep the
    full real sides at 60 frames a second (not measured on a Vita; Auto still narrows the sides if a game ever needs the time).
  - The Master System and SG-1000 upscale with xBR 3x now (it steps down to 2x and sharp when the game needs the time); the Game Gear and the Mega
    Drive keep xBR 3x and 2x.
- **Super NES wide view: the same picture for about half the work.** At the Vita's full width the wide view's own work is roughly halved (its
  picture is bit for bit what it was): a side that shows the soft frame is no longer worked out on every frame, margin layers that never reach the
  screen are not drawn, and the rest is shared with a helper thread. Rewind's snapshot is now written while the game waits for the screen instead of
  before the frame is shown. By the estimates the heaviest games (Yoshi's Island, Kirby Super Star) can keep the full width more often; Super Mario
  World and Super Metroid may still narrow it. Not yet measured on a Vita.
- **Super NES wide view: the sides are drawn on a thread of their own.** The extra picture beside the game is now drawn by a separate thread
  (the picture thread drew it before), and the helpers that share the wide view's work moved to the two cores that have room (one of them sat on the
  core of the thread it was meant to help). The picture is bit for bit what it was (15 test games, 3000 frames each, PC build and ARM under an
  emulator; thread checker clean), and a background colour effect (the sky of Super Mario World and Yoshi's Island) is drawn faster, exactly. By the
  estimates every thread now fits a 60 fps frame at the full width on all 15 test games; on a console without the fourth core a heavy scene of
  Yoshi's Island, Super Mario World or Zelda may still make Auto narrow the margin. Not yet measured on a Vita. An option that would save more by
  counting the two sides on alternate frames exists but is off: it changes the sides' fade timing visibly in some games (Street Fighter Alpha 2).
- **The home screen appears at once.** The first frame no longer waits for
  the card to be searched for games: a card that takes a minute to read (a
  large card in a slow adapter) now shows "Finding games... N found" with the
  last game found, the games already known from the last visit stay in
  view, and Circle stops the search and shows what was found. The list is
  the same one the full search would have made; a search that was stopped is
  not remembered, so the next visit looks again. A missing card does not
  hold the start. The log line `boot_times` gives the first frame, the first
  response to a button and the time the list was ready.
- **A failed save no longer claims the card is full.** The message now says
  what is true: "ux0: is full (3 MB free)" when it is, "has 150 GB free; is the
  folder read-only?" when it is not, or that the card could not be checked.
  Free space is counted in 64-bit sizes for the card the game is on (ux0,
  uma0, imc0 or ur0), so a big card no longer reads as nearly empty.
- **Settings > Controls > Tap screen for menu** (On by default) turns the
  tap-the-game menu off, in the main app and in the Super NES and Sega players,
  for players whose thumbs touch the screen. Start + Select opens the menu on
  every console (the Super NES and Sega players also accept L + R + Start);
  the game hints now name Start + Select for all of them.
- **Game Boy and Game Boy Color games are told apart by the cartridge, not
  the file name.** A `.gb` file whose header says Game Boy Color now plays and
  is listed as a Color game, and a `.gbc` file that is a plain Game Boy
  cartridge as a Game Boy game. Saves and states stay where they are: they
  follow the file's own extension. To override a game, add a line `gbc <file
  name>` or `gb <file name>` to `gb_override.txt` in the VitAdhoc data folder
  (a file only, there is no menu for it yet).
- **Fast forward at normal pitch.** Settings > Fast-forward (and Keep sound in
  the Super NES and Sega players) has a third choice beside Off and Sped up:
  the game's sound plays at its normal pitch while the game runs fast, with
  the skipped stretches joined by a short fade so it does not crackle.
- **More covers found.** When a game's exact name has no picture, the cover
  search now also asks for the same title under the other regions, without
  its revision and language tags, and with a subtitle's dash as a space (at
  most seven more requests, once per game). On the full No-Intro lists the
  share of games with a cover available rose from 87.1% to 89.3% on Game Boy
  Advance and from 84.5% to 88.5% on Super NES.
- **Rewind, fast forward, patches and RetroAchievements in the Super NES and
  Sega players.** Hold the right stick left to rewind up to 60 seconds (sound
  off), right to fast forward 2x or 3x (with sound if you like; hold or toggle);
  both are off in two player games and rewind is off in Hardcore. Sega games take
  an IPS, BPS or UPS patch like the others. Signed-in players get achievement
  toasts, challenge, progress and leaderboard markers, and Hardcore rules
  (no rewind, cheats or state loads), with the account taken from the app.
- **Sega Genesis / Mega Drive, Master System, Game Gear and SG-1000.** Each is
  a console in the picker with its own shelf, folders (`ux0:ROMS/Genesis`,
  `SMS`, `GG`, `SG-1000` and RetroFlow's Sega folders), zip and `.smd` support,
  names and box art by checksum, Starred, Recent, play time, Continue, state
  shelf, cheats from the bundled database, RetroArch save import and export,
  crash protection and the Cover button, played in the separate `md.bin`
  program. Two players for them is written but switched off for now.
- **The console picker is alphabetical** by the console's displayed name, so
  any new console lands in place; the game counts stay.
- **A console picker instead of a row of console tabs.** The top bar names
  only the console in use. Triangle (or a tap on its name) drops a list of every
  console under it, with the number of games found in each, the one in use
  highlighted. Each further Triangle or Down moves along it, Up goes back, and
  the library behind follows at once. The list closes by itself after a
  moment, on any other button, or on a tap beside it; a tap on a row picks it.
  Adding a console to the app is one line in its table.
- **Online play.** Play together > Connection: Online. The host's screen shows
  a code of five characters; the friend types it on a code pad (d-pad or
  touch) from the game menu or the game list (Settings > Join online). The
  Vitas link directly when they can and through a Cloudflare relay when they
  cannot ("Connecting through the relay."); **Always use the relay** hides
  each player's address. Works for GBA Link Cable, GBA Wireless Adapter, Game
  Boy / Game Boy Color Link Cable and sharing. VitAdhoc finds your copy of
  the host's game by its cartridge code and file fingerprint and starts it, or
  says which game to add. Messages say who acts (code expired, a newer or older
  VitAdhoc on the other Vita, a wrong clock, a busy day). A connection quality
  message (Great, Good, Slow) appears when the link forms. Link delay Auto may use
  up to 12 frames online (8 elsewhere). No accounts; the room service stores a room
  only until the link forms or 10 minutes pass. See docs/ONLINE.md. Not yet
  tried on two real Vitas or the live service.
- **The typed-address "Internet" connection is now "Direct address"**, shown
  only with `link_direct = 1` in `vita.ini`; an Internet setting from 1.4
  becomes Online.
- **Game Boy colours.** Original Game Boy games are now coloured as a Game
  Boy Color coloured them: **GBC auto** (the default) picks the palette the
  Color's start-up chose for that game. Settings > Display > Game Boy colours
  also offers the twelve palettes you could hold buttons for at start-up
  (GBC Up - brown and the rest), the Super Game Boy's 32 palettes (1-A to
  4-H), and the plain Grey, DMG green and Pocket. The row shows the four colours as you
  choose, and "This game" can use its own. A colour you chose before 2.0 is kept (the old
  Auto becomes GBC auto).
- **Undo for save states, and slot lock, on every console.** In the save
  states page, R undoes the last save over a slot (the state that was there
  comes back, one step) or the last load (you go back to just before it).
  L locks a slot: a locked slot shows a padlock and cannot be overwritten or
  deleted, by a menu save or a quick save, until you unlock it. Not in Hardcore or while linked.
- **RetroAchievements shows more.** A small mark when an achievement is one
  step from unlocking, a progress count ("3/10") under the toast, and the live
  value of a leaderboard run at the corner of the screen (leaderboards run
  in Hardcore). Your rich presence is now logged when it is sent. Settings >
  Achievement indicators turns the marks off; unlock toasts stay.
- **Under the hood:** a TLS client of our own (Mbed TLS 3.6.8, eight root
  certificates, certificate checks always on) for the room service, STUN,
  hole punching and a TURN relay client on the link's own UDP socket, and the
  room service itself (`online/service/`, one Cloudflare Worker and one
  Durable Object). See docs/TECHNICAL.md and docs/BUILDING.md (building needs the
  Mbed TLS prefix).
- **GBA fixes from GBAdhoc 5.0.0-beta.6.** Top Gun: Combat Zones now has no
  save chip, as on the real cartridge, so its menu opens. Games that start
  without the BIOS begin on the screen line where the BIOS hands over (Polarium
  Advance no longer derails in its first frames). Rock 'N Roll Racing no longer
  breaks when a routine pushes the program counter. A game that probes for a
  link cable (Activision Anthology's two-player test) finds none instead of a
  ghost player. The built-in BIOS now plays the BIOS sound driver, so games that
  leave mixing to it (X-Men: Reign of Apocalypse) are no longer silent.
