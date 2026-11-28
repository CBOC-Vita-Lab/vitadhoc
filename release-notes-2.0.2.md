# VitAdhoc 2.0.2: WonderSwan 60 FPS, Scan Cancel, SNES/Sega Stalls, and GBA Hack Fixes

**VitAdhoc 2.0.2** is a maintenance and performance release addressing post-launch community reports on WonderSwan 30 fps pacing, Circle cancellation during automatic ROM scans, remaining SNES and Sega handoff stalls, and expanded GBA Pokémon decompilation ROM hacks.

All changes were verified on a physical PS Vita in both 4-core and 3-core (`core4=0` stock) modes, plus full host, twin, and ARM Cortex-A9 NEON test suites.

## What's Fixed

### WonderSwan 60 FPS (`eboot.bin`, `wswan/`)

- Added a WonderSwan steady-pacing profile (`steady_config_wswan()`) tuned for WonderSwan's 75.47 Hz -> 60 Hz cadence (~26% double-stepped `n=2` refreshes), stopping `Steady Pacer` from trapping titles like *Klonoa* at 30 fps (`PACE_HALF`).
- Tightened `LookGovernor` startup step-down (`down_s = 2`, `down_start_s = 2`, `force_s = 1`) and prevented skipped frames from feeding stale framebuffers into the frame blender (`wscore.c`).
- Delivered a bit-exact **30–41% speedup** in Beetle WonderSwan (`gfx.c`, `v30mz.c`, `wswan-memory.c`, `sound.c`, `interrupt.c`): 16-bank direct memory reads, inlined low-RAM writes, `ws_irq_pending` guard, zero-delta `Blip_Synth` skip, tile-cache/palette range guards, precomputed mono palette LUTs, and per-scanline sprite buckets — verified 100% bit-exact x86 vs ARM Cortex-A9 NEON.

### Super NES & Sega Handoff Stalls and `LookGovernor` (`snes.bin`, `md.bin`, `playerkit`)

- Bounded `Handoff::begin_write()` timeouts accurately on spurious wakeups (`pk_handoff.cpp`) and reduced Vita `gate_timeout_us` from 250 ms to 22 ms across `snes.bin`, `md.bin`, and `eboot.bin`, so heavy `xBR 2x` frames drop a stale queued frame instead of stalling emulation for 34–50 ms and draining audio.
- Fed `missed_refr` and `gate_timeouts` into `LookGovernor`'s missed-refresh count in `snes.bin` and `md.bin` so handoff stalls are never hidden when `PACE_HALF` ran part of the second.
- Counted over-budget drawn frames across `half <-> full` pacing bounces, reported blocking peak scaler cost to prevent flapping back up, and tightened `down_start_s = 2`.

### Circle Cancel During Automatic ROM Scans (`eboot.bin`)

- Added 16-sample chronological `pspcompat_pad_scan_cancel_begin()` / `pspcompat_pad_scan_cancel()` polling in `pspcompat.c` with a held-at-entry release guard, face-swap support, and controller-2 support.
- Enabled immediate interactive scan UI on cold boot and external-player return (`snes.bin` / `md.bin`), polled Circle on every `sceIoDread()`, before `sceIoDopen()`, and after slow archive peeks, stopped caching cancelled partial scans, suppressed stray post-cancel browser edges, and prevented `vita_browser_others()` from scanning other consoles after cancellation.

### GBA Decompilation ROM Hacks & Covers (`eboot.bin`)

- Cleared hardcoded vanilla ROM addresses (`idle_loop_target_pc` and `translation_gate_targets`) from `gba_over.h` whenever `is_hack` is detected in `gpsp/gba_memory.c`, preventing gpSP's dynarec idle-loop eliminator from firing on unrelated live code in 32 MiB `pokeemerald-expansion` / `pokefirered` hacks while keeping official carts untouched.
- Added `.mark3` (Master System) and `.sg1000` (SG-1000) to `af_ext_sys()` in `gpsp/vita/artfetch.c` for parity with `md.bin`.

## Real PS Vita Hardware Verification

Measured on a physical PS Vita (both 4-core Auto and forced 3-core `core4=0` stock layout):

| Area | Before (2.0.1) | After (2.0.2) |
| --- | --- | --- |
| WonderSwan 64-sprite stress (4-core & 3-core) | Trapped at 30 fps (`PACE_HALF`) | **Locked 60.0 fps** (`fps_x1000=60106/60112`, `half_passes=0`, `60/60` shown, **0 underruns**) |
| SNES *Yoshi's Island* (4-core, `xBR 2x` Auto) | 46.4 fps, `42.3 ms` stalls, `LookGovernor` blind | **Locked 59.9–60.0 fps** (eased in 2 s, `over=0`, `max=4.8–4.9 ms`, **0 underruns**) |
| SNES *Rock 'n' Roll Racing* (4-core & 3-core) | `30 <-> 60` oscillation; `50.5 ms` 3-core stalls | **Locked 59.9–60.0 fps** (4-core); 3-core `wmax=17.1 ms`, `fill=49.4–84.2 ms`, **0 underruns** |
| Mega Drive *Gunstar Heroes* (4-core & 3-core) | `34.3 ms` spike stalls | **60.0 fps** under `xBR 2x`; burst spikes eased in 2 s (`up_need=1.10`, **0 underruns**) |
| Cold boot library scan (2,887 ROMs) | Un-cancellable scan on return / missed taps | **15 ms first frame**, 76 interactive frames, 16-sample chronological Circle cancel |

## Install or Upgrade

- **Upgrading from 2.0.1, 2.0, or 1.x:** Close VitAdhoc and install `VitAdhoc.vpk` directly over your existing bubble (`GBAD00001`) using VitaShell or the [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest). Do not uninstall first. Saves, save states, covers, and settings remain intact.
- **Online compatibility:** Online Play room codes are compatible with 2.0, 2.0.1, and 2.0.2.

### Release Assets

- **`VitAdhoc.vpk`** - Ready-to-install PS Vita package (`TITLE_ID: GBAD00001`, `APP_VER: 02.02`, BIOS-free and ROM-free).
- **`VitAdhoc-install.zip`** - Full bundle containing `VitAdhoc.vpk`, source code, documentation, and checksum verification scripts.
- **`cboc-release.json`** - Manifest for the CBOC Vita Installer (supports in-place upgrade from 1.0, 1.1, 1.2, 1.3, 1.3.1, 2.0, and 2.0.1).
- **`SHA256SUMS`** - Complete SHA-256 manifest for every tracked file in the release bundle.

SHA-256 `VitAdhoc.vpk`:
`323a4b10046b33bc60d5ba2778b907abbd388059c77b62f62876eee07991ddaf`
