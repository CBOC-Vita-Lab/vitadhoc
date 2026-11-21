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
- `core/sound/minimp3` (MP3 audio for the one cartridge module that plays it)
- `core/debug` (the CPU trace tools)
- The libretro layer (`libretro/`), the libretro cheat code and its `.cht` handling, the front-end option tables.
- Not copied from upstream: `HISTORY.txt`.

## Sega CD

The Mega CD code (`cd_hw/*`, `m68k/s68kcpu.c`, the CD parts of `genesis.c`, `mem68k.c`, `loadrom.c`, `state.c`) is upstream's and is on: `md/core/Makefile`
builds with `-DMD_SEGACD -DUSE_LIBCHDR -DUSE_LIBTREMOR`. The core reads the disc itself and streams it (`cdd.c`: a few KB cached, never the whole
image); `md/frontend/host.c` supplies what upstream's libretro layer did: the BIOS images (handed over by the player from the player's own files,
never shipped: `md_host_cd_bios_add`), the disc by path (`md_host_load_disc`), the backup RAM and the optional RAM cartridge as one save, and the
state header's system field (a disc state is never loaded into a cartridge). What the tests can and cannot show is in `md/README.md` ("Sega CD").

Vendored for it (all from the pinned commit, nothing downloaded since: tremor from its `core/sound/tremor`, libchdr and the libraries it needs from its `libretro/deps/` (`libchdr`, `lzma-19.00`, `zlib-1.2.11`, `zstd/lib`), which is the set upstream's libretro build compiles `cdd.c` against, not the older copy in `core/cd_hw/libchdr`; all permissive licences, texts in `licenses/SegaCD-disc-readers.txt`):

| Path | What | Licence |
|---|---|---|
| `cd_hw/libchdr/src`, `include/libchdr` | libchdr, the CHD disc image reader | BSD-3-Clause |
| `cd_hw/libchdr/deps/lzma` | LZMA SDK (CHD codec; `Lzma86*` files dropped) | public domain |
| `cd_hw/libchdr/deps/zlib` | zlib 1.2.11 inflate (`crc32.c`/`crc32.h` dropped: the core's `crc32.c` is zlib compatible) | zlib licence |
| `cd_hw/libchdr/deps/zstd` | Zstandard decoder (common and decompress only) | BSD-3-Clause (the project is dual BSD / GPL-2.0; BSD is used) |
| `cd_hw/libchdr/include/dr_libs/dr_flac.h` | dr_flac (CHD FLAC codec) | public domain or MIT-0 |
| `sound/tremor` | Tremor, integer Ogg Vorbis decoder (CD audio as `.ogg`) | BSD-3-Clause |

Edits to the vendored files: `static INLINE` became `static inline` in libchdr (the core's `INLINE` already contains `static`) and the
`<retro_inline.h>` includes were removed; zlib's `inffixed.h` is not in upstream's folder, so the build defines `BUILDFIXED` (zlib builds the
fixed tables at run time). The CHD, Ogg and WAV readers are exercised on the Linux twins by synthetic discs (`md/twin/tools/mkscd.py`).

The BIOS (a Sega CD / Mega CD boot ROM, 128 KB) is the player's own file: `us_scd1_9210.bin`, `eu_mcd1_9210.bin`, `jp_mcd1_9112.bin` or another dump,
found in `ux0:data/GBAdhoc/bios` or beside the game and recognised by its content (`host.c`: a structure check and a table of known dump CRCs),
never by name alone. No BIOS, fake or real, is in the package or the source. The test suite uses a stand-in written for it
(`mkscd.py standin`, a few hundred bytes of 68000 code, marked "NOT A BIOS", refused as a BIOS unless a test forces it).

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
| `loadrom.c` | `md_cd_wanted` (new, under `MD_SEGACD`): `cdd_load` runs only when the host asked for a disc | a cartridge named `rom.md` must never be opened as a disc |
| `osd.h` | `cdStream*` macros over the host's `md_cd_fopen` / `md_cd_fread` / `md_cd_fseek` / `md_cd_ftell` / `md_cd_fgets` / `md_cd_fclose` (stdio underneath) | the core's disc reads go through the host: a 32 KB buffered stream, a seek that keeps the buffer when the drive reads on (the Vita's C library drops it on every `fseek`), and a NULL stream (a track without a file) reads nothing instead of crashing |
| `cd_hw/cdd.h` | includes `libchdr/chd.h`, `libchdr/cdrom.h` | the vendored libchdr's layout |
| `cd_hw/cdd.c` | `cdd_load` clears the table of contents first | a refused disc must not leave its TOC behind (the player may try another) |
| `cd_hw/cdd.c` | the cue sheet reader skips a UTF-8 byte order mark and TAB indentation | such sheets had no recognised `FILE` / `TRACK` / `INDEX` line: the disc did not load, or a track was left without its file |
| `cd_hw/gfx.c` | `gfx_reset` starts `tracePtr` / `mapPtr` inside Word-RAM | the state held the address of the RAM until the first graphics operation: two runs gave different states |
| `cd_hw/libchdr/deps/lzma/src/Alloc.c` | `SzAlignedAlloc` uses the file's own aligned allocator on the Vita | the Vita C library has no `posix_memalign` |
| `state.c` | the version register keeps bit 5 clear for a Mega CD after a load (as `io_reset` leaves it) | a loaded state told the game there was no CD unit |
| `system.c` | `audio_context_save/load` also hold the CD-DA and PCM blip buffers; `system_frame_scd` sets `vdp_status_only` as `system_frame_gen` | exact sound after a load; the VDP worker's status reads on a disc |
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

### VDP worker thread (performance edit, opt-in at run time)

The line renderer (`vdp_render.c`) can run on its own thread. Files added: `thread_ops.h` (what the platform hands over: semaphores,
thread start; the core has no thread code of its own), `vdp_thread.h/.c` (the write log: a single producer, single consumer ring of
32 bit words, the worker, the joins), `vdp_render_w.c` + `vdp_render_w_names.h` (the same `vdp_render.c`, compiled a second time with
its functions and the 22 shared names renamed `vw_*`, so the worker owns a private copy of everything the renderer touches).
Edits to upstream files:

- `vdp_render.c`: `VDPW_NO_WRAP` before the includes; one global `tms_status_set` (the TMS9918 status bits the sprite parser sets,
  merged by the emulating thread); `vdp_fn_encode/decode` (the renderer's function tables as numbers for the log).
- `shared.h`: includes `vdp_thread.h`, whose macros turn `render_line`, `remap_line`, `blank_line`, `parse_satb`, `window_clip`,
  `color_update_m4/m5`, `render_init/reset` and `spr_col` into queue calls while the worker runs (and the plain calls otherwise).
- `vdp_ctrl.c`: `MARK_BG_DIRTY` also sends the pattern to the worker; bulk VRAM rewrites (reset, state load, SG 4K/16K decode, 16K/64K
  switch) mark the whole VRAM changed; the two status register reads join the worker first (the sprite collision / overflow bits come
  from the renderer); the places that clear those bits tell the worker.
- Joins: before a status register read (only when a line was drawn since), at the end of every frame, at reset / state save / load /
  unload and before an option change. With the worker off nothing changes: the proof is the same hashes (picture, sound, state, RAM) on
  all 96 test ROMs with the worker on and off, on x86 and armhf (qemu), also with a join after every drawn line and with a per frame
  comparison of the raw VDP memory (`md_twin --vdp-thread | --vdp-check | --vdp-stress`).

### Wide view (2.0): strips beside the picture, drawn by the same renderer

The player can ask the Mega Drive renderer for a narrow strip of picture beyond each side of every line (`md/INTEGRATION.md`, "Wide view";
`md/frontend/wide.cpp` decides what of it is shown). Nothing the CPUs, the sound or the picture inside the screen can see changes: the proof is
`md/twin/selftest.sh` section 11 (picture in the window, sound, state and RAM identical with the strips on and off, with and without the VDP
worker, on every test ROM) and `arm_check.sh`. Edits to upstream files:

- `vdp_render.c`:
  - the strip pass (`wide_line()`, called at the end of `render_line`): `render_bg` and `render_obj` run again per side on a few columns with the
    scroll shifted (`wide_pass`, `WIDE_COLS`, `WIDE_VW`, `WIDE_SPR_DX`, `wide_xs()` in the three mode 5 plane renderers and the two sprite
    renderers); the status bits, the sprite masking state and the part of the line buffers the pass writes are saved and put back;
  - a plane can be left out of the strips (`wide_planes`), and plane A is on the side of a line the window plane takes (`wide_a`; the window
    plane is never continued, and that side gets a strip only where the window's two cells at the edge are empty);
  - lines with per 2-cell vertical scroll: a side gets its strip only where the three columns next to that edge scroll alike (on the left the
    partly shown column too); the strip scrolls like the column at the edge (`wide_vsb`, `wide_ys` in `render_bg_m5_vs`);
  - learned rows (`wide_row_set`, `wide_rows_clear`): name table rows the strip pass of the plain renderer reads instead of the VRAM row while the
    VRAM row is byte for byte what it was when the row was given (`wide_ovr`, `wide_ovo`, `wide_ont`);
  - the strip cache of the worker's copy (`wide_cache`, `wide_gen`, `wide_parse_gen`): a line's strips are copied from the last frame while no
    record that changes what they are drawn from has arrived; `--wide-cache-check` draws anyway and compares;
  - `render_obj_m5_ste` clears and merges its sprite buffer over the strip's width inside the pass (the screen's width outside it, as before);
  - per line records for the host (`wide_rec`, `wide_line_t`, flags `WL_*` / `WS_*`), the opaque masks (`wide_opq`), and for the 8 bit consoles
    the palette line 0 was drawn with (`wide_pal`) and the mode 4 scroll / name table mask of every line.
- `vdp_render.h`: the declarations of the above. `vdp_render_w_names.h`: their `vw_` names.
- `vdp_ctrl.c`: `MARK_BG_DIRTY` also notes the written 32 byte VRAM block in a list the host takes after the frame (`wide_vd`, `wide_vl`,
  `wide_vn`; `wide_vall` at bulk rewrites): the analysis rereads only what was written. Not read by anything the machine can see.
- `vdp_thread.c/.h`: the wide fields of the state record; `OP_CALL` / `vdpw_q_call` (run a function on the worker after everything queued, its
  copy of the VDP brought level with the emulating thread's first: the wide view's analysis); the generation counter of the strip cache is
  bumped by every record that changes what a strip is drawn from; accessors for the renderer's copy of the VDP memory.

### Frames that are not shown (2.0)

- `system.c`, `vdp_thread.h/.c` (`vdp_status_only`, `VDP_LINE_STATUS_ONLY`), `vdp_render.c` (`render_line`): a frame the host skips
  still runs the sprite renderer and the sprite table parse of every line (no background, no pixels out), because the collision and
  overflow bits of the status register come from there. Upstream drew nothing, so a skipped frame was a different machine. A lens frame
  the core itself skips (3-D glasses) is unchanged. Proof: `md/twin/ahead_check.sh` part 1.
- `vdp_render.c` (`parse_satb_tms`, `parse_satb_m4`, `render_obj_tms`, `render_obj_m4`), `osd.h`: `config.no_sprite_limit` draws the
  sprites beyond the hardware's 4 / 8 per line without changing the status register: the overflow flag and the fifth sprite number are
  set at the hardware's limit, and what the extra sprites do to the collision bit and position is undone. Upstream lifted the limit
  for the flags too. Mode 5 (Mega Drive) ignores the option. Proof: `ahead_check.sh` part 3.
- Run-ahead and exact state loads (`md_host_run_frame_ahead`, and every state the player saves or loads): `render_carry_*`
  (`vdp_render.c`), `vdp_carry_*` (`vdp_ctrl.c`), `psg_carry_*` (`sound/psg.c`), `vdpw_carry_*` (`vdp_thread.c`) and `machine_carry`
  (`system.c`: the pause latch, `eeprom_i2c_carry`, `eeprom_spi_carry`, `eeprom_93c_carry`, `cart_flash_carry` / `flash_cfi_carry`) save and
  put back what the core's own state does not hold: the first line's sprite list and pending sprite flags, the FIFO byte flag and cycles,
  HV latch, latched scroll, table bases, border, viewport, the PSG's queued level changes, the serial EEPROMs' protocol position, and, on a
  cartridge with a flash chip, the save window (`sram.sram`) and the ROM banks the chip has rewritten (the first change of each 64KB bank
  keeps its original; four at most). The host appends them to the state (`md/frontend/host.c`, state version 2).
- `md_cart.c` (`md_cart_context_load`): on a flash cartridge a state load moves only which 64KB bank sits in the save window; the flash
  chip's own handlers stay (upstream put the plain SRAM handlers there, which broke the next flash access after a load).
- `vdp_ctrl.c` (`vdp_context_load`): a fast load (`fast_savestates`, the run-ahead way back) puts back the tile cache's dirty flags and
  list as they were with the state, and refreshes only the blocks of VRAM that really differ (`vdp_set_all_vram` marks them per block for
  the worker and the wide view; it no longer sets "all VRAM changed"); a normal load invalidates the whole cache as before. Upstream
  refreshed only tiles that are drawn, which left the Game Gear's undrawn tiles stale. `vdp_z80_data_w_sg` tells the worker and the wide
  list when it writes VRAM through the SG-1000 path.
- Start-up state that upstream left to chance and that made the first game in a process differ from the next: `gen_init` clears the 68000
  context, `z80_init` the effective address and last fetch, `sound_init` the FM buffer, `vdp_init` the FIFO and its cycles, `render_init`
  the object lists and flags; the host clears the rest of the ROM buffer after the game (a flash cartridge rewrites it; a longer ROM
  loaded before left its tail), and `md_host_sram_write` fills the rest of the battery RAM with 0xFF.

- State loads that are not trusted (`state.c`, `vdp_ctrl.c`, `sound/psg.c`): the sanity limits on a loaded state are the machine's own value
  ranges, so a state the machine wrote loads unchanged (a count, `carry_refused`, says when the extra block was refused; the proofs require 0):
  the Z80 interrupt line keeps the VDP's levels (0, 4, 6) and is refused only above 6; the PSG's odd registers are table volumes (up to
  `chanVolume[0]`) and register 6 is the whole noise byte; a Game Gear colour word keeps every bit (every colour read masks it; the other
  systems 9 bits); the viewport may be x -48..16 and y -24..48 (Game Gear window); the table bases are checked against the VDP's own maxima.
  Proof points of the load tests: frame 1, 97, 300, 655.
- State fuzzing (H1, 20 Nov 2026; `tools/statefuzz`, `md/twin/statefuzz.cpp`): the loaders were fuzzed under ASan + UBSan with damaged states
  (checksums repaired) on the SGDK samples and the Master System / Game Gear / SG-1000 test games, and each limit below was kept only if the
  load proof (frames 1, 97, 300, 655, then 60 frames, battery RAM carried beside the state) stays equal on 96 test games. Marked `CBOC-Vita`:
  `system.c` / `frontend/host.c`: `audio_max_frames`, the most sample frames `audio_update` may write (the size of the host's buffer);
  `sound/blip_buf.c`: a clock offset beyond the buffer is dropped; `sound/sound.c`: `fm_cycles_start` (how far the FM chip ran past the end of the
  frame) is 0..2 sample steps, else 0 (it set how many samples the next `fm_update` writes into `fm_buffer`); `sound/ym2413.c`:
  `YM2413ContextKeep` / `YM2413ContextCheck` around the context load put back what the chip's own setup computed (clock, rate, `fn_tab`,
  the increments and the timer overflow) and hold the table indexes and counters (`ar`, `dr`, `rr`, `ksr`, the envelope shifts and selects,
  `wavetable`, the wave select, `lfo_am_cnt`, `eg_timer`) to what the tables allow; they are not recomputed, because a freshly reset chip
  holds zeros there that the formulas would not give; `sound/ym2612.c`: the timer registers; `vdp_ctrl.c`: a pending VDP DMA is dropped on
  hardware without a Mega Drive VDP (it read the 68k bus of a machine that has none); `md_cart.c`: a bank offset; `state.c`: the Z80 bank and
  state words. SVP is not in a state (the core's `state.c` holds no SVP block), so there is no SVP load path to harden.

### Open

- None known in this part. (The Game Gear tile cache difference after a state load, recorded here before, is fixed: see "Run-ahead and exact state loads".)
