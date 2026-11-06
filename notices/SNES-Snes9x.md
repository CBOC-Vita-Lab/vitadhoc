# SNES: Snes9x (separate program)

SNES games run in `snes.bin`, a separate program in the same package. It is
not part of `eboot.bin` and shares no code with it. VitAdhoc starts it with the
game's path and it starts VitAdhoc again when you quit.

- **Core:** Snes9x 1.63 (https://github.com/snes9xgit/snes9x), in `snes/core`.
  Its licence is the Snes9x licence (`licenses/Snes9x-LICENSE.txt` in the
  package, `snes/core/LICENSE` in the source). It allows copying and changing
  for non-commercial purposes only. It is not the GPL and cannot be combined
  with GPL code in one program, which is why it is a program of its own.
  Charging money for it, bundling it in a commercial product, or using it to
  promote one is not allowed without the authors' permission.
- **Included with it:** the sound DSP in `snes/core/apu/bapu/dsp` (snes_spc,
  Shay Green, LGPL 2.1) and the other third-party parts named in the Snes9x
  licence file.
- **Our code in snes/:** `snes/frontend`, `snes/twin` and `snes/vita` are new
  code, MIT licence (Copyright CBOC-Vita), written against Snes9x's public
  interface. It includes libvita2d (MIT), stb (MIT or public domain) and the
  Inter font (SIL OFL 1.1), each with its notice beside it.
- Provenance and the few local edits to the core: `snes/core/PROVENANCE.md`.

VitAdhoc, as a package containing Snes9x, must stay free of charge and
non-commercial.
