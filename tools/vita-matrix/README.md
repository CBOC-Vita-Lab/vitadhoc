# vita-matrix: unattended measurement sessions

Turns one test session on a real Vita into many measurements. You write a
playlist; the tool configures the app, starts it, waits for it to leave, pulls
the logs back, and prints one table row per entry. At the end (and on Ctrl-C,
and if anything fails) your own `vita.ini`, `snes/threads.ini`, `autoboot.txt`
and the two log files are put back exactly as they were.

```sh
export VITA=<the Vita's address>            # vitacompanion must be running
python3 tools/vita-matrix/matrix.py tools/vita-matrix/session-1.4.txt --dry-run   # the plan, touches nothing
python3 tools/vita-matrix/matrix.py tools/vita-matrix/session-1.4.txt             # the real run
python3 tools/vita-matrix/matrix.py PLAYLIST --only 1,3,5-7    # part of a list
python3 tools/vita-matrix/matrix.py --summarize OUTDIR         # print an old run's table again
python3 tools/vita-matrix/matrix.py --restore OUTDIR           # only if the tool itself was killed
```

The games must already be on the Vita under `ux0:data/GBAdhoc/roms/`. Leave the
Vita awake and on the charger; do not touch it during a run.

## Playlist

One entry per line: `game file name | duration | key=value ... | label`.

- `duration`: `60s` or `60` (seconds) or `3600f` (frames). Empty = `--default-secs`.
  GBA/GB games: sets `autoexit_frames` (seconds x 60) so the app leaves by itself;
  put `autoexit_frames=` in the keys to override. SNES games run in the separate
  player, which has no autoexit: the tool stops the app after the given time.
- keys go to `vita.ini`; a `t.` prefix sends the key to `snes/threads.ini`
  (`t.core4=off`). Every entry starts from an EMPTY `vita.ini` and the keys of
  that line (add `--merge` to start from the owner's own), and `threads.ini` is
  absent unless the line has `t.` keys, so no entry is coloured by your own settings.
- `@base k=v ...` on its own line: keys every later entry starts from; a bare
  `@base` clears them. The entry's own keys win.
- `#` starts a comment (a whole line, or after a space).

`session-1.4.txt` is the owner session plan. Its key names marked TODO at the top
are placeholders; unknown keys are ignored by the app, so a wrong name does not
break the run, it just means that layer changes nothing.

## What it does per entry

1. Deletes the app's own old logs (they are restored later), writes `vita.ini`,
   `threads.ini` and `autoboot.txt`.
2. `quit all`, then `launch GBAD00001`; waits for `EVT exit` in the log (GBA) or
   the given time (SNES), with a time-out of start-up + 1.3 x duration + 20 s.
3. Pulls `log/gbadhoc.log` and `snes/log.txt` into `OUTDIR/NN-game-label/`
   (`gbadhoc.log`, `snes-log.txt`, `entry.json`; the emulator backend adds
   `vita3k.log`), and quits the app.

Before the first change the originals are copied to `OUTDIR/backup/`
(`manifest.json` says which files existed). Restore reads each file back from the
Vita and compares it; a problem is printed and the exit status is 3.
Exit status: 0 ok, 2 an entry failed (the run stops unless `--keep-going`),
130 Ctrl-C, 3 restore problem.

## Backends

- `--backend vc` (default): a real Vita through vitacompanion: FTP on port 1337
  for files, command server on 1338 (`quit all`, `launch`). The address comes from
  the environment variable `VITA` only.
- `--backend v3k`: the local Vita3K emulator (rehearsal; its timing is not the
  Vita's). Set `V3K_BIN` to the emulator executable (`V3K_PREF` to its
  preference folder if not the default). On a shared machine run it under your
  heavy-job lock. The app must already be installed there. `--data-dir` overrides the data folder.
- `--backend dir --data-dir D`: only writes files into D, launches nothing (tests).

## The table

| column | from |
|---|---|
| `end` | `ok` app left by itself, `timed` SNES stopped at its time, `timeout` we had to stop it, `no-log`, `ERR` |
| `missed/s` | the vcount-miss counter per second (mean and worst window); until that key is final, falls back to `vita_win late=` (tag `L`) then `fps skipped=` (tag `S`). Names tried are `VC_MISS_KEYS` in matrix.py |
| `emu fps` | `fps emu=` mean and minimum |
| `handoff` | `t`imeouts / `d`ropped summed from the `handoff:` lines |
| `look` | highest `look: lvl=` / the top level |
| `jit xlat/sync us` | worst `xlat_us` / `sync_us` of the `vita_jit_worst` lines (the worst-frame part, before the `\|`) |
| `snes fps` | `t=N fps=` mean and minimum |
| `snes audio` | `A t=` mean of the middle `ratio=` value (ppm) and the final `ur=` total underruns |
| `faults` | count of fault lines (`invalid_access`, `\|C\|`, `Unhandled access`, `fault=` non-zero, `EVT crash`...) and the first one; `0` if none |

A field that is not in the log prints `-`. The table is `COLUMNS` in matrix.py: one
function per column, each wrapped so that a surprise in one log line cannot stop
the others. Add a column or a key name there.

## Tests

`python3 tools/vita-matrix/test_matrix.py` (plain Python, no network or emulator):
the playlist parser, every column, and the restore logic for files that existed,
files that did not, a failing entry, Ctrl-C, and `--restore` after a killed run.

### Starting from a save state

`autoboot.txt` takes an optional second line `slot=N` (1 to 9): the game opens that
save state as soon as it runs, so a measurement can start at a battle or a busy
scene instead of the title. The state is the game's own (`states/<console>/<bucket>/<name>.st<N-1>`),
so a test copy of a game needs its states copied under the test copy's name.

