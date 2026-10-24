# Assets And Licenses

| Included | Basis / source |
| --- | --- |
| Emulator/frontend source | GBAdhoc maintainer's contributions are GPL-2.0-or-later; other components retain their terms. See [Licensing Status](../LICENSING.md) |
| Open GBA BIOS | GPL-2.0, Normmatt and VBA/VBA-M; `gpsp/bios/` source |
| Vita LiveArea art | Original linked-handheld mark and generator, `gpsp/vita/tools/make_livearea.py` and `vstyle_art.py` |
| Inter and Outfit fonts | SIL OFL 1.1; complete notices in `licenses/` |
| libvita2d/stb and shaders | Their MIT/dual notices and provenance retained |
| CPU xBRZ | GPL version 2 with original exception/notices retained |
| Experimental GPU xBRZ reference | MIT/GPL-3.0 upstream notices retained; combined-binary compatibility remains under review |
| mGBA-derived cheat engine | MPL-2.0; corresponding source and change markers retained |
| `cheats.db` and cartridge catalogue | Pinned libretro-database, CC BY-SA 4.0; local conversion described in notices |
| rcheevos (RetroAchievements runtime) | MIT; unchanged v12.5.0 subset in `gpsp/vita/third_party/rcheevos/` |
| `gamedb.txt` descriptions | Wikipedia contributors, CC BY-SA 4.0; 1,268 article/history links in the attribution file; Wikidata mappings CC0 |

See [Third-Party Notices](../notices/THIRD-PARTY.md) for revisions, modifications
and [Credits](../CREDITS.md) for contributors. These licenses do not grant rights
to the games named in a database. Source/test files retain their own notices;
the root license is not a blanket relicensing of every asset.

The original BIOS, commercial ROMs, saves, screenshots containing commercial
games, cover collections and old binary releases are excluded. Optional cover
and title-image fetching happens on the user's device, not in this package.
Users remain responsible for the material they import and share.

The README's screenshots in `store/screenshots/` are captures of the app's
own screens from its computer test build (the same UI code, drawn in software),
showing the project's test cartridge or a placeholder cover and a Wikipedia
description from `gamedb.txt`. They contain no commercial game graphics and are
labelled as test-build captures, not Vita photos.

`docs/media/livearea-startup.png` is the exact included startup
artwork, copied from `gpsp/vita/sce_sys/livearea/contents/startup.png`. It
uses the project's original mark and licensed fonts, under the project's
existing terms. The README labels it as artwork, not a gameplay or hardware
capture. Visual review found no personal or game content; the PNG has no
text, EXIF or XMP chunks.

The distribution includes full readable font/code/database licenses and
Wikipedia attribution both alongside the VPK and inside it. Embedded fonts
are not merely links to a website: Inter Regular/SemiBold and Outfit
Medium/SemiBold TTFs are present in the executable.
