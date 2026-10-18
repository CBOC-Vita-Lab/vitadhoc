# Source Record

| Item | Value |
| --- | --- |
| Selected build | Build 21 |
| Original source revision | `5f0fa23a1bed08f1eec0ae9751405ac9e15c0f16` |
| Original source archive SHA-256 | `3c2d20846101243c983caaddaa16d471cfd9f4b974eae4fb028361130550b4d7` |
| Original bundle SHA-256 | `0c1868c20167d725bf6c04841da30872581f3ab19fc71ea5d0c830f8c9e4d3ca` |
| Selected runtime eboot SHA-256 | `d7b06b6cbb6242c702c27961878fc7deb4a9add0beb4090fb0798f88747ca016` |
| Notice-complete private VPK SHA-256 | `059da051df7ab10a25eb01e5fc8e7f24163063ac9cbd08fedbd8df6668012a2a` |
| Title ID | `GBAD00001` |

The complete reviewed source is under `gpsp/`, with vendored dependencies,
open BIOS, fonts and notices. Commercial BIOS, ROMs, private saves, original
Git history and game artwork are not included. Required upstream credits
remain. [Building](BUILDING.md) records the independent clean builds and
their differences from the selected original executable.

The selected runtime passed an in-place update, launch and external-folder
scan. The later notice-only repack did not change runtime bytes and was not
installed again. Fresh installation, gameplay and save/load acceptance remain
pending. See [Known Issues](KNOWN-ISSUES.md).

The GPL-2.0 frontend / GPL-3.0 shader question remains unresolved.
[Upstream issue](https://github.com/ShoshinFauteux/GBAdhoc/issues/14) had no
reply when checked on 2026-10-18. No license was changed based on an assumption.

## Guide Map

[Install](INSTALL.md), [Controls](CONTROLS.md), [Play Together](PLAY-TOGETHER.md),
[Technical Notes](TECHNICAL.md), [Building](BUILDING.md), [Assets](ASSETS.md),
[Notices](notices/THIRD-PARTY.md), [Credits](CREDITS.md) and
[Changes](CHANGELOG.md) cover the player and developer handoff separately.
