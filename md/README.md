# md: Sega Mega Drive / Genesis, Master System, Game Gear, SG-1000

Core and host glue of the Sega player (`md.bin`, a separate program with its own non-commercial licence). Nothing here is built
into `eboot.bin`.

| Directory | What | Licence |
|---|---|---|
| `core/` | Genesis Plus GX at a pinned commit, a few local edits (`core/PROVENANCE.md`), build of one relocatable object | Genesis Plus GX licence (`core/LICENSE.txt`): non-commercial, source must accompany binaries |
| `frontend/host.{h,c}` | the `md_host_*` API: load, run frame, pads, picture and its geometry, 48 kHz sound, saves, states, achievements memory, cheats, options | MIT, Copyright CBOC-Vita (linked only into `md.bin`) |
| `twin/` | headless runner (x86 and armhf under qemu), self test, ARM check, baseline numbers | MIT |

Integration: `md/INTEGRATION.md`. What was measured, what passes and what does not: `md/twin/baseline_results.md`.

## Build

```
make -C md/core            # host object build/host/mdcore.o (core + host.c as ONE relocatable object)
make -C md/core vita       # arm-vita-eabi (VitaSDK)
make -C md/core armhf      # arm-linux-gnueabihf (for qemu)
make -C md/core check      # fails if the object exports anything but md_host_*
make -C md/twin            # build/x86/md_twin     (ARCH=armhf for the qemu runner)
MD_TEST_ROMS=<dir> MD_TEST_SMS_ROMS=<dir> sh md/twin/selftest.sh
MD_TEST_ROMS=<dir> MD_TEST_SMS_ROMS=<dir> sh md/twin/arm_check.sh
```

`MD_TEST_ROMS` is a clone of the SGDK repository after building its samples (`sample/**/out/release/rom.bin`); `MD_TEST_SMS_ROMS`
is any tree of public Master System / Game Gear / SG-1000 homebrew. No ROM, BIOS or save file is kept in this repository and the tests
SKIP without them. In the gate these are `GATE_MD_ROMS` and `GATE_MD_SMS_ROMS` (steps `build-md-twin`, `md-selftest`, `md-arm-check`).

## Core flags (and where they come from)

Genesis Plus GX's own PS Vita build (`psp2/Makefile`, read, not vendored) uses `-O3 -fomit-frame-pointer`, `LSB_FIRST`, `ALT_RENDERER`,
`ALIGN_LONG` and 15 bit pixels. This build uses the same except 16 bit pixels (RGB565, the Vita's native format, same cost):
`-DLSB_FIRST -DALIGN_LONG -DALT_RENDERER -DUSE_16BPP_RENDERING -DUSE_DYNAMIC_ALLOC -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE`,
`-marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard` on ARM. `ALT_RENDERER` is upstream's fast line renderer, the one its Vita build uses.

## Behaviour set by this port

- FM: the discrete YM2612 by default (`fm` option: ASIC, enhanced or Nuked on request, never the default). Sound is made at 48 kHz by the core.
- Region from the header (field at 0x1F0) with Auto / USA / Europe / Japan NTSC / Japan PAL; true 50 Hz (49.7015 Hz) for PAL.
- Exact save-state round trip (picture, sound and machine) on the Mega Drive and Master System; see `core/PROVENANCE.md`.
- Sega CD, 32X and Pico are not supported. The Sega CD code is compiled out of the load path behind `MD_SEGACD` (OFF).
- Largest ROM: 10 MB (the plan said 32 MB; only Sega CD images needed that).

## Threads (performance)

- VDP worker (`core/vdp_thread.c`, `threads.ini` `vdp_thread=0|1|2`: off, Mega Drive only (default), all systems): the line renderer runs on
  its own thread behind a write log. The emulating thread joins before a VDP status read, at the end of every frame, and at reset, state
  save / load, unload and option changes. Output is bit-identical with the worker on and off: picture, sound, state and RAM hashes on all
  96 test ROMs (30 SGDK samples + 66 8 bit ROMs), x86 and armhf under qemu, also with a join after every drawn line (`md_twin
  --vdp-stress`), with a per frame comparison of the raw VDP memory (`--vdp-check`), and the state round trip with the worker on.
  ThreadSanitizer (x86, 16 ROMs, check mode + state round trip): no report. Census: `core_split vdp_us= status_joins= join_wait_us=`
  once a second in the player log.
- Games that read the VDP status register on every line make the emulating thread wait for the worker on every line (a Mega Drive
  hello-world sample does this about 200 times a frame); that is correct but gives no overlap. Not measured on a Vita.
- The FM synthesis worker is not built (see "Not done / open").
- Fourth core and priorities: `R_RENDER` is placed on core 1 next to the drawing thread (`playerkit/src/pk_cores.cpp`, `CP_MD`).

## Not done / open

- **Quick response (run-ahead by one frame) is offered** (Speed page: Off / Auto / On, the same wording as the other consoles). Auto on the
  Master System, Game Gear and SG-1000, Off on the Mega Drive (a heavy Mega Drive frame run twice does not fit a 60 Hz frame on the model:
  below). The governor (`playerkit/src/pk_ahead.h`) starts it only while the measured cost leaves room and stops it when frames run late, so
  a game that is too heavy simply runs without it. What was proved on the PC twin (host numbers, `twin/ahead_check.sh`, in the gate as
  `md-ahead`): the machine is the same with it on or off (sound, state, RAM, battery RAM; 105 ROMs, single thread, VDP worker, wide view);
  with no button pressed the picture of frame n is the plain run's frame n + 1 (Game Gear included; the tile cache is put back with the
  state); a small test cartridge that answers the pad one frame later (`twin/tools/sms_latency_gen.py`) shows a press 1 frame after it
  plain and 0 with run-ahead. Cost (model, `docs/PERFORMANCE.md`): about twice the plain emulation, so the heaviest Mega Drive samples
  (xgm-player, 4.2 M instructions a frame plain, 7.7 M ahead) do not fit and the busiest 8 bit test ROMs (2.2 M plain, about 4.4 M ahead)
  fit only at the optimistic IPC; nothing ran on a Vita, so Auto decides on the console from what it measures.
- **A loaded state continues exactly** as the uninterrupted run (also a rewind landing and the state taken for run-ahead): the state file
  holds a "carry" block after the core's own state (the first line's sprite list and pending flags, the FIFO byte flag, HV latch, latched
  scroll, viewport, the PSG's queued changes, the EEPROM chips' protocol position, the ROM banks a flash chip has rewritten). Version 2;
  version 1 files of the same game still load (without the block, as before). Proof (`md_twin --proof N M`, 105 ROMs): load at frame N,
  run M frames equals the run that was not interrupted, in the same process, after unloading and loading the ROM again, and into a fresh
  machine, single thread and worker, N = 1, 97, 300, 655.

- The FM (YM2612 + PSG + resample) worker is not written; FM stays on the emulating thread.

- Nothing has run on a real Vita: speeds are host numbers and a model (see the baseline). A locked 60 on a busy Mega Drive game
  is expected to be tight on one thread and is not proven.
- No genuine Game Gear or SG-1000 ROM was available: those systems are tested by forcing Master System images onto them (boot, sound,
  state) and by Game Gear test cartridges made for the purpose.
- `md/frontend/LICENSE` and a `md/frontend` Makefile are not part of this package; the object is built by `core/Makefile`.
- Core speed work (for example moving the palette remap to NEON, skipping unchanged lines) was measured and left alone: it must keep the
  output bit for bit, and the numbers do not yet show it is needed.
