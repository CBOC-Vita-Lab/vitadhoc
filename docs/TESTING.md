# Testing

One command runs every test in this repository:

```sh
python3 tools/gate/gate.py            # default tier
python3 tools/gate/gate.py --quick    # fast subset: unit tests, twin selftests, playerkit, widekit, save tests
python3 tools/gate/gate.py --full     # everything, incl. armhf under qemu and the Vita package build
python3 tools/gate/gate.py --changed  # only what the working tree changes touch (against HEAD)
python3 tools/gate/gate.py --list     # every step, its tier and whether it can run on this machine
```

It prints one row per step (result, checks, failed, seconds, log) and writes
`tools/gate/out/summary.md`, `summary.json` and one log per step under
`tools/gate/out/logs/`. The exit status is non-zero if a step failed. A step
that is skipped because a prerequisite or a private ROM folder is missing is
reported as SKIP and does not fail the run. See `tools/gate/README.md` for
caching, `--only`, `--skip`, `--baseline` and how `--changed` maps paths.

A PASS is cached. A repeated run skips every step whose inputs and prerequisites
are unchanged, so after a small change only the affected steps run.

## Prerequisites

- Python 3 and GNU make. gcc and g++ for the host and C tests. Pillow for the
  app tests (`app_tests.py`, `rewind_tests.py`, `local_tests.py`, `link_tests.py`,
  and the SNES conformance script).
- RGBDS (`rgbasm`, `rgblink`, `rgbfix`) for the GB cartridges. The gate looks in
  `/opt/rgbds/bin` or in `$GATE_RGBDS` (or `$RGBDS`).
- `arm-none-eabi-gcc` for the bare-metal GBA cartridges.
- `arm-linux-gnueabihf-gcc` and `qemu-arm` for armhf (`--full`).
- VitaSDK at `$VITASDK` (default `/opt/vitasdk`) for the Vita package and the
  Vita compile checks. The audio regressions use its headers.

A missing tool makes its steps SKIP with the name of the tool.

## Private ROMs

Some checks need ROMs that are not in the repository. Point these variables at
the folders (or, for WonderSwan, at one file):

| variable | runs |
|---|---|
| `GATE_GBA_ROMS` | every GBA, GB and GBC ROM in the folder, x86 against armhf (`--full`) |
| `GATE_SNES_ROMS` | SNES thread comparison and reference-screenshot conformance; also the SNES selftest's ROM sections |
| `GATE_NES_ROMS` | NES blargg-style conformance against `nes/twin/conformance_expect.txt` |
| `GATE_WS_ROM` | the WonderSwan acceptance section |

## What each step covers

**Release** (`--quick`): `release-unit` (the download checker, `tests/`),
`release-package` (the VPK built here by `make vpk`, gpsp/vita/VitAdhoc.vpk:
no BIOS or games, the notices match; skipped when nothing is built).
At release time only (`--release`, which is `--full` plus these):
`release-package-pinned` (the committed root VitAdhoc.vpk with the pinned
eboot.bin / snes.bin hashes) and `check-files` (every file in `SHA256SUMS`
matches). Both are stale by design between releases until
`tools/pin-release.py` refreshes them.

**Save and local link** (`--quick`): `save-sync` (RetroArch edits, short
sources, backups), `save-backup-failure` (a backup that cannot be made leaves
the save untouched), `local-save-fail` (a second game's save is never lost),
`play-log` (the recent-games ring).

**Libraries** (`--quick`): `playerkit` (unit tests and menu walk under ASan and
UBSan), `widekit` (tests under ASan and UBSan), `gamedb` (title database).

**Twin selftests** (`--quick`): `snes-selftest`, `nes-selftest`,
`wswan-selftest`: determinism, save states, battery RAM, copier headers,
pad mapping, audio pacing, and each core's own picture and blend tests. Each
needs its twin, built by a shared prerequisite step.

**Host app** (default): `unit-host` (about 40 C suites from
`gpsp/tools/run_host_tests.py`, under sanitizers), `app-x86` (the whole app:
game list, menus, states, fast forward, screenshots), `rewind-x86` and
`rewind-unit-x86` (rewind in the app and its unit tests), `local-x86` (two
players on one console), `link-x86` (two app twins over loopback), `library-x86`
(the library screen), `audio-regressions`, `artfetch` (cover fetch), `upscale`
(xBRZ and the Vita path), `cheats-unit-x86` (cheat engine and cheat list).
`wswan-private`, `snes-mt-compare`, `snes-conformance` and `nes-conformance`
run when their ROM variable is set.

**Full** (`--full`): the same app steps on armhf under qemu (`app-armhf`,
`rewind-armhf`, `local-armhf`, `link-armhf`, `rewind-unit-armhf`,
`cheats-unit-armhf`), `rewind-x86-long` (20 minutes of play a console),
`cores-own-rom` (the core-level test ROM, x86 against armhf, and the link bench),
`cheats-e2e-x86` and `cheats-e2e-armhf`, `nes-arm-check` (NES host and armhf
byte-identical), `widekit-arm`, `widekit-vita` and `playerkit-vita` (compile for
the Vita), and `vita-build` (`make -C gpsp/vita vpk`).

## Not wired

These exist in the tree and are not run by the gate:

- `gpsp/vita/hostcore/app_tests.py armhf-vitasim` and `x86-san`: the Vita's own
  translation-cache path and the app under ASan (they need separate builds).
- `wswan/twin/conformance.py` (needs nasm and clones of public test suites in `WSWAN_TESTSRC`).
- `snes/twin/exact_check.sh` and `snes/twin/baseline.sh`.
- `gpsp/tools/run_clock_reliability.py`, `run_autopilot_script_tests.py`,
  `run_gb_tests.py` and the other `gpsp/tools/run_*` runners outside
  `run_host_tests.py`.
- The rewind campaign and survey scripts in `gpsp/tools/rewind_*.sh`.
- Anything that needs a physical PS Vita. A build and an emulator run do not
  replace a test on the console.
