# SNES for VitAdhoc (version 1.3, phase 1)

An accurate SNES core (Snes9x 1.63, the current release) that runs as its own
program inside the VitAdhoc package, plus the tools to measure it. Phase 1 is
a single-threaded accurate player with measurement hooks. Phase 2 (below) splits
the work across the Vita's cores using what phase 1 measures.

Numbers in this file are from the Linux build on an x86 machine unless they say
otherwise. The first hardware results (7 Nov 2026) are in "On a Vita" below and
in `docs/PERFORMANCE.md`.

## On a Vita (7 Nov 2026)

The first runs on a console found four faults the twin could not show, all
fixed: the game path never reached snes.bin (`sceAppMgrLoadExec` puts the
program's path in front of the arguments it is given, so VitAdhoc passed
"snes.bin" as the game); passes were not tied to the display (a pass whose
picture went into the queue without waiting started the next frame early:
games ran at twice their speed); no power tick (the screen went off a few
seconds into a demo and the game stopped); and battery RAM writes on the game
thread (Yoshi's Island changes its battery RAM all the time: a 330 ms write
every 2 s). Passes now follow `sceDisplayGetVcount`, the .srm is written by a
low-priority thread (`srm_writer`), when it has settled for 1 s or every 30 s
while it keeps changing, and the governor is off (see below).

Then, with the same output (mt_compare: 289 test programs and the 15 owner
games identical, with and without threads, against the build before):

- SA-1 idle loops are replayed (`sa1cpu.cpp`): a loop the SA-1 comes back to is
  recorded once (state after each instruction, cycles, plain bytes read) and,
  when one pass ends where it started without writes or I/O reads in ROM-resident
  code, later calls advance along the recorded points without interpreting,
  while nothing it reads changes; anything the SNES CPU writes to the SA-1's
  registers, a load or a reset ends it. Super Mario RPG went from 16.5 to
  11.5 ms per frame on the game thread, Kirby Super Star from 14.9 to 11.0.
- The SNES CPU's WAI is skipped to the next H event, NMI or timer IRQ when there
  is no SA-1 (`cpuexec.cpp`).

The GSU (Doom: 189,000 instructions a frame, about 18 ms) is the limit left for
Super FX games that keep it busy. On the Vita the SNES player paces it: the GSU's
time is measured (`S9xSuperFXClock`, `snes_host_superfx_take_us`) and
`Settings.SuperFXClockMultiplier` is set every 60 frames to the share that fits
90% of a refresh (30-100%, back up when there is room), so game, music and controls
keep full speed (Doom 59 of 60 instead of 41) and the 3D view animates more slowly.
The twin keeps 100%. A faster GSU interpreter is what would lift the 3D rate.

Super FX pacing with the cycle-accurate GSU (Snes9x `8783b54` and follow-ups): the chip's budget per line is
now `H_Max * SuperFXClockMultiplier / 100` GSU cycles (the real chip's speed at 100%) instead of a flat
instruction count, so the chip runs fewer instructions per line than before (Doom 921 to 576, Stunt Race FX 920
to 398, Yoshi's Island 681 to 443 steps a line, measured). The pacing hooks are unchanged (`S9xSuperFXClock`
measures the GSU's time, `snes_host_set_superfx_clock` sets the share) and still work, in both modes. Option
`gsu_cycles=0` selects the old flat budget (takes effect at the next ROM load). The game's own timing of the
3D view follows the real chip now; whether the Vita keeps 60 is not measured here.

## Layout

| Folder | What | Licence |
|---|---|---|
| `core/` | Snes9x 1.63 core sources, vendored. `PROVENANCE.md` lists the tag, commit, files, the upstream fixes taken after 1.63 and every local edit. | Snes9x licence (non-commercial, not GPL) |
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
snes/twin/selftest.sh                  # host glue tests: determinism, states, battery RAM (incl. forced write failures), copier header
snes/twin/exact_check.sh               # proves the WAI skip and the SA-1 replay are exact (builds with and without them)
python3 snes/twin/mt_compare.py --roms DIR   # threads on and off: frames and audio bit for bit (DIR: ROMs, not copied)
snes/twin/conformance.py               # compare with the demo authors' reference screenshots (needs Pillow)
make -C snes/twin build/player_twin    # the player's own code (menu, states, launch, cheats) with playerkit's software renderer
build/player_twin test ROM SHOTS WORK  # scripted walk with checks and a PNG per screen (ROM: tests/make_cheat_rom.py)
python3 snes/twin/pace_check.py --twin build/snes_twin --roms DIR [--expect bench.json]   # steady pacing changes nothing
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
| `--srm F` | battery RAM file, loaded if present and saved at the end. The existing file is first copied to `F.bak` (or to `--backup-dir`) and the copy read back; if that fails the save is left untouched, the error is printed and the exit status is 3 |
| `--backup-dir D` | keep the backups of `--srm` in `D` (`name.<crc>.bak`) like the Vita |
| `--srm-async` | save like the Vita's writer thread (take, write on a thread, forget on failure, retry) |
| `--unblock P` | after a failed save delete `P` and retry once (used by the failure tests) |
| `--cheat CODE` (repeatable), `--cheats-root DIR` | add a code: Game Genie `XXXX-XXXX`, Action Replay `AAAAAAVV`, raw `AAAAAA=VV`, `AAAAAA:VV` or `AAAAAA=CC?VV`. With `--cheats-root` the game's list is read from and saved to `DIR/cheats/<rom>.cht` |
| `--cheat-toggle F:I:0\|1`, `--cheat-remove I`, `--cheat-disable-all`, `--cheat-list` | at frame F switch entry I off or on; remove an entry; switch all off; print the list |
| `--cheat-decode CODE` (ROM `-`) | print a code as address=value cells, or the error, without a ROM |
| `--peek HEX` | print the work RAM byte at that offset at the end of the run |
| `--save-state F --state-frame N`, `--load-state F --state-frame N` | save after / load before frame N |
| `--opt name=value` | core options: `block_vram`, `sprite_tiles`, `interp`, `superfx_clock`, `gsu_cycles`, `transparency`, `echo_buffer` |

Without `--timing`/`--summary` the program prints one line with the fps from the
thread's **CPU time** (the build machine is shared, so wall time is not used).

### Save safety (`frontend/host.cpp`)

Battery RAM is written as: temp file, read back and compared, rename over the save, final file read back.
The first overwrite of an existing save needs a **verified backup** (size and checksum read back; on the Vita
`<name>.<crc>.bak` in the backup folder, an interrupted earlier copy is replaced, a different file with the
same name gets the next name). If any step fails the existing save is untouched, `snes_host_save_error()` gives the
real reason (log and an on-screen note on the Vita), and the Vita retries after 10 s, doubling to 60 s,
without blocking the game thread. A longer `.srm` than the cartridge's RAM keeps its extra bytes.
`CMemory::SaveSRAM` is not called by the frontend and uses the same temp-and-rename write. `selftest.sh`
forces the failures (file size limit, backup folder blocked, truncated earlier backup, name clash, temp file
blocked, stale `.bak`, retry after unblock, sync and async) and asserts the save stays byte-identical.
Hardware behaviour of the Vita file system (rename over an existing file, fsync) is not tested here.

### Cheats (`frontend/host.cpp`, `frontend/host.h`)

The engine is Snes9x's own (`core/cheats2.cpp`): each code is poked into memory at the end of every frame on the
emulation thread, and the APU and PPU workers read none of that memory, so the threads stay exact (`selftest.sh`
runs codes with the threads on and checks the frames equal the single thread). The host adds:

- **Decoding**: Game Genie through the core's `S9xGameGenieToRaw`, Action Replay through `S9xProActionReplayToRaw`,
  and raw `AAAAAA=VV` / `AAAAAA:VV` / `AAAAAA=CC?VV` (conditional). Up to 16 cells a code, 512 codes a game,
  address 0 refused. `snes_host_cheat_validate` and `snes_host_cheat_decode` check a code without adding it.
  `cheat_vectors.py` checks the Game Genie decoder against an independent Python implementation of the same
  bit layout (300 random codes, fixed vectors, refused spellings). That is a check against the layout as written
  in the core, not against a real cartridge's codes.
- **The list**: `snes_host_cheats_load(root)`, `snes_host_cheat_add/enable/remove/get/count`,
  `snes_host_cheat_disable_all`. The list is kept in RetroArch's `.cht` format in `<root>/cheats/<rom>.cht`. Keys and
  lines this host does not use are kept. A file that exists but cannot be read is never overwritten.
- **Writing**: the `.cht` is written like a save (temp file, read back, verified backup of the old file, rename).
  A failed write keeps the change in memory, `snes_host_cheat_error()` gives the reason, and the next change (or
  `snes_host_cheats_save()`) retries.
- **Reset and state load** poke the codes again (`cheats_suspend` / `cheats_resume`).
- **A ROM patched by a code** (Game Genie on the ROM image) switches the SA-1 idle-loop replay off for that
  cartridge until the next load (`S9xSA1RomPatched`), because the replay assumes ROM code does not change.

The Vita program's menu has a Cheats page (list, on/off, add with the system keyboard, delete; see "The in-game
menu" below). The Snes9x cheat database is not used. Hardware behaviour of the file writes is not tested here.

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
1 then 2); the previous save is kept as `.bak`; no `.tmp` file is left; the save-safety failures listed under
"Save safety"; the cheat tests (decode vectors, RAM and ROM codes, threads with codes, state load, the `.cht`
file, a foreign file, a failed write that leaves the file byte-identical).

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

Started by VitAdhoc with a launch file (`launch/snes.ini`, see "Launch protocol") or, as before, `snes.bin <rom path>`
(`rom|save` for a zipped game). The player's behaviour is `vita/player.cpp` (portable; the Linux twin
`twin/player_twin` runs the same code), `vita/main.cpp` is the console around it. The core's picture (RGB565, 256 or 512 wide) is copied into a GPU
texture and everything else happens on the GPU (see "Picture" below). Sound: the core's 32040 Hz output
resampled to 48000 Hz with linear interpolation and a rate control that holds about 60 ms in the queue.
The CPU clock follows the app's request (`cpu_mhz` in the launch file: 444 or 500; 0 or no file: 500 MHz, 444 if
refused), bus 222, GPU 222, crossbar 166.

| Vita | SNES |
|---|---|
| Cross / Circle / Square / Triangle | B / A / Y / X (the console's own layout; remappable) |
| L, R, Start, Select, D-pad | the same (the left stick also acts as D-pad) |
| hold START + SELECT, or tap the picture | the in-game menu (hold L + R + Start still works) |
| SELECT + L / SELECT + R | quick save / quick load, slot 1 |

### The in-game menu (playerkit)

The same control centre as the Game Boy Advance side (`playerkit/`, MIT): Resume, Save states, Cheats,
Settings, Reset game, Game list, Exit, every row touchable, Enter/Back by the launch file's `enter_button` (the
console's setting without a launch file).

- **Save states**: ten slots with pictures (`<root>/states/snes/<game>.st0` .. `.st9` with `.thumb`, `.shot`,
  `.meta`: the main app's shelf reads them like GBA states). X (Enter) on an empty slot saves, on a filled one
  loads; Square saves over a filled slot (asks first); Triangle deletes (asks); Select (or the button) undoes
  the last load. Writes run on the writer thread; the slot shows at once. A failed write keeps the slot as it
  was and says why (red notice).
- **Settings > Picture**: Screen size (Original, Zoom, Full screen, Pixel perfect), Pixel shape (8:7 or square),
  Image quality (Sharp, Smooth), Scanlines, LCD grid, Side glow (Off, 1-10), Blend hi-res columns, Blend
  interlace lines, Apply to (All games / This game). Stored in `snes/picture.ini` and `snes/games/<game>.ini`
  when the menu closes (on the writer thread).
- **Settings > Controls**: left and right stick (Off, D-pad, Buttons), rear touch panel, Multitap (Auto, On,
  Off), Button mapping for players 1-4 (every SNES button, turbo A/B, quick save/load), turbo speed and mode,
  Apply to (all games / this game), Reset. Stored in `controls/snes.ini` and `controls/snes/<game>.ini`.
- **Settings > Speed**: Steady pacing (On by default, see below).
- **Cheats**: the game's list (`<root>/cheats/<game>.cht`, RetroArch format), on/off, "Add a cheat" (the
  system keyboard: the code, then an optional name), Delete (Triangle or the row's button, asks).
- **Reset game**, **Game list** (back to VitAdhoc) and **Exit** (close VitAdhoc) ask first.
- Notices (state saved/loaded, cheat on/off, a save that failed and why) clear themselves; errors have a red
  dot and stay 5 s.

The battery RAM is never written before the menu is on screen: it is handed to the writer thread after the
menu's first frame.

### Launch protocol

`<root>/launch/snes.ini` (written by VitAdhoc, keys in `playerkit/FORMATS.md`) is read at start: game, save,
enter_button, cpu_mhz, shape/quality (unless the game has its own picture file from this menu), `ovr.*` per-game
values (`scanlines`, `lcd_grid`, `glow`, `shape`, `quality`, `pixel_square`, `hires_blend`, `interlace_blend`,
`multitap`, `steady_pacing`), `start` (`continue`: the resume point, only when made with the same `patch_key`;
`slot` + `slot`), `patch` (IPS/BPS/UPS applied to the ROM in memory; the game is not started if it does not
apply, the save and states keep the original game's name), `retroarch_save` (copied over the save when newer;
the replaced save is kept in `backups/snes/<game>.before-retroarch.srm`). Without the file (or when its `game`
differs from the argument: an old file) the argument is used as before. On exit `launch/session.txt` is written:
seconds played, save changed / failed, the last error, states touched, resume point present, how it ended.
The resume point (`states/snes/<game>.rsm`, `.rsm.pt`, `.rsm.meta`, and `scenes/<game>.shot`) is written on
exit, every 2 minutes and about a second after the game saved (at most every 30 s), on the writer thread.
Old states (`snes/states/<game>.st0` .. `.st3`, the four slots of earlier versions) are moved once into the same
slot, or the first free one when it is taken; nothing is overwritten or deleted.

### Players (couch multiplayer)

Pad 1 is the Vita's own controls. Pad 2 is Vita port 2 when a controller is paired there, else port 1 on a
handheld (external pads through ds34vita, MiniVitaTV and the like); on a PS TV port 1 mirrors port 0 and is
not used. Pads 3 and 4 are ports 3 and 4. The ports are checked once a second, so a pad can be plugged in or
removed during play (a missing pad reads as no buttons). Input is read with `sceCtrlPeekBufferPositiveExt2`
(the Vita's L/R arrive as L1/R1, a PS TV pad's L2/R2 as the old L/R bits; both mean SNES L/R).
Pads 3 and 4 reach a game only through the **Super Multitap** in controller port 2 (Snes9x supports it):
pause menu "Multitap" = Auto (on for titles known to use it: Bomberman, Secret of Mana, Seiken Densetsu 3,
NBA Jam, NBA Give 'n Go, NHL, FIFA, Madden), On or Off. Without the multitap, pad 2 is a plain pad in port 2.
Tested on Linux with a cartridge that copies the auto-joypad registers into its RAM (`selftest.sh`); the Vita
port detection and the multitap have not been tried with real controllers.

### Picture

Settings are in the pause menu under Picture, saved for all SNES games (`ux0:data/GBAdhoc/snes/picture.ini`)
or, with "Apply to: this game", for one game (`ux0:data/GBAdhoc/snes/games/<game>.ini`).

| Setting | Values |
|---|---|
| Shape | **Original shape** (default: correct 4:3, full height, about 725x544; the side areas show the glow), Zoom (blank top/bottom rows are cropped, the picture is enlarged but not stretched), Full screen (stretched), Pixel perfect (integer height scale; Pixels: 8:7 or square) |
| Quality | **Sharp** (default: nearest-neighbour prescale by an integer factor into a render target, then bilinear: "sharp bilinear") or Smooth (plain bilinear) |
| Side glow | Off, 1-10 (default 5). A 64x64 and a 16x16 shrink of the game's own picture, drawn bilinear over the whole screen and darkened, refreshed every 4 frames. Replaced by a bezel when one exists |
| CRT scanlines, LCD grid | Off, 1-10 |
| Blend hi-res columns | Off (default) or On. For 512-wide pictures: the GPU averages each pair of columns (bilinear sample exactly between two texels, no extra pass), which smooths the dithering/transparency tricks hi-res games use. Opt-in because it softens text |
| Blend interlace lines | Off (default) or On. The same for 448/478-high interlaced pictures: pairs of lines are averaged, which removes the line flicker |
| Bezel | not a setting: `ux0:data/GBAdhoc/overlays/<game>.png`, else `overlays/snes.png`, a PNG (transparent window) drawn over the whole screen; it replaces the glow |

No shader compiler exists in this build environment (vitasdk has only the runtime `libshacccg.suprx` loaded
on the console; no offline `psp2cgc`, and vita2d here ships precompiled texture/colour programs only), so no
custom fragment shader is used. Scanlines and the grid are alpha-blended dark rectangles drawn between the SNES
lines / pixels by the GPU; sharp bilinear is the two-pass render-target method; the glow is two shrink passes.
Hi-res (512 wide) and interlaced (448/478 high) pictures use the same paths; the 4:3 shape does not change, and
the scanline and grid counts follow SNES lines (an interlaced picture counts two rows as one line).
The layout maths (`frontend/picture.cpp`) is tested on Linux (`selftest.sh`); the GPU passes are compiled only.

### Threads on the Vita

APU worker on core 2, PPU worker on core 1, emulation (65816, DMA, GPU submission) on core 0, audio output
thread on core 2 at a more urgent priority than the APU worker, and a low-priority writer thread on core 1 (battery
RAM, states, the resume point, settings and the log: nothing is written to the card on the game thread while
playing). `audio_prio`/`main_prio` outside 0x40-0xBF are replaced by the defaults. Priorities: workers 0x60, audio 0x40, main
thread 0x60 (`audio_prio=` / `main_prio=` in threads.ini, 0 = leave as the system made it). Switches in
`ux0:data/GBAdhoc/snes/threads.ini` (`apu_thread=1`, `ppu_thread=1`, `spin=300`, `audio_legacy=0`; the threads
are on by default). An idle worker polls `spin` times (a few microseconds) and then blocks on a semaphore;
nothing spins while the game waits. The main loop never waits for the PPU worker: it queues the end-of-frame
copy and shows the newest finished picture (normally the previous frame, one frame of display latency). The
Linux twin keeps its exact waiting fetch for the hash checks (`--nowait` exercises the Vita's fetch: every
picture it returns must equal a picture of the reference run).

### Audio pacing (`frontend/audio_pace.cpp`)

The audio thread renders 512-frame grains at 48 kHz from a lock-free ring of the core's 32040 Hz frames. A PI
controller on the smoothed ring fill (measured at each grain start) sets the resampling ratio; the target fill is an
adaptive cushion (70 ms, 50-140 ms, +20 ms per underrun, -5 ms after 30 s without one). An underrun, a pause and a
resume all fade out/in over 64 frames and prime again at half the cushion; an overfull ring is trimmed with a
crossfade. When the emulation runs slower than real time (speed over the last 8 s below 0.985) the pitch follows the
speed. `audio_legacy=1` in threads.ini selects the first player's audio for A/B tests. The same code runs in the
twin's `--audio-sim` (see `twin/audio_sim.sh`, results in `twin/baseline_results.md`).

Log, one extra line per second (counters of the audio thread's last second):

```
A t=12 in=31987 out=48128 fill=55.6/67.4/79.7 ema=69.4 tgt=70 ratio=-4453/-2554/945 base=1.0000 ur=0/0 drop=0 trim=0 agap=10.9 w16=0 w20=0 w25=0 stall=16.8 wmax=11.4 v2
```

`in`/`out` frames pushed / rendered; `fill` ring fill in ms at grain starts (min/avg/max), `ema` smoothed fill, `tgt`
cushion; `ratio` is (ratio-1) in ppm (min/avg/max, the slow-emulation base included), `base` the base speed; `ur=` underruns
this second / in total; `drop` frames refused by a full ring; `trim` overfill trims; `agap` longest gap in ms between two
`sceAudioOutOutput` returns; `w16/w20/w25` passes of the main loop whose work took over 16.7 / 20 / 25 ms; `stall` longest
time between two passes; `wmax` longest pass; last word `v2` or `legacy`. Each underrun also gets a line of its own:
`UNDERRUN #n (k new) at out=..s ema=..ms target->..ms ratio=..ppm queue_now=..ms work=..ms`. The log is kept in memory and
written at most every 5 s, by the writer thread.

### Steady pacing (on by default)

The game always runs at its true rate (sound and input untouched). When the last second shows that drawn
frames do not fit a refresh (10% of them over 16.7 ms, or their average over 97%), the player draws every
second emulated frame on an exact even cadence (the other frames take the core's render-off path, as the
governor below did); a pass that missed a refresh then runs the missed frame too, undrawn. It goes back to
every frame only after 3 s in which every drawn frame (the cost a full-rate frame has) stayed under 80% of the
budget (one stray spike allowed) and at least 2 s in half mode, so it does not flap. A drawn frame's cost is the
emulation of the rendered frame plus fetching and submitting its picture, or the picture thread's work for it,
whichever is larger. The decision is `playerkit/src/pk_pace.cpp`, a pure function with unit tests (bursts,
sustained overload, recovery, boundary flapping). Settings > Speed > Steady pacing switches it off
(`snes/player.ini`, `steady_pacing=0`). The log shows each change (`pace: full -> half ...`) and, on the `t=`
line, `not_rendered=` and `pace=<mode> drawn= over= max= changes=`. `twin/pace_check.py` proves the emulation
does not change: with odd frames not rendered (and both workers on) the audio, the RAM at the end and every
rendered frame are identical to the normal run, on the owner's 15 games (3000 frames).

### Smoothness governor (off)

`frontend/picture.cpp` still has a governor that would skip drawing every other frame after 24 slow ones
(level 2; level 1, which dropped picture options, is not used). On the Vita it is not called: the picture is
drawn on its own thread, so skipping it saved the game thread about half a millisecond while halving the
frames shown. A frame that runs long is shown late. The main loop emulates exactly one frame per display
refresh (the console's 60.0988 Hz against the Vita's 59.94 Hz is absorbed by the audio rate control; the former
"two frames to catch up" branch is gone). The governor is not called and has no log field any more.

Files:

| File | Where |
|---|---|
| Battery RAM | `Game.srm` beside the ROM (RetroArch's name). Written about 2 s after it last changed, at quit and when the console suspends. The previous file is kept as `Game.srm.bak` before the first overwrite of a session. Carts whose RAM has no battery (Super FX work RAM) write nothing. |
| Save states | `ux0:data/GBAdhoc/states/snes/<game>.st0` to `.st9` (+ `.thumb`, `.shot`, `.meta`), resume point `.rsm` |
| Picture settings | `ux0:data/GBAdhoc/snes/picture.ini`, `.../games/<game>.ini`; steady pacing in `snes/player.ini` |
| Controls | `ux0:data/GBAdhoc/controls/snes.ini`, `controls/snes/<game>.ini` |
| Cheats | `ux0:data/GBAdhoc/cheats/<game>.cht` |
| Launch / session | `ux0:data/GBAdhoc/launch/snes.ini` (read), `launch/session.txt` (written) |
| Threads | `ux0:data/GBAdhoc/snes/threads.ini` |
| Log | `ux0:data/GBAdhoc/snes/log.txt` (appended) |

### Reading the log

One line per second:

```
t=12 fps=59.9 emu_fps=59.9 emu_ms=9.40 last_ms=9.7 aq=2100 | busy ms/fr: main=11.2 ppu=3.1 apu=4.8 | waits/s: ppu=0 apu=3 (0.4 ms) | drawn=60 skipped=0 new_pic=60 fence_skip=0
```

Since the speed pass the line also has `main split ms/fr: emu= copy= gpu_submit=` (the main figure divided into the core call, the picture copy and GPU submission) and `rt_reuse=` (a running total since the program started of how often a render target was reused without being redrawn). `fps` is what was shown, `emu_fps` frames the core ran, `emu_ms` time inside the core call on the main thread,
`aq` the audio queue in frames. `busy ms/fr` is the time each thread spent working per emulated frame
(Vita thread CPU clocks are not available, so each thread measures around its own work; the main figure is the
whole loop without the display wait). A frame fits when main stays under about 15.5 and the workers under 16.7.
`waits/s` counts how often the main thread had to wait for a worker and how long in total. `fence_skip` counts
pictures not copied because the GPU still read every texture. Without threads the line also carries
`prof(60 fr): cpu= apu= ppu= chip= front= ms/fr=` for one second in four. Other lines: `pads:` when a controller
appears or goes, `overlay`, `render targets:` (0 means a target could not be created).

Not implemented: zip files, core options in the menu, mouse/Super Scope, a cheat screen (the host API is in
place, see "Cheats" above), rewind, RTC data for
S-RTC/SPC7110 carts (their clock RAM is not saved), BS-X and Sufami Turbo, MSU-1 audio files.

## Library hook (VitAdhoc side)

`.sfc` and `.smc` files appear under a new console, **SNES** (`FE_CONSOLE_SNES`,
after GBA, GB, GBC). Folders searched: `ux0:ROMS/SNES`, `ux0:roms/SNES`,
`ux0:data/RetroFlow/ROMS/Nintendo - Super Nintendo Entertainment System`, plus
`snes_roms =` in `config.ini` and the Game folders setting. Choosing a game calls
`plat_exec_program`. The list shows a save badge when `Game.srm` exists. A launcher
(RetroFlow) can start `.sfc/.smc` through the same `param=` path.

Done since phase 1: `.zip` files holding a SNES game (unpacked like the
others; the save stays beside the zip), box art from the libretro thumbnail
set, own covers, every folder under `ux0:ROMS` read as a game folder, and the
`snes_library` test in `app_tests.py`.

Left to do on the VitAdhoc side: the "Continue" resume entry for SNES,
rewind and fast forward in the SNES player, and a short "Back from SNES"
return screen.

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

Phase 3 (this branch): Vita build re-checked and in the VPK; Vita thread placement, non-blocking picture fetch, picture options, governor and multiplayer ports (see "The Vita program"). Still not done: Super FX thread, measuring any of it on a real Vita. Nothing here has run on a real Vita.

### The Super FX demos' "65816/DMA" bucket: found and fixed

The PeterLemon Super FX demos spent 92% of their time in the 65816/DMA bucket although the GSU barely ran. A
callgrind run showed 86% of all instructions in `S9xBuildDirectColourMaps` and `S9xFixColourBrightness`, called
from the INIDISP write about 935 times per frame: the demos change the screen brightness all the time, and
Snes9x rebuilt both colour tables (2048 + 320 entries) at every write. They are now rebuilt once, lazily, before
the next render flush (`S9xRefreshColours`), which gives the same tables at every point where they are read.
Result: GSU 8bpp PlotPixel 256x192 went from 240 to 1462 fps on x86; frame hashes and audio are identical to the
old build on all 289 test ROMs and the 15 owner ROMs (`mt_compare.py --ref <old build>`, with and without
threads). It was not a busy-wait on the GSU and no idle-loop skip was needed. Games that fade the screen gain a
little too.

## Speed pass (profile-driven, all output bit-identical)

Method: callgrind, `--separate-threads=yes`, 600 frames of scripted play on each of the 15 owner games with
both workers on (cg-01 main, cg-02 APU, cg-03 PPU), minus the startup cost of a 1-frame run, so the numbers are
millions of x86 instructions per emulated frame on each thread. "Before" is the build at the start of this pass
(Super FX fix included), "after" is the final twin (same flags, `-O2`). The bottleneck thread is the largest of
the three. These are instruction counts, not Vita times: nothing here has run on a real Vita.

| Game | main before | main after | APU before | APU after | PPU before | PPU after | bottleneck before | after |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ChronoTrigger | 2.25 | 2.03 | 2.24 | 1.71 | 2.76 | 1.39 | 2.76 | 2.03 |
| Donkey Kong Country | 2.32 | 2.19 | 2.31 | 1.79 | 4.16 | 2.22 | 4.16 | 2.22 |
| Doom | 1.92 | 1.82 | 2.57 | 1.96 | 4.71 | 2.76 | 4.71 | 2.76 |
| F-Zero | 1.92 | 1.72 | 2.63 | 1.88 | 4.34 | 2.24 | 4.34 | 2.24 |
| Kirby Superstar | 6.47 | 5.73 | 2.77 | 2.34 | 2.67 | 1.71 | 6.47 | 5.73 |
| Mega Man X 2 | 2.07 | 1.91 | 1.97 | 1.63 | 1.60 | 0.66 | 2.07 | 1.91 |
| Pilotwings | 2.08 | 1.88 | 2.25 | 1.93 | 2.34 | 1.57 | 2.34 | 1.93 |
| Street Fighter Alpha 2 | 1.73 | 1.58 | 2.31 | 1.74 | 3.31 | 0.85 | 3.31 | 1.74 |
| Stunt Race FX | 2.13 | 1.95 | 2.64 | 2.23 | 2.59 | 1.39 | 2.64 | 2.23 |
| Super Mario Kart | 2.09 | 1.85 | 2.26 | 1.88 | 2.48 | 1.16 | 2.48 | 1.88 |
| Super Mario RPG | 6.20 | 5.98 | 2.48 | 2.19 | 1.43 | 0.63 | 6.20 | 5.98 |
| Super Mario World | 2.42 | 2.56 | 2.64 | 1.86 | 3.96 | 1.66 | 3.96 | 2.56 |
| Super Metroid | 2.21 | 2.07 | 2.00 | 1.78 | 1.90 | 1.01 | 2.21 | 2.07 |
| Yoshi's Island | 3.64 | 3.59 | 2.47 | 1.98 | 3.94 | 2.37 | 3.94 | 3.59 |
| Zelda: A Link to the Past | 1.95 | 1.75 | 2.30 | 1.80 | 3.31 | 0.77 | 3.31 | 1.80 |

Super Mario World's main thread went up about 6% (2.42 to 2.56); the cause was not isolated (the inlined memory
fast paths change code layout; its bottleneck, the PPU, still fell a lot). Startup (1 frame) fell from
about 430 to about 120 million instructions on the main thread for most carts (Doom: 244 to 88), because the
ROM's SHA-256 is no longer computed at load (the hash was only used to match a database that is not built in).

What changed:

- **PPU thread (largest gain, up to about 75% fewer instructions).** The tile plotters keep the screen, depth and
  colour-table pointers in one struct in registers instead of reloading globals for every pixel. Eight-pixel
  tile rows are drawn with 64-bit byte tricks (transparent, opaque and depth tests for a whole row at once),
  and the add/subtract colour maths for a row runs as one vector operation (SSE2 on x86, NEON on the Vita,
  through GCC vector types). The backdrop is filled in groups of pixels: covered groups are skipped, flat
  areas are stored in bulk, only mixed groups go per pixel. The depth buffers are cleared only where something
  was drawn (a dirty mark per frame) instead of whole.
- **Main thread (about 5 to 12% fewer).** Memory reads and writes for the common case (RAM/ROM block) are
  inlined; the rare case (I/O, SRAM, chips) stays a function call. SA-1 reads got the same fast path.
- **APU thread (about 15 to 30% fewer).** The DSP skips the Gaussian interpolation of a voice whose envelope is
  zero (the output is zero either way).
- **Vita GPU side.** Scanline/grid bands are drawn as one triangle-list draw instead of one draw per band; the
  sharp-bilinear render target is not redrawn when the picture and settings did not change; the log shows
  where the main thread's time goes. Two new opt-in Picture rows (above).
- **Tried and not done:** whole-program LTO and PGO (not measurable without the console; PGO data from x86 would
  not describe the Vita's cores), fusing the SPC timers, an SA-1 idle-loop skip (not exact), anything in
  the DSP-1, S-DD1 and Cx4 code (never more than 2% of a thread).

Checks: `mt_compare.py` shows identical frame hashes and audio against the pre-pass build for all 289 test ROMs
at 300 frames and for the 15 owner ROMs at 1800 frames, single-threaded and with both workers on; `selftest.sh`
passes. Not tested: anything on a real Vita (speed, the batched draws, the render-target reuse, the blend rows).

Note: the speed pass skips the ROM SHA-256 at load (`memmap.cpp`); `Memory.ROMSHA256` is all zeros. Only Snes9x cheat-database lookups (`cheats2.cpp`) read it, and snes.bin does not use them; compute it on demand if cheats are added.
