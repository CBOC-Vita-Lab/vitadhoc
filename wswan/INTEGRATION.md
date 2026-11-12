# Using the WonderSwan core inside the main app (in-process)

Same route as the NES core: Beetle WonderSwan (`wswan/core`, GPL-2.0-or-later) plus `wswan/frontend/host.cpp` (MIT, no C++
runtime) built as ONE relocatable object, `wswancore.o`, whose only global symbols are the `wswan_host_*` functions of
`wswan/frontend/host.h`. The core's generic names (`wsc`, `rom_size`, `Settings`, `RAM`, ...) are localised inside it.
Three more small portable files go on the link line by themselves: `blend.c`, `layout.c`, `safe_io.cpp` (all built for Vita
and clean under `-Wall`).

## Build lines

```
make -C wswan/frontend                       # host:  wswan/frontend/build/host/wswancore.o
make -C wswan/frontend TARGET=vita           # VitaSDK (/opt/vitasdk): build/vita/wswancore.o   (also: make vita)
make -C wswan/frontend armhf                 # arm-linux-gnueabihf
make -C wswan/frontend check TARGET=vita     # fails on any global that is not wswan_host_*
```
Core flags: `-DMEDNAFEN_VERSION_NUMERIC=0x00093501 -std=gnu99 -w -fno-strict-aliasing`, Vita `-marm -mcpu=cortex-a9 -mfpu=neon
-mfloat-abi=hard -O3`; `rtc.c` additionally `-include wswan/frontend/time_shim.h`. `host.cpp`: `-std=gnu++11 -fno-exceptions
-fno-rtti -fno-threadsafe-statics`. Include path for the app: `-Iwswan/frontend`. Sizes (Vita, `make check`): text 137 KB,
data 56 B, bss 1.34 MB; the object needs only libc/libgcc (`localtime`, `time`, `_ctype_`, `__aeabi_*`, malloc/free, memcpy...).

A `vita/wswancore.mk` shaped like `vita/nescore.mk` (not edited here):

```make
WS_DIR := $(GP)/../wswan
include $(WS_DIR)/core/sources.mk          # CORE_SRCS (relative to wswan/core)
WS_CFLAGS := $(CORE_ARCH_FLAGS) -g -std=gnu99 -w -fno-strict-aliasing -DMEDNAFEN_VERSION_NUMERIC=0x00093501 \
   -I$(WS_DIR)/core -I$(WS_DIR)/core/mednafen -I$(WS_DIR)/core/mednafen/include \
   -I$(WS_DIR)/core/libretro-common/include -I$(WS_DIR)/frontend -MMD -MP
$(B)/wswan/core/%.o: $(WS_DIR)/core/%.c ; $(CC) $(WS_CFLAGS) -O3 -c -o $@ $<
$(B)/wswan/core/mednafen/wswan/rtc.o: $(WS_DIR)/core/mednafen/wswan/rtc.c
	$(CC) $(WS_CFLAGS) -O3 -include $(WS_DIR)/frontend/time_shim.h -c -o $@ $<
$(B)/wswan/host.o: $(WS_DIR)/frontend/host.cpp ; $(CXX) $(CORE_ARCH_FLAGS) -O3 -std=gnu++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics -I... -c -o $@ $<
$(B)/wswan/wswancore.o: <all of the above>
	$(LD_R) -r -o $@.tmp $^ && $(OBJCOPY) -w --keep-global-symbol='wswan_host_*' $@.tmp $@
# plus wswan/frontend/blend.c layout.c safe_io.cpp as ordinary objects (-O2), link WS_O + those
# licences: wswan/core/LICENSE = licenses/Beetle-WonderSwan-GPL-2.0.txt, licenses/LGPL-2.1.txt already added; notices/WSWAN-Beetle.md
```
(`wswan/frontend/Makefile` is the working reference for every flag.)

## The API contract

`host.h` is the reference (each function has its rules in the comments). Summary: init, load_mem (`.ws .wsc .pc2` buffer,
64 KiB..16 MiB, copied), run_frame, set_pad (WSWAN_* bits), video (224x144 RGB565), audio_take (stereo int16, 48000 Hz, about
636 frames per emulated frame), reset, set_render (skip drawing; emulation, sound and state identical), state_size /
save_mem / load_mem (size fixed per game, zero padded: equal ring slots), save regions + `srm_*`, RTC clock hook, `ra_read` /
`peek`, cheats, rom_info. One instance, one thread, no allocation per frame. Before the first frame the app should call
`wswan_host_set_time_source` (a function returning local wall clock seconds: the Vita libc has no time zone), and
`wswan_host_set_owner` (name shown by games that read the console's owner data).

Console type: at load the core is set to mono or Colour from the cartridge's colour byte (footer byte 7), so port 0xA0 reports
mono hardware for mono cartridges (0x85) and Colour hardware for Colour cartridges (0x87). Earlier builds always reported Colour;
the change is in `wswan_host_load_mem` and in `core/mednafen/wswan/gfx.c` (see `core/PROVENANCE.md`).

## Frame pacing: 75.4716981 Hz on a 59.94 Hz screen

The console runs 159 lines x 256 clocks at 3.072 MHz: 75.4716981 frames/s (`wswan_host_frame_rate()`;
`wswan_host_frame_rate_now()` follows a game that changes the line count, port 0x16). The Vita refreshes at 59.94 Hz
(60000/1001), so each refresh covers **1.259119** emulated frames (3072000/40704 divided by 60000/1001).

Recommended loop (the audio device is the clock, the NES player's pacer pattern: pull audio, never drop it):

```c
wsb b; wsb_init(&b, 224, 144, 3072000, 40704, 60000, 1001);
each display refresh (vblank):
    int n = wsb_frames_needed(&b);            /* 1 or 2 (1.259 average); the sequence is exact and periodic */
    while (n--) { wswan_host_set_render(n == 0 || !blend);  /* optional: skip drawing of frames the blend does not need */
                  wswan_host_set_pad(pad); wswan_host_run_frame(); push audio; wsb_submit(&b, wswan_host_video(...)); }
    wsb_present(&b, out_rgb565);              /* the temporal blend: exact 8 bit weights, sum 256; present out_rgb565 */
```
Without the blend: run `wsb_frames_needed()` frames and present the LATEST one (every 4th refresh shows 2 steps of motion;
judder at the 59.94/75.47 beat of about 15 Hz). The blend is **default on**: weights come from exact integer tick overlap
(`te = 2,442,240,000`, `td = 3,075,072,000`), so a refresh blends 2 frames (445,283 of 600,000) or 3 (154,717), never 1, the
average over 600,000 refreshes is 1.259120, identical frames blend bit exactly (no flicker on still pictures), and moving
objects advance by the covered emulated time (checked to 0.018 px in `tests/blend_test.c`, 11 checks; the NEON kernel is
checked against the scalar one under qemu-arm: equal on 200 random weight sets). Optional `b.ghost` (0..255) adds LCD persistence.
After a pause or a state load call `wsb_reset`. For a frame that runs twice in one vblank on a heavy scene the cost is two
frames: see the cost section. If the audio queue is the pacer instead, the same `wsb_*` calls work with the audio-driven number of frames.

## Orientation and presentation

Games are held horizontally (224x144, picture as drawn) or upright (vertical, the 224x144 image is meant to be turned 90
degrees counter-clockwise, and the second cross, Y1-Y4, is the d-pad). The hardware never says which; some games switch scene by scene
(Klonoa: Moonlight Museum has horizontal and vertical stages in the same cartridge).

**Detection (`wswan_host_orientation()`, evaluated every frame, effective answer with the override applied):**
1. At load: the footer flag (byte 12 bit 0).
2. Key matrix polling: the game selects groups with port 0xB5 (bit 4 = Y, bit 5 = X, bit 6 = buttons). A frame in which only the Y group
   was scanned means vertical, only X means horizontal. 6 consecutive frames of the same evidence switch (frames with no evidence do
   not reset the count).
3. LCD icons (port 0x15, bit 1 = vertical icon, bit 2 = horizontal icon): exactly one set decides when the game scans both groups
   (as Klonoa does; it shows the vertical icon in vertical stages). 2 consecutive frames switch.
4. A per game table (`wswan_host_orientation_table_set(developer id, cart id, mode)`, keyed on footer bytes a translation
   patch does not change, not the checksum; empty by default): a fixed orientation. Not suited to games like Klonoa.
5. Manual override `wswan_host_set_orientation_mode(AUTO / HORIZONTAL / VERTICAL)`: the app should bind it to SELECT (cycle).
6. After a state load the icon is applied at once.
`wswan_host_orientation_info()` reports the evidence (for an on-screen debug line).
Limits: a game that scans both groups and never shows an icon gets only the footer flag at boot plus the manual toggle.

**Presentation** (`layout.h`; `wsl_layout(vertical, presentation, 960, 544, integer, &rect)`):

| scene | mode | scale | size on 960x544 |
|---|---|---|---|
| horizontal | integer | 3x | 672x432 |
| horizontal | best fit (default) | 3.78x | 846x544 |
| vertical, upright (default) | integer | 2x | 288x448 |
| vertical, upright (default) | best fit | 2.43x | 350x544 |
| vertical, turned (player turns the Vita) | | 3.78x | 846x544, picture NOT rotated |

Upright rotates the 224x144 frame 90 degrees CCW (`wsl_rotate_ccw`: the frame's right edge becomes the top, as beetle and higan do).
The default for a vertical scene is **upright** (the Vita stays in the hands); "turned" is an option for players who hold the Vita
like a WonderSwan (turn it counter-clockwise: the same direction as the picture rotation) and fills 846x544.

**Pad mapping** (`wsl_map_pad(vita_mask, vertical, presentation)`; SELECT is for the app):

| Vita | horizontal | vertical upright (default) | vertical turned |
|---|---|---|---|
| d-pad up / right / down / left | X1 / X2 / X3 / X4 | Y2 / Y3 / Y4 / Y1 | Y1 / Y2 / Y3 / Y4 |
| triangle / circle / cross / square | A / A / B / B | X2 / X3 / X4 / X1 | X1 / X2 / X3 / X4 |
| right stick up / right / down / left | Y1 / Y2 / Y3 / Y4 | (not used) | (not used) |
| L / R | (not used) | A / B | A / B |
| Start | Start | Start | Start |
(Horizontal: A = circle and triangle, B = cross and square, the way the face buttons sit.) The upright vertical mapping turns the
d-pad with the picture: up on screen is the unit's right side. Verified by scripted input (hash-equal to the raw key script) and by
watching Klonoa walk, jump and shoot in both orientations.

## Saving

`wswan_host_srm_*`: the single combined file `<game>.srm`: the cartridge SRAM or cartridge EEPROM bytes exactly as RetroArch
beetle-wswan writes them (so a RetroArch `.srm` loads, and ours loads in RetroArch), then `"WSiE"`, u32 LE 1024, and the
1024 bytes of the internal console EEPROM (owner data, some games keep settings there; RetroArch ignores the tail). Use
`safe_io.h` (`wswan_write_safe`: write to `.tmp`, keep the old as `.bak`, recover `.old`) the way the NES frontend does: save
when `wswan_host_srm_dirty()` goes 1 and at close, call `wswan_host_srm_mark_unsaved()` if the write fails. Per-region
queries (`save_size/data/dirty`) exist for a status display. Verified (selftest, synthetic SRAM and EEPROM carts, forced write
failure keeps the original) and with Klonoa (the title shows "Continue" after a run that saved).

## File extensions, console id, RetroAchievements, thumbnails

- Extensions: `.ws` (WonderSwan), `.wsc` (Color), `.pc2` (Pocket Challenge V2: a WonderSwan cartridge image). The core reads the
  footer, not the extension; the app should treat `.ws .wsc .pc2` as `FE_CONSOLE_WSWAN` (zip: inner file).
- Thumbnail repositories (libretro-thumbnails): "Bandai - WonderSwan" for `.ws`/`.pc2`, "Bandai - WonderSwan Color" for `.wsc`
  (or a `.ws` whose footer byte 7 is non-zero, `wswan_host_game_is_color()`).
- ROM folders (proposal, not found in the repo yet): `ux0:ROMS/WS`, `ux0:ROMS/WSC`, `ux0:roms/WS`, `ux0:roms/WSC`, RetroFlow
  `ux0:data/RetroFlow/ROMS/Bandai - WonderSwan` and `.../Bandai - WonderSwan Color` (RetroFlow uses the libretro system names).
- RetroAchievements: console id **53** (WonderSwan, Color shares it). `wswan_host_ra_read(addr, out, n)` / `peek`: 0x00000-0x0FFFF
  the 64 KiB system RAM, 0x10000-0x8FFFF the cartridge save RAM (SRAM, else EEPROM, contiguous). The hash is believed to be
  the MD5 of the whole file (rcheevos has no WonderSwan special case in the checked tree; NOT verified against the RA server).
  `wswan_host_rom_crc()` is the CRC-32 of the whole file, usable for box-art matching.

## Memory footprint and cost

- `wswancore.o` Vita: text 137 KB, bss 1.34 MB (fixed core tables); heap: the ROM padded to a power of two (up to 16 MiB; Klonoa
  1 MiB), cartridge SRAM, one second of stereo audio (192 KB), a state scratch buffer. State: `wswan_host_state_size()` = 79,637 bytes for Klonoa (1 MiB cartridge).
- Measured per frame on the x86 build machine (thread CPU time, shared machine, rough): synthetic ROMs 0.22-0.33 ms; Klonoa title and
  stages 0.06-0.16 ms average, vertical stage 0.31-0.37 ms (half of it the picture drawing: 0.15 ms with render off), worst single frame 1.2 ms.
- Instructions (valgrind lackey on the host build, Klonoa vertical stage): about 3.4 million x86 instructions per frame.
  Under qemu-arm (armhf build with NEON, static results equal to the host: identical frame hash) the same scene takes
  2.8 ms per frame emulated, 1.5 ms with render off; qemu is an emulator, so that is only a relative number (7.5x the host).
  The qemu instruction-trace count (`-one-insn-per-tb -d exec`) gave about 43,000 per frame for the same scene, which does
  not agree with valgrind and is not used.
- **Vita estimate** at 75.47 frames/s, budget **13.25 ms** per emulated frame: from the host time scaled 10-20x (Cortex-A9
  against a desktop core, the same ratio as the NES doc): 3-6 ms typical, up to 12 ms on the heaviest vertical scenes
  measured, with picture drawing about half of it; from instructions (3.4 M at about 0.8 IPC at 444 MHz): about 9.6 ms. So
  one frame fits the budget; **two frames in one vblank (the 1.259 schedule runs 2 about a quarter of the time) is 2x**:
  6-12 ms typical, up to 19-24 ms on a heavy scene, which would drop a refresh occasionally. Mitigation built in: `set_render(0)` for
  the first frame of a pair (the blend uses both pictures, so only use it when the blend is off or skip blend on slow frames).
  This is an estimate, not a measurement: no Vita run exists.

## What is not verified

- Everything on a real Vita (speed, audio pacing, memory, the Vita pad, the physical hold direction of "turned" mode, which follows
  beetle/higan CCW and is consistent with Klonoa).
- Commercial games other than Klonoa: Moonlight Museum, fan English translation, mono, 1 MiB. Of its stages only Visions 1-1 (horizontal)
  and 1-3 (vertical, reached with a RAM poke because the hub locks later visions) were played; the game's other vertical stages were not
  enumerated. Colour games and WonderSwan Color: only a synthetic ROM and the WSHWTest ROM. RTC games, EEPROM cartridges, WonderWitch
  (flash) carts, Pocket Challenge V2 key mapping and behaviour: untested. The per game orientation table has no entries.
- Games that scan both key groups and never use the LCD icons: only footer flag plus manual toggle.
- The mono value of port 0xA0 (0x85) is from the documented bit layout, not read from a mono console.
- RetroAchievements hash and addresses: from the documented memory map, not run against the server.
- The public test ROMs only show results on screen; `twin/baseline_results.md` records what was seen. ws-test-suite (asiekierka)
  needs the Wonderful toolchain and was not built.
