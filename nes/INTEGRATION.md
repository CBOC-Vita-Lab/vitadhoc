# Using the NES core inside the main app (in-process)

The NES player is a core linked into the main app, like the Game Boy core. Nothing here starts another program.
The core is FCEUmm (`nes/core`, GPL-2.0-or-later) plus `nes/frontend/host.c` (plain C, MIT), built as ONE relocatable
object, `nescore.o`, whose only global symbols are the 75 `nes_host_*` functions declared in `nes/frontend/host.h` (57 before the wide view
additions of 12 Nov 2026; the 57 are unchanged).
Everything else FCEUmm defines (`RAM`, `X`, `PPU`, `GameInfo`, `XBuf`, `dendy`, `crc32`, `FCEU_*`, ...) is localised
inside the object, so it cannot clash with gpSP or TGB Dual.

(The earlier standalone `nes/vita` program, `nes.bin`, was left as it was when the plan changed. It still builds
against the same host and is not part of this route. Its `host_files` layer is not in `nescore.o`.)

## Build lines

```
make -C nes/frontend                 # host x86_64:   nes/frontend/build/host/nescore.o
make -C nes/frontend vita            # VitaSDK:       nes/frontend/build/vita/nescore.o   (VITASDK=/opt/vitasdk)
make -C nes/frontend armhf           # arm-linux-gnueabihf-gcc, if installed
make -C nes/frontend check [TARGET=vita]   # fails on any global that is not nes_host_*; lists the undefined libc names
```

What it does (the same as `gpsp/vita/corebuild.mk` does for TGB Dual):

```
$(CC) <flags> -c each of nes/core/*.c boards/*.c input/*.c libretro-common/streams/memory_stream.c   (list: nes/core/sources.mk)
$(CC) <flags> -c nes/frontend/host.c
$(LD_R) -r -o nescore.o.tmp <all objects>
$(OBJCOPY) -w --keep-global-symbol='nes_host_*' nescore.o.tmp nescore.o
```

Flags: Vita `-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -std=gnu99 -fno-strict-aliasing -O3`
(the ~400 mapper files `-O1`, they run on cartridge writes only: `BOARD_OPT`; the core's own warnings are off with `-w`,
`host.c` is built `-Wall -Wextra` and is clean). Defines: `-D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DNDEBUG`
(plus `-D__vita__`). Include paths (core only; the app needs only `nes/frontend` for `host.h`): `nes/core`,
`nes/core/compat`, `nes/core/libretro-common/include`, `nes/core/input`, `nes/core/boards`.

For `gpsp/vita/corebuild.mk` (not edited here; this is the shape):

```make
NES_O := ../../nes/frontend/build/vita/nescore.o
$(NES_O): FORCE
	$(MAKE) -C ../../nes/frontend TARGET=vita
# add $(NES_O) to the link, and -I../../nes/frontend for host.h
# licences into the VPK: ../../nes/core/LICENSE=licenses/FCEUmm-GPL-2.0.txt and nes/core/PROVENANCE.md (licenses/ and
# notices/ are already packed by DIST_NOTICES)
```

Checked here: `make -C nes/frontend check` for host (75 globals with the wide view, with and without SCOUT=1) and earlier for host and vita (57 globals after the Zapper, paddle and sprite-exception additions of 12 Nov 2026; 51 before; all `nes_host_*`), and
`nes/twin/tests/link_test.c` linked with the host object next to user globals named `RAM, X, PPU, GameInfo, XBuf,
dendy, FCEUI_Emulate` and a user `crc32`, which then runs a game from memory (part of `selftest.sh`); the Vita object
also combines with that test through `arm-vita-eabi-ld -r` without a duplicate symbol. Not done: a link into the real
`eboot.bin`, and nothing has run on a console.

Undefined symbols (the object calls only the C library): `malloc calloc realloc free memcpy memmove memset memcmp
strlen strcmp strncmp strcasecmp strchr strrchr strstr strtok_r strtoul snprintf sprintf vsnprintf sscanf fopen fread
fclose fseek ftell stat sin cos atan`, the toolchain's `__aeabi_*` division helpers and `_ctype_` (newlib). The file
functions are used only by the core's own file code, which the memory API never reaches. No C++ runtime, no threads, no
global constructors. Link with `-lm`.

## The API contract (host.h is the reference; every function has its rules in the comments there)

One instance per process, one thread (the one that runs frames), no locking. Nothing is called from an interrupt or the
audio thread: the audio thread reads the main app's own ring that the emulation thread filled from `nes_host_audio_take`.

| need | calls |
|---|---|
| boot | `nes_host_init()` once; options before the first load: `nes_host_set_option("region"/"no_sprite_limit"/"fourscore"/"ram_init"/"sound_quality"/"lowpass", v)` |
| load from memory | `nes_host_load_mem(rom, size, "Game.nes", err, errlen)`: iNES, NES 2.0, UNIF and FDS; the ROM buffer is copied (free it after); powers on |
| Disk System | `nes_host_is_fds_image(buf, n)`, then `nes_host_set_fds_bios(8192 bytes)` (the user's `disksys.rom`; copied, never shipped); without it the load returns 4 and a message naming `disksys.rom` |
| unload | `nes_host_unload()` (cheats cleared); `nes_host_shutdown()` |
| one frame | `nes_host_run_frame()` (cheats are applied inside); `nes_host_set_render(0)` skips drawing for fast forward / rewind, with identical game state and audio (tested: same pictures after, same samples) |
| video | `nes_host_video(&w,&h,&pitch)`: 256x240 RGB565 (emphasis applied), valid until the next frame; `nes_host_overscan(&top,&bottom)` = 8/8 on NTSC, 0/0 on PAL/Dendy; `nes_host_visible_rows` = 8..231; for a GPU palette path `nes_host_palette()` (768 RGB565 entries: 0..63 base, then 8 emphasis banks of 64 at 256+(e<<6)) and `nes_host_video_indexed()` (the index bytes and a per-row emphasis byte; the twin proves they rebuild the same picture) |
| audio | `nes_host_set_audio_rate(hz)` once, 8000..96000, default 48000: the core synthesises at that rate directly (mono, doubled to stereo), no resampler in our code. The main app's `audio_start(in_rate, ...)` accepts 8000..65536 and `fe_host_sample_rate()` gives the rate the GB core feeds it (the GPSP option `gpsp_sound_rate` is 32768): set `nes_host_set_audio_rate(fe_host_sample_rate())` or 32768 or 48000 before the first frame, and call `audio_push(buf, frames)` with what `nes_host_audio_take(buf, max)` returns after each frame (about 546 frames of 32768 Hz or 800 of 48000 Hz per NTSC frame). The take never blocks; if nothing drains it, the oldest second is dropped. |
| pads | `nes_host_set_pad(0..3, mask)`, bits `NES_A=1 B=2 SELECT=4 START=8 UP=16 DOWN=32 LEFT=64 RIGHT=128`; opposite directions cancel; pads 3 and 4 need the Four Score (`nes_host_set_fourscore(NES_4P_AUTO/ON/OFF)`; auto uses FCEUmm's list of known games, the same one RetroArch uses) |
| states | `nes_host_state_size()` is fixed for the loaded game (measured at load, plus 1 KiB headroom); `nes_host_state_save_mem(buf, cap>=size)` always returns exactly that size (zero padded), so a rewind ring can use equal slots; `nes_host_state_load_mem(buf, size)`. Measured sizes: NROM 14,766 bytes, MMC1 14,812, MMC3 14,845, a one-side Disk System game 113,714 (a four-side disk is about 4x the disk part). Tested equal across 12 saves in 300 frames for each. |
| battery RAM | `nes_host_sram_size()` / `nes_host_sram_data()` (pointer into the core), `nes_host_sram_load(bytes, n)` right after the load (marks it saved), `nes_host_sram_dirty()` (a compare against the last saved copy), `nes_host_sram_mark_saved()` after your write, `nes_host_sram_mark_unsaved()` after a failed one. The main app does its own safe saving: the bytes are the RetroArch `.srm` layout (8 KiB for a standard cartridge; for FDS the whole disk image). |
| achievements | `nes_host_ra_read(addr, out, n)` implements the console 7 map (`$0000-$07FF` system RAM, `$6000-$7FFF` cartridge RAM, addresses are the CPU's own; a read is cut at the end of its window, unmapped returns 0 bytes, no side effects); `nes_host_ram()` is the 2 KiB RAM for fast polling; `nes_host_peek(addr)` reads one byte through the CPU map |
| light gun, paddle | `nes_host_zapper_active()` is 1 when the loaded game uses a Zapper (the core's game database lists Duck Hunt, Hogan's Alley, Wild Gunman, Operation Wolf, Gumshoe, ...; or option `zapper` = 1 forces one on port 2). Then each frame: `nes_host_set_zapper(x, y, trigger)` with the touched picture pixel (0..255, 0..239 of the full 256x240 frame; anything outside = aimed off the screen) and trigger = finger down. The gun reads the picture the core drew: it needs rendering on (not `nes_host_set_render(0)`), and the core draws no cross hair. Tolerance is a 6 pixel rounded target. `nes_host_paddle_active()` / `nes_host_set_paddle(x, button)`: the Arkanoid "Vaus" (NES port 2 or Famicom expansion port, by the database or option `paddle` = 1), x = 0..255 screen column. Tested with synthetic cartridges (`gun.nes`, `paddle.nes`: left half white / right half black; position byte 9d / 55 / 0d for x = 0 / 120 / 255). Not tested with Duck Hunt itself. |
| no flicker | option `no_sprite_limit` (default 1). Game state is identical with it on and off (tested, below); for the few games that hide parts of the picture behind the 8 sprites per line limit a built-in CRC table turns it off by itself (`nes_host_sprite_limit_exception()` = 1 for the loaded game so a menu can say so; `nes_host_sprite_exception_add(crc)` adds a CRC-32 of PRG+CHR, as returned by `nes_host_rom_crc`, at run time). |
| cheats | `nes_host_cheat_add("SXIOPO+1234:05")`: Game Genie (6/8 letters, ROM read patches), raw `AAAA:VV` / `AAAA?CC:VV` and Pro Action Replay; `nes_host_cheat_reset()`, `nes_host_cheat_count()`; RAM pokes are written each frame inside `nes_host_run_frame`. Invalid codes return 0. |
| console | `nes_host_reset()`, `nes_host_region()/_detected()/nes_host_is_pal()/nes_host_fps()` (60.0988 or 50.007), `nes_host_rom_title/_info/_crc` |
| Disk System drive | `nes_host_fds_info(&side,&sides,&inserted)`, `nes_host_fds_toggle_disk()` (eject / insert), `nes_host_fds_next_side()` (the disk must be ejected first) |
| messages | `nes_host_take_message(buf, cap)` (the core's last on-screen text, e.g. "disk inserted"), `nes_host_set_log(fn)` |

PAL and Dendy games run at 50.007 Hz: with a 59.94 Hz display the caller runs 5 emulated frames in 6 refreshes (the
standalone program does this with an accumulator; `nes_host_fps()` is the rate to use).

Not supported (the core has the code, the host does not wire it): Power Pad and the other extra devices (mouse, keyboards, Family Trainer, Hyper Shot, ...), NSF music files (refused with a message), the core's hi-res NTSC filter, overclocking. Wired: the standard
pads, Four Score and the Famicom 4 player adapter, Zapper, Arkanoid paddle.

## File extensions, console id, RetroAchievements, thumbnails

`gpsp/vita` already classifies `.nes`, `.fds`, `.unf`, `.unif` as `FE_CONSOLE_NES`. The core recognises a file by its content,
not its extension (so a zip's inner file works). RetroAchievements console id: **7** (NES / Famicom;
Disk System is 81). Thumbnails repository: "Nintendo - Nintendo Entertainment System" (Disk System games:
"Nintendo - Family Computer Disk System"). Checksum matching for box art and for achievements hashes the iNES file
**without** the 16 byte header and without the 512 byte trainer (when header byte 6 has bit `0x04` set), over PRG + CHR
only: `start = 16 + (flags6 & 4 ? 512 : 0)`, `length = prg_units * 16384 + chr_units * 8192`. That equals the CRC-32 that
`nes_host_rom_info()` prints as `crc=` and `nes_host_rom_crc()` returns (verified for a trainer ROM in `selftest.sh`).
UNIF files carry their data in chunks: match them by name only. FDS images: the file without its optional 16 byte
`FDS\x1a` header.

## Memory footprint and cost

- `nescore.o` sections (from `make check`): Vita ARM build text 674 KB, data 63 KB, bss 2.62 MB (host x86_64: 777 KB,
  94 KB, 3.94 MB, with 8 byte pointers). The bss is the core's fixed tables (the 64K-entry CPU read/write handler
  arrays 256 KB each on ARM, a 512 KB flash image buffer for some mappers, a 256 KB sound phase table) and counts
  whether or not a game uses it; untouched pages are never resident on a system that maps bss lazily.
- Heap: the loaded ROM image (PRG + CHR as the cart needs, 40 KB to 1 MB or more), mapper RAM, one second of stereo
  audio (`calloc`, 192 KB at 48000 Hz, 128 KB at 32768 Hz), and a 1.5 MB temporary buffer that measures the state size
  during `nes_host_load_mem` (freed before it returns: plan for that peak). `host.c`'s own static data: the 120 KB
  RGB565 frame and a 2 KB palette.
- Measured on the Linux twin (x86_64, peak resident set of the whole process, runner included): 6.1 MB for a
  synthetic game, 6.3 MB for MMC3 and FDS, 6.1 MB for the 41 KB public test ROM `ny2011`. The runner and libc are
  about 2 MB of that. Not measured on a Vita.
- Per-frame cost, measured as instructions (callgrind on x86_64, scripted pad, 12 Nov 2026 build): synthetic NROM 2.84 M,
  MMC1 2.68 M, MMC3 2.79 M, public demos and tests 2.6-3.4 M (`DROPOFF7` 3.19, `TEST.NES` 3.15, `SPRITE` 3.40, `ny2011` 3.24).
  Counting executed ARM instructions under qemu-arm (`-one-insn-per-tb`, 12 frames minus 2) gives 1.04 (NROM) to 1.13
  (`ny2011`) times the x86 count, so about 2.9-3.8 M ARM instructions per frame. At 444 MHz that is roughly 9-12 ms at an
  assumed IPC of 0.7, 6.5-8.5 ms at 1.0 and 5-6.5 ms at 1.3, against 16.6 ms; IPC on a Cortex-A9 was NOT measured (no
  hardware), and it excludes GPU upload, audio output thread and the app. Before the 12 Nov 2026 `FCEU_BuildBgPairLUT`
  cache the same demos cost 10-14 M instructions per frame, so older figures are obsolete.
- Where it goes (NROM): RefreshLine 24%, X6502_Run 23%, RGB565 conversion in `nes_host_run_frame` 14%, sound 9%, PPU loop 8%.
- Object sizes (`make check`): armhf text 761 KB, data 68 KB, bss 2.62 MB; Vita build text 676 KB, data 63 KB, bss 2.62 MB.

## Wide view (16:9) — additions of 12 Nov 2026

NES pictures fill a 960x544 screen at square pixels (84 px per side for 240 lines) with real game picture where it can
be proven, never a guessed tile. The original 256 columns stay exactly as `nes_host_video` shows them (tested pixel for
pixel); only the 8 columns the game masks on the left are filled in where the picture there is known. All existing
`nes_host_*` functions are unchanged; these are additions (host.h, section "wide view"):

| need | calls |
|---|---|
| switch on | `nes_host_wide_enable(1)` and `nes_host_wide_margin(84)` (0..96; 0 or mode 0 = off, the default). Off = the core records nothing |
| each frame | after `nes_host_run_frame()`: `nes_host_wide_frame(&rgb, &w, &pitch, &stats)` returns (256 + 2 * margin) x 240 RGB565 (0 = not available this frame: use `nes_host_video`). Scale it with ONE factor (no stretch) |
| learned map (second visit of a level) | `nes_host_wide_map_key()` (PRG+CHR CRC, 0 = do not save), `nes_host_wide_map_save(NULL,0)` = size, `nes_host_wide_map_save(buf,cap)`, `nes_host_wide_map_load(buf,n)` right after the game load, `nes_host_wide_map_dirty()`. The app does the safe file I/O (the image is CRC checked; a bad file loads as empty) |
| options | `nes_host_wide_option("period", 0/1)` (periodic foreground continuation: OFF by default, can be wrong over pits; per-game opt-in only), `"fade_px"`, `"appear_frames"`, `"debug"`; `nes_host_wide_profiles_load(text, n, msgs, cap)` = widekit profile table (by CRC or title) |
| logging | `nes_wide_stats` (mean real px per side, status bar lines, ring size, map cells, scene events, hindsight); `nes_host_wide_real()` per row real px; `nes_host_wide_prov()` (debug) |
| scout (look-ahead) | `nes_host_scout_available()`, `nes_host_scout_enable(1)` BEFORE the game load, `nes_host_scout_step(max_frames, max_us)` once per frame, `nes_host_scout_option("ahead"/"resync", v)`, `nes_host_scout_set_clock(fn)`, `nes_host_scout_stats(&st)` |

Build: `nescore.o` now also contains `nes/frontend/wide.c` and widekit (`widekit/src/*.cpp`, C++11 compiled with
`-fno-exceptions -fno-rtti`, no C++ runtime calls, no static constructors: check the undefined list of `make check`).
The scout needs `make -C nes/frontend SCOUT=1 [vita]` (output `build/<target>-scout/nescore.o`): a second copy of the
core + host.c inside the object with every global renamed `nesscout_*` (`ld -r` + `objcopy --redefine-syms`, like the
second GBA instance), so about twice the code and bss of the core. Without SCOUT=1 the scout functions exist and do
nothing.

Threading of the scout: `nes_host_scout_step` may run on a worker thread at the same time as `nes_host_run_frame` (the
two machines share no memory), never at the same time as `nes_host_wide_frame` or any other `nes_host_wide_*` /
`nes_host_scout_*` call, load, unload, reset or state load. Suggested frame: main `run_frame` || worker
`scout_step(2, budget_us)`; join; main `wide_frame`. `nes_host_wide_frame` takes the snapshot (a 14-15 KB state save of
the live game) when the scout needs one; `scout_step` loads it into the scout machine. One scout frame costs about one
emulated frame; the scout stops when it is `ahead` frames (default 90) in front of the game and waits.

What it does (details in `nes/frontend/wide.c`, `nes/core/nesp_wide.h`, `widekit/INTEGRATION.md`):

* The core records per line the scroll, fine X, BG banks, nametable mapping, palette, mask/emphasis, whether the line
  was steady, and on plain boards renders the margin pixels with read-only fetches (`nes/core/PROVENANCE.md`). Every
  nametable byte written through `$2007` gets a frame stamp.
* The adapter folds the mirroring into widekit's ring (vertical mirroring / four-screen: 512 px wide; horizontal /
  one-screen: 256 px, no neighbour data), gives each cell a redraw code (tile + palette) and a content hash (tile
  graphics included: CHR RAM and bank switching are compared by graphics), keeps cells just written / written during
  the picture / under the masked seam column out of the learned map, and draws learned cells with the banks in force.
* Lines widekit finds fixed (status bars) stay original width with a soft edge. On a game that never scrolled sideways
  (one-screen), lines whose whole row is backdrop continue as flat backdrop.
* Boards with a fetch hook (MMC2, MMC4 and the others that install `PPU_hook`), MMC5 and PEC-586 get no margin
  rendering: full-width picture with soft edges (`stats.lines_blocked`). MMC3 IRQ counting is untouched (the margin
  fetches never call a hook); verified identical frames/RAM/state/audio on MMC3 cartridges including the IRQ split
  cartridge.
