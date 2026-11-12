# NES for VitAdhoc

An NES / Famicom core for the main app: FCEUmm (libretro-fceumm, GPL-2.0-or-later) in `core/`, wrapped by a small
C host (`frontend/host.c`) and built as one relocatable object, `nescore.o`, that exports only the `nes_host_*`
functions of `frontend/host.h`. A Linux runner (`twin/`) tests the host without a console. How to link it into the main
app is in [INTEGRATION.md](INTEGRATION.md).

Numbers in this file are from the Linux build on a shared x86_64 machine. **Nothing here has run on a Vita.**

## Layout

| path | what |
|---|---|
| `core/` | FCEUmm `src/` without the libretro driver, commit and local edits in `core/PROVENANCE.md` (three files edited, all marked `NESP`: `fds.c`, `fds.h`, `ines.c`), licence `core/LICENSE` (= `licenses/FCEUmm-GPL-2.0.txt`) |
| `frontend/host.h`, `host.c` | the API for in-process use (plain C, no file access, no C++ runtime). MIT |
| `frontend/Makefile` | `nescore.o` for host, `vita`, `armhf`; `make check` proves only `nes_host_*` is global |
| `frontend/host_files.*`, `safe_io.*` | optional file layer (load by path, safe `.srm` and state writes) used by the runner and the standalone program, not part of `nescore.o` |
| `frontend/picture.*` | picture geometry and settings file of the standalone program (tested, not used by the core API) |
| `twin/` | `nes_twin` runner, `selftest.sh`, `conformance.py`, synthetic ROM generator (`tests/make_roms.py` with a small 6502 assembler `tests/nesasm.py`), `baseline_results.md` |
| `vita/` | the earlier standalone `nes.bin` (own program with a pause menu). Left as it was when the plan changed to the in-process core; it builds and has never been run on a console |
| `INTEGRATION.md` | API contract, build lines, symbols, footprint, per-frame cost |

## What the host does

iNES, NES 2.0 (also with a trainer) and UNIF from a memory buffer; Famicom Disk System images when the caller hands in
`disksys.rom` (the BIOS is never bundled); region NTSC / PAL / Dendy from the header, tags and the core's database, with
an override; 256x240 RGB565 video with the NTSC overscan rows (8 top and bottom) reported; audio synthesised straight
at the requested rate (8000-96000 Hz, default 48000); four pads with the Four Score / Famicom adapter automatic for the
known games; battery RAM in RetroArch's raw `.srm` layout, with a dirty check, for the caller to save; fixed-size
save states for a rewind ring; the achievements memory map for console 7; Game Genie, raw and Pro Action Replay
cheats; sprite-limit removal (default on); a render-off flag for fast forward. Not wired: Zapper, paddles, Power Pad,
NSF, the NTSC filter.

## Build and test (Linux)

```
make -C nes/twin -j4                 # build/nes_twin
make -C nes/frontend -j4             # build/host/nescore.o   (also: vita, check)
nes/twin/selftest.sh                 # generates its own ROMs; prints PASS/FAIL lines
NES_TESTROMS=/path/to/nes-test-roms python3 -I nes/twin/conformance.py --md out.md
```

The runner: `nes_twin ROM --frames N --pad SCRIPT [--pad2..4] --hashes F --save-state F --load-state F --state-frame N
--wav F --srm F --opt NAME=V --cheat CODE --render-off A-B --state-sizes --mem-load --ra ADDR,LEN --verify-indexed ...`
(`--help` lists all). Test ROMs: the self test needs none (it generates them: pattern, pad-driven palette, square tone,
battery RAM at `$6000`, MMC1 and MMC3 with bank switching and a scanline IRQ, header variants, UNIF, FDS with a stand-in BIOS
that is not Nintendo's). The conformance script runs the public blargg-style ROMs of
<https://github.com/christopherpow/nes-test-roms> (cloned outside the repository, never vendored) and reads their
result at `$6000`.

## Results

Self test (`twin/selftest.sh`): 66 checks, all pass: deterministic hashes; save state / load state replay
(NROM, MMC1, MMC3); battery RAM load, save, `.bak`, no `.tmp` or `.old`, longer and shorter save files, a failed write keeps
the original and reports, `.old` recovery; header variants give identical pictures; PAL, Dendy and region override; four
players with signatures; sprite limit; tone; mapper banking and IRQ; FDS (missing BIOS, header, eject/insert); state size
fixed; render off; load from memory equals load from file; cheats; achievements map; palette rebuild; `nescore.o` exports
only `nes_host_*` and links beside clashing globals.

Conformance (`twin/baseline_results.md`, 290 public test ROMs): 55 pass, 52 fail, 6 need the reset button, 176 report on screen
only, 1 is not a game. The failures are listed there, grouped; they are cycle-exact PPU/MMC3/APU timing tests and unofficial
opcodes, while both `official_only` instruction suites pass. I did not run the same ROMs in stock libretro-fceumm to compare.
The run found a real bug: a NES 2.0 header with 1 KiB of CHR-RAM let the PPU write past the buffer (heap corruption); fixed in
`core/ines.c` (marked `NESP`, in `core/PROVENANCE.md`).

Speed: see the per-frame table in INTEGRATION.md.

## Untested

Everything on a console: the Vita build of `nescore.o` compiles and combines with a test object, but was never linked into
the app or run. Real commercial games were not tried (none are available here); the mapper coverage is therefore what the
synthetic ROMs and the public test ROMs reach. FDS was tested only with a stand-in BIOS and a synthetic disk. Four Score
was tested with a cartridge that reads the ports, not with a real game. The audio is checked for pitch and level, not by ear.
