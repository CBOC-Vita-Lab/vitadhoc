# Provenance of wswan/core

- **Upstream:** beetle-wswan-libretro, <https://github.com/libretro/beetle-wswan-libretro>
  (Mednafen's WonderSwan module, derived from Cygne by Dox, via libretro).
- **Commit:** `4b01295838ea89e3f1355bbe4cb5cf98aa6108cd` (default branch head when taken; commit dated 2026-07-31).
- **Vendored:** 2026-11-12 into this directory.
- **Licence:** GPL-2.0-or-later (Mednafen / Cygne). `LICENSE` is upstream's `COPYING`, shipped as
  `licenses/Beetle-WonderSwan-GPL-2.0.txt`. Parts: `mednafen/sound/Blip_Buffer.c` and
  `mednafen/include/blip/Blip_Buffer.h` are Shay Green's Blip_Buffer 0.4.1, LGPL-2.1-or-later (`licenses/LGPL-2.1.txt`);
  `mednafen/wswan/v30mz.c` (NEC V30MZ by Bryan McPhail, Oliver Bergmann, Fabrice Frances, David Hedley, small changes by
  dox) may be used commercially or not "if you give the author, Bryan McPhail, a small credit somewhere (such as in the
  documentation for an executable package)": that credit is given in `notices/THIRD-PARTY.md`;
  `libretro-common/` headers and `compat_strl.c` are MIT (`licenses/libretro-common-MIT.txt`).

## What is here

- `mednafen/wswan/{sound,interrupt,comm,rtc,tcache,gfx,wswan-memory,v30mz,eeprom}.c` with their headers,
  `mednafen/wswan/wstech24.txt` (hardware notes), `mednafen/sound/Blip_Buffer.c`, `mednafen/mempatcher.c`,
  `mednafen/state.c`, `mednafen/settings.c` and the headers they include (`mednafen/*.h`, `mednafen/include/`),
  byte-identical to upstream except the change below.
- `libretro-common/include/{boolean.h,retro_inline.h,retro_common_api.h,compat/strl.h,compat/msvc.h}` and
  `libretro-common/compat/compat_strl.c`, unchanged.
- `sources.mk`: the list of translation units we build (ours, MIT).

## Not taken

`libretro.c` (the libretro driver: its Load, Reset, Emulate, cheat and save logic is re-implemented in
`wswan/frontend/host.cpp`), the other platform glue, the Makefiles, `msvc_compat.h`.

## Local changes to upstream files

- `mednafen/wswan/eeprom.c` / `eeprom.h`: new function `uint8_t *WSwan_GetInternalEEPROM(void)` returning the
  internal EEPROM array, so the host can save it (upstream keeps it static and only persists the cartridge memory).
- `mednafen/wswan/gfx.c`, port 0xA0 read: the mono value is 0x85 (was 0x86). Port 0xA0 is documented as `C??? swcl`
  (ws.nesdev.org I/O port map) with the Colour flag at bit 1 (0x02). 0x86 had that bit set, so mono cartridges
  read as Colour hardware, and the spritepriority test (MiSTer testroms, which tests the same bit with WS_COLOR=0x02) locks
  the CPU in that case; with 0x85 it runs. The Colour value stays 0x87. Not verified against a real mono console.

## Build notes (not changes to files)

- Compile with `-DMEDNAFEN_VERSION_NUMERIC=0x00093501` (state.c needs it; beetle uses 931).
- `mednafen/wswan/rtc.c` is compiled with `-include wswan/frontend/time_shim.h`, which maps `time()` to the
  host's clock hook (`wswan_host_set_time_source`).
- The host defines the globals libretro.c defined for the core (`wsc`, `rom_size`, `WSButtonStatus`, `StateAction`
  glue, ...).
