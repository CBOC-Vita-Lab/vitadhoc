# tools/gate

One command for every test in this repository. `gate.py` is standard library
only (Python 3.8 or later) and runs the existing suites as they are; it
changes no test and no build file.

```sh
python3 tools/gate/gate.py                 # default tier
python3 tools/gate/gate.py --quick         # the fast subset
python3 tools/gate/gate.py --full          # everything, incl. armhf under qemu and the Vita package
python3 tools/gate/gate.py --release       # --full plus the release pins (committed VPK hashes, SHA256SUMS)
python3 tools/gate/gate.py --changed       # steps selected by the paths changed since HEAD
python3 tools/gate/gate.py --list          # every step, its tier, tags, prerequisites, and whether it can run here
python3 tools/gate/gate.py --dry-run       # the plan, with cache hits and skips marked
python3 tools/gate/gate.py --only a,b      # these steps and their prerequisites
python3 tools/gate/gate.py --skip a,b      # leave these out
python3 tools/gate/gate.py --force         # ignore the cache
python3 tools/gate/gate.py --baseline F    # compare with a saved summary.json
```

## Tiers

| tier | what runs |
|---|---|
| `--quick` | release unit tests, the package test on a fresh `make vpk` build, the three save tests, `gamedb`, `playerkit`, `widekit`, the SNES, NES and WonderSwan selftests |
| default | quick, plus the host app (`app`, `rewind`, `local`, `link` on x86), the unit suites, audio, cover art, cheats, library, and the private-ROM steps (skipped unless their folder is set) |
| `--full` | everything, plus armhf under qemu, the cheats end-to-end, the NES armhf check, the Vita compile checks, and the Vita package build (`make vpk`) |
| `--release` | `--full` plus the release pins: `release-package-pinned` (the committed root VitAdhoc.vpk and its pinned hashes) and `check-files` (SHA256SUMS); stale by design between releases |

`--only`, `--skip` and `--changed` act within the tier you name, and
`--only` ignores tiers. A step's prerequisites are always added.

## What gate does

1. **Builds once.** Each prerequisite (the twins, the host app, the GBA and
   GB test cartridges, the link ROM, the cheats and rewind ROMs) is a step of
   its own. Steps that need it depend on it, so it is built once per run and
   reused. A build step that is already up to date costs `make` a second.
2. **Runs the suites as steps.** Each step is a command (run by `bash -o pipefail`: no step passes through a pipe), a working directory,
   prerequisites, tags, a timeout and a log. Commands run under `nice -n 10`,
   at most `--jobs` at a time (default: the number of cores).
3. **Shares resources safely.** Steps in one group never overlap: `gpsp-make`
   (make inside `gpsp/`), `hostcore-run` (app runs that share data roots, the
   screenshot folder and loopback ports), `tmp-fixed` (`run_host_tests.py`
   compiles into fixed `/tmp` names).
4. **Caches PASSes.** The key of a step is a SHA-256 of: its command, its
   working directory, the content of its input directories (tracked and
   untracked files that git does not ignore), its prerequisites' keys, the
   value of its private-ROM variable, and `gate.py` itself. A PASS is written to
   `out/.gate-cache/<step>.json`. The next run skips the step while the key is
   the same and its declared outputs exist. Only PASS is cached.
5. **Private ROMs.** Steps that need ROMs you cannot commit run only when the
   variable is set, and otherwise report SKIP:

   | variable | steps |
   |---|---|
   | `GATE_GBA_ROMS` (folder) | `cores-private-gba` (run_tests.sh over the folder, x86 against armhf) |
   | `GATE_SNES_ROMS` (folder) | `snes-mt-compare`, `snes-conformance` (informational); `snes-selftest` also uses it for its ROM sections |
   | `GATE_NES_ROMS` (folder) | `nes-conformance` (against `nes/twin/conformance_expect.txt`) |
   | `GATE_WS_ROM` (file) | `wswan-private` (the WonderSwan acceptance section) |

   The cache key of a private step includes the folder path, not the ROM files.
   After changing the ROMs, run with `--force` or delete the step's cache file.
6. **Reads each suite's own totals.** The parsers in `PARSERS` read the
   summary line each suite prints (`N checks, M failed`, `N passed, M failed,
   K skipped`, `Ran N tests`, `PASS`/`FAIL` lines, and so on). A step with no
   totals counts as one check that passed or failed by exit status (shown as
   `-` in checks). The totals are only as strict as the suite that prints them.
7. **Writes one summary.** A table on stdout, `out/summary.md` and
   `out/summary.json`. Logs are in `out/logs/<step>.log`. The exit status is
   1 if a step FAILs or is BLOCKED, or if a baseline regression is found.

## Results

| result | meaning | counts as failure |
|---|---|---|
| PASS | exit 0, no reported failures (cached PASSes are marked) | no |
| FAIL | non-zero exit, a reported failure, or a timeout | yes |
| BLOCKED | a prerequisite failed or is blocked, so the step did not run | yes |
| SKIP | a prerequisite is missing from this machine, or a private folder is not set | no |
| INFO | informational: shown with its numbers, not gating | no (unless baseline) |

## Baseline

`--baseline FILE` reads a `summary.json` (or any JSON with a `steps` object).
A step is flagged when its check count dropped or its failure count rose.
Flags appear in the table's note column, in `summary.md`, and set exit
status 1. A step that did not run this time is not flagged.

```sh
cp tools/gate/out/summary.json ~/gate-baseline.json       # after a good run
python3 tools/gate/gate.py --quick --baseline ~/gate-baseline.json
```

## --changed

`--changed` compares the working tree with `--base` (default `HEAD`; untracked
files count too) and maps each path to tags. The table is `CHANGE_MAP` at the
top of `gate.py`: the longest matching prefix decides, and a step runs if one of
its tags is selected.

| path | selects |
|---|---|
| `nes/` | NES steps, the app steps (the NES core is built into the app), `vita` |
| `snes/`, `wswan/`, `playerkit/`, `widekit/` | that core's steps, `vita` |
| `gpsp/vita/tests/savesync/`, `.../localsave/` | the save tests |
| `gpsp/` (any other path) | every gpsp step |
| `tests/`, `tools/` (except `tools/gate/`), `docs/`, `licenses/`, `notices/`, `SHA256SUMS`, top-level files | release steps (`SHA256SUMS` lists every shipped file) |
| `tools/gate/` | everything |
| any other path | everything (the safe answer) |

Changing `gate.py` changes every key, so the next run is a full run.

## Adding a step

Add a `Step(...)` to `STEPS`. Give it a tier, the tags it answers to, its
input directories, its prerequisites (`deps`), and its requirements
(`("exe", name)`, `("path", p)`, `("py", module)`). Set `parse` to the name of
a parser, or leave it as `"exit"`. Run `--list` and `--dry-run` to check it.

## Output

`tools/gate/out/` is gitignored by the root `**/out/` rule. It holds `logs/`,
`summary.json`, `summary.md`, `.gate-cache/` and `bin/` (the C test binaries
of the save tests). Delete it at any time; the next run rebuilds it.

## Not wired

See `docs/TESTING.md`, section "Not wired", for the suites this gate does not run.
