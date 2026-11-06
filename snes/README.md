# SNES for VitAdhoc (version 1.3, phase 1)

An accurate SNES core (Snes9x 1.63, the current release) that runs as its own
program inside the VitAdhoc package, plus the tools to measure it. Phase 1 is
a single-threaded accurate player with measurement hooks. Phase 2 (below) splits
the work across the Vita's cores using what phase 1 measures.

Nothing here has run on a real Vita yet. Every number in this file is from the
Linux build on an x86 machine, unless it says otherwise.

## Layout

| Folder | What | Licence |
|---|---|---|
| `core/` | Snes9x 1.63 core sources, vendored. `PROVENANCE.md` lists the tag, commit, files and the four small local edits. | Snes9x licence (non-commercial, not GPL) |
| `frontend/` | `host.cpp/.h`: the glue between Snes9x and any frontend (load ROM, run a frame, video, audio, pad, battery RAM, states). Third-party files for the Vita program in `third_party/` (vita2d, stb, Inter font). | MIT, "Copyright CBOC-Vita" |
| `twin/` | The Linux headless runner `snes_twin` and the test scripts. | MIT |
| `vita/` | The Vita program `snes.bin`: `main.cpp` (frontend) and the Makefile. | MIT |

### Licence separation

Snes9x's licence allows non-commercial use only, so it cannot be linked into the
GPLv2 VitAdhoc program. The SNES player is therefore a **second executable**:

- `app0:snes.bin` is built by `snes/vita/Makefile` from `snes/core`,
  `snes/frontend` and `snes/vita`. It includes **no file from `gpsp/`**.
- `gpsp/vita/Makefile` only packs the finished `snes.bin` into the VPK (a small,
  marked block near `VPK_EXTRA`). The licence and provenance files travel in the
  VPK under `licenses/` and `notices/`.
- VitAdhoc (`eboot.bin`) knows nothing of Snes9x. Its whole link to SNES is
  `plat_exec_program("snes.bin", rom_path)` (`gpsp/vita/plat_vita.c`, one call to
  `sceAppMgrLoadExec`). `snes.bin` returns by starting `app0:eboot.bin` the same
  way with the argument `--from-snes`.
- The package as a whole must stay free of charge and non-commercial. A written
  legal opinion has not been obtained; this is the design, not a ruling.

## Build and test (Linux)

```
make -C snes/twin -j1                  # build/snes_twin  (takes a few minutes)
snes/twin/selftest.sh                  # host glue tests: determinism, states, battery RAM, copier header
snes/twin/conformance.py               # compare with the demo authors' reference screenshots (needs Pillow)
snes/twin/baseline.sh > table.md       # the baseline table below
```

`snes_twin ROM [options]` (see `--help`):

| Option | Effect |
|---|---|
| `--frames N` | frames to run |
| `--pad "60:start,70:none,200:a+right"` | scripted pad (buttons `a b x y l r start select up down left right`) |
| `--hashes F` | one line per frame: `frame width height fnv1a64-of-the-picture` |
| `--shots 100,200 --shot-prefix P` | PNG screenshots of chosen frames |
| `--wav F` | all audio, 32040 Hz stereo |
| `--timing F` | per-frame CSV: total, 65816 CPU, APU, PPU, chip and frontend microseconds (turns profiling on) |
| `--summary` | one line: fps and the time split (profiling on) |
| `--srm F` | battery RAM file, loaded if present and saved at the end (`F.bak` keeps the previous file) |
| `--save-state F --state-frame N`, `--load-state F --state-frame N` | save after / load before frame N |
| `--opt name=value` | core options: `block_vram`, `sprite_tiles`, `interp`, `superfx_clock`, `transparency`, `echo_buffer` |

Without `--timing`/`--summary` the program prints one line with the fps from the
thread's **CPU time** (the build machine is shared, so wall time is not used).

### Test ROMs

No commercial ROMs are used. ROMs live outside the repository, in
a directory next to the checkout on the build machine (`SNES_ROMS=` to point elsewhere):

| Source | Licence | Use |
|---|---|---|
| https://github.com/PeterLemon/SNES ("krom") | No licence file in the repository. Used locally for tests only, never copied into this repository or the package. | 164 prebuilt ROMs: Mode 7, HDMA, windows, hi-res and interlace, mosaic, hi-colour, Super FX (58), SPC700 audio demos, CPU/SPC700/GSU instruction tests, a small game (Monster Farm Jump) |
| https://github.com/undisbeliever/snes-test-roms (+ its `bass-untech` assembler, built from source) | zlib | 108 ROMs built here: HDMA/INIDISP timing and glitch tests, windows, Mode 7 VMAIN, object dropout |
| https://github.com/gilyon/snes-tests | MIT | cloned; its ROMs come only as release downloads, which were not reachable, so unused |
| https://github.com/undisbeliever/untech-engine, terrific-audio-driver | game resources are not for reuse; audio driver licence not read | cloned, unused |

`snes/twin/tests/make_sram_rom.py` writes our own 32 KB test cartridge (adds one
to its battery RAM at every boot) for the `.srm` rules.

## Results so far

### Self tests (`selftest.sh`): all pass

Same ROM and pad script give identical frame hashes twice; a state saved at frame
99 and loaded at frame 150 replays frames 100-199 bit-identically; a 512-byte
copier header is skipped; battery RAM loads and saves (the test cartridge counts
1 then 2); the previous save is kept as `.bak`; no `.tmp` file is left.

### Screenshot conformance (`conformance.py`)

Each PeterLemon ROM with a reference PNG is run and its last frame compared in
RGB555 (one step per channel allowed: Snes9x draws full brightness as 30/31 on
some channels).

91 of 150 match the reference screenshot (RGB555, one step per channel allowed, frame 299)

The other 59 are 53 pixel differences and 6 size mismatches (hi-res/interlace
ROMs whose reference picture is stored at another height). Not every difference is
an emulation error: animated and input-driven ROMs were compared at a fixed frame
(299) and the references may show another frame. This was not sorted case by case;
the list is printed by `snes/twin/conformance.py`.

Open finding: the PeterLemon Super FX graphic demos (PlotPixel/PlotLine/FillPoly)
do not match their reference pictures under Snes9x 1.63. The GSU instruction tests
match. Disabling the invalid-VRAM-access block improves only PlotLine, so the cause
is likely DMA/HDMA/forced-blank timing sensitivity in the demos. It needs a
cycle-accurate reference (bsnes) to adjudicate; it is not resolved here.

### Baseline: x86, single thread

| Test ROM | fps (no profiling) | ms/frame | CPU 65816 | APU | PPU | chip | frontend | PPU flushes/frame | APU port accesses/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Hello World (text) | 2726.3 | 0.37 | 44.4% | 37.1% | 18.2% | 0.0% | 0.3% | 1.0 | 0.0 |
| Mode 7 RotZoom | 1978.1 | 0.51 | 35.9% | 29.2% | 33.1% | 0.0% | 1.8% | 1.0 | 0.0 |
| Mode 7 Perspective | 2049.0 | 0.49 | 30.6% | 36.4% | 32.3% | 0.0% | 0.7% | 1.0 | 0.0 |
| Mode 7 StarWars crawl | 1722.6 | 0.58 | 31.8% | 22.5% | 44.8% | 0.0% | 0.9% | 1.8 | 0.0 |
| Mode 7 HDMA | 2039.9 | 0.49 | 32.8% | 29.1% | 37.8% | 0.0% | 0.3% | 1.0 | 0.0 |
| Mode 7 PlotLine (CPU draw) | 2024.8 | 0.49 | 37.3% | 29.7% | 30.4% | 0.0% | 2.5% | 1.0 | 0.0 |
| Mode 7 PlotPixel (CPU draw) | 2036.2 | 0.49 | 34.2% | 30.3% | 32.6% | 0.0% | 2.9% | 1.0 | 0.0 |
| Windows + HDMA | 2351.4 | 0.43 | 34.2% | 32.1% | 30.0% | 0.0% | 3.8% | 5.0 | 0.0 |
| HDMA wave | 2100.1 | 0.48 | 40.0% | 31.3% | 27.5% | 0.0% | 1.2% | 1.0 | 0.0 |
| Hi-colour 3840 (blend, HDMA) | 1422.3 | 0.70 | 18.9% | 25.7% | 55.1% | 0.0% | 0.2% | 1.0 | 0.0 |
| Pseudo hi-res 64 per row | 942.1 | 1.06 | 18.5% | 13.9% | 67.4% | 0.0% | 0.1% | 214.2 | 0.0 |
| Interlace RPG (hi-res 512x448) | 817.5 | 1.22 | 12.0% | 15.9% | 71.2% | 0.0% | 1.0% | 1.1 | 0.0 |
| Mosaic mode 5 (hi-res) | 566.3 | 1.77 | 9.6% | 11.7% | 77.8% | 0.0% | 0.9% | 1.0 | 0.0 |
| Rings (BG + windows) | 1504.6 | 0.66 | 28.5% | 25.1% | 45.3% | 0.0% | 1.2% | 1.0 | 0.0 |
| Sprites (object dropout test) | 1923.3 | 0.52 | 31.6% | 27.7% | 39.8% | 0.0% | 0.9% | 1.0 | 0.0 |
| Windows (shapes) | 2197.7 | 0.46 | 31.0% | 37.6% | 26.8% | 0.0% | 4.6% | 74.2 | 0.0 |
| Mode 7 tilemap scroll | 1947.1 | 0.51 | 27.9% | 35.9% | 35.3% | 0.0% | 1.0% | 1.0 | 0.0 |
| HDMA parallax (double buffered) | 2533.1 | 0.39 | 38.7% | 48.5% | 10.3% | 0.0% | 2.5% | 1.0 | 0.0 |
| HDMA shear (indirect) | 2333.8 | 0.43 | 44.7% | 36.1% | 17.6% | 0.0% | 1.5% | 1.0 | 0.0 |
| Monster Farm Jump (game) | 1253.5 | 0.80 | 20.5% | 23.5% | 55.2% | 0.0% | 0.7% | 1.6 | 0.0 |
| Super FX 8bpp PlotPixel 256x192 | 230.9 | 4.33 | 92.1% | 5.6% | 1.9% | 0.2% | 0.1% | 13.3 | 0.0 |
| Super FX 8bpp FillPoly 256x192 | 214.0 | 4.67 | 92.0% | 5.2% | 2.4% | 0.3% | 0.1% | 14.8 | 0.0 |
| Super FX 4bpp FillPoly 256x192 | 214.6 | 4.66 | 92.2% | 4.9% | 2.5% | 0.2% | 0.2% | 15.0 | 0.0 |
| Super FX 2bpp PlotLine 256x192 | 214.6 | 4.66 | 91.7% | 5.6% | 2.1% | 0.1% | 0.5% | 22.3 | 0.0 |
| SPC700 Axel-F | 3015.1 | 0.33 | 42.7% | 48.9% | 7.6% | 0.0% | 0.9% | 1.0 | 422.9 |
| SPC700 FF7 Prelude | 3024.3 | 0.33 | 46.2% | 44.6% | 7.8% | 0.0% | 1.3% | 1.0 | 434.9 |
| SPC700 Speech synth | 3027.7 | 0.33 | 45.4% | 44.5% | 8.4% | 0.0% | 1.8% | 1.0 | 572.4 |
| SPC700 Pitch mod | 2958.0 | 0.34 | 33.4% | 55.5% | 9.9% | 0.0% | 1.2% | 1.0 | 64.1 |
| SPC700 Noise | 3115.4 | 0.32 | 46.9% | 43.0% | 7.9% | 0.0% | 2.3% | 1.0 | 3.4 |
| CPU test ADC | 2555.0 | 0.39 | 37.7% | 43.8% | 18.1% | 0.0% | 0.4% | 1.0 | 0.0 |
| Synthetic Super FX (GSU saturated) | 814.9 | 1.23 | 15.4% | 11.9% | 3.9% | 67.5% | 1.2% | 1.0 | 0.0 |
| Synthetic SA-1 (saturated) | 1404.7 | 0.71 | 35.6% | 8.6% | 3.3% | 52.4% | 0.1% | 1.0 | 0.0 |

Reading the table:

- "fps" is emulation only (no display, no audio device, no profiling), best of 3,
  `g++ -O2`, one thread. It is **not** a Vita figure. A Vita core at 444-500 MHz
  is very roughly 10 to 25 times slower than a desktop core on this kind of code;
  the real figures come from `ux0:data/GBAdhoc/snes/log.txt` (below).
- The split comes from a second run with the hooks on. Buckets: **CPU** is the
  65816 plus everything not claimed by another bucket (DMA/HDMA, the scheduler,
  PPU register writes); **APU** is the SPC700, the DSP and sample landing; **PPU**
  is scanline rendering (`S9xUpdateScreen`); **chip** is Super FX / SA-1;
  **frontend** is our glue at the end of the frame.
- These are test programs. Most idle in a polling loop, so the 65816's share is
  lower than in a commercial game, and the Super FX demos run the GSU for a short
  burst. Treat the split as a lower bound for CPU/chip load. Commercial games
  are the next measurement (the owner will supply some).
- Profiling costs time: the clock is read on every hook. The unprofiled fps and
  the profiled split are therefore from separate runs.

## The Vita program (`snes.bin`)

Started with `snes.bin <rom path>`. Picture: RGB565 texture at the core's native
size (256x224, 512x448 for hi-res and interlace), scaled to 4:3 at 544 lines high,
bilinear. Sound: the core's 32040 Hz output resampled to 48000 Hz with linear
interpolation and a rate control that holds about 60 ms in the queue. 500 MHz is
requested for the CPU (444 MHz if refused), bus 222, GPU 222, crossbar 166.

| Vita | SNES |
|---|---|
| Cross / Circle / Square / Triangle | B / A / Y / X |
| L, R, Start, Select, D-pad | the same (the left stick also acts as D-pad) |
| L + R + Start (held together) | pause menu |

Pause menu: Resume, Save state, Load state (L/R pick the slot, 1 to 4), Reset game,
Quit to VitAdhoc. Menu confirm follows the console's confirm-button setting
(Cross or Circle); the other is "back".

Files:

| File | Where |
|---|---|
| Battery RAM | `Game.srm` beside the ROM (RetroArch's name). Written about 2 s after it last changed, at quit and when the console suspends. The previous file is kept as `Game.srm.bak` before the first overwrite of a session. Carts whose RAM has no battery (Super FX work RAM) write nothing. |
| Save states | `ux0:data/GBAdhoc/snes/states/<game>.st0` to `.st3` |
| Log | `ux0:data/GBAdhoc/snes/log.txt` (appended) |

Log: one line per second, `t=12 fps=59.9 emu_fps=59.9 emu_ms=9.40 last_ms=9.7 aq=2100`
(fps shown, frames the core ran, average and last frame time in ms, audio queue in
frames) and, for one second in four, the split `prof(60 fr): cpu=38% apu=30% ppu=27%
chip=0% front=5% ms/fr=9.8`. The profiled seconds run a little slower.

Not implemented in phase 1: zip files, pause-menu settings (filters, frame skip,
core options), controller ports beyond pad 1, mouse/Super Scope, cheats, rewind,
RTC data for S-RTC/SPC7110 carts (their clock RAM is not saved), BS-X and Sufami
Turbo, MSU-1 audio files.

## Library hook (VitAdhoc side)

`.sfc` and `.smc` files appear under a new console, **SNES** (`FE_CONSOLE_SNES`,
after GBA, GB, GBC). Folders searched: `ux0:ROMS/SNES`, `ux0:roms/SNES`,
`ux0:data/RetroFlow/ROMS/Nintendo - Super Nintendo Entertainment System`, plus
`snes_roms =` in `config.ini` and the Game folders setting. Choosing a game calls
`plat_exec_program`. The list shows a save badge when `Game.srm` exists. A launcher
(RetroFlow) can start `.sfc/.smc` through the same `param=` path.

Left to do on the VitAdhoc side: `.zip` files holding a SNES ROM (the zip reader
knows only GBA/GB/GBC), cover art and the "Continue" resume entry for SNES,
per-console display settings (the SNES slot has the default profile but the
Settings rows apply to the GBA/GB cores only), a short "Back from SNES" return
screen, and hostcore twin tests for the new console.

## Timing hooks (what they are and cost)

`core/s9x_prof.h` is compiled in when `S9X_PROF` is defined (both Makefiles do).
When `g_s9xp.on` is 0 the hooks are one load and one branch. Hook points:

| Bucket | Where |
|---|---|
| APU | `S9xAPUExecute` (every CPU access to $2140-$2143 and every scanline), `S9xAPUEndScanline` (DSP catch-up, sample landing) |
| PPU | `S9xUpdateScreen` (the real renderer; `RenderLine` only snapshots per-line registers) |
| chip | `S9xSuperFXExec`, the per-instruction `S9xSA1MainLoop` call |
| frontend | `S9xSyncSpeed` (our callback at the start of vertical blank: audio mix) |
| CPU | everything else |

SA-1 is timed around every instruction, which is expensive; the SA-1 split is for
orientation only, and no SA-1 test ROM was available.

## Phase 2 design: using all of the Vita's cores

Goal: faster and better looking than the other Vita SNES emulators without
giving up Snes9x's behaviour. Rule for every step: **with the threads on, frame
hashes and the audio WAV must be bit-identical to the single-thread build**
on every test ROM (`snes_twin --hashes/--wav`, `conformance.py`, `selftest.sh`).
A change that cannot meet that is an option the player turns on, never a default.

### What the measurements say

From the baseline table (x86, so only the proportions carry over to the Vita):

- **APU is a fixed tax**: about a quarter to a half of an idle frame (12 to 55%) (the SPC700
  and DSP run at 1.024 MHz / 32 kHz whatever the game does). It is also the
  cleanest thing to move: it talks to the rest of the machine through four
  one-byte ports.
- **PPU** is 10 to 30 percent of a normal frame, 55 to 78 percent for hi-colour,
  hi-res, interlace and mosaic content (the 512-wide renderers), and grows with
  the number of mid-frame register changes (214 renderer flushes per frame in the
  pseudo-hi-res row against 1 in most others).
- **65816 + DMA + scheduler** is what is left: 10 to 45 percent here; commercial
  games will be higher (the test ROMs idle in loops).
- **Chips**: the synthetic saturated Super FX ROM spends 67% of a 1.23 ms frame
  in the GSU (about 0.8 ms) and the saturated SA-1 ROM 52% of 0.71 ms (about
  0.37 ms; the SA-1 figure includes the per-instruction timer overhead). The free
  Super FX demos barely use the GSU (0.2 to 0.4%) but cost 4.3 to 4.7 ms/frame
  in the 65816/DMA bucket, which is the frame-sized transfers of the plotted
  bitmap, not the chip; the cause was not isolated. No free ROM loads the chips
  like a real game, so real games are a required measurement.

If CPU (65816 + DMA + chip) stays on one core while APU and PPU move out, the
frame time falls from `CPU + APU + PPU` to about `max(CPU, APU, PPU)` plus
synchronisation. On the idle test ROMs that is a bound of roughly 2x to 3x; the
figure that matters (a CPU-heavy game on a Vita) is not known yet.

### Core layout

| Vita core | Work |
|---|---|
| 0 (main thread) | 65816, DMA/HDMA, scheduler, PPU register file, pad, Super FX / SA-1, state save/load, presenting the frame |
| 1 | SPC700 + DSP + resampling to 48 kHz + `sceAudioOut` (today's audio thread folds into this one) |
| 2 | PPU scanline renderer, then the GPU submission |

The Vita allows three application cores; the system takes part of the third, so
thread priorities and affinity must be set explicitly and measured
(`sceKernelCreateThread` affinity mask, `sceKernelGetThreadRunClockLow` per thread
in the log). Core 2 should be tried both ways (renderer there, or renderer on core
0 and Super FX there) once real games are measured.

### 1. APU on its own core (exact)

Snes9x already runs the SPC700 lazily: `S9xAPUExecute` catches the APU up to
`CPU.Cycles` only when the 65816 touches a port (`S9xAPUReadPort/WritePort`) or at
the end of a scanline. That is the whole contract, so a thread can keep it:

- The 65816 side publishes its time (`CPU.Cycles` converted to SMP clocks, plus a
  scanline counter) to an atomic after every port access and every scanline.
- A **port write** becomes a record `(smp_time, port, value)` pushed on a
  single-producer ring. The APU thread applies it when its own clock reaches
  `smp_time`, exactly as `cpu.port_write` does now.
- The APU thread runs SPC700 + DSP freely up to the newest published time
  (never beyond: the 65816 may still write a port at that time).
- Its output ports are logged as `(smp_time, port, value)` too. A **port read** at
  time `t` waits until the APU clock is at least `t`, then reads the logged value
  at `t`. Most reads never wait because the APU trails the 65816 by less than a
  scanline.
- Samples never go back to the 65816: DSP output goes straight to the resampler
  and audio port. The audio-queue rate control (already in `snes.bin`) stays.
- The counter `apu_port/fr` in the baseline table is the number of such
  synchronisations per frame. The test ROMs show 0; commercial games must be
  measured. Games that poll a port in a tight loop (the SPC upload, some sound
  drivers) are the cost: each poll waits for the APU to reach `t`. If that
  turns out to be thousands per frame, spin on the atomic for a bounded time,
  then yield.

What must stay exact: the SMP-clock timestamp of every port access (the ratio
arithmetic in `S9xAPUGetClock`, including the fractional remainder), the order of
writes, DSP register writes by the SPC700 (they happen inside the APU thread,
nothing to do), and the echo buffer (`SeparateEchoBuffer` stays off). Save states
must quiesce the thread (drain the ring, copy SMP/DSP state, resume).

Expected gain: the APU bucket leaves the main thread: 15 to 50 percent of a frame
in the table.

### 2. PPU rendering on its own core (exact if the snapshots are exact)

Snes9x does not draw a line when the beam passes it. `RenderLine` only copies the
per-line registers (scroll, Mode 7 matrix, windows) into `LineData`; the pixels
are produced later, in batches, by `S9xUpdateScreen`, which is called when a PPU
register write would change how the *pending* lines look (`FLUSH_REDRAW`) and
once at the end of the frame. Most games flush once per frame; HDMA-heavy games
flush up to once per line (`ppu_flush/fr` in the table: 1 to 5 typically, 214 in
the pseudo-hi-res demo).

Plan: replace the body of `S9xUpdateScreen` with "snapshot, enqueue, return".
A flush record holds:

- the line range `[PreviousLine, CurrentLine)` and the frame number;
- the registers the renderer reads (`PPU`, `IPPU`, `GFX` fields, `LineData`,
  `LineMatrixData` rows for those lines) copied by value;
- the changes to VRAM, CGRAM and OAM since the previous flush, as a list of dirty
  256-byte VRAM pages and 16-byte CGRAM/OAM runs. The CPU side already knows
  when it writes them (`IPPU.TileCached`, `OBJChanged`, `ColorsChanged`
  bookkeeping); the page list is that bookkeeping made explicit.

The renderer thread owns a private copy of VRAM/CGRAM/OAM, applies the dirty
pages, draws the lines into the frame buffer with the existing tile code
(`tile.cpp`, `gfx.cpp`; unchanged), and hands finished frames to the GPU thread.
Cost on the main thread: one 64 KB copy per frame in the worst case, dirty pages
otherwise.

Things that must stay on the main thread and exact: the sprite range/time-over
flags (`PPU.RangeTimeOver`, read through $213E; `SetupOBJ` computes them, so
the cheap per-line sprite counting part has to run synchronously and the tile
fetching part can move), `$2137` latches, H/V counters, the OAM/VRAM/CGRAM
read-back registers, and forced blanking and brightness as they are at each
register write. Latency: one frame behind the main thread (about 16 ms), hidden by
presenting the previous frame; input is read on the main thread, so the extra
latency is the display's only. The `BlockInvalidVRAMAccessMaster` check uses
timing, not rendering, and stays where it is.

Hi-res and interlace: the renderer writes 512x448; the GPU does the scaling, so
nothing is averaged away on the CPU (the libretro "blend" modes are exactly what
we should not copy).

Expected gain: the PPU bucket leaves the main thread: 10 to 78 percent of a frame
in the table, the largest single gain for hi-res and hi-colour content.

### 3. Super FX / SA-1 on a spare core

- **Super FX**: Snes9x runs the GSU in a chunk per scanline (`S9xSuperFXExec` at
  the end of the line and when the 65816 touches its registers or RAM). A thread
  can run the chunk for line N while the 65816 executes line N+1, **if** every
  65816 access that could observe the GSU waits for the chunk: GSU registers
  ($3000-$32FF), the cartridge RAM banks ($70-$71, which the GSU draws into) and
  ROM while the GSU owns it (RON/RAN). Those blocks must be remapped to a
  checking handler instead of the direct pointers the memory map uses now. DMA
  from $70 (the demo ROMs do this) goes through the same check. The synthetic
  "saturated Super FX" row (0.9 ms/frame on x86) is the gain to be had.
- **SA-1**: it shares ROM, I-RAM and BW-RAM with the 65816 and is stepped after
  *every* 65816 instruction (`S9xSA1MainLoop`). Handshakes through the message
  registers and interrupts are frequent. It stays in lock step in phase 2 (on
  core 0, with the 65816), and is revisited only when real SA-1 games show it
  dominating. Threading SA-1 needs optimistic execution with rollback, which is
  not planned.
- **Other chips** (DSP-1..4, C4, S-DD1, SPC7110, OBC1, SETA): cheap and mostly
  triggered by register writes; they stay on core 0.

### 4. GPU: scaling and filters

The picture is already a GPU texture (RGB565). Phase 2 adds, at no CPU cost:
selectable integer or 4:3 scaling with sharp-bilinear sampling, scanline/CRT
and LCD-grid shader passes (GXM fragment programs, as vita2d's own
`texture_tint` shader), and pseudo-hi-res handled as a 512-wide texture. The
frame goes through the renderer thread's finished buffer, so the texture upload
(`memcpy` today) also moves off core 0. Filters stay optional per game and on a
1-10 strength scale where they have strength; the "better looking" default is
sharp bilinear. Any CPU-side filter (xBRZ-style) is out of scope: it would eat
the cores the threading just freed.

### 5. What phase 2 changes in the code

- `snes/core`: the four `S9XP_` edits stay; add three small seams: the APU
  port functions (apu.cpp), the body of `S9xUpdateScreen` (gfx.cpp), and the
  memory-map entries for GSU RAM/registers (memmap.cpp, fxemu.cpp). Each seam
  is behind a compile-time switch so the single-thread build remains the
  reference, and PROVENANCE.md grows a line per seam.
- `snes/frontend`: thread start/stop, the rings, a barrier for state save/load
  and for quit.
- `snes/vita`: affinity, the log's per-thread times, the options in the pause menu.

### 6. Order of work and checks

1. Real-game baseline on the Vita (this phase's log) and on x86 (owner's ROMs):
   the `apu_port/fr`, `ppu_flush/fr` and split numbers decide the order below.
2. APU thread. Check: hash and WAV identity on every ROM; `apu_port` waits per
   frame in the log; no change in audio underruns.
3. PPU thread. Check: hash identity; flush count; the pseudo-hi-res (214 flushes)
   and Windows (74) ROMs as the stress cases.
4. Super FX thread, then GPU filters.
5. Frame skip only as a last resort and only when the player enables it: skipping
   the PPU job (not the CPU/APU work) is exact for game logic, because the PPU
   thread already owns rendering.

## Phase 2 status (threads)

Switches (twin: `--opt NAME=1`; Vita: ini keys of the same names): `apu_thread`, `ppu_thread`. Both off gives the exact phase-1 path.

Done:
- `apu_thread`: SPC700 and DSP run on a worker that replays the CPU-side command stream (run-to-cycle, port read, port write, end-of-scanline) in order, so output is the same by construction. Port reads help-drain the queue under a lock instead of handing over threads (a wait-per-read design was about 70x slower on polling ROMs).
- `ppu_thread`: each render flush posts a state snapshot (PPU, IPPU, line tables, colour tables, dirty VRAM blocks); a second copy of the renderer objects (symbols renamed at build time, `core/rcopy.sh`) draws on its own thread. The picture is snapshotted when the host fetches it, because Snes9x can hand the picture over after the next frame has started drawing; copying at the end of the frame instead made the Super FX demos differ.
- `python3 twin/mt_compare.py [--mt ppu_thread=1,apu_thread=1] [--frames N] [--roms DIR]` runs every ROM both ways and compares frame hashes and WAV bytes.

Results (x86, shared 2-core box, so timings are noisy):
- 289 of 289 ROMs identical (300 frames) with ppu_thread, and with both threads; owner set 15 of 15 identical at 1800 frames.
- Main-thread CPU time on the owner set drops (for example Doom 0.43 s to 0.25 s for 600 frames, Chrono Trigger 0.40 to 0.16), but wall time did not improve on this machine because the box has only two busy cores and the workers spin briefly when idle. Real speedup needs measuring on the Vita's three cores; not done.
- Commercial ROMs: 190-1223 APU port accesses per frame, 1-31 PPU flushes per frame.

Not done yet: Super FX thread, Vita affinity/priority tuning and per-thread busy log in the 1-second line (affinity masks in `s9x_mt.h` are now the SDK user-core masks, untested), snes.bin vpk build re-check, picture options (ambient, zoom, shaders, governor), the Super FX demo and 92% bucket investigation. Nothing here has run on a real Vita.
