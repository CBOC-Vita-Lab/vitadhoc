## VitAdhoc 1.2: bring your saves, your folders and your zips

Game Boy Advance, Game Boy and Game Boy Color, native on PS Vita. A box-art
home screen, previews of your own play, Continue, smoothing, rewind,
RetroAchievements and link-play support, built on the work of
[GBAdhoc](https://github.com/ShoshinFauteux/GBAdhoc), gpSP and TGB Dual.

### New in 1.2

- Import battery saves from RetroArch or an `import` drop folder without
  replacing a save VitAdhoc already has.
- Browse and play supported zipped games.
- Choose game folders in **Settings > Library > Game folders**. (#5)
- Start a game directly from RetroFlow and other launchers. (#3)
- Ten save-state slots, up from five.
- PNG screen overlays and adjustable LCD/scanline strength.
- Choose your screenshot folder in **Settings > Interface**.

### Fixes and under-the-hood work

Safer texture handling, recovery from a latched graphics error, deferred GPU
frees, less repeated work in rewind/smoothing and safer settings writes.
The audio fixes from 1.1 are retained; the changelog also corrects wording
that overstated parts of the original sound fix.

Performance depends on the game and settings. The historical 59.9 fps Ruby
result was measured with 1.0, not 1.2 or Pokemon Unbound. Host tests exercise
the changed code but do not establish physical-Vita speed. See the
[performance report](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/docs/PERFORMANCE.md)
for hardware evidence and capture provenance.

**New hardware check:** Pokemon Unbound 2.1.1.1 averaged about 60 emulated
fps and about 44 frontend-produced frames/s across three opening-beach
samples, at full-screen CPU xBRZ 3x. Exact readings, audio underruns,
exploratory slowdown and direct Vita screenshots are in the report. This is
not a claim of 60 visible frames per second or full-game compatibility.

### Install or update

Install `VitAdhoc.vpk` with VitaShell. Updating from 1.0 or 1.1? Close the app,
back up the data folder and saves beside games, then install over the existing
bubble. The title ID stays `GBAD00001`; no uninstall or data migration is
needed. Games, saves, states, settings and artwork stay in place.

`VitAdhoc-install.zip` contains the same VPK, full source, guides and checksums.
No commercial games or proprietary BIOS are included.

[Manual install and save import](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/INSTALL.md)
| [Detailed features and limitations](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/docs/FEATURES.md)
| [Known issues](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/KNOWN-ISSUES.md)

The historical acceptance run included a save-timing timeout that passed on
reruns; broader host-suite and hardware gaps are recorded in the
[source record](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/docs/SOURCE.md).

SHA-256 `VitAdhoc.vpk`:
`b844da404b1ed36ca776818d6870a19c3effc23a4c39ed6503854eeb16c940de`
