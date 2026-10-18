# Changes

## 2026-10-18: Source Handoff

- Added the source/export identity and a compact map of the complete guides.
- Rechecked the upstream license question: no reply as of this date.
- No source/runtime replacement, install or public-release change.

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
- Include the full CPU xBRZ notice and linked SDK runtime notices in the
  VPK and install ZIP, with the SDK source lock. Keep the existing full
  libvita2d/stb notices and verify their bytes against the vendored originals.
  This notice-only repack changes no executable, game data or app resource.

This candidate is not tagged as a finished public release. The selected
original executable has passed an in-place Vita update, launch and game-browser
check with external-folder scanning. Fresh installation, open-BIOS fallback,
gameplay and save/load remain untested on this package. The independent build
succeeds but is not byte-identical. Earlier build-20 results remain historical
evidence, not acceptance of build 21.
