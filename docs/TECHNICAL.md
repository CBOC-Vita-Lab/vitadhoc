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
- **Continue** (`vita/main_vita.c` resume_*): a state taken in memory on the
  frame thread when a game is left, every two minutes of play and 30 frames
  after an in-game save, written by the I/O worker as `Game.rsm` (temp, old
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
  rcheevos v12.5.0 with `RC_NO_THREADS`): the rc_client runs on the game
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

## Diagnostics

Switches include `drc = 0`, `render_thread = 2` for comparison,
`gpu_xbrz = 1` for experiments, `clips = 0` / `art = 0` to turn off optional
work, and `ra_host = http://host:port` for a test RetroAchievements server.
Logs under `ux0:data/GBAdhoc/log/` may contain ROM paths and network
addresses; review them before sharing. The package does not install a clock
or plugin profile. [Known Issues](../KNOWN-ISSUES.md) lists what still needs
a Vita.
