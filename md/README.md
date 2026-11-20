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

## Not done / open

- Nothing has run on a real Vita: speeds are host numbers and a model (see the baseline). A locked 60 on a busy Mega Drive game
  is expected to be tight on one thread and is not proven.
- No genuine Game Gear or SG-1000 ROM was available: those systems are tested by forcing Master System images onto them (boot, sound,
  state), and the Game Gear picture after a state load differs from the uninterrupted run (open item in `core/PROVENANCE.md`).
- `md/frontend/LICENSE` and a `md/frontend` Makefile are not part of this package; the object is built by `core/Makefile`.
- Core speed work (for example moving the palette remap to NEON, skipping unchanged lines) was measured and left alone: it must keep the
  output bit for bit, and the numbers do not yet show it is needed.
