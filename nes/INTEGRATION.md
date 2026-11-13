# Using the NES core inside the main app (in-process)

The NES player is a core linked into the main app, like the Game Boy core. Nothing here starts another program.
The core is FCEUmm (`nes/core`, GPL-2.0-or-later) plus `nes/frontend/host.c` (plain C, MIT), built as ONE relocatable
object, `nescore.o`, whose only global symbols are the 78 `nes_host_*` functions declared in `nes/frontend/host.h` (57 before the wide view
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

Checked here: `make -C nes/frontend check` for host (78 globals with the wide view and the split API, with and without SCOUT=1) and earlier for host and vita (57 globals after the Zapper, paddle and sprite-exception additions of 12 Nov 2026; 51 before; all `nes_host_*`), and
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

Cost, measured 12 Nov 2026 (`nes/twin/tests/wide_cost.c` linked with `nescore.o` SCOUT=1: `scroll_vert.nes` from a saved state
at frame 300, holding right, 84 px margins; instructions per frame as the difference of two run lengths):

| | x86_64 (callgrind) | armhf (qemu-arm, `-one-insn-per-tb`) |
|---|---|---|
| wide view off | 2.71 M | 2.72 M |
| wide view on (live thread) | 7.14 M (+4.43 M) | 6.25 M (+3.52 M) |
| wide view on + scout (`scout_step(2)` per frame) | 15.09 M | 12.54 M (+6.30 M, meant for a second core) |

Where the +4.4 M (x86) goes: widekit `wk_compose_frame` 2.2 M (of it the soft-edge base drawn under every margin pixel
0.6 M), margin rendering in the core (`nesp_tiles`) 0.54 M, `describe` (ring codes / hashes / stamps) 0.26 M,
`wk_layers_update` 0.21 M, the rest under 0.1 M each. Vita estimate (444 MHz, IPC NOT measured): the wide view adds 3.5 M
ARM instructions to the emulation thread = 7.9 ms at IPC 1.0, 11.3 ms at 0.7, 6.1 ms at 1.3; with the core (2.7 M) that is
14.1 / 20.1 / 10.8 ms against 16.6 ms. At IPC 0.7 it does NOT fit with the scout on the same thread; even alone it is tight:
the compositor is the first place to save (skip the soft-edge base under opaque real pixels; draw only the margins that
changed). The scout's 6.3 M (two scout frames per frame while it catches up, nothing while it is `ahead` frames in front)
needs its own core: 14 ms at IPC 1.0. Memory: `nescore.o` host text 877 KB / bss 4.17 MB (wide view and widekit included),
SCOUT=1 1.66 MB / 8.2 MB; Vita SCOUT=1 text 1.47 MB, data 125 KB, bss 5.57 MB; heap at enable: widekit map 32768 cells
(about 1 MB) + layers + compositor (~0.5 MB), the scout's ROM copy and one state buffer.

Split capture / compose (13 Nov 2026; additions to host.h, `nes_host_wide_frame` is now exactly capture + compose):

    emulation thread:  nes_host_run_frame(); nes_host_wide_capture(&snap[i]);
    compose thread:    nes_host_wide_compose(&snap[i], &rgb, &w, &pitch, &stats);      // one frame behind
    scout thread:      nes_host_scout_step(2, budget_us);                              // after capture(N)

`nes_wide_snapshot` is a caller-owned fixed block (`NES_WIDE_SNAPSHOT_BYTES` = 264 KB, `nes_host_wide_snapshot_size()` bytes used;
zero it once, 8 byte aligned; rotate two or three). It holds per-line records with page indexes instead of pointers (scroll,
banks, palette, masks), the nametable pages and their write stamps (copied only when the recorder's generation `nesp_ntgen`
changed), the pattern pages the steady lines use (1 KB each, at most 16 per frame, a line needing more is treated as unsteady),
the margin pixels the PPU drew (raw, the line's grayscale / emphasis pass is applied by compose), the background-only pixels
under the mask and at the right edge, the palette table and the centre picture. `compose` reads only the snapshot and the wide
view's own state: `describe()` now works on indexes, every global it read is gone (checked by grep: no `nesp_*`, `XDBuf`, `PALRAM`,
`GameInfo`, `nesw_*` in the compose half). The compose side owns widekit and the learned map (`wk_map`).
Contract: one compose per capture, in order; capture(N + 1) is called after compose(N) returned (compose(N) overlaps
run_frame(N + 1) and scout_step); enable / margin / option / profiles / map save+load+dirty / scout stats / load / unload /
reset / state load only while no compose runs.
Scout under the split: its control messages go compose -> next capture (start: capture takes the state of the live game when
compose(N) asked for it; compose(N + 1) measures the link at that frame; capture(N + 2) hands it to the scout), its state is
mirrored capture -> compose, and what it learns is posted to a lock-free SPSC queue (8 posts of 34 KB) that the compose of
capture K drains for posts made before capture K. Every exchange is at a point where the other side is quiet, so the result
does not depend on timing: the one-call path and the split path give identical frames. The scout therefore starts two frames
after the decision (it started in the same frame before; scout runs differ from the 12 Nov goldens by that schedule, truth
checks unchanged: zero wrong pixels, leads within 1 px).
Proof: `nes_twin --wide-split` runs capture on the main thread and compose + scout step on two real threads (a mutex / condition
variable job each); `wide_golden.sh` with `WG_EXTRA=--wide-split` writes the same 37 hash files as without (first pass, second
pass with the learned map, scout, map reused in another play, the 9 synthetic cartridges): `diff -r` empty. Under
`-fsanitize=thread` (`make B=build_tsan EXTRA="-fsanitize=thread -g" OPT=-O1`): 5 scrolling cartridges x (first pass, second pass,
scout), 700 frames each, zero reports.

Cost after the split, instructions per frame (`wide_cost` modes 0 off, 1 `wide_frame`, 3 capture only, 4 capture + compose + scout;
x86_64 callgrind; armhf `-O2 -mcpu=cortex-a9 -mfpu=neon` under qemu-arm `-one-insn-per-tb`; scroll_vert at frame 300, right held, 84 px):

| | x86_64 before (12 Nov tree) | x86_64 after | armhf after |
|---|---|---|---|
| wide off | 2.72 M | 2.72 M | 2.82 M |
| wide on, one call | 7.56 M | 7.19 M | 6.95 M (12 Nov figure 6.25 M, other build) |
| emulation thread, split (core + recorder + capture) | 7.56 M | 3.50 M (+0.78 M) | 3.44 M (+0.61 M) |
| of it capture alone | - | 0.17 M | - |
| compose thread | - | 3.68 M | 3.51 M |
| scout thread (two frames per step while catching up) | 7.16 M | 7.12 M | 6.53 M |

What changed: the PPU recorder keeps the margin pixels raw (no per-pixel grayscale pass: -0.7 M on the emulation thread) and
`nesp_tiles` advances the tile coordinates incrementally (-0.1 M); the compose half converts raw pixels with a 128 entry table
per line state. Not done (gain too small or too risky for identical output): describe() caching (0.23 M of the compose thread),
a soft edge row cache (base_fill is 1.2 M of the compose thread), per-column learned map memo. CHR tile identity caching
was not needed: the pattern pages are copied (4-16 KB, 0.17 M with everything else).
Vita estimate (444 MHz, IPC 0.7 / 1.0, armhf figures, not measured on hardware): emulation thread with wide on (split)
3.44 M = 11.1 / 7.7 ms (core alone 2.82 M = 9.1 / 6.4 ms); compose thread 3.51 M = 11.3 / 7.9 ms; scout thread 6.53 M = 21.0 / 14.7
ms (only while it catches up; nothing while it is `ahead` frames in front). Compose and scout each need a core of their own.

Look fixes (13 Nov 2026), checked on strips in `scratchpad/nes-wide-shots/w2/`:
* the soft edge samples background only (PPU records the background pixels x = 0..15 and 248..255 without sprites; with the left
  mask on, the left edge is sampled at x = 8..), so a sprite at the screen edge no longer smears into the soft edge;
* the 8 masked columns: true pixels where known (64 column ring with a certified margin; 32 column ring: the ring's part and the
  learned map's cell); everything else continues the margin (the composed pixel at x = -1, provenance SOFT, never real) instead of
  a dark band;
* the two "stray tiles" of scroll_vert_scout_s00400.png are SPRITES the cartridge draws (an enemy that drifts through the left
  edge, an object on the ground): the truth picture has no sprites. `--wide-truth` now classifies every centre pixel that differs
  from the truth: sprites (from the game's OAM), status bar rows, scene-change frames, the masked column, rows the truth does not
  know, unexplained. scroll_vert, scroll_chrram, single_screen: unexplained 0 / 84 / 0 px of 55 M; scroll_mmc3 0; scroll_4way
  113 k px, all in the last rows (232..239) of the vertical level (truth image edge), not investigated further.

Tests (`nes/twin/selftest.sh` section 13; `nes/twin/wide_public.sh`):
* identity: frames, RAM, saved state, audio, battery RAM byte identical with the wide view off / on / on + scout on the
  synthetic cartridges (NROM, MMC1, MMC3 with IRQ, Zapper, and the 5 scrolling ones), and off / on / on + scout over the 289
  public test ROMs that load (`wide_public.sh`; 1 of 290 files does not load in this core); the centre of every wide frame
  equals `nes_host_video` pixel for pixel. `conformance.py --expect`: 75 expected passes, 0 regressions.
* truth (`tests/scroll_roms.py` writes each cartridge and its full true level picture; `nes_twin --wide-truth`): zero wrong
  margin pixels shown as real, zero wrong masked-column fills, zero wrong backdrop / status bar continuations, on the first
  pass, with the learned map of a first pass, with the map used in a different play, and with the scout.
