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

The core is Snes9x 1.63 plus the changes below. Everything not listed is
byte-identical to upstream. Three kinds of change: **A** speed-ups that are exact (same frames
and audio as without them; each can be switched off, see the end), **B** upstream fixes taken from
the Snes9x master after 1.63 (commit ids are upstream's), **C** hooks and glue for the Vita frontend.

### A. Exact speed-ups and threading (CBOC)

| Files | Change |
|---|---|
| `s9x_prof.h`, `gfx.cpp`, `apu/apu.cpp`, `fxemu.cpp`, `cpuexec.cpp` | The timing hooks (compiled to nothing unless `S9X_PROF`): a clock, five buckets (CPU, APU, PPU, CHIP, FRONT), wrappers `S9xUpdateScreen_impl`, `S9xAPUExecute`, `S9xSuperFXExec_impl`, the `S9xSA1MainLoop` call. |
| `gfx.cpp`, `gfx.h`, `ppu.cpp` | INIDISP writes only mark the colour tables stale; `S9xRefreshColours` rebuilds them at the next render flush. |
| `getset.h`, `cpuaddr.h` | Inline fast paths for RAM/ROM reads and writes; `alwaysinline` on the addressing modes. |
| `tile.cpp`, `tileimpl*.cpp`, `gfx.h`, `gfx.cpp` | Row plotters with a per-line context (`PixCtx`) and a NEON blend; the depth-buffer dirty marks. Same pixels as the original per-pixel code. |
| `apu/bapu/dsp/SPC_DSP.cpp` | Envelope-zero skip in the voice loop. |
| `apu/apu.cpp`, `apu/apu.h` | The APU on its own thread (`apu_thread=1`): SMP and DSP run ahead, samples are landed at the same points. Bit-identical to the single thread. |
| `ppu_mt.cpp`, `s9x_mt.h`, `rcopy.sh`, `rcopy_check.sh`, hooks in `cpu.cpp`, `snapshot.cpp` | The PPU on its own thread (`ppu_thread=1`): the register state of each flush is snapshotted, lines are rendered by a copy of `gfx`/`tile*` made by `rcopy.sh` (`rcopy_check.sh` fails if the copy is stale). Bit-identical. |
| `sa1.cpp`, `sa1.h`, `sa1cpu.cpp` | SA-1 idle-loop replay (`SA1L`): a loop that ends where it started without writes or I/O reads in ROM-resident code is recorded once and later calls advance along the recorded points while nothing it reads changes. `sa1_replay` has no integer divide in the common case (the quotient is 1-3; the Vita's Cortex-A9 has no divide instruction). |
| `cpuexec.cpp` | WAI skip: while the CPU waits with no IRQ pending and no SA-1, the wait is advanced to the next H event in one step (see B4 for how it was merged with the upstream WAI change). |
| `memmap.cpp` | The ROM SHA-256 is not computed (only the cheat-database lookup read it): about 0.5 s of load time on a Vita. |
| `fxemu.cpp` | `S9xSuperFXClock` / `S9xSuperFXTime` wrap `S9xSuperFXExec_impl` so the frontend can measure the GSU's time. `Settings.SuperFXClockMultiplier` scales the per-line budget (in both timing modes). |

### B. Upstream fixes taken after 1.63 (re-implemented on this tree, one at a time)

| Upstream commit | What | Files | Effect on the 15 owner games at 3000 frames |
|---|---|---|---|
| `8a9b8cf` | SA-1: banks $40-$4F map to `MAP_SA1RAM` (the SA-1 side of BW-RAM), not to the SRAM size mask | `memmap.cpp` | None (only reads/writes past the SRAM size are different). |
| `8028d3b` | S-DSP register init (registers 0x42 and 0x52 start at 1; Magical Drop) | `apu/bapu/dsp/SPC_DSP.cpp` | None (only `SPC_DSP::load`, which runs when a state is loaded or the DSP reset is applied). |
| `c1aa979` | Super FX RPIX: flags are cleared at the end of the instruction | `fxinst.cpp`, `fxinst.h` | Stunt Race FX frames change (23,869 RPIX in 3000 frames; Doom 294 and Yoshi 588 RPIX gave no change). |
| `d17fd00` | Super FX 8-bit plot transparency (PLOT_* defines) | `fxinst.cpp`, `fxinst.h` | None: in the 8.9M 8-bit plots of Stunt and 3.65M of Doom no transparency decision differs. |
| `8783b54`, `1994532`, `1bcc369` | Super FX cycle-accurate timing: ROM/RAM/cache access costs (`GSU.v*`), cache-line fill, the instruction fetch pipe, a per-line budget in GSU cycles, the Winter Gold delay hack (`Timings.GSUCelDelay`) and `Settings.DisableGSUCycleMode` | `fxinst.cpp`, `fxinst.h`, `fxemu.cpp`, `snes9x.cpp`, `snes9x.h`, `memmap.cpp` | Doom, Stunt Race FX and Yoshi's Island change (frames and audio; the chip now does fewer instructions per line, as the real one). The 12 other games are identical. |
| `c0dffce` | IRQ: a write to $4211 latches an IRQ that is already due | `ppu.cpp` | See the step table in the session report; owner games unchanged. |
| `cf2b0e5`, `1b21357` | Mode 7: the registers $210D, $210E, $211B-$2120 are latched per line (`S9xPPULatchM7`, from dot 44), `RenderLine` uses the latched values | `ppu.cpp`, `ppu.h`, `gfx.cpp`, `cpuexec.cpp`, `snapshot.cpp` | Owner games unchanged (F-Zero, Mario Kart, Pilotwings included). |
| `642f0ac` | WAI: a waiting pass costs exactly one CPU cycle plus event processing; an IRQ wake adds one dot | `cpuexec.cpp` | See the report. Merged with the local WAI skip: the skip now advances by the same one-cycle passes (`W = ONE_CYCLE`), so it is exact. |

Merge notes. **Super FX**: the frontend's pacing hooks are unchanged. The measured GSU time
still feeds `SuperFXClockMultiplier`; in cycle mode the multiplier scales the per-line budget
(`H_Max * multiplier / 100` GSU cycles) as it scaled the instruction count before. The Winter Gold
hack's counters are file-level and are cleared in `S9xResetSuperFX`. After a state load the GSU
cache mask is cleared (`snapshot.cpp`). The frontend option `gsu_cycles=0` selects the old flat
per-line budget (`Settings.DisableGSUCycleMode`; effective at the next ROM load).
**SaveSRAM**: `CMemory::SaveSRAM` in `memmap.cpp` now writes through `S9xSafeWriteFile`
(temp file, read-back check, rename). The frontend does not call it (`S9xAutoSaveSRAM` is an
empty stub; `frontend/host.cpp` saves battery RAM itself), so this is a safeguard for other users of
the core.

### C. Other local files

| File | Change |
|---|---|
| `sources.mk` | New. The list of translation units. |
| `PROVENANCE.md` | New. This file. |

### Switches that prove the exact speed-ups

Build with `-DCBOC_NO_WAI_SKIP` (every WAI pass is interpreted) and `-DCBOC_NO_SA1_REPLAY`
(`sa1_code_in_rom()` is false, so every SA-1 instruction is interpreted). `snes/twin/exact_check.sh`
builds both ways and compares frames and audio of every ROM bit for bit. The threaded workers are
checked against the single thread by `snes/twin/mt_compare.py`.

The frontend that drives this core is in `../frontend` (MIT) and does not modify it. To update to a
newer Snes9x: copy the same file list, re-apply the changes above (search for `S9XP_`, `CBOC_`),
and run `snes/twin/conformance.py`, `exact_check.sh` and `mt_compare.py`.
