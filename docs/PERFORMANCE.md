# Performance And Hardware Evidence

## Pokemon Unbound 2.1.1.1 On VitAdhoc 1.2

**About 60 emulated fps in the sampled opening beach scene, at full-screen
stretch with CPU xBRZ 3x.** The frontend produced about 44 frames per second
in the three repeat samples. Those are different counters: this is not a
claim of 60 visible frames per second, or full-speed performance throughout
Unbound.

![Pokemon Unbound on a physical Vita, full-screen CPU xBRZ 3x](../store/screenshots/vita-1.2/unbound-intro-beach.png)

### Test Conditions

| Item | Setting or evidence |
| --- | --- |
| Date | 2026-11-03 |
| Hardware | One physical PS Vita; not Vita3K or the computer test build |
| Executable | Existing installed 1.2 runtime, read back and hash-matched before testing |
| Game | User-supplied Pokemon Unbound 2.1.1.1, 32 MiB; private ROM, not distributed |
| Output | 960 x 544, full-screen stretch; not a cropped integer-scale image |
| Smoothing | CPU xBRZ, Enhanced 3x; every selected upscaler window reports factor 3 |
| Other display options | Wide view off, colour correction off, motion blend off; ambient setting 2 |
| Gameplay options | Keep full speed on; 60-second rewind and gameplay previews enabled |
| Fast-forward | Off during all selected measurement intervals |
| Clock | Unchanged by this test. Platform API reported 444 MHz; the startup timing probe estimated 496 MHz. These are not interchangeable readings or a verified stock-clock comparison. |
| Capture | Installed VitaCompanion 1.10 display-plane capture, outside measurement intervals |
| Scope | Animated water and dialogue in the opening beach scene, no player input; not walking, battles, a late-game stress test or completion testing |

No clock plugin, experimental USB module or replacement executable was
installed. An existing personal GBA BIOS was used on the test device; the
public VPK instead includes an open replacement. BIOS choice is another
reason not to treat these numbers as a universal result.

### Three Repeat Samples

These are consecutive sampling blocks in one session, not three cold boots.
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
This is not evidence of flawless sound.

### What The Counters Mean

- **Emulated fps:** handheld frames advanced per elapsed second. Keep full
  speed can run catch-up frames without drawing all of them.
- **Frontend-produced frames:** non-null core video callbacks before the
  display sink. Calculated from `(win - skipped) / dt_us`. This is useful
  rendering-throughput evidence, not a measurement of unique frames
  physically visible on the panel.
- **Upscaler factor:** the actual CPU smoothing factor reported by the
  runtime, not simply the selected menu label.
- **Audio underruns:** cumulative runtime counters, differenced across the
  selected windows. A nonzero count should not be hidden by an fps claim.

The `q` diagnostic values count queue conditions and waits, not displayed
frames. They are not used to calculate visible fps. Independent high-speed
camera or display-presentation instrumentation would be needed for that.

### Reproduction And Limits

Use the exact 1.2 executable, the same game revision and BIOS, and the
settings above. Advance to the pictured opening beach dialogue. Release
fast-forward, settle, then collect complete `vita_win`, `vita_upscale`,
`vita_audio` and `fps` intervals without capturing or transferring files.

Rates are computed as total frame counts divided by total elapsed time,
not averages of rounded readings. Vita-window durations use consecutive
log-emission timestamps, so small logging-overhead differences remain.
Frontend windows use their explicit `dt_us` and are not aligned to the
600-loop windows. Only wholly contained frontend intervals are included.

Exploratory setup also showed scene-dependent slowdown: one early interval
logged 55.022 emulated fps and 28.978 frontend callbacks/s. Although no
fast-forward, menu or suspend event overlaps that interval, capture/FTP
interference cannot be excluded. It is not a second controlled benchmark.
The controlled beach result does not override that limitation.

There is no matched comparison with another emulator, with version 1.0,
or with different clock profiles here. No battery-life, input-latency,
full-game compatibility or network-play result follows from these samples.
If this combination feels uneven, try Original pixels or a lighter image
quality setting; that is a practical option, not a measured result in this
test.

### Inspectable Metrics And Captures

- [Structured metrics and selection rules](performance/unbound-2.1.1.1-metrics.json)
- [Vita performance windows](performance/unbound-vita-windows.csv)
- [Independent frontend windows](performance/unbound-frontend-windows.csv)
- [Three sampling blocks](performance/unbound-batches.csv)
- [Capture provenance and screenshots](../store/screenshots/vita-1.2/README.md)

The public exports contain fixed-schema numeric metrics, not raw device logs,
account information, network addresses, ROMs, BIOS files or saves. The test
copy used a unique filename so it could not overwrite an existing save.
The original game's battery save and resume files were read back unchanged.
Settings and play history were restored; the FPS counter was left off as
requested.

## Earlier Evidence

The historical **1.0 Pokemon Ruby** result was about 60 emulated fps with
CPU xBRZ and wide view enabled. It is a different game and software version,
not a 1.2 Unbound result or a guarantee for every ROM hack.

Version 1.2 changes reduce repeated work in rewind, smoothing and texture
handling. Their host tests establish scoped behaviour, not a measured
percentage speed-up on a Vita. See [Source Record](SOURCE.md) for the
historical test results, reruns and outstanding acceptance gaps.
