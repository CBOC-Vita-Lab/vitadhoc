# Technical Notes

## Architecture

`gpsp/vita/Makefile` builds the native Vita app. gpSP supplies the GBA core
and ARM dynamic recompiler; TGB Dual supplies GB/GBC. The common frontend
owns cartridges, saves and link state. The Vita session loop is
`gpsp/vita/main_vita.c`, with separate network, local-pair, cheat, audio and
rendering modules. Host twins compile much of the same code for tests.

The Vita frontend reuses the PSP menu/settings logic through a compatibility
layer, but uses GXM/libvita2d, embedded Inter/Outfit fonts, Vita audio/input,
front touch and Vita networking. It does not run a PSP executable inside an
emulator. Original gpSP, GBAdhoc and dependency notices remain in the source.

## Display And Timing

- Per-console settings include 2x/3x, fit, stretch and 4x crop; nearest,
  bilinear, sharp bilinear, LCD, scanline, Scale3x and CPU xBRZ filters;
  colour correction and motion blending.
- Scale3x/xBRZ use worker cores, changed regions and a scrolling image cache.
  During heavy fades, smoothing can fall back to 2x inside existing textures.
- A GBA render worker draws from recorded scanline state. The main thread
  joins it before presentation. Two-player paths retain their own timing.
- Keep full speed can run up to two catch-up frames without drawing before
  a presented frame. Emulated frames, submitted pictures and actual displayed
  FPS are different measurements; a log counter alone is not a benchmark.
- Presentation waits before input sampling to avoid filling the display queue.
  Audio starts with a 60 ms cushion and can adapt up to 120 ms after underruns.
- Experimental GPU xBRZ is off by default: earlier Vita measurements were
  slower than CPU xBRZ. It needs the user's own shader compiler module and has
  a slow-frame fallback. No proprietary compiler is distributed.

GBA Wide view can draw beyond the original viewport using game profiles,
Mode 7 support or an experimental background cache/scout. Emerald has prior
testing; other profiles and generic/scout behaviour are not universally
accepted. Where information is unavailable, margins use the selected fallback.
It does not turn all games into native widescreen games or modify their saves.

## Build 21

The functional change is external ROM-root discovery and `extra_roms`, with
case-insensitive root deduplication, hidden-file exclusion, bounded scans and
saves beside external games. The publishing copy corrects the LiveArea
generator target and adds readable notices. It does not silently introduce a
new emulation engine or remove upstream features.

## Evidence

The recovered build-21 executable is retained in the BIOS-free repack. Source
tests and archive inspection are not a physical-device result. The publishing
copy passes its cartridge database test and both x86/ARM cheat unit/end-to-end
suites; the Vita source build and cheat compile check succeed. Earlier build-20
notes report approximately 59.94 FPS in one Emerald walk at 500 MHz, including
heavy display settings; they do not establish current-build performance or
other-game compatibility. No timing result from Vita3K or Linux is presented
as a Vita measurement. [Known Issues](KNOWN-ISSUES.md) tracks acceptance gaps.

Diagnostic switches include `drc = 0`, `render_thread = 2` for comparison,
`gpu_xbrz = 1` for experiments and `clips = 0` / `art = 0` to disable optional
work. Leave ordinary defaults in place when reporting normal behaviour.
Logs under `ux0:data/GBAdhoc/log/` may contain ROM paths and network addresses;
review them before sharing. The package does not install a clock/plugin profile.
