# NES: FCEUmm

NES and Famicom games run on FCEUmm, linked into the main program as the single relocatable object `nescore.o` (like the
Game Boy core). Its only exported symbols are the `nes_host_*` functions of `nes/frontend/host.h`.

- **Core:** FCEUmm (libretro-fceumm, derived from FCE Ultra / FCEUX),
  <https://github.com/libretro/libretro-fceumm>, in `nes/core`. GPL-2.0-or-later
  (`licenses/FCEUmm-GPL-2.0.txt`, `nes/core/LICENSE`, authors in `nes/core/AUTHORS`). Commit, files taken and
  the three small local edits: `nes/core/PROVENANCE.md`. Source: `nes/`.
- **Included with it:** libretro-common headers and `memory_stream`, MIT (`licenses/libretro-common-MIT.txt`).
- **Our code in nes/:** `nes/frontend` and `nes/twin` are new code, MIT (Copyright CBOC-Vita). The program that links
  `nescore.o` is distributed under the GPL, as the rest of the main program's GPL parts already are.
- **Disk System BIOS:** not included and not needed for cartridges. Famicom Disk System games need
  `disksys.rom` supplied by the user; it is never shipped.
- `nes/vita` holds an earlier standalone player (`nes.bin`, a separate program using the same core and the SNES program's
  MIT audio pacer and libvita2d, stb and Inter font). It is not part of the in-process route.
