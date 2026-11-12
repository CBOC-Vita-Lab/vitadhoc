# Third-Party Notices

VitAdhoc is based on GBAdhoc and gpSP. Original copyright and component
license notices remain in place; the root GPL text is not a blanket license
for every file. See [Licensing Status](../LICENSING.md) for the maintainer's
clarification and the remaining package review.

- **GBAdhoc:** ShoshinFauteux explicitly permitted their own contributions
  under GPL-2.0-or-later on October 19, 2026, including GPL-3.0 use.
  [Project](https://github.com/ShoshinFauteux/GBAdhoc) and
  [permission](https://github.com/ShoshinFauteux/GBAdhoc/issues/14#issuecomment-6267226116).
  Other contributors' terms are not changed by this grant.
- **PSP-only source caveats:** `gpsp/psp/font_8x16.c` retains GPL-2.0-only
  terms; `gpsp/psp/me` has unresolved provenance flagged by the maintainer.
  Neither is linked by the reviewed build-21 Vita build. Their presence in
  the source tree is not permission to relicense them.
- **gpSP:** original work by Exophase, with libretro and other contributors.
  See source headers and `gpsp/original_readme.txt` for the original credits.
- **Open GBA BIOS:** Normmatt and the VBA/VBA-M team, from
  <https://github.com/Nebuleon/ReGBA/tree/master/bios>. GPL-2.0. Source is in
  `gpsp/bios/`. This is not Nintendo's BIOS.
- **TGB Dual:** GB/GBC core by the TGB Dual authors, with the libretro
  contributors and local linked-instance changes. Its GPL notice and source
  are in `gpsp/gbcore/tgbdual/`.
- **Snes9x 1.63 (SNES player, separate program):** the Snes9x team,
  <https://github.com/snes9xgit/snes9x>. Snes9x licence (non-commercial, not
  GPL), `snes/core/LICENSE`, shipped as `licenses/Snes9x-LICENSE.txt`; it
  includes Shay Green's snes_spc DSP (LGPL 2.1). It is built into `snes.bin`
  only, never linked with `gpsp/` code. Tag, commit and local edits:
  `snes/core/PROVENANCE.md`. Our SNES frontend (`snes/frontend`, `snes/vita`,
  `snes/twin`) is MIT and vendors its own libvita2d, stb and Inter font, each
  with its notice. See [SNES: Snes9x](SNES-Snes9x.md).
- **FCEUmm (NES core):** the FCE Ultra, FCEUX and FCEUmm authors,
  <https://github.com/libretro/libretro-fceumm>, GPL-2.0-or-later
  (`licenses/FCEUmm-GPL-2.0.txt`), plus libretro-common (MIT,
  `licenses/libretro-common-MIT.txt`). Linked as one relocatable object (`nescore.o`) that exports only
  `nes_host_*`. Commit and the three local edits: `nes/core/PROVENANCE.md`. Our NES host (`nes/frontend`,
  `nes/twin`) is MIT. See [NES: FCEUmm](NES-FCEUmm.md).
- **Beetle WonderSwan / Mednafen WonderSwan (WonderSwan core):** Dox (Cygne), the Mednafen team and the libretro
  contributors, <https://github.com/libretro/beetle-wswan-libretro>, GPL-2.0-or-later
  (`licenses/Beetle-WonderSwan-GPL-2.0.txt`). It includes Shay Green's Blip_Buffer 0.4.1 (LGPL-2.1-or-later,
  `licenses/LGPL-2.1.txt`), libretro-common headers (MIT, `licenses/libretro-common-MIT.txt`) and the NEC V30MZ
  emulator by Bryan McPhail, Oliver Bergmann, Fabrice Frances and David Hedley, used here with the credit its author
  asks for: **"NEC V30MZ emulator (c) Bryan McPhail"**. Linked as one relocatable object (`wswancore.o`) that exports only
  `wswan_host_*`. Commit and the one local edit: `wswan/core/PROVENANCE.md`. Our WonderSwan host (`wswan/frontend`,
  `wswan/twin`) is MIT. See [WonderSwan: Beetle](WSWAN-Beetle.md).
- **mGBA cheat engine:** Copyright (c) 2013-2016 Jeffrey Pfau. MPL-2.0;
  `licenses/MPL-2.0.txt`. Source and marked modifications are in
  `gpsp/cheat_engine.c`, `gpsp/cheat_engine_tables.h` and related files.
  `gpsp/vita/third_party/README.cheats` identifies the source revisions and
  separate test-only VisualBoyAdvance-M reference excerpts.
- **libvita2d:** Copyright (c) 2014 xerpi, MIT. Vendored subset from
  `a8f15ab09d5233f0a4e4ad0e8f6ade0da888cbed`; local changes are described in
  `gpsp/vita/third_party/vita2d/PROVENANCE`. Its MIT notice accompanies the
  source and distribution (`licenses/vita2d-MIT.txt`). The included
  compiled shaders have their `.cg` source beside them.
- **stb:** Sean Barrett and contributors, offered under MIT or public-domain
  terms. The complete dual notice is in
  `gpsp/vita/third_party/stb/LICENSE` and `licenses/stb.txt`;
  individual headers retain their notices. `gpsp/psp/stb_image.h` (v2.31,
  release commit 48fb1c9) carries four local changes, marked
  "VitAdhoc local patch" in the file: stray bytes between JPEG segments are
  skipped; a marker error before the first scan fails instead of returning
  an undecoded image (upstream e4efdc8 now fails every bad-marker case;
  VitAdhoc keeps the partial image when a scan was already decoded); the
  `stbi__mad4sizes_valid` guard covers the PNG-only build (upstream 2.31 does
  not link in that configuration); and the progressive MCU loop keeps the
  pre-3f9200a decode, because upstream 3f9200a drops MCUs of valid
  progressive JPEGs. The snes copy, `snes/frontend/third_party/stb/stb_image.h`
  (v2.31), has only the progressive change, marked the same way; the snes
  overlay loader reads PNG only. The stb_truetype.h (v1.27) and
  stb_image_write.h (v1.17) copies in `gpsp/vita/third_party/stb/` and
  `snes/frontend/third_party/stb/` are unmodified upstream release files.
- **xBRZ:** Zenju, HqMAME and contributors, used under GPL version 2 with the
  original exception retained. The code and local-change notice are in
  `gpsp/vita/third_party/xbrz/`. The app does not claim the MAME exception as
  permission for unrelated game material. The original header is also in
  `licenses/CPU-xBRZ-notice.txt` inside the player package.
- **rcheevos:** RetroAchievements.org, MIT. Release v12.5.0
  (`1433173220a7eaede6a9ed7a18e94117be1821e0`, still the newest release on
  2026-11-06), a subset in `gpsp/vita/third_party/rcheevos/` with six small
  bug fixes backported from later upstream commits, including the null
  checks of db8ca2e (`PROVENANCE` lists them and what was left out).
  Its notice, which accompanies the source and every distribution:

  > MIT License
  >
  > Copyright (c) 2018 RetroAchievements.org
  >
  > Permission is hereby granted, free of charge, to any person obtaining a
  > copy of this software and associated documentation files (the
  > "Software"), to deal in the Software without restriction, including
  > without limitation the rights to use, copy, modify, merge, publish,
  > distribute, sublicense, and/or sell copies of the Software, and to permit
  > persons to whom the Software is furnished to do so, subject to the
  > following conditions:
  >
  > The above copyright notice and this permission notice shall be included
  > in all copies or substantial portions of the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
  > OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  > MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
  > NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
  > DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
  > OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
  > USE OR OTHER DEALINGS IN THE SOFTWARE.

  The root CA certificates compiled into `gpsp/vita/ra_roots.h` (for
  RetroAchievements' HTTPS) are the public certificates of their issuers,
  taken from the Mozilla root store as packaged by Debian's ca-certificates;
  `gpsp/vita/tools/ra_roots.py` names them. RetroAchievements is a separate
  service; signing in is the player's choice and is not required to play.
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
- **VitaSDK import stubs and headers:** vitasdk contributors, MIT, headers
  revision `e66ebe90b73d1fa4cce005a5b2072cec27322544`.
  `licenses/VitaSDK-Headers-MIT.txt` preserves that revision's `LICENSE.md`.
  These are open SDK import definitions, not copies of Sony's firmware modules.
- **Newlib C/math runtime:** VitaSDK newlib revision
  `2e428297c0b6aefd830c5a75a7daa7e774562a42`. Its collected component notices
  are preserved in `licenses/Newlib-Notices.txt`; source files can carry
  additional notices. This is a multi-license library, not a blanket GPL grant.
- **GCC runtime:** GCC 15.2.0, Free Software Foundation and contributors.
  GPL-3.0-or-later with the GCC Runtime Library Exception 3.1 for applicable
  runtime files; `licenses/GPL-3.0.txt` and
  `licenses/GCC-Runtime-Exception.txt`. This exception is separate from the
  experimental GPU shader's unresolved combined-work review.

The linked SDK inputs above are pinned by `notices/VitaSDK-build-lock.json`,
not inferred from whichever SDK is installed later. See [Building](../docs/BUILDING.md).
Adding these notices does not finish the component-level licensing review.

No game ROM, Nintendo BIOS, user save, downloaded cover collection or private
recording is included. Game titles identify compatibility and are not an
endorsement. Database inclusion does not grant rights to game artwork.
