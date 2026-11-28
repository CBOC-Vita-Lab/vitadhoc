# VitAdhoc 2.0.1: SNES, Mega Drive / Sega CD, and Faster Cover Downloads

**VitAdhoc 2.0.1** is a maintenance and performance release addressing early 2.0 community reports on Super NES slowdowns and audio, Mega Drive and Sega CD sound glitches, and broken or slow box art downloads.

All changes were verified on a physical PS Vita in both 4-core and 3-core (stock, no capunlocker) modes, plus full host and ARM NEON test suites.

## What's Fixed

### Super NES (`snes.bin`)

- Fixed a priority-inversion spinlock in `Writer::work_one()` where `speed_pack()` ran while holding the queue spinlock, stalling the emulation thread on rewind writes. Added a lock-free `done_n_` fast-path in `take_done()` and NEON-accelerated `delta_encode()` in PlayerKit rewind.
- Raised SNES rewind capture interval (`interval_min` from 4 to 10 frames) to cut background memory-copy pressure.
- Held `Quick response: Auto` off automatically when the 4th CPU core is unavailable (3-core stock Vita), preventing 1-frame run-ahead from consuming the 3-core frame budget.
- Fixed `hd_draw_thread` VBlank timing so normal display swaps are excluded from draw load while GPU-full retry waits stay counted.
- Fixed `PACE_HALF` skipped frames so they no longer emit duplicate `K_FRAME_END` copies while still polling `S9xPPUMTVideoPoll` on odd frames and presenting rewind landing frames immediately.
- Tuned the SNES audio pacer (`0.75` priming cushion, smoothed base rate tracking) to absorb frame jitter without underruns.
- Tightened `LookGovernor` step-down from 7 seconds to 3 seconds (`down_s = 2`, `force_s = 1`) so heavy scenes shed `xBR 2x` quickly when overloaded.

### Mega Drive, Sega CD, Master System, Game Gear, and SG-1000 (`md.bin`)

- Removed `md_writer` from the 150 ms Core 3 watchdog so only `md_audio` is monitored. Previously, a normal rewind write tripped the watchdog seconds into gameplay, collapsing 4-core Vitas down to 3-core mode and causing audio crackle.
- Fixed the same `Writer::work_one()` spinlock, added NEON rewind delta encoding, enabled deferred rewind capture (`capture_idle()`), and raised the Sega CD rewind capture interval to 10 frames.
- Applied the same `hd_draw_thread` VBlank accounting fix, audio pacer priming cushion, and 3-second `LookGovernor` step-down.

### Box Art Recovery and Throughput (`eboot.bin`)

- Added an automatic HTTPS mirror fallback to `raw.githubusercontent.com/libretro-thumbnails/...` over VitAdhoc's bundled Mbed TLS 3.6.8 client (`np_https`) whenever `thumbnails.libretro.com` fails or returns 404 across all 11 systems.
- Upgraded `.missing.txt` from `v3` to `v4` so misses recorded during the `thumbnails.libretro.com` outage are cleared automatically on first launch.
- Fixed TLS 1.3 keep-alive reuse in `np_https` by draining post-handshake `NewSessionTicket` records instead of closing and re-handshaking on every request, and added a 4-entry 5-minute DNS IPv4 cache.
- Overlapped next-cover HTTPS prefetch on Core 2 (`np_https`) with PNG saving, decoding, and thumbnail generation on Core 1 (`gbadhoc_bg`), keeping Core 0 free for the 60 Hz Home screen.
- Eliminated 6 linear FAT32 `stat()` scans per cover on Vita when the in-memory folder inventory is valid, replaced chunked stdio writes with direct single-syscall `open`/`write`/`close` on Vita, and compiled `artfetch.o` and `stb_impl.o` at `-O3 -ftree-vectorize`.
- Added fast O(1) RAM skip loops for already-cached and already-missing library entries, winning-candidate reuse for title screens (`AK_TITLE`), thread-safe slice-by-4 CRC32 (`gamedb.c`), 250 ms cached online checks, and automatic cover pausing during Online Play and local Link sessions.

### Online Room Service (`online/service/`)

- Handled Durable Object throws in `worker.js` with structured JSON `503` responses (`over_budget` for daily quota limits, `unavailable` with short retry for resets or transient errors) instead of uncaught HTML 1101 errors.

## Real PS Vita Hardware Verification

Measured on a physical PS Vita (both 4-core Auto and forced 3-core stock layout):

| Area | Before (2.0) | After (2.0.1) |
| --- | --- | --- |
| Mega Drive Core 3 watchdog (`Gunstar Heroes`, `Sonic 2`) | Tripped at `t=4s` every run | **0 watchdog trips**, `writer` CPU `886 -> 25 ms/s` (35x lower) |
| Mega Drive 4-core & 3-core (`Gunstar Heroes`) | Audio crackle, 30 fps stalls | **Locked 59.8-60.0 fps**, `over=0`, `max=6.8-11.3 ms`, **0 audio underruns** |
| SNES 3-core (`Rock 'n' Roll Racing`, `Super Mario RPG`) | Slowdowns, audio issues | **Locked 59.9-60.0 fps**, `over=0`, `max=3.2-14.9 ms`, **0 audio underruns** |
| SNES 4-core (`Super Mario RPG` SA-1) | 7 s LookGovernor delay | **Locked 59.9-60.0 fps**, 3 s LookGovernor step-down, **0 audio underruns** |
| Live Wi-Fi box art (713-ROM Genesis shelf) | 0 covers (404 outage, poisoned `v3` misses) | Auto-recovered via GitHub HTTPS mirror (`1,368-1,452 ms/cover`, down from `2,019 ms/cover` initial) |
| Home menu SD I/O during live cover downloads | `stat=528 ms`, `read=1,426 ms`, `max_io=244 ms` | **`stat=0.80 ms` (660x faster), `read=97.4 ms` (14.6x faster), `max_io=19.5 ms` (12.5x faster)** |

## Install or Upgrade

- **Upgrading from 2.0 or 1.x:** Close VitAdhoc and install `VitAdhoc.vpk` directly over your existing bubble (`GBAD00001`) using VitaShell or the [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest). Do not uninstall first. Saves, save states, covers, and settings remain intact, and previously poisoned missing-cover caches clear automatically.
- **Online compatibility:** Online Play room codes are compatible with 2.0 and 2.0.1.

### Release Assets

- **`VitAdhoc.vpk`** - Ready-to-install PS Vita package (`TITLE_ID: GBAD00001`, `APP_VER: 02.01`, BIOS-free and ROM-free).
- **`VitAdhoc-install.zip`** - Full bundle containing `VitAdhoc.vpk`, source code, documentation, and checksum verification scripts.
- **`cboc-release.json`** - Manifest for the CBOC Vita Installer (supports in-place upgrade from 1.0, 1.1, 1.2, 1.3, 1.3.1, and 2.0).
- **`SHA256SUMS`** - Complete SHA-256 manifest for every tracked file in the release bundle.

SHA-256 `VitAdhoc.vpk`:
`8075d120b96121f2c6076a3105edb5ad095d89d262d4e5bad974acb46bb63a31`
