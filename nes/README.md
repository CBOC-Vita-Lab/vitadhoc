# NES for VitAdhoc

An NES / Famicom core for the main app: FCEUmm (libretro-fceumm, GPL-2.0-or-later) in `core/`, wrapped by a small
C host (`frontend/host.c`) and built as one relocatable object, `nescore.o`, that exports only the `nes_host_*`
functions of `frontend/host.h`. A Linux runner (`twin/`) tests the host without a console. How to link it into the main
app is in [INTEGRATION.md](INTEGRATION.md).

Numbers in this file are from the Linux build on a shared x86_64 machine. **Nothing here has run on a Vita.**

## Layout

| path | what |
|---|---|
| `core/` | FCEUmm `src/` without the libretro driver, commit and local edits in `core/PROVENANCE.md` (local edits all marked `NESP`: `fds.c`, `fds.h`, `ines.c`, plus the 12 Nov 2026 accuracy patches in `ops.h`, `x6502.c`, `ppu.c`, `ppu.h`, `fceu.c`, `boards/mmc3.c` and the UB fixes in `boards/mmc5.c`, `boards/asic_vrc2and4.c`), licence `core/LICENSE` (= `licenses/FCEUmm-GPL-2.0.txt`) |
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
cheats; sprite-limit removal (default on, with a per-game exception table keyed by ROM CRC: both Legend of Zelda
revisions built in, more via `nes_host_sprite_exception_add`); Zapper (touch) and Arkanoid paddle; a render-off flag for
fast forward. Not wired: Power Pad, NSF, the NTSC filter.

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

Self test (`twin/selftest.sh`): 82 checks, all pass (also with `NES_TESTROMS` set, which adds the public ROMs to the
sprite-limit identity check): deterministic hashes; save state / load state replay (NROM, MMC1, MMC3); battery RAM;
header variants; PAL, Dendy and region override; four players; sprite limit on/off gives identical RAM, saved state, audio
and SRAM, and the CRC exception table; Zapper and paddle (synthetic `gun.nes`, `paddle.nes`, header-selected, option off);
tone; mapper banking and IRQ; FDS; state size fixed; render off; load from memory equals load from file; cheats;
achievements map; palette rebuild; `nescore.o` exports only `nes_host_*` (57) and links beside clashing globals.

Conformance (`twin/baseline_results.md`, 290 public test ROMs): 75 pass (55 before the 12 Nov 2026 accuracy work), 32 fail,
6 need the reset button, 176 report on screen only, 1 is not a game. `twin/conformance_expect.txt` lists the 75;
`conformance.py --expect` fails on any regression. The remaining failures are cycle-exact PPU/APU/CPU-interrupt timing.
Against stock libretro-fceumm at the same commit (before our patches): no verdict difference and identical system RAM on
all 289 loadable ROMs. Against Nestopia: 105 of 107 protocol ROMs pass (2 more need reset); see baseline_results.md.

ARM: `twin/arm_check.sh` builds `nescore.o` for host and armhf and compares per-frame picture, audio, RAM and SRAM hashes
under qemu-arm; the twin itself was run for armhf over all 290 ROMs: bit-identical to x86_64. Also clean under
AddressSanitizer + UBSan over the whole public set (that found a missing `SFORMAT` terminator in the VRC2 board, fixed).

Speed: see INTEGRATION.md (about 2.7-3.4 M x86 instructions per frame, ARM about 1.04-1.13 times that).

## Untested

Everything on a console: the Vita build of `nescore.o` compiles and runs under qemu-arm (armhf), but never on Vita hardware
(cycle cost there is an estimate); qemu user mode does not trap unaligned accesses. Real commercial games were not tried (none are available here); the mapper coverage is therefore what the
synthetic ROMs and the public test ROMs reach. FDS was tested only with a stand-in BIOS and a synthetic disk. Four Score
was tested with a cartridge that reads the ports, not with a real game. The audio is checked for pitch and level, not by ear.
