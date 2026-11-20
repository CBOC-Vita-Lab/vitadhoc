# Performance

## 2.0 performance pass: MMPX, the NES wide view, the wide-view boot frame (host, qemu and model numbers; nothing here ran on a Vita)

Nothing in this pass lowers a picture: every change is bit-identical to what it replaces, and the tests below prove it.
Labels: **host** = x86_64 callgrind (instructions), **qemu** = armhf guest instructions counted under qemu-arm, **model** = a
conversion to milliseconds (444 MHz, IPC 0.7 to 1.0), **estimate** = a judgement. The pictures of the MMPX measurements are
six frames of a private test set resampled to 240 x 160 (noisier than a true 1:1 frame, so they have more edges than a real
game picture and are close to a worst case for the scaler).

| Change | Proof of identity | Cost before / after | Vita (model) |
| --- | --- | --- | --- |
| MMPX 2x in NEON (gpsp/vita/mmpx_vita.c): eight pixels at a time, all the rules as compares and selects, luma computed from the 565 value, a whole group of eight flat pixels is one lookup and eight vector stores | upscale_test: NEON loop equals the C loop on 44 pictures (noise, flat with specks, lines, all colours; 11 sizes; cut at odd columns) under qemu, plus the existing MMPX stripe / slide / ring tests; the same output hash as the C loop on all six real frames | qemu, guest instructions per 240 x 160 frame, C -> NEON: 9.19 M -> 3.99 M, 3.61 M -> 1.33 M, 5.62 M -> 2.50 M (2.3x to 2.7x fewer; NEON instructions are not all one cycle, so the time gain is smaller) | about 1.5x to 2x faster MMPX (estimate); runs on the draw helper threads, not the game thread |
| NES wide view adapter (nes/frontend/wide.c): the margin fill runs as three runs (map seam, raw margin, learned map) with the palette pass hoisted out of the pixel loop; the fixed-line "one colour" test checks a tile's row once when its two bit planes are 00 or FF | wide_golden.sh: all 37 composed-picture hash files equal the committed baseline (wide_golden_sha256.txt), before and after | host, scrolling cartridge, 300 frames with the view on: compose 304.6 M -> 192.3 M; wide view's extra cost (view on minus off, twin hashing left out) 3.50 M -> 2.97 M per frame (-15 %) | about 1.2 to 1.7 ms less per frame on the compose thread (model, 444 MHz, IPC 1.0 / 0.7) |
| WonderSwan portrait rotation (wswan/frontend/layout.c): 8 x 8 blocks, a transpose in registers on ARM | rotate_test (blocked equals one-pixel-at-a-time, x86 and armhf under qemu), wswan-selftest orientation property test | the strided write of the pixel loop touched a new cache line per pixel; counted only as correct, not timed | small (32 K pixels a frame), estimate under 0.3 ms |

What the copy audit found. The per-frame picture path is: core draws into its buffer, the hand-over copies it into a slot (needed:
the core overwrites its buffer while the draw thread works), the upscaler copies it into its own "current" buffer while comparing it
with the picture before (one pass; the old picture has to be kept anyway), then the upscaled picture is uploaded. For a 240 x 160
GBA picture each copy is 77 KB, well under 0.1 ms; the Super Game Boy border copies only when the border changes and the game's 160
rows each frame; the NES palette pass and the WonderSwan mix (already NEON) are single passes. No copy was found that a change could
remove without a risk to the hand-over's timing, so none was removed: they are listed here so nobody looks for them again. What was
found and fixed is a loop, not a copy: the WonderSwan rotation above.

Not done, plainly: the NES wide view does not cache the nametable ring analysis (describe) between frames. Its output depends on the
frame counter while a cell is "settling" (a few frames after a write), so a cache would need a per-cell settle test, for about 3 % of
the view's cost; the learned map, the margin recorder in the core (nesp_tiles, on the emulation thread, 7 % of a wide-view frame)
and widekit's own passes (base_fill, edge colours) are other people's code in this pass and were left alone. The NEON MMPX loop is not
timed on a Vita; the first run on one should compare the `up_us` of the log with the C loop (build with -DMMPX_NO_NEON).

The GBA boot picture: a boot that starts on scanline 126 (the Polarium Advance fix, as mGBA does) has no whole first frame, so the
render thread's check no longer compares that one partial picture (it was the only frame the check ever failed on, once per start,
in the render_thread test).

## Super NES On VitAdhoc 2.0 (host and model numbers, nothing here ran on a Vita)

Counted on an x86_64 host with callgrind over the emulator's own code (Minstr per emulated frame, 60 frames of gameplay of the owner's pad
script) and turned into milliseconds by a model: x86 instructions counted as ARM instructions, 444 MHz, IPC 1.0 and 0.7. They are
estimates; the 1.3 table below is the only measured Vita data in this file.

| Game | Core, wide view off | Wide, margin 41 (core + compose) | Compose alone, 41 / full width | Same at IPC 1.0 / 0.7 (compose, 41) |
| --- | ---: | ---: | ---: | --- |
| Yoshi's Island (Super FX) | 7.8 | 13.7 | 4.5 / 8.0 | 10.0 / 14.4 ms |
| Super Mario World | 6.9 | 13.9 | 4.8 / 8.0 | 10.7 / 15.4 ms |
| Super Mario Kart | 5.0 | 10.7 | 3.8 / 5.9 | 8.6 / 12.3 ms |
| Super Metroid | 7.0 | 10.0 | 1.4 / 2.4 | 3.2 / 4.5 ms |
| Kirby Super Star (SA-1) | 6.2 | 12.8 | 5.1 / 7.7 | 11.6 / 16.5 ms |

The wide compose is 10 to 28 % cheaper than at the end of 1.3's work (same method; see snes/WIDE-INTEGRATION.md for the before column and for what is
and is not done). The pixel art scaler (xBR 2x) is estimated at about 3 ms of the draw thread typically and 8.5 ms in the worst second, the upload at 1.5 ms.
The rewind capture on the game thread: 823 KB per state before, about 300 KB (plain cartridge), 430 KB (Super FX) or 580 KB (SA-1) now, 1.16 M -> 0.53 M host
instructions in Super Mario World.

## Super NES On VitAdhoc 1.3

Measured on a PS Vita (firmware 3.74, PSVshell profile at
500 MHz; the core measured 494-496 MHz) on 7 November 2026, with the
release's SNES player and its default picture (Original shape, side glow).
Each game was started through VitAdhoc and left running its title screen,
attract mode or demo with no input for 100-120 seconds; the first four
seconds (start-up) are left out. "Game thread" is the time the emulated CPU
(plus the cartridge's extra chip, where there is one) needs per frame; the
picture and the sound run on two other cores and are not the limit in any of
these games.

| Game (owner's cartridge dump) | Chip | Emulated frames/s | Game thread per frame (average / worst second) | Shown |
| --- | --- | ---: | --- | --- |
| Super Mario World (PAL) | none | 50.0 of 50 | 5.5 / 7.5 ms | every frame |
| Donkey Kong Country | none | 59.7 of 60 | 4.7 / 14.9 ms | every frame |
| F-Zero | none | 59.9 of 60 | 5.1 / 5.8 ms | every frame |
| Chrono Trigger | none | 59.9 of 60 | 4.6 / 10.2 ms | every frame |
| Yoshi's Island | Super FX 2 | 59.7 of 60 | 8.2 / 13.5 ms | every frame |
| Super Mario RPG | SA-1 | 59.1 of 60 | 11.5 / 16.8 ms | every frame |
| Kirby Super Star | SA-1 | 59.1 of 60 | 11.0 / 15.3 ms | every frame |
| Stunt Race FX (PAL) | Super FX | 49.4 of 50 (46.5 before pacing) | 10.0 / 21.9 ms | every frame |
| Doom | Super FX 2 | 59.0 of 60 (40.7 before pacing) | 13.4 / 23.0 ms | every frame |

The display runs at 59.9-60 Hz in every game; "every frame" means no frame
was dropped to save time. Super Mario World and Stunt Race FX are European
(50 Hz) dumps, so 50 is their full speed.

What changed for these numbers, all with the same output as before (frames
and sound compared bit for bit on 289 test programs and the 15 games, with
and without the extra threads):

- **One emulated frame per screen refresh.** The first hardware run showed
  games running at twice their speed: a pass that handed its picture over
  without waiting started the next frame early. Passes now follow the
  display's own refresh count.
- **Battery RAM written on its own thread.** Super FX games such as Yoshi's
  Island change their battery RAM all the time, and writing it on the game
  thread every two seconds cost a third of a second each time (a stutter
  every two seconds, about 52 frames a second on average). It is now copied
  and written by a low-priority thread, when it settles or every 30 seconds.
- **SA-1 idle loops replayed.** Super Mario RPG and Kirby Super Star spend
  about 80% of their SA-1 instructions waiting in a loop. Such a loop is
  recorded once, proven to repeat exactly, and then advanced without
  interpreting it while nothing it reads changes (`snes/core/sa1cpu.cpp`).
  Their game thread went from 16.5 and 14.9 ms to 11.5 and 11.0 ms a frame.
- **WAI waits skipped to the next event** when no SA-1 is present.
- **The console stays awake** while a SNES game runs (it used to turn the
  screen off, and stop the game, during demos with no buttons pressed).

- **Super FX pacing.** Doom keeps the GSU busy all the time; at the
  hardware's instruction rate a frame needed 22-27 ms on the game thread
  (GSU 15-18 ms), so everything ran at two thirds of its speed, music
  included. The SNES player now times the GSU (`superfx clock` lines in its
  log) and sets its instructions per line to the share that fits a refresh
  (90% of it, adjusted every second; never below 30%, back to 100% when there
  is room). Game, music and controls keep full speed; the 3D view gets fewer
  GSU instructions per emulated frame than on a SNES and so animates more
  slowly. Doom settles near 35%, Stunt Race FX between 42% and 77% in races;
  Yoshi's Island stays at 100%. This changes the emulated GSU's speed, not
  its results; the twin and the bit-for-bit comparisons run it at 100%.

A faster GSU interpreter (or the GSU on a core of its own) is what would give
these games their full 3D frame rate.

## Pokemon Unbound 2.1.1.1 On VitAdhoc 1.2

**About 60 emulated fps in the sampled opening beach scene, at full-screen
stretch with CPU xBRZ 3x.** The frontend produced about 44 video callbacks
per second across three repeat samples. Settings, exact results and counter
definitions are below.

![Pokemon Unbound on a physical Vita, full-screen CPU xBRZ 3x](../store/screenshots/vita-1.2/unbound-intro-beach.png)

### Test Setup

| Item | Setting or result |
| --- | --- |
| Date | 2026-11-03 |
| Hardware | One physical PS Vita |
| Executable | Existing installed 1.2 runtime, read back and hash-matched before testing |
| Game | User-supplied Pokemon Unbound 2.1.1.1, 32 MiB; private ROM, not distributed |
| Output | 960 x 544, full-screen stretch |
| Smoothing | CPU xBRZ, Enhanced 3x; every selected upscaler window reports factor 3 |
| Other display options | Wide view off, colour correction off, motion blend off; ambient setting 2 |
| Gameplay options | Keep full speed on; 60-second rewind and gameplay previews enabled |
| Fast-forward | Off during all selected measurement intervals |
| Clock | Unchanged by this test. Platform API reported 444 MHz; the startup timing probe estimated 496 MHz, using a different measurement method. |
| Capture | Installed VitaCompanion 1.10 display-plane capture, outside measurement intervals |
| Scope | Animated water and dialogue in the opening beach scene, with no player input |

The core selected its built-in GBA BIOS for Unbound. The test used the
existing app and clock settings.

### Three Repeat Samples

These are three consecutive sampling blocks from one session.
Each block contains two complete 600-loop performance windows. A 15-second
settling exclusion follows the starting snapshot. There were no controller
commands, captures or FTP requests during the controlled interval.

| Sample | Duration | Emulated frames/s | Frontend-produced frames/s | Frontend sampling coverage |
| --- | ---: | ---: | ---: | ---: |
| 1 | 25.659 s | 59.940 | 44.106 | 20.020 s |
| 2 | 27.494 s | 59.285 | 44.906 | 10.199 s |
| 3 | 26.426 s | 59.940 | 44.758 | 20.019 s |

The three blocks span 79.579 seconds and average **59.714 emulated fps**.
Seven whole frontend sampling intervals contained in that overall span
average **44.316 callbacks/s** across 70.382 seconds; some cross a block
boundary and therefore do not appear in an individual row's coverage.

The original three-by-three-window plan needed nine qualifying windows;
only eight were captured. The repeat blocks therefore use the first six,
in order, with two windows each. Including **all eight** eligible windows
gives 104.771 seconds at **59.768 emulated fps**, and 45.141 frontend
callbacks/s over 90.429 seconds of independent frontend samples.

The audio counter recorded **six additional underruns** over the selected
six windows, and seven over all eight. Across the broader starting/ending
snapshots, including settling and partially excluded boundaries, it rose
from 23 to 35. No audio-port errors were recorded in the selected windows.

### What The Counters Mean

- **Emulated fps:** handheld frames advanced per elapsed second. Keep full
  speed can run catch-up frames without drawing all of them.
- **Frontend-produced frames:** non-null core video callbacks before the
  display sink. Calculated from `(win - skipped) / dt_us`.
- **Upscaler factor:** the actual CPU smoothing factor reported by the
  runtime.
- **Audio underruns:** cumulative runtime counters, differenced across the
  selected windows.
- **Physical display fps:** unmeasured in this run; a high-speed camera or
  display-presentation instrumentation would measure it separately.

The `q` diagnostic values count queue conditions and waits.

### Reproduce The Test

Use the 1.2 executable, the same game revision and the
settings above. Advance to the pictured opening beach dialogue. Release
fast-forward, settle, then collect complete `vita_win`, `vita_upscale`,
`vita_audio` and `fps` intervals without capturing or transferring files.

Rates are computed as total frame counts divided by total elapsed time.
Vita-window durations use consecutive
log-emission timestamps, so small logging-overhead differences remain.
Frontend windows use their explicit `dt_us` and are not aligned to the
600-loop windows. Only wholly contained frontend intervals are included.

Exploratory setup also showed scene-dependent slowdown: one early interval
logged 55.022 emulated fps and 28.978 frontend callbacks/s. Although no
fast-forward, menu or suspend event overlaps that interval, capture/FTP
interference is unknown for this exploratory interval.

For lighter display processing, choose Original pixels or a lower image
quality setting.

### Inspectable Metrics And Captures

- [Structured metrics and selection rules](performance/unbound-2.1.1.1-metrics.json)
- [Vita performance windows](performance/unbound-vita-windows.csv)
- [Independent frontend windows](performance/unbound-frontend-windows.csv)
- [Three sampling blocks](performance/unbound-batches.csv)
- [Capture provenance and screenshots](../store/screenshots/vita-1.2/README.md)

The public exports contain fixed-schema numeric metrics. Raw device logs,
account information, network addresses, ROMs, BIOS files and saves are
excluded. The test copy used a unique filename, keeping existing saves separate.
The original game's battery save and resume files were read back unchanged.
Settings and play history were restored; the FPS counter was left off as
requested.

## Earlier Evidence

| Version | Game | Recorded result | Settings |
| --- | --- | --- | --- |
| 1.0 | Pokemon Ruby | About 60 emulated fps | CPU xBRZ and wide view enabled |

Version 1.2 changes reduce repeated work in rewind, smoothing and texture
handling. [Source Record](SOURCE.md) lists the automated test results,
reruns and remaining hardware checks.

## Sega (Genesis, Master System, Game Gear) On VitAdhoc 2.0

Nothing here has run on a Vita. Labels: host = x86_64 callgrind instruction counts, model = those counts at 444 MHz and an assumed IPC.

Instructions per frame, host x86_64 callgrind (120 frames, worker off, scaler not included):

| Game | Frame | VDP line renderer | Sound | Z80 | 68000 |
|---|---|---|---|---|---|
| xgm-player (SGDK sample, H40) | 4.20 M | 2.21 M | 0.41 M | 0.64 M | 0.90 M |
| bad-apple (SGDK sample) | 3.49 M | 1.46 M | 0.83 M | 0.22 M | 0.93 M |
| busy SMS homebrew | 1.07 M | 0.50 M | 0.24 M | 0.31 M | none |
| Game Gear (an SMS ROM forced to Game Gear, a stand-in) | 0.92 M | 0.34 M | 0.24 M | 0.31 M | none |

Model, per thread, margin to 16.7 ms (NTSC; PAL has 20 ms, so 3.3 ms more):

| Game | Single thread, IPC 1.0 / 0.7 | Emulating thread with the VDP worker, IPC 1.0 / 0.7 | VDP worker, IPC 1.0 / 0.7 |
|---|---|---|---|
| xgm-player | 9.5 ms (7.2) / 13.5 ms (3.2) | 4.5 (12.2) / 6.4 (10.3) | 5.0 (11.7) / 7.1 (9.6) |
| bad-apple | 7.9 (8.8) / 11.2 (5.5) | 4.6 (12.1) / 6.5 (10.2) | 3.3 (13.4) / 4.7 (12.0) |
| busy SMS | 2.4 (14.3) / 3.4 (13.3) | 1.3 (15.4) / 1.8 (14.9) | 1.1 (15.6) / 1.6 (15.1) |
| Game Gear (stand-in) | 2.1 (14.6) / 2.9 (13.8) | 1.3 (15.4) / 1.8 (14.9) | 0.8 (15.9) / 1.1 (15.6) |

The numbers leave out the picture scaler, the upload and the audio thread (all on other threads or in the drawing thread). x86 and ARM
instruction counts differ (ARM is usually 1.1 to 1.4 times more); with that factor the single thread xgm-player at IPC 0.7 is over 16.7 ms,
which is what the worker is for.

Measured on the host twin: the VDP worker is bit-identical to the single thread (hashes of picture, sound, state and RAM, 96 ROMs, x86 and
armhf under qemu). Worker busy time, host x86_64 wall clock, an SMS homebrew: about 41 microseconds a frame, joins about 23 microseconds a frame.
