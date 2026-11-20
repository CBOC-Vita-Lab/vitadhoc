# Provenance of md/core (Genesis Plus GX)

## Source

- **Genesis Plus GX** by Charles MacDonald (original Genesis Plus) and Eke-Eke, <https://github.com/ekeeke/Genesis-Plus-GX>.
- **Pinned commit:** `b9c2316441978d4015ba30e9a95d9b0e6f8ee981` ("libretro: define ALIGN_LONG for 32-bit ARM unix builds (#415)",
  committed 2026-11-09, the newest commit on the default branch when this was vendored; the project has no tags to pin to).
  Why this one: it is the newest state, it contains the 32-bit ARM `ALIGN_LONG` fix that the Vita (Cortex-A9, armhf) build
  needs, and it is the state the research and every measurement in `md/twin` were done on.
- Taken: the `core/` directory only. The repository's `libretro/`, `psp2/`, `gx/`, `sdl/`, `psp/` and other front ends are
  not vendored (the PS Vita front end `psp2/` was read for its compiler flags, see `md/README.md`).
- **Licence:** `LICENSE.txt` here is the project's licence file, verbatim. It is a non-commercial licence: the code may not be
  sold or used in a commercial product or activity, and a modified redistribution must include the complete source of
  everything a binary is built from. This directory, with the changes listed below, is that source. The same text is in
  `licenses/GenesisPlusGX-LICENSE.txt`. The file also carries the notices for Nuked OPN2 (LGPL-2.1, `sound/ym3438.c`) and for the
  parts that are NOT vendored here (tremor, minimp3).
- Because of that licence the Genesis player is a program of its own (`md.bin`), not part of the GPL main program, and it links
  none of the GBAdhoc-derived GPL files (`gpsp/netdrv/netdrv.c`, `fe_gblink.c`, `fe_evt.c`, `fe_util.c`).
- Copyright headers of all upstream files are unchanged.

## What is kept

Mega Drive / Genesis (68000, Z80, YM2612 discrete by default and the optional Nuked YM3438, SN76489, VDP, cartridge hardware:
SRAM, EEPROM 93C/I2C/SPI, flash, SVP, Action Replay / Game Genie emulation code, MegaSD register code), Sega Master System,
Game Gear, SG-1000 / SC-3000 (including the YM2413 FM unit, discrete `ym2413.c` and optional Nuked `opll.c`), the input devices.

## What is dropped (not vendored)

- `core/ntsc` (md_ntsc / sms_ntsc filters; the app has its own picture filters)
- `core/sound/tremor` (Ogg Vorbis), `core/sound/minimp3`, `core/cd_hw/libchdr` (with zstd and lzma): audio tracks and CHD
  images are only used by Sega CD
- `core/debug` (the CPU trace tools)
- The libretro layer (`libretro/`), the libretro cheat code and its `.cht` handling, the front-end option tables.
- Not copied from upstream: `HISTORY.txt`.

## Sega CD

The Mega CD code (`cd_hw/*`, `m68k/s68kcpu.c`) is still in the tree and in `sources.mk`, but it cannot be reached: `loadrom.c`
only looks for a CD image, a Sega CD BIOS or the cartridge add-ons when built with `-DMD_SEGACD`. That define is **OFF** in
`md/core/Makefile` and nothing in `md/frontend/host.c` can load a disc, a BIOS or a CD save RAM. It has not been built or
tested with the define on. Turning it on needs a CD image reader (the `cdd_load` path), BIOS files supplied by the user, and
the CD pieces of the host API; none of that exists. Sega CD is not supported by VitAdhoc 2.0.

## Local edits

Every difference from the pinned commit (the files are otherwise byte for byte upstream; line endings are as upstream):

New files

| File | What and why |
|---|---|
| `osd.h` | Replaces `libretro/osd.h`, which upstream's `shared.h` includes: no libretro, no stream layer; declares `md_rand_seed`, `md_rand_unit`, `crc32`. |
| `rng.c` | Seedable random numbers (splitmix32): the core's only use of `rand()` is the power-on cycle offset after a soft reset (`genesis.c`). A fixed seed makes a reset reproducible. |
| `crc32.c` | Table CRC-32 (zlib compatible) in place of `scrc32`/zlib; used by `loadrom.c` for the ROM checksum and by the host's save-state container. |
| `sources.mk`, `Makefile` | Build of the relocatable object `mdcore.o` (see `md/README.md`). Flags: `LSB_FIRST`, `ALIGN_LONG`, `ALT_RENDERER`, `USE_16BPP_RENDERING` (RGB565, the Vita's format), `USE_DYNAMIC_ALLOC`, `HAVE_YM3438_CORE`, `HAVE_OPLL_CORE`, `MAXROMSIZE=10485760`. |
| `LICENSE.txt`, `PROVENANCE.md` | The licence text; this file. |

Changed files

| File | Change | Why |
|---|---|---|
| `genesis.c` | `rand()` becomes `md_rand_unit()` | seeded randomness |
| `loadrom.c` | `cdd_load` and the cartridge add-on branch only under `MD_SEGACD` | Sega CD stays behind a compile flag, OFF |
| `vdp_render.c` | The two `ntsc` includes and the NTSC branch in `remap_line` removed | NTSC filter not vendored |
| `cart_hw/yx5200.c` | Replaced by an inert stand-in (same functions, do nothing, empty state) | the upstream file decodes MP3 files with minimp3, which is not vendored; the few games with that module run without those tracks |
| `sound/blip_buf.c/.h` | Added `blip_save_raw` / `blip_load_raw`, a fixed little-endian image of one buffer's offset, two integrators and the carried samples (160 bytes) | exact sound after a load (below) |
| `system.c/.h` | Added `audio_context_save/load` (`llp`, `rrp`, the blip image of `snd.blips[0]`) | same |
| `sound/sound.c` | `sound_context_save/load` also hold `fm_cycles_busy`, `fm_last[2]` and the audio image | same |
| `sound/ym2612.c` | `YM2612SaveContext` zeroes the table and connection pointers in the saved image (they are rebuilt on load) | the saved bytes no longer depend on where the program sits in memory, and are the same on every run |
| `state.c` | Saves and loads `m68k.refresh_cycles`; zeroes the two Z80 pointers (`daisy`, `irq_callback`) in the saved image, `daisy` is set to NULL on load | hidden state (below); address independent state bytes |

Not edited although worth knowing: upstream's `blip_load_buffer_state` has a mono/stereo (`BLIP_MONO`) mismatch in code that is
not compiled in this build and that we do not call.

### Exact save-state round trip

A state saved, loaded and replayed has to give the same picture, sound and state hash as the run that never stopped. The hidden
state that upstream's state does not hold, found with the twin's data+bss diff (`MD_TWIN_GLOBDIFF`, `md/twin/tools/globdiff.py`):

1. **`m68k.refresh_cycles`**, the phase of the 68000's DRAM refresh stalls. After a load it kept the value from the reset, the
   68000's timing drifted from the original within a few frames, and so did the Z80's `R` register. Now in the state.
2. **YM2612 BUSY expiry (`fm_cycles_busy`) and the last FM output (`fm_last`)**: the Z80 polls the BUSY flag, so a lost
   value changes Z80 timing. Now in the state.
3. **The output stage**: the resampler's integrators and carried samples (`blip`), and the low-pass memory `llp/rrp`. Upstream clears
   them on load, which makes the first frames of sound after a load differ in the last bits. Now in the state. This is the
   "audio state": what is **not** saved is the 3-band equalizer memory (`eq[]`), which only exists when the equalizer option is
   on; this build never turns it on (equalizer off, as RetroArch's default), so there is nothing to lose. A load while that
   option is on would restart the equalizer's filters: inaudible, and not offered.
4. Derived, rebuilt data (the tile pattern cache `bg_pattern_cache`, `bg_name_list`, the 68000's `jmp_buf`) is not state: it is
   rebuilt on load and does not change the output on the Mega Drive or Master System (verified on 30 + 65 ROMs). One exception
   is open: see "Open" below.

The host's container (`md_host_state_*`): 16 byte header (`MDST`, version, system, length), the core's state zero padded to a
constant size, CRC-32 trailer; a state with a flipped byte is refused and leaves the machine untouched.

### Other decisions

- **`MAXROMSIZE` is 10 MB**, not the 32 MB of the plan: the largest ROM seen (a 9.5 MB demo) fits, and 32 MB was only needed for Sega CD
  images. Larger images are refused with a message.
- **Sound defaults:** YM2612 "discrete" (not Nuked, not the "enhanced" ASIC); output at 48 kHz from the core's own resampler
  (`audio_init(48000, 0)`), so no second resampling happens in the host.
- **Picture:** RGB565 straight from the VDP (`USE_16BPP_RENDERING` with `ALT_RENDERER`, GPGX's own fast line renderer). Upstream's
  Vita build uses 15 bit; 16 bit gives the same cost and the Vita's own format.
- **Save RAM, EEPROM, flash** share one 64 KB buffer as upstream; `.srm` is that buffer up to the last byte that is not 0xFF, which is what
  RetroArch's core writes.

### Open

- A Master System image run with the console forced to Game Gear (a smoke test, no real Game Gear ROM was available) shows the
  picture differing from the uninterrupted run after a state load, starting a few frames in, while the state hash is the same. The
  difference is in the tile cache (`bg_pattern_cache`, `bg_name_dirty`) which upstream only refreshes for tiles that are drawn,
  and the Game Gear window does not draw them all. It has not been run down because no genuine Game Gear ROM exists in the test
  set. The Master System, SG-1000 mode and Mega Drive round trips are exact.
