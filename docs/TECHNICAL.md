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
- Tests: `app_tests.py --only save_sync,save_gb_keep,save_import` and
  `vita/tests/savesync/test_save_sync.c` (the compile line is in its header).

## Version 1.3 Changes

**Smoothing that gives way** (`vita/upscale_vita.c` governor,
`vita/main_vita.c` game loop). Issue #7: with full-screen CPU xBRZ the game
ran at about 60 emulated frames but fewer actually shown.

What was already there: xBRZ/Scale3x are cut into 8-row stripes and scaled
on two helper threads (cores 1 and 2) while the emulator keeps core 0, only
changed columns are scaled again, scrolling turns a ring instead of
rescaling, fades drop to 2x inside the same textures, and each finished
picture waits behind a GPU fence (`vb_tex_writable`) before its texture is
written again. A picture that is not ready when the next arrives is dropped
(the game never waits), and 20 late pictures in a row step the job factor
down. That per-picture rule only sees pictures the helpers finished late,
not what reaches the screen. Bands of rows already give identical output to a
single pass: `band_split` in `tests/upscale/upscale_test.c` checks xBRZ cut
into uneven row bands and rectangles against the whole picture, and the
existing run compares each finished texture (with 2 helpers under
`GBADHOC_UPSCALE_ASYNC=1`) against a from-scratch scale.

Added: a governor driven by what the player sees. Once per frame the game
loop calls `upscale_gov_frame(work_late)`. A frame is bad when its own work
(catch-up frames left out) took more than 16 ms, when `pace_frame` counted a
missed refresh, or when smoothing missed a picture (slot still busy, picture
over 16.7 ms, or the texture still fenced). In windows of 60 frames:
6 or more bad frames with smoothing in use for at least half the window is one
step down (the factor the picture asks for minus one: xBRZ 4x, 3x, 2x, then
`upscale_factor` returns 0 and the user's own GPU filter draws); 10 clean
windows (at most 1 bad frame) in a row is one try back up. A try that fails
doubles the wait (10, 20, 40 ... 600 windows); one that holds for 20 windows
forgets the doubling. The rule is `upscale_gov_step` (pure, tested). Windows with
smoothing not in use change nothing. If the lowest step is still bad, the
emulation itself is over budget: no step up is tried, and Keep full speed's
catch-up frames (which skip drawing only, emulation and sound stay at
60 Hz, default on) are what remains. This is the "Frame skip: Auto"
behaviour, so no second setting was added. The governor follows Keep full
speed, is off in fast forward and link sessions, and resets with each game.
The game menu shows one plate line ("Smoothing is lighter to keep full
speed.") while stepped down. The log gets `smooth_gov step=down|up level=N`
and `vita_upscale ... stepped=` counts the downs.

On the Linux twin the governor only runs with `GBADHOC_GOV=1`, since a twin
finishes a picture before `upscale_submit` returns and its clock is the host's.

Tests: `tests/upscale` (band split, governor rules including an on-the-edge
case that makes 22 moves in an hour of frames), `app_tests.py --only
smooth_gov` (slow frames while xBRZ draws: steps down, later up, few steps; none
with Keep full speed off).

Measurements (Linux x86 twin, Pokemon Emerald copy, fit 4x xBRZ, 1800 frames
of menu mashing and walking, relative only; the machine was shared):
- Smoothing off (bilinear): 11.9 ms of CPU per frame (whole process, emulation
  included, 1800 frames). xBRZ 4x: 11.95 ms per frame. The scaler's cost is
  inside the noise on this shared x86 machine; the twin cannot show the Vita's
  44-of-60 shown frames, which came from the console's own cores and memory.
- The governor itself was exercised with simulated slow frames (`--slow`), see
  the `smooth_gov` test, not with timings.
Nothing here was measured on a Vita. The step sizes and windows are from
reasoning and the twin, not from console traces; the thresholds (16 ms, 6 of 60)
are the first thing to adjust once a console log with `smooth_gov` lines exists.

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
- lr-gpsp-amcc OAM order changes: see the report; not ported.

**Tests:** `cpufix` tests 12 to 21 (`testrom/cpufix.c`): reload read after
overflow, register-shift conformance from ROM and VRAM, Rm = PC in a shift,
BX to a halfword address, a branch into a conditional run (ROM and IWRAM), FIQ
banking, `LDM {pc}^`, `MSR CPSR_c` flags. The cart takes `-DRUN_MASK=bits` to
run some of them.

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
  SceHttp/SceSsl with the root certificates in `ra_roots.h`; plain HTTP over
  sockets for tests). The game's hash is the MD5 of the whole file (patched
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
