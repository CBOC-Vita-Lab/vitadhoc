# VitAdhoc

**Game Boy Advance, Game Boy and Game Boy Color, native on PS Vita.**

Your games, together on a home screen with box art, a little history and
previews of your own play. Pick up where you left off, smooth the pixels or
keep them crisp, and trade with a friend. VitAdhoc brings the library and the
handheld experience together, with touch controls and the Vita's own sensors.

**[Download VitAdhoc 1.2](https://github.com/CBOC-Vita-Lab/vitadhoc/releases/latest/download/VitAdhoc.vpk)**
| [Install or update](INSTALL.md) | [All features](docs/FEATURES.md)
| [Performance and hardware evidence](docs/PERFORMANCE.md)

![The VitAdhoc home screen on a physical PS Vita](store/screenshots/vita-1.2/home.png)

*Captured on a physical PS Vita running the 1.2 release executable.*

You'll need your own games. An open-source GBA BIOS is built in; no BIOS
download is required. Updating from 1.0 or 1.1? Install the VPK over the old
version. Keep your games, saves, settings and artwork where they are.

## New In Version 1.2

**Bring your saves, your folders and your zips.**

- **Bring an existing save.** Pick up a RetroArch battery save automatically,
  or put one in the `import` folder. An existing VitAdhoc save is not replaced.
- **Play zipped games.** Supported `.zip` files appear under the right console.
- **Choose your game folders in Settings.** Add or remove folders without
  editing a configuration file.
- **Launch straight into a game** from RetroFlow or another launcher.
- **Ten save-state slots**, up from five, with previews.
- **Make the screen your own.** PNG overlays, adjustable LCD/scanline strength
  and a screenshot folder of your choice.
- **Graphics and rewind work.** Safer texture handling, recovery from graphics
  errors and less repeated work in the rewind and smoothing paths.

[Release changelog](CHANGELOG.md) | [Setup and limitations](docs/FEATURES.md)

## Features

### A library made for the Vita

Browse with buttons or touch. Box art and title screens download on Wi-Fi;
your own pictures take priority. Built-in descriptions cover about 1,300
games. Star favourites, search by name, check Recent and play time, or rest on
a game to watch the last ten seconds you recorded.

Continue returns to an automatic resume point. Start over uses the game's
battery save instead. Existing `ux0:ROMS/` and RetroFlow folders can stay in
place, alongside folders you add yourself.

### More ways to see and play

- Original pixels, bilinear and sharp-bilinear scaling, CPU xBRZ or Scale3x.
- Colour correction, motion blending, LCD/scanline looks, screen sizes,
  Super Game Boy borders and your own PNG overlays.
- Wide view with profiles for supported games, including the original
  Pokemon Emerald family. It is not automatic widescreen for every ROM hack.
- Rewind, fast forward, ten save-state slots, turbo buttons and screenshots.
- IPS, BPS and UPS soft patches, without changing the original game file.
- RetroAchievements, including Hardcore mode for supported games.
- Motion and sunlight cartridge support: Twisted!, Yoshi, Koro Koro,
  Kirby Tilt 'n' Tumble and Boktai, with sensor or stick settings.

### Play together

GBA Link Cable and Wireless Adapter support, plus GB/GBC linking: two Vitas
over Wi-Fi, internet or ad hoc, or two emulated handhelds side by side on one
Vita. Save/state sharing and a Mystery Gift receiver are included too.

Vita-to-PSP links are not supported. Two-device, controller and phone-to-game
flows still need broader hardware validation; see [Play Together](PLAY-TOGETHER.md)
and [Known Issues](KNOWN-ISSUES.md) before relying on them for an important save.

## Performance

Performance depends on the game, scene, display settings and clock profile.
The emulator targets the original handheld timing; that is not a promise
that every game or ROM hack runs at full speed with every option enabled.

| Evidence | What it establishes |
| --- | --- |
| Historical 1.0 Vita run | Pokemon Ruby was reported at 59.9 emulated fps with CPU xBRZ and wide view enabled. This is not a 1.2 or Pokemon Unbound benchmark. |
| 1.2 host tests | Exercise the changed rewind, smoothing, graphics and feature paths. Computer results do not measure Vita gameplay speed. |
| Current hardware checks | Package hashes, settings, scenes, measurements and capture provenance are recorded in the [performance report](docs/PERFORMANCE.md). |

**Pokemon Unbound 2.1.1.1:** three repeat samples of the opening beach scene
averaged about **60 emulated fps** at full-screen CPU xBRZ 3x, with about
**44 frontend-produced frames/s**. This is not 60 measured visible frames
per second or a full-game guarantee; the report includes exact results,
audio underruns and exploratory slowdown observations.

Keep full speed may run catch-up frames without drawing each one. Emulated
fps, submitted pictures and displayed frames are different measures. GPU
xBRZ is experimental and off by default; CPU xBRZ is the normal smoothing
path. The [full feature guide](docs/FEATURES.md) explains the tradeoffs.

## Bug Fixes Since 1.0

- Recovery from a graphics error that could leave the picture frozen.
- Safer smoothing and staging textures, deferred GPU frees and fallback
  handling when texture allocation fails.
- Audio output fixes carried forward from 1.1: persistent buffers, corrected
  GBA direct-sound stereo routing, timing, fades and state-load cleanup.
- Accented Latin names fold to readable letters in the menu font.
- Safer settings writes when a memory card refuses a rename or write.

See the [changelog](CHANGELOG.md) for the player-facing changes and
[technical notes](docs/TECHNICAL.md) for the implementation details.

## Install

You need a homebrew-enabled PS Vita, VitaShell and your own games.

1. Transfer `VitAdhoc.vpk` with VitaShell over USB or FTP, then install it.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`.
3. Add games to `ux0:data/GBAdhoc/roms/`, use an automatically detected folder,
   or choose **Settings > Library > Game folders**.
4. Pick a game and press Cross. Triangle switches GBA, GB and GBC.

For the desktop installer, update precautions, save import, launchers and
every folder, use the [manual install guide](INSTALL.md).

## Playing

| Button | Default action |
| --- | --- |
| Cross / Circle | A / B |
| Hold Start + Select, or tap the game | Open the menu |
| Square | Fast forward |
| Right stick left, or Select + Square | Rewind |
| Select + L / Select + R | Quick save / quick load |
| L + R + Select | Screenshot |
| Start on the home screen | Settings |

[All controls](CONTROLS.md) | [Detailed features and settings](docs/FEATURES.md)
| [Troubleshooting](KNOWN-ISSUES.md)

## Screenshots

Hardware captures and their settings are documented in the
[performance report](docs/PERFORMANCE.md).

| Pokemon Unbound 2.1.1.1: opening scenes on a PS Vita | Full-screen, CPU xBRZ 3x |
| --- | --- |
| ![Unbound's ice cavern introduction on the Vita](store/screenshots/vita-1.2/unbound-intro-ice.png) | ![Unbound's bridge introduction on the Vita](store/screenshots/vita-1.2/unbound-intro-bridge.png) |

These are unaltered display captures apart from lossless PNG encoding and
metadata removal. The in-app FPS overlay was turned off for the screenshots.
They establish the pictured scenes, not a full-game compatibility guarantee.

The following images are from the
computer test build and use the project's own test cartridges, not commercial
gameplay or physical-Vita captures.

| Continue | Save states |
| --- | --- |
| ![Continue on the test-build home screen](store/screenshots/continue.png) | ![Test-build save-state previews](store/screenshots/save-states.png) |
| **Display settings** | **Two players** |
| ![Test-build display settings](store/screenshots/display-settings.png) | ![Two synthetic test cartridges side by side](store/screenshots/two-players.png) |

## Download Contents

| File | Contents |
| --- | --- |
| `VitAdhoc.vpk` | The app to install on a Vita. |
| `VitAdhoc-install.zip` | The same VPK, manual, checksums and complete source. |
| `cboc-release.json` | Package metadata for the desktop installer. |
| `SHA256SUMS` | Checksums for the extracted source/install files. |

## For Developers

[Building and tests](docs/BUILDING.md) | [Technical notes](docs/TECHNICAL.md)
| [Source record](docs/SOURCE.md) | [Check a download](docs/CHECK-DOWNLOAD.md)
| [Licensing](LICENSING.md) | [Assets](docs/ASSETS.md)
| [Third-party notices](notices/THIRD-PARTY.md)

The native frontend is in `gpsp/vita/`; emulator cores and shared frontend
code are in `gpsp/`. Host test builds are useful verification tools, not a
substitute for checking the release package on a physical Vita.

## Credits

VitAdhoc builds on [GBAdhoc by ShoshinFauteux](https://github.com/ShoshinFauteux/GBAdhoc).
Thank you for the emulator, link-play work and original save formats this
project builds on. The GBA core is gpSP by Exophase and the libretro
contributors; GB/GBC uses TGB Dual. RetroAchievements uses
[rcheevos](https://github.com/RetroAchievements/rcheevos).

Built by AI agents (Claude), directed and play-tested by a human. See
[Credits](CREDITS.md) for the other projects and contributors.

An unofficial fan project, not affiliated with Nintendo, Sony or
RetroAchievements. No commercial games or proprietary BIOS are supplied.
