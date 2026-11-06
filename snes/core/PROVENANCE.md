# Snes9x core: provenance

| | |
|---|---|
| Upstream | https://github.com/snes9xgit/snes9x |
| Release tag | `1.63` (the latest release tag at the time of vendoring, 6 November 2026) |
| Commit | `921f9f7b83660eb44ad263022a57a4a029057c37` (2024-07-07) |
| Licence | The Snes9x licence, file `LICENSE` here, unchanged. It allows free redistribution for non-commercial use only and is not compatible with the GPL. |

## What is here

Only the emulation core: the 65816 CPU, PPU and tile renderers, DMA/HDMA, the
SPC700 and DSP (`apu/`), and the cartridge chips (Super FX, SA-1, S-DD1,
SPC7110, SETA, DSP-1/2/3/4, C4, OBC1, S-RTC, MSU-1, BS-X). The file list is the
one upstream's libretro build compiles (`libretro/Makefile.common`) minus
`libretro.cpp` and the NTSC filter, plus the `.cpp` files those sources
`#include`. `sources.mk` lists the translation units to compile; the
`apu/bapu` files it leaves out are included by `smp.cpp` and `sdsp.cpp`.

Not taken: every GUI port (gtk, qt, win32, macosx), netplay, the debugger, the
shaders, the Vulkan/GL output, the zip/JMA readers, libretro glue, and
`apu/bapu/smp/debugger`.

All the `*.h` files at the top of the upstream tree were copied (they are
small and the sources include them freely).

## Local changes

Every file is byte-identical to upstream except the four below. Each change
is one small wrapper or call to the optional timing hooks in `s9x_prof.h`
(compiled to nothing unless `S9X_PROF` is defined).

| File | Change |
|---|---|
| `s9x_prof.h` | New. The hooks: a clock, five time buckets (CPU, APU, PPU, CHIP, FRONT) and enter/leave helpers. |
| `gfx.cpp` | `S9xUpdateScreen` renamed `S9xUpdateScreen_impl`; a wrapper of the old name counts its time as PPU. Include added. |
| `apu/apu.cpp` | `S9xAPUExecute` wrapped the same way (APU); `S9xAPUEndScanline` counts its DSP catch-up and sample landing as APU. Include added. |
| `fxemu.cpp` | `S9xSuperFXExec` wrapped the same way (CHIP). Include added. |
| `cpuexec.cpp` | The per-instruction call of `S9xSA1MainLoop` counts as CHIP. Include added. |
| `gfx.cpp`, `gfx.h`, `ppu.cpp` (phase 2) | Brightness changes (INIDISP) only mark the colour tables stale; `S9xRefreshColours` rebuilds them (same code, same result) at the start of the next render flush instead of at every write. Some Super FX demos wrote INIDISP ~900 times per frame. Frame hashes and audio are identical to before on all test and owner ROMs. |
| `sources.mk` | New. The list of translation units. |
| `PROVENANCE.md` | New. This file. |

The frontend that drives this core is in `../frontend` (MIT) and does not
modify it. To update to a newer Snes9x: copy the same file list, re-apply the
four edits above (search for `S9XP_`), and run `snes/twin/conformance.py`.
