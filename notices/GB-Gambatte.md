# Game Boy: Gambatte (the Accurate core)

Game Boy and Game Boy Color games can run on Gambatte, chosen in Settings (Game Boy core: Standard / Accurate, and
per game under This game). TGB Dual stays the default and the core for link play. Gambatte is linked into the main
program as the single relocatable object `gambattecore.o`, whose only exported symbols are the `gambatte_host_*`
functions of `gpsp/gbcore/gambatte_host.h`.

- **Core:** Gambatte, libretro fork, <https://github.com/libretro/gambatte-libretro>, Copyright (C) 2007 Sindre Aamas
  and contributors, in `gpsp/gbcore/gambatte`. **GPL-2.0 only** (`licenses/Gambatte-GPL-2.0.txt`,
  `gpsp/gbcore/gambatte/COPYING`). Commit and the local patches: `gpsp/gbcore/gambatte/PROVENANCE.md`.
- **Our code:** `gpsp/gbcore/gambatte_host.{h,cpp}` (the C face, audio resampler, battery and state formats) is
  GPL-2.0-or-later (Copyright CBOC-Vita); `gpsp/gbcore/tests/suites_gambatte.cpp` and `gambatte_core.cpp` are
  test code.
- **Effect on the licence of the program:** the main program links this core, so the combined program is
  distributed under GPL-2.0 (a version-2-only part is in it); see [Licensing Status](../LICENSING.md). The experimental GPU xBRZ shader, whose
  GPL-3.0 notice could not be combined with a version-2-only part, was removed from the main program in 2.0.
