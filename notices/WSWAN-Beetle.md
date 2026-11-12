# WonderSwan: Beetle WonderSwan (Mednafen)

WonderSwan and WonderSwan Color games run on the Mednafen WonderSwan module in its libretro form, linked into the main
program as the single relocatable object `wswancore.o`. Its only exported symbols are the `wswan_host_*` functions of
`wswan/frontend/host.h`.

- **Core:** beetle-wswan-libretro, <https://github.com/libretro/beetle-wswan-libretro>, in `wswan/core`.
  GPL-2.0-or-later (`licenses/Beetle-WonderSwan-GPL-2.0.txt`). Commit, files taken and the one local edit:
  `wswan/core/PROVENANCE.md`.
- **Inside it:** Blip_Buffer 0.4.1 by Shay Green (LGPL-2.1-or-later, `licenses/LGPL-2.1.txt`; the object is statically
  linked into a GPL program, which LGPL 2.1 section 3 allows); libretro-common headers (MIT); the NEC V30MZ emulator
  by Bryan McPhail et al., used on condition of this credit: **NEC V30MZ emulator (c) Bryan McPhail.**
- **Our code in wswan/:** `wswan/frontend` and `wswan/twin` are new code, MIT (Copyright CBOC-Vita).
- **No BIOS or boot ROM is needed or shipped.** No commercial ROM is included; the test ROMs are generated from
  hand-written machine code by `wswan/twin/tests/make_roms.py`.
