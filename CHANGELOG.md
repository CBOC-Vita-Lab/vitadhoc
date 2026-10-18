# Changes

## Build 21 Publishing Candidate

- Scan existing external game folders, including common RetroFlow roots.
- Support up to eight explicit `extra_roms` roots, or disable external scans.
- Ignore hidden files and case-equivalent duplicate roots; cap each console
  list at 2,048 games, retaining the primary root first.
- Keep saves/states beside external ROMs; favourites distinguish full paths.
- Shared package removes the original Nintendo BIOS while retaining the
  original build-21 executable and its built-in open replacement.
- Add complete readable font, dependency and database notices, plus Wikipedia
  attribution. Correct the stale LiveArea generation target in the source copy.
- Separate concise player guides from technical/build documentation.

This candidate is not tagged as a finished public release. The selected
original executable has passed an in-place Vita update, launch and game-browser
check with external-folder scanning. Fresh installation, open-BIOS fallback,
gameplay and save/load remain untested on this package. The independent build
succeeds but is not byte-identical. Earlier build-20 results remain historical
evidence, not acceptance of build 21.
