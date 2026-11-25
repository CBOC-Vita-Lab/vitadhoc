# Provenance of gpsp/gbcore/gambatte

- **Upstream:** Gambatte, the libretro fork, <https://github.com/libretro/gambatte-libretro> (Gambatte by Sindre
  Aamas and contributors), the `libgambatte/` directory and `common/`.
- **Commit:** `138bcbbf311300a271896bf93a76370c72cab339`.
- **Licence:** GNU General Public License **version 2 only** (the source headers say "version 2", not "or
  later"). `COPYING` is upstream's, shipped as `licenses/Gambatte-GPL-2.0.txt`. Because the main program links this
  core (as one relocatable object, `gambattecore.o`, that exports only `gambatte_host_*`), the main program as a
  whole is distributed under GPL-2.0 for this core's part; see `LICENSING.md`.
- **Built:** `gpsp/vita/gambattecore.mk`, as C++11 with `-fno-exceptions -fno-rtti`, `-DHAVE_NETWORK` (the serial
  port hook the Game Boy Printer and the test suites use), `-DVIDEO_RGB565`, `-D__LIBRETRO__`. Our host,
  `gpsp/gbcore/gambatte_host.{h,cpp}` (C face, audio filter, battery and state formats, cheats), is ours
  (GPL-2.0-or-later) and is linked into the same object.

## What is here

`libgambatte/src`, `libgambatte/include`, `common/*.h` and `COPYING`: upstream's files, unchanged except those
named below. Nothing of upstream's libretro front end (`libretro/`), its shaders or its build files is here.

## Local patches (all marked `gbadhoc` or `gambatte_wall_time` in the source)

1. **Wall clock.** `std::time(0)` in `mem/rtc.cpp`, `mem/huc3.cpp` and `initstate.cpp` is `gambatte_wall_time()`
   (declared in `libgambatte/include/gbadhoc.h`, defined in `gambatte_host.cpp`): the host's clock as UTC, so the
   real-time clock follows the same clock and the same "no clock" rules as the other core.
2. **Colour.** `gbcToRgb32` in `video_libretro.cpp` calls `gambatte_gbc_pixel()`: the Game Boy Color palette entry
   goes through the app's colour model (the same tables the other core draws with) or a plain expansion.
3. **Access for the host.** `gambatte.h/.cpp`, `gambatte-memory.h`, `mem/cartridge.{h,cpp}`,
   `mem/cartridge_libretro.cpp`, `mem/rtc.h`, `mem/huc3.h`: accessors for the CPU's view of memory (cheats,
   cheat search, the achievements memory map), cartridge RAM and ROM (the battery image, Game Genie), the
   clock (`GbadhocRtc`), VRAM and the CPU registers.
4. **No libretro.h.** `mem/cartridge_libretro.cpp` and `video_libretro.cpp` do not include libretro.h;
   `compat/gambatte_log.h` stands in for the log calls (it logs nothing).
