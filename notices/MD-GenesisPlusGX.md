# Sega Mega Drive / Genesis, Master System, Game Gear, SG-1000: Genesis Plus GX (separate program)

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
  (MAME); the sound resampler is blip_buf by Shay Green (LGPL-2.1). Not included: the NTSC filters, the Ogg Vorbis (tremor)
  and MP3 (minimp3) decoders and the CHD library, which only Sega CD or the filters used.
- **Our code in md/:** `md/frontend` and `md/twin` are new code (Copyright CBOC-Vita), MIT licence, linked into `md.bin` only.
- **Not supported:** Sega CD / Mega CD, Sega 32X, Pico, Mega Drive add-on hardware beyond the cartridge. No BIOS file is needed or
  used for any supported system.
- No game, BIOS or save file is shipped. Games are the user's own.
