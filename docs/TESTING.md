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
- `aarch64-linux-gnu-gcc` and `qemu-aarch64` (Debian / Ubuntu: `qemu-user`) for the aarch64 twin
  (see "The aarch64 twin" below; not a gate step).
- VitaSDK at `$VITASDK` (default `/opt/vitasdk`) for the Vita package and the
  Vita compile checks. The audio regressions use its headers.
- Node 22.5 or newer and npm for the online room service (`online-service` and the
  twins' fake service; the twins need `node` only, `npm ci` is the service step's).
- Mbed TLS, built once by `python3 tools/online/build-mbedtls.py --target host64|vita --work DIR`
  (see [Building](BUILDING.md)): `GATE_MBEDTLS_HOST=DIR/mbedtls/host64` lets `online-http`
  run its TLS cases (without it they are SKIPPED, loudly) and `GATE_MBEDTLS_VITA=DIR/mbedtls/vita`
  is passed as `MBEDTLS=` to the `make vpk` of `vita-build` (without it that step SKIPs: a
  package without TLS cannot be built).

A missing tool makes its steps SKIP with the name of the tool.

## Private ROMs

Some checks need ROMs that are not in the repository. Point these variables at
the folders (or, for WonderSwan, at one file):

| variable | runs |
|---|---|
| `GATE_GBA_ROMS` | every GBA, GB and GBC ROM in the folder, x86 against armhf (`--full`); and `online-x86`'s determinism scenario, which then also plays the first ROM the link table marks Link Cable at a 150 ms round trip (the cache key does not include the files: `--force` after changing them) |
| `GATE_SNES_ROMS` | SNES thread comparison and reference-screenshot conformance; also the SNES selftest's ROM sections |
| `GATE_NES_ROMS` | NES blargg-style conformance against `nes/twin/conformance_expect.txt`; Same Wi-Fi and Online Two players on a NES game (`link_tests.py nes_two`, `online_tests.py nes_punch / nes_relay`, and the NES refusals): they play `nes15-NTSC.nes` found under the folder (a `git clone` of the public nes-test-roms collection), or the file `VITADHOC_NES_ROM` names |
| `GATE_WS_ROM` | the WonderSwan acceptance section (`wswan-private`): a real game that makes sound; also the fallback for `GATE_WS_ONLINE_ROM` |
| `GATE_WS_ONLINE_ROM` | Same Wi-Fi and Online Two players on a WonderSwan game (`ws_two`, `ws_punch`, `ws_relay`, the WonderSwan refusals): any homebrew test ROM will do (a prebuilt one is in the testroms folder of the public WonderSwan_MiSTer repository); falls back to `GATE_WS_ROM` |
| `GATE_SG_ROM` (file), or a `.sg` file in the `GATE_MD_SMS_ROMS` folder | the SG-1000 game of `link_tests.py sega_two`, `online_tests.py sega_online / game_sg / version_sg` and `md/twin/link_test.py` (a homebrew built with SDCC and the public devkitSMS SGlib); without one they use a synthetic image |
| `GATE_SG_ROM` and `GATE_GG_ROM` (files), or a `.sg` / `.gg` file in the `GATE_MD_SMS_ROMS` folder | the SG-1000 and Game Gear files of the Sega player's own tests (`md-player`: walk, pads, look governor, hand-over, rewind and fast forward, Quick response, RetroAchievements). Without a file the player tests make their own small cartridges (`md/twin/tools/sg_gen.py`: an SG-1000 `.sg`, and a Game Gear `.gg` with the Game Gear header, so the extension and not a forced console id picks the console); a public homebrew replaces them. No public prebuilt `.sg` or `.gg` could be found by `git clone` of the public repositories tried (devkitSMS and PSGlib hold sources only, and building them needs SDCC); a homebrew built from them works |

Before and after a change to the GBA core, a private set of cartridges can be compared between two host builds (a scripted pad,
the same N frames, a hash of every audio sample and of screenshots taken at fixed frames). The direct (skip-BIOS) boot starts on
scanline 126 (cpu.cc), which moves every game's timeline by half a frame and so changes every audio hash; to see only the other
changes, build the host twin with `make -C gpsp/vita/hostcore ARCH=x86 B=build/x86-nb CORE_PLAT_DEFS="-DMMAP_JIT_CACHE -DGPSP_BOOT_LINE0"`
(`GPSP_BOOT_LINE0` starts on line 0 as before the change). On six private cartridges that build matches the 2.0 release's audio and
pictures bit for bit over 3000 frames.

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

Search memory (the cheat search) is tested in three places: the engine and its
menu pages in `playerkit/tests/test_csearch.cpp` (steps, widths, byte orders, the
code each console takes, the 50-result list, refusal in Hardcore and link); the
Super NES and Sega players' twin selftests (`snes/twin/player_twin.cpp`,
`md/twin/player_twin.cpp`, run by `snes-selftest` and the Sega player selftest)
walk the real menu: new search, a typed number, Increased, the result made a
cheat; and the app scenario `cheat_search` (`app_tests.py --only cheat_search`)
does the same on the Cheats page of the app's GBA cartridge and checks the
`cheat_search` log lines and the `.cht`.

**Twin selftests** (`--quick`): `snes-selftest`, `nes-selftest`,
`wswan-selftest`: determinism, save states, battery RAM, copier headers,
pad mapping, audio pacing, and each core's own picture and blend tests. Each
needs its twin, built by a shared prerequisite step.

`md-ahead` (default tier, needs `GATE_MD_ROMS`; `md/twin/ahead_check.sh`) is the Sega proof of frames nobody sees: undrawn frames leave the
machine as drawn ones do; Quick response (run-ahead) changes nothing in the machine, with and without the VDP worker and the wide view, shows the
next frame's picture, and answers a press one frame sooner (a small test cartridge, `md/twin/tools/sms_latency_gen.py`, answers the pad a frame
later: 1 frame plain, 0 with run-ahead); and a state loaded at frame N then run M frames equals the run that was never interrupted (picture, sound,
state, RAM, battery RAM of every frame; same machine, a reloaded ROM, a fresh machine; worker on and off).

Game Boy Camera and Game Boy Printer: `gpsp/gbcore/tests/gbcam.c` (a hand-assembled cartridge drives the camera registers; pixels, capture time,
banking, state, the frontend's crop and mirror, checked against answers worked out by hand) and `gbprinter.c` (packets, compression, checksum errors,
multi-band strips, the strip pause, and the same stream through the core's serial port) run in `gpsp/gbcore/tests/run_accuracy.py` (host, sanitizers);
`app_tests.py --only gbcam,gbprinter` runs the whole app on small cartridges (camera feed in the three modes, the saved photo, the PNG in `prints/`
decoded and compared pixel by pixel, the setting Off). The twin's script has `camera=ramp|step|dark|bright` for the camera.

`app-ahead` and `app-ahead-latency` (default tier; `gpsp/vita/hostcore/ahead_check.sh`, `ahead_latency.sh`) are the same proof for the app's own Quick
response on Game Boy, Game Boy Color, NES and WonderSwan: per ROM the app twin runs with it off and with it forced on (`GBADHOC_AHEAD=0` / `2`, every
frame at normal speed), and the sound, the quick save and the battery file must be identical (the cartridge clocks are frozen so two runs start
alike; with `GBADHOC_AHEAD_SKIP=N` both also run the undrawn-frame pattern). `GBADHOC_AHEAD_DIFF=1` is the survey that found what the states
miss: it names the program's static variables a look-ahead frame leaves changed. The latency script builds one small cartridge per console
(`tools/ahead_latency_gen.py`: RGBDS, an own 6502 assembler, nasm) that answers the pad one frame after it reads it, presses a button and counts the
refreshes until the picture changes: it must be one less with Quick response. `ahead_cost.sh` measures the cost in host instructions (callgrind).
Game Boy Advance has no such proof because it has no Quick response; `GBADHOC_AHEAD_GBA_PROBE=1` (twin only) makes each frame also do what a
run-ahead frame would, to measure it.

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

## State loaders (`tools/statefuzz`)

A state comes from a file, from Share game data, or from the host of an online game, so every core's loader must survive a state made up on
purpose. `tools/statefuzz/sf_common.h` is the shared harness (MIT); each core has a harness next to its twin:
`md/twin/statefuzz.cpp`, `snes/twin/statefuzz.cpp`, `nes/twin/statefuzz.cpp`, `wswan/twin/statefuzz.cpp`, `gpsp/gbcore/tests/statefuzz.cpp` (Game Boy
and Color) and `gpsp/vita/hostcore/statefuzz/statefuzz_gba.cpp` (GBA). A harness runs a game, takes a genuine state, damages it the way a hostile
peer would (random bytes, field-sized values, wrong sizes; counts and checksums are repaired so the loader's own checks see a valid-looking
state), loads it and runs a few frames. A refused state is fine; a crash, a sanitizer report or a frame that does not end (a watchdog) is saved to
the case directory (`crash-*.st`, `hang-*.st`) and fails the run. Cores and harness are built with AddressSanitizer + UBSan (shift, signed
overflow and alignment reports are off: they are defined on the Vita's ARM and common in these cores). The GBA harness uses the interpreter build,
because a memory checker cannot watch generated code: the dynamic recompiler's own state fuzzing is not covered.

`--proof` is the other half: for every ROM, a state loaded at frame 1, 97, 300 and 655 and run 60 more frames must end byte for byte where the
machine that was never interrupted ends, so no check can have changed a legitimate value. Known exceptions, all upstream bookkeeping that is not in
the state and is blanked or documented: the NES harness blanks MMC1's last-write time (`LRST`) and the Disk System sound's envelope divider (`EDIV`);
the GBA's proof starts at frame 97 (a state of the very first frame runs on differently, also on the core as it was before any of these
changes); the Sega proof carries battery RAM beside the state, as a game's own save would be, because a load never takes a save back.

`sh tools/statefuzz/statefuzz.sh OUTDIR [SECONDS]` builds the harnesses and does both for the ROMs the gate variables name (`GATE_MD_ROMS`,
`GATE_MD_SMS_ROMS`, `GATE_SNES_ROMS`, `GATE_NES_ROMS`, `GATE_WS_ROM`, `GATE_GBA_ROMS`, and the test cartridges under `gpsp/vita/hostcore/testrom`
for the Game Boy); the gate step `state-fuzz` runs it with a short budget. Longer runs: `STATEFUZZ_SECONDS=300 STATEFUZZ_SEED=7`. Replay a saved case with
`<harness> ROM DIR --case FILE`. A case is a whole game state, so it is not committed; `tools/statefuzz/regress/README.md` describes how to keep one as
a small recipe (changed bytes on the harness' own base state, `--base-out`, `recipe.py`) and lists the classes of damage found.

## Undrawn frames (fast forward, Steady half mode, catch-up)

A frame the player does not draw must leave the emulation exactly as a drawn
one does (state, RAM, audio, battery file). Three scripts run a console twice on
the same input, once drawing every frame and once drawing only every Nth, and
compare the audio, the saved state, the RAM and the battery file:

- `snes/twin/skip_check.sh` (`snes_twin --render-mod N`, N = 2, 3);
- `nes/twin/skip_check.sh` (`nes_twin --render-mod N`);
- `gpsp/vita/hostcore/skip_check.sh` (the app twin with `GBADHOC_SKIP_MOD=N`:
  GBA, GB, GBC and WonderSwan through the same frame-skip path as the app).

The gate runs the SNES and NES ones as `snes-undrawn` and `nes-undrawn` (needs
`GATE_SNES_ROMS` / `GATE_NES_ROMS`). Result (host x86): SNES, NES, GBA, GB and
GBC are identical drawn and undrawn; no core needed a fix. One SNES state byte
(a render-cache flag that is recomputed, not emulation state) is excluded in the
script. The WonderSwan state differs only in its wall-clock clock bytes and
footer, which differ the same way between two drawn runs; its audio matches.

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

Vita3K notes for scripted runs: closing the emulator while the app is open counts as a crash to the boot guard
(`ux0:data/GBAdhoc/guard`), and after two of those the next start shows the safe look (Image quality Original, Wide view off, in
Settings > This game) and a game-start crash notice; clear the `guard` folder before each start. Vita3K's
`sceCtrlPeekBufferPositiveExt2` fills the buttons but leaves the sticks at 128, so the players read the sticks from the plain call
when Ext2 shows both centred (the right stick drives fast forward and rewind).

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
| Room service | `cd online/service && node --test "test/*.test.mjs"` (Node 22.5 or newer; no `npm install`; about 30 s; 124 tests when last counted) | the eight routes, lifecycle and lazy expiry, eviction of the Durable Object, rate limits and the day's budget, relay credential guards, privacy (nothing logged, nothing outlives its room), the dev server, and the deploy and zone scripts against a stub `npx`, a stub `curl` and a mock of the zone API (no wrangler or Cloudflare needed). The quotes matter: Node 22 does not expand a folder. |
| TURN key script | `bash online/service/scripts/create-turn-key.selftest.sh` | the one-time key script against a stub `curl`: file modes, the secret never printed, never two keys, billing and permission refusals |
| NAT layer | `make -C gpsp/netdrv test-nat` | STUN, punch, keep-alive and the TURN client on the transport, against the NAT/TURN simulator in `gpsp/netdrv/tests/natsim/` (build it with `gpsp/netdrv/tests/natsim/build.sh <out>`; `<out> --port 47000 --turn` runs it by hand) |
| NAT codecs, hostile input | `make -C gpsp/netdrv fuzz-nat` | STUN and TURN answers that are damaged, cut, over-long or signed by a lying server (400,000 datagrams under AddressSanitizer and UBSan, no sockets, a few seconds; `FUZZ_N=`, `FUZZ_SEED=`). Not a gate step: run it after a change to `nat_stun.c` or `nat_turn.c`. |
| Transport peer lock | `make -C gpsp/netdrv SAN=address test-lan LAN_TESTS=peerlock` | An online transport takes session datagrams only from its peer: strangers cannot push the peer out of the liveness table, reach netdrv or the bulk lane; a held transport (peer not known yet) takes none. Part of `test-lan`. |
| Room client and HTTPS client | `python3 -I gpsp/vita/tests/onlinehttp/test_onlinehttp.py` | JSON, flows, errors, time limits, the room client against `fake_service.py` and the real `online/service/dev/server.mjs`, TLS 1.2 and 1.3 with ECDSA and RSA leaves, wrong host, untrusted chain, the clock floor, the client's self-test. The TLS cases are SKIPPED without the host Mbed TLS prefix (`MBEDTLS_HOST`, default `/tls/mbedtls/host64`: see [Building](BUILDING.md)). |
| Online connection | `python3 -I gpsp/vita/tests/onlineconn/test_onlineconn.py` (about 10 s; `-v` shows the lines) | the stage machine on a fake clock against a fake room service and the NAT layer: punch, relay fallback, cadence, failures |
| Library match | `make -C gpsp/vita/tests/library -f online_lib.mk test` | the card-to-file step: folder walks, duplicate codes, the pure decision |
| Menus | the host UI test (`gpsp/vita/host/ui_host_test.c`) | code pad, room card, host page, sentences, `link_direct` |

Gate steps, one per row above and all in the default tier unless said:
`online-service` (`npm ci` then `npm test`), `online-nat`, `online-http`
(its TLS cases run only with `GATE_MBEDTLS_HOST` set to the host64 Mbed TLS
prefix, and are reported as SKIPPED without it), `online-conn`,
`online-library`, `fe-padpair` (two players on one machine under the real
session, the adapter behind Two players for NES and WonderSwan:
`gpsp/tools/tests/test_padpair.c`) and, on the twins, `online-x86` (default
tier) and `online-armhf` (full tier). `online-x86`'s determinism scenario also
plays a private Link Cable game when `GATE_GBA_ROMS` has one. The full tier's
`vita-build` needs `GATE_MBEDTLS_VITA`, the Vita Mbed TLS prefix, and SKIPs
without it. A missing tool (node, npm, g++, qemu) makes its step SKIP with the
tool's name.

### Twin scenarios: `online_tests.py`

Two twins of the whole app, each behind its own simulated home router
(`gpsp/netdrv/tests/natsim`, with a TURN server), talking to the real service
code of `online/service` under node (`dev/server.mjs`) on the loopback. Every
step goes through the real menus: the host's twin logs `EVT online_room
code=XXXXX`, the test types the code on the joiner's code pad with the d-pad.
A small HTTP proxy (`HttpProxy`) stands between a twin and the service where a
scenario needs a lost answer, an answer made up, or a room card that shows
another version (the twins' own version is fixed).

    cd gpsp/vita/hostcore
    python3 online_tests.py x86 --hz 20            # all of them (about two hours at 20; --hz 30 is faster)
    python3 online_tests.py x86 --hz 30 --only punch,relay,gb,share
    python3 online_tests.py armhf --hz 12          # the cheap set: punch, relay, gb, lost_answer, library_join
    python3 online_tests.py --list                 # every scenario name

| scenario | what it proves |
|---|---|
| `punch`, `relay`, `relay_joiner` | GBA Link Cable over the punch (port-restricted NATs), and over the relay with the host's, or the joiner's, NAT symmetric: the linktest cartridge's verdict on both, equal machines, the delay at least 3, at most 60 requests to the service, no public address of the other console in either log (relay) |
| `relay_host_setting`, `relay_joiner_setting` | "Always use the relay" on the host, or on the joiner, with open NATs: the room goes relay |
| `punch_opens_host` | the room is 200 s old (poll every 10 s): the joiner's PUNCH moves the host on before its poll names the guest |
| `nes_punch`, `nes_relay`, `ws_punch`, `ws_relay` | Two players on a NES / WonderSwan game over the punch and over the relay: both consoles run one machine (fe_padpair), the same last frame and machine hash, an agreed end, the radio given back. Need `GATE_NES_ROMS` / `GATE_WS_ROM` (skipped, with a note, without) |
| `snes_online`, `sega_online` | Two players Online from the game list (Settings > Two players) for SNES, Genesis, Master System and SG-1000, over the punch and over the relay: the room names the console, the link forms and is written to `launch/<console>.ini` for snes.bin / md.bin (the punch key, the compat word and the delay agree; relay: each file names its own allocation and the other's, none released). Every key press that matters waits for the app's log line that shows it was seen (see below) |
| `sega_library_join` | the same Sega rooms joined with the game list's Settings > Join online (code pad, card, library match, launch file as a guest) |
| `game_<console>` for nes, ws, snes, md, sms, sg | the joiner has no copy of the host's game: the card reads `UI_FAIL_ONLINE_GAME` ("Your friend is playing a different game. Add X to join."), the library was searched and nothing opened, no join request, no link |
| `game_gba`, `game_gb`, `game_gbc` | the Link Cable consoles carry the partner's cartridge across the link, so a joiner whose copy of the game differs from the host's is not turned away at the card: the link forms with the host's file and both consoles end cleanly |
| `version_<console>` for gba, gb, gbc, nes, ws, snes, md, sms, sg | the host newer (`ONLINE_VER_OLD_HERE`: update this Vita), the host older (`ONLINE_VER_OLD_THERE`: your friend must update), a service that wants a newer app for a joiner and, for the consoles played in the app, for a host (`ONLINE_UPDATE`): no join request, no link, nothing started |
| `gbc`, `gbc_relay` | Game Boy Color cable over the punch and over the relay (the same cartridge as `gb`, built for the Color) |
| (every `*_relay` and `snes_online` / `sega_online`) | the service gives relay credentials only once both members have polled (403 `relay_not_allowed` before that): the joiner's first ask is refused and the app asks again every 3 s (up to 12 times) instead of failing the join |
| `gb`, `gb_relay` | Game Boy cable over the punch and over the relay: 200 bytes each way, the cartridge's verdict in memory and in the committed save. The cartridge needs its player to press A once the link is up (that console is the master); the first smoke did not and read zeros |
| `adapter` | Wireless Adapter over Online: room, idle link, teardown (no adapter traffic exists to cross) |
| `share` | Share game data over Online, byte for byte |
| `wrong_code`, `expired`, `update_required`, `version` | the card's sentences: a wrong code, a code that aged out (`POST /_test/advance`), a service that wants a newer app (426, joiner and host), the host newer / older (card shown with another `ver`), same release with another setup (`link_build`); no join request is made |
| `busy`, `slow_down`, `service_down` | 503 `over_budget` for joiner and host, a short 429 (its own sentence), nothing listening |
| `lost_answer` | the first answer to the create and to the join is lost; the request id makes the retry succeed (two requests each, both answered) |
| `cancel_after_failure` | a join that fails at the service, Cancel at once: the menus again, radio given back |
| `drop` | the path dies in the middle of the game: the link ends (`peer_lost`), each game carries on |
| `rtt_100`, `rtt_150`, `rtt_200` | round trips of 100 / 150 / 200 ms, jitter 20 ms, loss 2 %, made by natsim and again by the transport's own shim (`link_latency` / `link_jitter` / `link_loss`): the delay is measured, at most 12 (the Online cap), `want=` logged, hashes equal; stalls per minute are printed, not judged |
| `delay_cap` | the same delay on Same Wi-Fi stays capped at 8 |
| `determinism` | 150 ms round trip, jitter, loss, scripted buttons on both consoles for 90 s: every periodic hash and the final hash agree (linktest.gba always; a private Link Cable game when `GATE_GBA_ROMS` has one) |
| `different_game`, `library_join` | `different_game`: a cable game with the host's game code but another file, joined from Play together, ends cleanly on both consoles. `library_join`: Join online from the game list opens the matching game and joins; a cable game whose file differs from the host's asks and opens nothing by itself; with the host's file among two it is the one offered |
| `two_games` | two different GBA cartridges over Online (Ruby with Sapphire), over the punch and over the relay: the partner's cartridge crosses the link once and is kept in the cache, and a second session of the same two data folders finds it there. Skipped when the second test cartridge (`gpsp/tools/linktest/linktest_b.gba`) is not built |
| `direct_hidden` | the Connection page has no Direct address unless `link_direct = 1` |

WAITING FOR THE SCREEN, NOT FOR A CLOCK.  A twin's input script is timed in display frames, and a busy machine (two twins, a
service, a NAT simulator and other jobs on two cores) can stall the app's loop for longer than a press lasts, so a fixed
script dropped presses (a wrong code typed, a menu row missed).  plat_host.c has two more script entries for this:
`FRAME:wait=TEXT` stops the script clock until a new line of the app's own log holds TEXT (at most 240 s), and
`FRAME:hold=BUTTONS=TEXT` does the same with BUTTONS held down all the while (at most 8 s), so the press ends once the
app has logged what it did.  The code pad logs `online_pad_key box= sym=` for every key; `snes_online`, `sega_online`
and the refusals of the SNES and Sega consoles type the code that way. The twin built elsewhere can be named with
`GBADHOC_HOST_X86` (make ARCH=x86 B=<dir>).

The Vita build with Mbed TLS and the heap and package-size numbers are
checked by the build (`vita-build` in the full tier), not by the twin.

## The aarch64 twin

`make -C gpsp/vita/hostcore ARCH=aarch64` (see [Building](BUILDING.md)) builds the app for 64-bit
ARM Linux with the GBA interpreter. The test scripts take it as a mode, run under `qemu-aarch64`
(on an aarch64 machine directly; `QEMU_A64=` changes the command), and compare pictures with the
x86 references the way the other ARM mode does:

    cd gpsp/vita/hostcore
    python3 app_tests.py aarch64 --only list,session,ff,play,cpufix
    python3 link_tests.py aarch64                    # both twins aarch64 (Same Wi-Fi: cable, Game Boy, adapter, ad hoc ...)
    python3 link_tests.py mixed64                    # an x86 twin and an aarch64 twin
    python3 online_tests.py aarch64 --hz 12          # punch, relay, gb, lost_answer, library_join

A twin built elsewhere is named with `GBADHOC_HOST_AARCH64=/path/to/gbadhoc_host`. The aarch64
twin's link "backend" word is `interp` (x86 with the recompiler is `x86`, ARM with it `arm`), so
an aarch64 twin and a console running the recompiler are never offered each other's GBA Link
Cable rooms ("different version"); Game Boy, Wireless Adapter, NES, WonderSwan and Share rooms
hash only the build and are open to it. `mixed64` therefore runs only those.

## Second player on a Mac

A Mac cannot run the Vita app, but it can run the twin and be the other player in a two-player or
online session with one real Vita. The twin has no window and no keyboard: a scripted pad drives
its menus and plays, so an automated session (or a person typing one command) can be the Mac's
side. Nothing here has been run against a real Vita.

There are two ways to get the twin on the Mac. **Use the native build:** it uses the Mac's own
network, so Same Wi-Fi (the Vita and the Mac on one Wi-Fi) and Online both work, and a Linux VM
(whose network may have no route to the room service) is not needed. The aarch64 Linux build in a
VM is the fallback (see the end of this section).

1. Build it on the Mac (macOS 15, Xcode command line tools, `brew install llvm`; details in
   [Building](BUILDING.md), "The twin on a Mac"):

       sh tools/build/macos-build.sh --host-version <the Vita build's CORE_VERSION>

   The program is `gpsp/vita/hostcore/build/macos/gbadhoc_host`. `--host-version` must be the
   string the Vita package reports (`CORE_VERSION` in `gpsp/vita/Makefile`), or the Vita refuses
   the twin ("different version"). Also `python3 -m pip install pillow` (Pillow; use a virtual
   environment if pip refuses on a Homebrew Python): `mac_player.py` imports the test helpers.
   Put the same game as the Vita's in a folder (the twin carries a Link Cable partner's cartridge
   across, but a Game Boy / NES / WonderSwan session needs the file on both sides). A GBA BIOS must
   be the same file as on the Vita if either has one (`--bios`).
2. The first time the twin opens a network socket macOS may ask "Do you want the application
   gbadhoc_host to accept incoming network connections?": allow it (Same Wi-Fi rooms are found by
   broadcast and need to be heard). If it was refused, System Settings > Network > Firewall.
   macOS 15 also asks the terminal application (Terminal, iTerm...) for "Local Network" access the
   first time a program started from it talks to the LAN: allow it, or turn it on in System Settings >
   Privacy & Security > Local Network; without it the Vita's room is never heard.
3. Run `gpsp/vita/hostcore/mac_player.py` (on the Mac: `python3 gpsp/vita/hostcore/mac_player.py ...`), which writes the data root and the pad script and starts
   the twin in real time (`--realtime`; all of its output is `ROOT/log/gbadhoc.log`, pictures in
   `ROOT/shots/`):

       # Online: the Vita hosts (Play together > Online), shows the 5 character code
       python3 mac_player.py --exe ./gbadhoc_host --rom game.gb --join ABCDE --play 90

       # Same Wi-Fi: the Vita hosts, the Mac joins the first room in the list
       python3 mac_player.py --exe ./gbadhoc_host --rom game.gb --join-lan

       # the Mac hosts (prints "ROOM CODE: XXXXX" for Online), the Vita joins
       python3 mac_player.py --exe ./gbadhoc_host --rom game.gb --host-online
       python3 mac_player.py --exe ./gbadhoc_host --rom game.gb --host-lan

   `--ip` (and `--mask`) is the machine's address on the Wi-Fi, told to the twin as its stand-in for the
   console's Wi-Fi (`nethost.ini`); on a Mac `mac_player.py` finds it itself (`en0`, `en1`...) and prints
   it, so leave it out unless that is the wrong interface (check with `ifconfig en0`); in a VM
   it is the VM's (`ip addr`) and the VM's network must be BRIDGED; `--play SEC` is how long the pad plays after the link forms
   (it presses `--pattern cross,right,cross,left` every half second), after which the twin
   disconnects from the menu and quits. The exit status is 0 when the link formed.
4. The pad script is the plain text of the twin's input (`--input-file`, documented at the top
   of `gpsp/vita/hostcore/plat_host.c`): lines like `+30:cross:9` (hold Cross 9 refreshes, 30
   after the last entry), `+0:wait=online_card state=` (stop the script clock until the app's
   log says so), `+30:type=TEXT`. `mac_player.py` writes `ROOT/script.txt`; copy and edit it to
   play something else (a script that plays a particular game) and start the twin by hand:
   `gbadhoc_host --root ROOT --realtime --shot-dir ROOT/shots --input-file ROOT/script.txt`.

The Linux VM route (UTM, Parallels, Multipass; any aarch64 Linux) still works: build there with
`make -C gpsp/vita/hostcore ARCH=aarch64` (natively on the VM, `HOST_VERSION=` as above, and for Online
`MBEDTLS=<aarch64 prefix>`), bridge the VM's network to the Wi-Fi the Vita is on (same subnet, no
NAT) for Same Wi-Fi rooms, and run `mac_player.py` there with `--exe` and `--ip` of the VM.

Which sessions can include the Mac: Game Boy / Game Boy Color link, the Wireless Adapter, NES and
WonderSwan two players, Share game data, and Online. GBA Link Cable (the cycle-exact mirror) needs
the same CPU backend word on both sides and the Mac's is `interp`: the Vita would have to run its
interpreter AND say the same word, which is not the case today (its word is `arm-interp`), so
that pairing is refused until it is proven that the two interpreters agree cycle for cycle.
