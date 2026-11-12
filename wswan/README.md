# WonderSwan for VitAdhoc

Bandai WonderSwan, WonderSwan Color and Pocket Challenge V2 (`.ws`, `.wsc`, `.pc2`) as an in-process core, built the way
the NES core is: one relocatable object (`wswancore.o`) that exports only the `wswan_host_*` functions of
`frontend/host.h`. The main-app integration is in [INTEGRATION.md](INTEGRATION.md).

## Core decision: Beetle WonderSwan (Mednafen's WonderSwan module)

Chosen: **beetle-wswan-libretro** (Mednafen WonderSwan, Cygne lineage), GPL-2.0-or-later.

| Candidate | Verdict | Reasons |
|---|---|---|
| **Beetle WonderSwan / Mednafen** | **chosen** | Small plain C (about 9 source files + Blip_Buffer), no C++ runtime needed, licence compatible with the GPL main program, the code RetroArch has shipped for years (so a long tail of games was run against it), the save layout and RTC behaviour are the ones other front ends use (RetroArch `.srm` compatible), band-limited sound at any rate, a state format with sections. Whole core plus host is 137 KB of Vita code and 1.3 MB bss. Fast: 0.1-0.4 ms per frame on a desktop core. |
| Oswan | rejected | Older and, by its own reputation and the way later cores replaced it, less accurate and less maintained. Not built or measured here: this is a judgement from the code lineage, not a benchmark. |
| ares (WonderSwan core) | rejected | The most accurate of the set, but C++17 heavy, built around its own scheduler and framework (`nall`, `ares` node tree); it cannot be reduced to one small object with an `extern "C"` API without importing most of the framework, and it would cost several times the CPU on a Cortex-A9 that must run 75.47 frames a second. ISC licence would have been fine. |
| MAME `wswan` | rejected | Needs the MAME device framework; too heavy and too slow for the console. |

Known accuracy limit of the chosen core (measured, see `twin/baseline_results.md`): the V30MZ is not cycle exact (the
WSTimingTest ROM fails every line), and a few hardware-register tests of WSHWTest fail. Commercial games largely do not depend on
it; individual titles that do are untested.

## Layout

- `core/`: the vendored core (GPL), `PROVENANCE.md`, `sources.mk`.
- `frontend/`: `host.h/.cpp` (the API; no C++ runtime), `blend.[ch]` (exact temporal blend 75.47 Hz to 59.94 Hz),
  `layout.[ch]` (picture layout, rotation, Vita pad mapping), `safe_io.[h,cpp]` (atomic save writes, from the NES
  frontend), `Makefile`, `time_shim.h`.
- `twin/`: Linux test runner `wswan_twin` (headless: frames, scripted pad, hashes, PNG, wav, saves, states),
  `selftest.sh`, `tests/make_roms.py` (synthetic ROMs), `tests/blend_test.c`, `conformance.py`, `baseline_results.md`.

## Build and test (Linux)

```
make -C wswan/frontend && make -C wswan/frontend check          # also: vita, armhf (TARGET=vita check)
make -C wswan/twin
sh wswan/twin/selftest.sh                                        # 38 checks (+1 with a private ROM), optional WSWAN_PRIVATE_ROM=<your own ROM>
python3 -I wswan/twin/tests/make_roms.py OUTDIR                  # the 7 synthetic ROMs
WSWAN_TESTSRC=<clones> python3 -I wswan/twin/conformance.py      # optional, public test ROMs
```

## Tested and not tested

Tested here: the synthetic ROMs, two public test ROMs (screen results only), and ONE commercial game supplied by the owner
(Klonoa: Moonlight Museum, fan translation; used only as an input file, never copied into the repository): boot, title, scripted
input into stages, horizontal and vertical stages, orientation switching, saving, states, sound, control mapping.
**Every other commercial ROM is untested.** No run on a real Vita has happened.
Details and the list of everything untested are in INTEGRATION.md ("What is not verified").
