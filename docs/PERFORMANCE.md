# Performance

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
| Stunt Race FX (PAL) | Super FX | 46.5 of 50 | 13.1 / 26.2 ms | every frame |
| Doom | Super FX 2 | 40.7 of 60 | 23.0 / 27.5 ms | every frame |

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

Super FX 1 and 2 games that keep the GSU busy all the time (Doom, the races
in Stunt Race FX) are limited by the GSU interpreter on the game thread.
Running the GSU on its own core is the next step for them.

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
