# Sega Mega Drive / Genesis, Sega CD, Master System, Game Gear, SG-1000: Genesis Plus GX (separate program)

Sega games run in `md.bin`, a separate program in the same package. It is not part of `eboot.bin` and shares no code with it.
VitAdhoc starts it with the game's path and it starts VitAdhoc again when you quit.

- **Core:** Genesis Plus GX (Charles MacDonald's Genesis Plus, continued by Eke-Eke),
  <https://github.com/ekeeke/Genesis-Plus-GX>, in `md/core`. Its licence is the Genesis Plus GX licence
  (`licenses/GenesisPlusGX-LICENSE.txt` in the package, `md/core/LICENSE.txt` in the source). It allows use and changes
  for non-commercial purposes only. It is not the GPL and cannot be combined with GPL code in one program, which is why it
  is a program of its own. Selling it, bundling it in a commercial product or activity is not allowed. A modified
  redistribution must include the complete source of everything the binary is built from: `md/` is that source, and
  `md/core/PROVENANCE.md` lists every change to the upstream files (pinned commit `b9c2316441978d4015ba30e9a95d9b0e6f8ee981`).
- **Included with it (all inside the core's licence file):** Nuked OPN2 (YM3438) and Nuked OPLL, LGPL-2.1
  (`licenses/LGPL-2.1.txt`), used only when the optional "enhanced FM" is chosen; the 68000 core is Karl Stenerud's Musashi;
  the Z80 core is Juergen Buchmueller's (MAME); the YM2612 and YM2413 cores are based on Jarek Burczynski and Tatsuyuki Satoh's
  (MAME); the sound resampler is blip_buf by Shay Green (LGPL-2.1). Not included: the NTSC filters and the MP3 (minimp3) decoder.
- **Sega CD disc readers (in `md.bin`):** libchdr (BSD-3-Clause) with its codecs zstd (BSD-3-Clause, used under that licence), LZMA SDK
  (public domain), zlib 1.2.11 (zlib licence) and dr_flac (public domain / MIT-0), for `.chd` discs; Tremor (BSD-3-Clause) for Ogg audio
  tracks. All permissive, so they combine with the Genesis Plus GX licence and with the MIT code `md.bin` links. Their licence texts are in
  `licenses/SegaCD-disc-readers.txt`; the sources are in `md/core/cd_hw/libchdr` and `md/core/sound/tremor`.
- **Our code in md/:** `md/frontend` and `md/twin` are new code (Copyright CBOC-Vita), MIT licence, linked into `md.bin` only.
  One exception: the disc tray's open and close and the disc swap in `md/frontend/host.c` (`md_host_cd_eject`, `md_host_cd_close`,
  `cd_put_disc`, for `.m3u` games, 2.0) follow the disc control of Genesis Plus GX's libretro port (`libretro/libretro.c`,
  `disk_set_eject_state` and `disk_set_image_index`; Copyright Eke-Eke (2007-2022) and Daniel De Matteis (2012-2016)), which is under
  the same Genesis Plus GX licence as the core; those lines stay under that licence.
- **Sega CD / Mega CD:** supported, with the player's own BIOS (`us_scd1_9210.bin`, `eu_mcd1_9210.bin`, `jp_mcd1_9112.bin` or another dump of
  the console's boot ROM, in `ux0:data/GBAdhoc/bios` or beside the game). The BIOS is the player's, never part of the package or the source.
- **Not supported:** Sega 32X, Pico, Mega Drive add-on hardware beyond the cartridge and the Sega CD. No BIOS file is needed or used for any
  other system.
- No game, BIOS or save file is shipped. Games are the user's own.
