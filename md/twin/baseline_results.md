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
