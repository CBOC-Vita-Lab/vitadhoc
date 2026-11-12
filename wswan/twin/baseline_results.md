# WonderSwan conformance baseline

12 Nov 2026. Beetle WonderSwan commit 4b01295838ea89e3f1355bbe4cb5cf98aa6108cd, host glue as in this tree, x86_64 Linux twin
(`conformance.py`; the clones and the ROM builds live outside the repository, nothing is vendored).

Public test ROMs available without the Wonderful toolchain (built with `nasm`, which is installed):

| ROM (source, commit) | how it reports |
|---|---|
| WSTimingTest `timingtest.ws` (https://github.com/FluBBaOfWard/WSTimingTest, 025e606f, MIT-style source) | a table: Correct / Actual cycles per instruction, Pass mark |
| WSHWTest `WSHWTest.wsc` (https://github.com/FluBBaOfWard/WSHWTest, 34e33d5b) | on-screen menu; "Test All" (scripted: down, A) |

Both only show results on the screen; they were run 1500 frames and the PNGs read by eye.

## Results (read from the screenshots)

- **WSTimingTest: every visible line fails** (all 18 rows on the first page: Base loop, Unaligned, LOOP, NOP, NOP 2X, INC BL/BX, CLI, IN AL,
  IN AL DX, ADD/SUB SP, PUSH/POP SP, AX<-MEM ...): e.g. Base loop correct 54, actual 75; NOP correct 58, actual 86. The core
  is not cycle exact for the V30MZ (Mednafen counts clocks per instruction class, not the real ones). No game was seen to care,
  but cycle-sensitive titles are not guaranteed.
- **WSHWTest "Test All"**: the Interrupt Manager and the Timers sections ran: "Timers dont run when zero", "dont run when off", "run when
  on+repeat", "run when on+one shot", "dont run off+repeat", "continue by on/off" all **Ok**; "Timers always fire when c 1" **Failed**; "IO Registers"
  failed at register 0x00 (value C0). The later sections (noise values, sound sweep, windows, mixer) were not reached in this run (the test
  stopped after the IO-register failure, or its text scrolled off; not investigated).
- Not run: asiekierka/ws-test-suite (https://github.com/asiekierka/ws-test-suite, 7dfa0e2e, MIT; cloned, source only, needs the Wonderful
  toolchain to build ROMs, which is not installed), MiSTer WonderSwan testroms (the API was not reachable; not cloned), rtctest.

These failures are a statement about this build, not a diff: the same ROMs were not run in stock RetroArch here.

## Commercial ROMs

Untested except the one supplied by the owner (Klonoa: Moonlight Museum, fan translation; private test data used as an input file only,
never copied into the repository). Its results (boot, orientation, saves, states, sound, mapping) are in the final report and INTEGRATION.md.
