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

This candidate is not tagged as a finished public release. The independent
clean build succeeds; final candidate selection and physical-device acceptance
remain open. Earlier
build-20 test results are historical evidence, not acceptance of build 21.
