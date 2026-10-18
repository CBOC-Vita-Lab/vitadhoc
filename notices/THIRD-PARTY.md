# Third-Party Notices

VitAdhoc is based on GBAdhoc and gpSP. The original copyright notices in
the source remain in place. The application and open GBA BIOS use GPL
version 2; see `licenses/GPL-2.0.txt` and `gpsp/COPYING`.

- **gpSP:** original work by Exophase, with libretro and other contributors.
  See source headers and `gpsp/original_readme.txt` for the original credits.
- **Open GBA BIOS:** Normmatt and the VBA/VBA-M team, from
  <https://github.com/Nebuleon/ReGBA/tree/master/bios>. GPL-2.0. Source is in
  `gpsp/bios/`. This is not Nintendo's BIOS.
- **TGB Dual:** GB/GBC core by the TGB Dual authors, with the libretro
  contributors and local linked-instance changes. Its GPL notice and source
  are in `gpsp/gbcore/tgbdual/`.
- **mGBA cheat engine:** Copyright (c) 2013-2016 Jeffrey Pfau. MPL-2.0;
  `licenses/MPL-2.0.txt`. Source and marked modifications are in
  `gpsp/cheat_engine.c`, `gpsp/cheat_engine_tables.h` and related files.
  `gpsp/vita/third_party/README.cheats` identifies the source revisions and
  separate test-only VisualBoyAdvance-M reference excerpts.
- **libvita2d:** Copyright (c) 2014 xerpi, MIT. Vendored subset from
  `a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed`; local changes are described in
  `gpsp/vita/third_party/vita2d/PROVENANCE`. Its MIT notice accompanies the
  source and distribution. The included compiled shaders have their `.cg`
  source beside them.
- **stb:** Sean Barrett and contributors, offered under MIT or public-domain
  terms. The complete dual notice is in
  `gpsp/vita/third_party/stb/LICENSE`; individual headers retain their notices.
- **xBRZ:** Zenju, HqMAME and contributors, used under GPL version 2 with the
  original exception retained. The code and local-change notice are in
  `gpsp/vita/third_party/xbrz/`. The app does not claim the MAME exception as
  permission for unrelated game material.
- **Inter:** The Inter Project Authors, SIL Open Font License 1.1.
  `licenses/Inter-OFL.txt`. Regular and SemiBold TTFs are embedded.
- **Outfit:** The Outfit Project Authors, SIL Open Font License 1.1.
  `licenses/Outfit-OFL.txt`. Medium and SemiBold TTFs are embedded; Bold is
  used in generated LiveArea artwork. Font names and notices are unchanged.
- **Cheat database and cartridge names:** libretro-database contributors,
  revision `d5bae90b22018ce3c9c5a8eff4141a4768c8dd5f`,
  <https://github.com/libretro/libretro-database/tree/d5bae90b22018ce3c9c5a8eff4141a4768c8dd5f>.
  Published there under CC BY-SA 4.0; `licenses/CC-BY-SA-4.0.txt`.
  The local converter merges and deduplicates lists, filters supported codes,
  shortens display names and associates records with cartridge checksums.
  This database remains separately licensed; it is not relicensed as GPL.
- **Experimental GPU xBRZ:** a separate implementation referring to libretro's
  xbrz-freescale-multipass shaders. Their headers include Hyllian's MIT notice
  and a Zenju GPL-3.0 notice; these are retained in
  `licenses/GPU-xBRZ-notices.txt` and `licenses/GPL-3.0.txt`. The original
  upstream shader revision was not recorded. This is not the same license
  evidence as the vendored CPU xBRZ copy. Combined-binary licensing remains
  under review; adding notices or leaving GPU mode off does not settle it.
- **Game descriptions:** English Wikipedia contributors, CC BY-SA 4.0.
  Article and history links, adaptation notes and attribution are in
  `notices/WIKIPEDIA-ATTRIBUTION.md`. Wikidata article mappings are CC0.

No game ROM, Nintendo BIOS, user save, downloaded cover collection or private
recording is included. Game titles identify compatibility and are not an
endorsement. Database inclusion does not grant rights to game artwork.
