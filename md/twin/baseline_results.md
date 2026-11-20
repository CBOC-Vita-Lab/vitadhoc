# Sega twin baseline

Measured with `md/twin` on the development machine (2 shared cores, heavily loaded while measuring). Nothing here has run on a
real Vita. Every figure is labelled **host** (x86_64, measured), **qemu** (armhf under qemu-arm, measured), or **estimate**
(computed with the plan's model: a Cortex-A9 at 444 MHz does about 350 M instructions per second, error bar x1.5; the armhf
instruction count per frame is assumed equal to the x86 count).

ROMs: the 30 sample ROMs built from the SGDK sample directories (public source, built by the tester) and 65 public
Master System homebrew ROMs from the retrobrews collection. No ROM is part of this repository. No genuine Game Gear or
SG-1000 ROM was found: the Game Gear and SG-1000 rows force a Master System image onto those systems (same core code path for the
VDP window, palette and PSG/input differences, but not a real cartridge).

## Acceptance (host, selftest.sh, MD_TEST_FRAMES=1500)

| Check | Result |
|---|---|
| Mega Drive ROMs load and run 1500 frames | 30 of 30 |
| two runs identical (picture, sound and state hashes) | 30 of 30 |
| state round trip identical (picture, sound, state): save at frame 300, replay 300 frames, load, replay 300 | 30 of 30 |
| a state with a flipped byte is refused and leaves the machine untouched | 30 of 30 |
| Master System ROMs: boot, 600 frames, state round trip exact | 65 of 65 |
| armhf under qemu-arm equals x86_64 (picture hash of every frame, sound hash, sample count, picture size, work RAM hash), 600 frames, `arm_check.sh` | 95 of 95 identical, 0 different |
| cheat vectors (`gpsp/vita/tests/cheats/sega_vectors.txt`): Mega Drive | 21 vectors checked, 0 failed, 5 parts refused by the rules |
| cheat vectors: Master System / Game Gear / SG-1000 | 24 checked, 0 failed |
| regions: Europe, Japan PAL 49.7015 Hz; USA, Japan NTSC 59.9227 Hz; sample counts at 48 kHz | pass |
| exported symbols of the core object: `md_host_*` only | pass |

Not available: real Game Gear and SG-1000 ROMs (those tests are SKIPped, not passed). A Master System image forced to Game Gear
passes boot, state and sound round trips but its picture differs after a state load (see `md/core/PROVENANCE.md`, "Open").

## Cost per frame

Instructions of `md_host_run_frame` per frame (inclusive of the VDP, CPUs, FM/PSG and the resampler, excluding the twin's own
hashing), callgrind over 300 frames, **host** x86_64, `-O3`. "Vita ms" is the **estimate**: instructions / 350 M per second.
The budget is 16.7 ms (NTSC) or 20.0 ms (PAL); the app's own work (blit, upscale, audio output) comes on top.

| Case | system / mode | instructions per frame (host) | Vita ms (estimate) |
|---|---|---|---|
| cube-3D (SGDK) | Mega Drive H32 (256x224) | 3.18 M | 9.1 |
| partic (SGDK) | Mega Drive H32 | 3.12 M | 8.9 |
| hello-world, benchmark, bad-apple, sound-test | Mega Drive H40 (320x224) | 3.59 M | 10.3 |
| sonic | Mega Drive H40 | 3.91 M | 11.2 |
| platformer | Mega Drive H40 | 4.00 M | 11.4 |
| starfield-donut | Mega Drive H40 | 4.08 M | 11.6 |
| h-int/wobble | Mega Drive H40, per line scroll | 4.54 M | 13.0 |
| xgm-player (heaviest measured) | Mega Drive H40, XGM driver on the Z80 | 4.73 M | 13.5 |
| hello-world, PAL | Mega Drive H40, 313 lines, 50 Hz | 4.01 M | 11.4 of 20.0 |
| xgm-player, PAL | Mega Drive H40, 50 Hz | 5.19 M | 14.8 of 20.0 |
| 3 Master System games | Master System 256x192 | 1.08 - 1.26 M | 3.1 - 3.6 |
| picross | Master System | 1.35 M | 3.9 |
| 2 games, forced | Game Gear window 160x144 | 1.11 - 1.19 M | 3.2 - 3.4 |
| picross, forced | SG-1000 | 0.86 M | 2.5 |

Where the Mega Drive frame goes (host, instructions per frame): bad-apple 3.59 M = VDP lines 1.55 M (background planes 1.0 M,
palette remap to RGB565 0.5 M), 68000 0.9 M, FM 0.57 M, Z80 0.43 M, PSG 0.2 M; xgm-player 4.73 M = VDP lines 2.55 M, 68000 0.9 M,
Z80 0.96 M (the sound driver), FM 0.71 M, PSG 0.1 M, resampler/mix 0.3 M. One frame of a quiet Master System game is 1.1 M, of which the VDP lines are
0.68 M and the Z80 0.34 M.

Wall time per frame in the twin (median of 600 frames, shared machine, noisy): **host** 0.26 - 0.40 ms for the Mega Drive samples,
0.085 ms for a Master System game. **qemu** (armhf, user mode emulation): 1.9 - 10.3 ms for the Mega Drive samples, 0.63 ms for the
Master System game; the ratio qemu/host is 7x - 40x on this loaded machine and cannot be turned into Vita time. Only the instruction
model above is used for the estimate.

## Reading the numbers

The model says a busy Mega Drive game takes 9 - 13.5 ms of the 16.7 ms frame on one Vita core at 444 MHz and a Master System or Game
Gear game 3 - 4 ms. The Mega Drive is therefore within the frame but with little headroom for the app's own per frame work
on one thread; the plan's thread split (core on one thread, picture and sound output on others) is what keeps 60 Hz locked. These are
estimates with a x1.5 error bar: **a locked 60 Hz Mega Drive is not proven**; it has to be measured on the device.

## Wide view cost (2.0)

Instructions per frame, **host** x86_64, `valgrind --tool=callgrind --separate-threads=yes` over 300 frames of `md_twin ROM --vdp-thread --wide 51`
(`--wide 41` Master System, 48 Game Gear), counted per thread and per function (inclusive). "Model ms" = instructions / 350 M per second, the
estimate used above, x1.5 either way; **nothing here is Vita time**. "Before" is the first wide view (the build of 20 Nov morning), "now" the build with the
analysis handed to the VDP worker, the learned picture, the window / per 2-cell vertical scroll lines, the strip cache and the shared compose.
The twin composes on its main thread; in the player the compose is the drawing thread's and its lines are shared with the scaler's helpers.

Mega Drive, the five heaviest samples (VDP worker on, as the player runs them):

| sample | emulation, emulating thread | VDP worker, wide off | before: worker (of it strips) + analysis on the emulating thread | now: worker (strips + analysis), nothing on the emulating thread | compose before -> now |
|---|---|---|---|---|---|
| xgm-player | 2.58 M | 2.67 M | 3.26 M (0.59 M) + 0.70 M | 3.79 M (0.39 M + 0.69 M) | 0.76 -> 0.81 M |
| fx/h-int/wobble (shadow / highlight sprites on every line) | 2.43 M | 2.68 M | 5.01 M (2.33 M) + 0.65 M | 4.69 M (1.13 M + 0.85 M) | 0.75 -> 0.69 M |
| demo/starfield-donut | 2.43 M | 2.20 M | 2.98 M (0.78 M) + 0.85 M | 3.80 M (0.91 M + 0.66 M) | 0.85 -> 0.79 M |
| game/platformer | 2.91 M | 1.71 M | 2.29 M (0.58 M) + 0.63 M | 3.00 M (0.62 M + 0.65 M) | 0.93 -> 0.86 M |
| demo/bad-apple | 2.50 M | 1.69 M | 2.21 M (0.52 M) + 0.80 M | 2.49 M (0.14 M + 0.64 M) | 0.90 -> 0.83 M |
| test cartridge (window lines, per 2-cell vertical scroll; `tools/md_scroll_gen.py`) | 2.55 M | | (no strips on these lines before) | 3.70 M (1.01 M + 0.72 M) | 0.99 M |

What the frame costs in time (model): a frame is finished when the worker has drawn it, and before this work the emulating thread then still analysed it.

| sample | before: emulating thread busy until | now: emulating thread | now: VDP worker | of the 16.7 ms frame |
|---|---|---|---|---|
| xgm-player | 9.3 + 2.0 = 11.3 ms | 7.4 ms | 10.8 ms | 65 % |
| fx/h-int/wobble | 14.3 + 1.8 = 16.1 ms (the ladder dropped the strips) | 7.0 ms | 13.4 ms | 80 % |
| demo/starfield-donut | 8.5 + 2.4 = 10.9 ms | 6.9 ms | 10.9 ms | 65 % |
| game/platformer | 8.4 + 1.8 = 10.2 ms | 8.3 ms | 8.6 ms | 51 % |
| demo/bad-apple | 7.1 + 2.3 = 9.4 ms | 7.2 ms | 7.1 ms | 43 % |

- The emulating thread's share of the wide view is the hand-over: under 2 K instructions (`Wide::frame`, `md_host_wide_run`).
- Strips: 0.14 M on a still picture with few changes (bad-apple: two fifths of the line strips are copied from the frame before, and a strip no plane
  or sprite goes into is not drawn at all), 0.4 - 0.6 M normally, 0.9 - 1.1 M where every line has its own scroll or the shadow / highlight sprite pass runs
  for each strip (wobble: 2.33 M before; that pass now clears and merges the strip's width, not the screen's).
- Analysis: 0.64 - 0.85 M, most of it widekit (`wk_layers_update` 0.2 - 0.5 M: it walks every run of lines with its own scroll; `wk_map_update` up to
  0.6 M on the frames the screen shows other cells, nothing on the others). The name tables are reread only when the game wrote them (the core keeps the
  list of written VRAM blocks). A layer whose lines scroll apart into more than 64 runs is not given to the map (starfield-donut: 1.98 M with it, 0.66 M without).
- Compose: 0.69 - 0.86 M (0.99 M on the test cartridge, whose margins are all real noise), of which `wk_compose_lines` 0.44 - 0.47 M can be taken by the
  helpers: the drawing thread's own share is about 0.39 M + a third of the lines = 0.54 M (1.6 ms) with two helpers, 0.86 M (2.4 ms) alone.
- With the VDP worker off everything is on the emulating thread: xgm-player 4.74 M off, 5.33 + 0.70 = 6.03 M before, 5.15 + 0.70 = 5.85 M now (16.7 ms by the
  model: one thread does not hold it, which is why the player runs the Mega Drive with the worker).
- By the model every sample keeps the full real strips with the worker at 51 - 80 % of the frame. At the model's upper error bar (x1.5) wobble is over
  (20 ms) and xgm-player at the line (16.2 ms): there the ladder in `md/vita/main.cpp` halves, then drops, the real strips in Auto, as before.

The 8 bit consoles (no VDP worker by default: the analysis and the drawing of the learned strips are on the emulating thread):

| game | emulation | analysis before -> now | compose before -> now | emulating thread, model |
|---|---|---|---|---|
| Master System test cartridge (`tools/sms_scroll_gen.py`), both sides learned on every line: the most there is to draw | 1.21 M | 0.002 -> 0.87 M | 0.51 -> 0.69 M | 5.9 ms |
| Master System gaudream (homebrew, the left side learned) | 0.92 M | 0.002 -> 0.32 M | 0.44 -> 0.45 M | 3.5 ms |
| Game Gear test cartridge | 1.16 M | 0.31 -> 0.34 M | 0.93 -> 0.70 M | 4.3 ms |

Identity and cross-machine: `selftest.sh` section 11 (96 ROMs: picture inside the window, sound, state, RAM, wide view on / off, with and without the VDP worker thread),
11b (the same decisions on the worker thread and on the emulating thread; the worker's strip cache against a fresh draw of every cached strip: 1.4 M strips,
0 different), 12 (the learned picture and the Mega Drive strips against the worlds of test cartridges, pixel by pixel: 0 wrong of 1.7 M, 1.7 M, 1.8 M and 1.1 M
real margin pixels; on the Mega Drive samples 29.6 K learned cells shown, 26.1 K of them reached by the screen later, 0 a different cell), `arm_check.sh` (armhf ==
x86_64 with the wide view on too: picture, sound, RAM and the wide view's own decisions), `wide_pipe_proof` (through the picture pipe, the VDP and the analysis on the
worker, the compose on a second thread with its lines shared between three threads: bit for bit the pictures of one thread doing everything), the same under
ThreadSanitizer (0 reports).

Also found while building this: the core keeps some memory across `md_host_unload` / `md_host_load_mem` in one process (two loads of the same ROM can differ in a few
pixels of a late frame); the player starts per game and `md_twin` runs one game per process, so nothing is affected; `wide_pipe_proof` runs each pass in its own process.
The SGDK `game/sonic` "ILLEGAL INSTRUCTION" the twin showed under the random pad script (`--pad auto`) is in the ROM, not the core: the 68000 executes an `ILLEGAL` opcode (0x4AFC at 0x00AA64)
that the ROM's code runs into after certain button mixes, and the unmodified Genesis Plus GX libretro core and PicoDrive (two other 68000 implementations) stop at the same
address with the same registers on the same inputs. With no buttons pressed (and with a held right / jump script) the ROM plays on.
