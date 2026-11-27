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

## Public ROM sweep (600 frames, no input, x86 -O2 twin, final frame read by eye)

Clones are in the scratchpad (`ws-public/`), never in the repository. Save type is "none" for every ROM here (the info line reports
no SRAM or EEPROM).

| ROM | source | mono/colour | orientation flag | save | result on screen |
|---|---|---|---|---|---|
| WSmono-screentester | dekadencedemo/wonderswan-tools, screen-tester | mono | horizontal | none | greyscale bar chart, 16 shades, sharp edges |
| test.orig / test.python | dekadencedemo/wonderswan-tools, extra | mono | horizontal | none | rings animation with scroller text; identical frame hashes |
| WSTimingTest timingtest | FluBBaOfWard/WSTimingTest | mono | horizontal | none | cycle table: every Pass mark is a fail (see below) |
| mister timingtest | MiSTer testroms | mono | horizontal | none | same table layout: every Pass mark is a fail (Actual about +20 cycles) |
| mister windowtest | MiSTer testroms | mono | horizontal | none | window layout of B tiles with a header line; drawn |
| mister spritepriority | MiSTer testroms | mono | horizontal | none | text and sprite-priority test pattern; drawn (see the port 0xA0 finding) |
| WSHWTest.wsc | FluBBaOfWard/WSHWTest | colour | horizontal | none | main menu (Test All not run: no input) |
| WSCpuTest.wsc | FluBBaOfWard/WSCpuTest | colour | horizontal | none | main menu (no input) |

Not run and not cloned: the asiekierka ws-test-suite (needs the Wonderful toolchain to build), the OpenWitch and
WonderWitch samples, and the Shaw02, joffb and other homebrew repositories (no ROM file found in their clones).

### Port 0xA0 finding (fixed)

The MiSTer spritepriority and windowtest ROMs were flat grey in earlier runs. The cause is the console-type bit on port 0xA0.
The test checks it with `WS_COLOR` = 0x02 and locks the CPU when the bit is set. Two things were wrong:

1. The host set the console type to Colour for every game (`wsc = 1`), so a mono cartridge saw Colour hardware.
   Now the console type follows the cartridge's colour byte at load (`wsc = footer[7] != 0`, host.cpp).
2. The core returned 0x86 for mono, which has bit 1 set. Port 0xA0 is documented as `C??? swcl` with the Colour flag at bit 1
   (ws.nesdev.org, I/O port map). Mono now returns 0x85 (core change in `core/mednafen/wswan/gfx.c`, recorded in PROVENANCE.md).
   The Colour value 0x87 is unchanged.

After both changes the spritepriority and windowtest ROMs draw. Selftest passes 39/39 with the private game, and the mono and
colour synthetic ROMs are unchanged. This was checked only against the documented bit layout and the test's own expectation; the
real value a mono console returns on port 0xA0 is not verified on hardware.

### Timing tests (not fixed)

Both timing ROMs show every row as failed, with the actual cycle count above the correct one (base loop 54 correct, 75 actual). The
earlier baseline reached the same result. The core's per-instruction timing differs from the reference on this test. Not investigated further.

### Earlier per-frame logs

WSHWTest per-frame hashes differ from a log made in an earlier run from frame 101 onward. The earlier log's options were not recorded.
The new run had no input script. The colour ROMs' outputs were not changed by either fix; the difference is most likely the input.

## Commercial ROMs

Untested except the one supplied by the owner (Klonoa: Moonlight Museum, fan translation; private test data used as an input file only,
never copied into the repository). Its results (boot, orientation, saves, states, sound, mapping) are in the final report and INTEGRATION.md.
