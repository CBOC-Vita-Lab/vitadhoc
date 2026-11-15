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

## Unattended device sessions (vita-matrix)

`tools/vita-matrix/matrix.py` runs a playlist of timed measurements (game, seconds,
`vita.ini` / `threads.ini` overrides) on a real Vita through vitacompanion, or on
Vita3K for a rehearsal, pulls `gbadhoc.log` and `snes/log.txt` back per entry and
prints a table (missed refreshes per second, emulated fps, hand-over time-outs,
look level, worst JIT translate/sync, SNES fps and audio, faults). The owner's own
`vita.ini`, `threads.ini`, `autoboot.txt` and logs are backed up first and restored
at the end, on Ctrl-C and on any failure. Its unit tests are
`python3 tools/vita-matrix/test_matrix.py`; the owner session is
`tools/vita-matrix/session-1.4.txt`; see `tools/vita-matrix/README.md`.

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

## Online play (2.0)

Nothing here leaves the loopback: no test contacts Cloudflare or any
`psvita.dev` or `workers.dev` address, and nothing deploys. The live service
and two real Vitas are the only proof of the rest.

| Suite | Run from the repository root | Covers |
|---|---|---|
| Room service | `cd online/service && node --test "test/*.test.mjs"` (Node 22.5 or newer; no `npm install`; about 30 s; 110 tests) | the eight routes, lifecycle and lazy expiry, eviction of the Durable Object, rate limits and the day's budget, relay credential guards, privacy (nothing logged, nothing outlives its room), the dev server, and the deploy and zone scripts against a stub `npx`, a stub `curl` and a mock of the zone API (no wrangler or Cloudflare needed). The quotes matter: Node 22 does not expand a folder. |
| TURN key script | `bash online/service/scripts/create-turn-key.selftest.sh` | the one-time key script against a stub `curl`: file modes, the secret never printed, never two keys, billing and permission refusals |
| NAT layer | `make -C gpsp/netdrv test-nat` | STUN, punch, keep-alive and the TURN client on the transport, against the NAT/TURN simulator in `gpsp/netdrv/tests/natsim/` (build it with `gpsp/netdrv/tests/natsim/build.sh <out>`; `<out> --port 47000 --turn` runs it by hand) |
| Room client and HTTPS client | `python3 -I gpsp/vita/tests/onlinehttp/test_onlinehttp.py` | JSON, flows, errors, time limits, the room client against `fake_service.py` and the real `online/service/dev/server.mjs`, TLS 1.2 and 1.3 with ECDSA and RSA leaves, wrong host, untrusted chain, the clock floor, the client's self-test. The TLS cases are SKIPPED without the host Mbed TLS prefix (`MBEDTLS_HOST`, default `/tls/mbedtls/host64`: see [Building](BUILDING.md)). |
| Online connection | `python3 -I gpsp/vita/tests/onlineconn/test_onlineconn.py` (about 10 s; `-v` shows the lines) | the stage machine on a fake clock against a fake room service and the NAT layer: punch, relay fallback, cadence, failures |
| Library match | `make -C gpsp/vita/tests/library -f online_lib.mk test` | the card-to-file step: folder walks, duplicate codes, the pure decision |
| Menus | the host UI test (`gpsp/vita/host/ui_host_test.c`) | code pad, room card, host page, sentences, `link_direct` |

The twin scenarios that play whole online sessions (two real menus, a fake
service, the NAT world: cable, Game Boy, relay, wrong code, different builds,
cancel and retry, expiry, a busy day, a lost connection, loss and delay at
100, 150 and 200 ms round trips, determinism, share) are `online_tests.py` in
`gpsp/vita/hostcore/`, run as `python3 online_tests.py x86 --hz 20`; the
gate steps for all of the above are listed by `python3 tools/gate/gate.py
--list`. Needs `node` and `g++`; a missing tool makes the step SKIP with its
name. Hold the build lock on a shared machine.

The Vita build with Mbed TLS and the heap and package-size numbers are
checked by the build (`vita-build` in the full tier), not by the twin.
