# VitAdhoc

**Game Boy Advance, Game Boy, Game Boy Color and Super NES, native on PS Vita.**

Your games, together on a home screen with box art, a little history and
previews of your own play. Pick up where you left off, smooth the pixels or
keep them crisp, and trade with a friend. VitAdhoc brings the library and the
handheld experience together, with touch controls and the Vita's own sensors.

**[Download VitAdhoc 1.3](https://github.com/CBOC-Vita-Lab/vitadhoc/releases/latest/download/VitAdhoc.vpk)**
| [Install or update](INSTALL.md) | [All features](docs/FEATURES.md)
| [Performance](docs/PERFORMANCE.md)

![The VitAdhoc home screen on a physical PS Vita, with the new SNES shelf](store/screenshots/vita-1.3/home-snes.png)

*The home screen on a PS Vita running VitAdhoc 1.3, with the new SNES shelf.*

You'll need your own games. An open-source GBA BIOS is built in; no BIOS
download is required. Updating from 1.0, 1.1 or 1.2? Install the VPK over the
old version. Keep your games, saves, settings and artwork where they are.

## New In Version 1.3

**Super NES joins the family.** Your SNES games sit on the same home screen,
with box art, title screens and a few lines about each game, and they play at
full speed with every frame shown.

- **Play Super NES games.** `.sfc` and `.smc` games (zipped too) appear on a
  new SNES shelf, found in `ux0:ROMS/` (any folder inside it), RetroFlow's
  folders and your own folders.
- **Accurate, and spread over the Vita's cores.** Built on Snes9x 1.63: sound
  and picture each run on a core of their own, with exactly the same result.
  Super Mario World, Donkey Kong Country, Chrono Trigger and Yoshi's Island
  need about a third to a half of each frame's time on a Vita.
- **Super FX and SA-1 at full game speed.** Doom, Stunt Race FX, Super Mario
  RPG and Kirby Super Star keep their speed, music and controls (the busiest
  Super FX games draw their 3D view a little less often).
- **Fills the screen without stretching**, in the SNES's own 4:3 shape with a
  soft glow at the sides, plus sharp scaling, CRT scanlines and an LCD grid.
- **Up to four players** with extra controllers and the Super Multitap.

**Play together, more ways.**

- **Split screen for two players** on one Vita or PS TV, with a second
  controller or player 2 on the touch screen.
- **Trade between versions.** Pokemon Red, Blue and Yellow now link with Gold,
  Silver and Crystal, and two Vitas can trade between Ruby and Sapphire, or
  FireRed and LeafGreen.

**Your library, looked after.**

- **Box art for every game, however it is named.** Games are recognised by
  their contents and by their names ("Act Raiser", "7th Saga",
  "Pocket Dogs (U)"), on every console.
- **Set your own cover** with the Cover button, or drop a PNG or JPG in
  `covers/`. Your picture always wins over downloaded art.
- **Saves are never cut short**, RetroArch saves stay in sync both ways, and
  one backup is kept before anything is written over a save from elsewhere.
- **Tidy game folders.** Only your game and its save stay beside it.
- **Pokemon clocks keep the right time**, and ROM hacks can choose their
  save type.

**Faster.** VitAdhoc asks for 500 MHz when an overclock plugin allows it,
and smoothing does less work for exactly the same picture.

## Included From Version 1.2

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
- **Graphics fixes and a lighter rewind.** Safer texture handling, recovery from graphics
  errors and less repeated work in the rewind and smoothing paths.
- **More reliable everyday use.** Readable accented game names and safer
  settings writes when a memory card refuses a rename or write.

## Included From Version 1.1

**Better sound across GBA, GB and GBC.**

- Steadier playback through fixed, aligned double buffers.
- Correct stereo, note lengths, volume envelopes and sweep for GBA's
  Game Boy-style tone channels.
- GBA DirectSound FIFO resets clear old samples and reset the read/write
  positions together.
- Smoother startup, pause, resume and state-load transitions, with fades
  based on the last sample played and proper audio-channel cleanup on exit.

[Release changelog](CHANGELOG.md) | [Feature guide](docs/FEATURES.md)

## Features

### A library made for the Vita

Browse with buttons or touch. Box art and title screens download on Wi-Fi;
your own pictures take priority, and any game can have a cover you choose
(the Cover button, or a picture in `covers/` named like the game). Built-in
descriptions from Wikipedia cover about 2,000 games across the four consoles. Star favourites, search by name, check Recent and play time, or rest on
a game to watch the last ten seconds you recorded.

Continue returns to an automatic resume point. Start over uses the game's
battery save instead. Existing `ux0:ROMS/` and RetroFlow folders can stay in
place, alongside folders you add yourself.

### More ways to see and play

- Original pixels, bilinear and sharp-bilinear scaling, CPU xBRZ or Scale3x.
- Colour correction, motion blending, LCD/scanline looks, screen sizes,
  Super Game Boy borders and your own PNG overlays.
- Wide view with profiles for supported games, including the original
  Pokemon Emerald family.
- Rewind, fast forward, ten save-state slots, turbo buttons and screenshots.
- IPS, BPS and UPS soft patches, without changing the original game file.
- RetroAchievements, including Hardcore mode for supported games.
- Motion and sunlight cartridge support: Twisted!, Yoshi, Koro Koro,
  Kirby Tilt 'n' Tumble and Boktai, with sensor or stick settings.

### Play together

GBA Link Cable and Wireless Adapter support, plus GB/GBC linking: two Vitas
over Wi-Fi, internet or ad hoc, or two emulated handhelds side by side on one
Vita. Save/state sharing and a Mystery Gift receiver are included too.

[Play Together](PLAY-TOGETHER.md) has the setup guide and supported modes.

## Performance

**CPU speed (1.3):** VitAdhoc asks for 500 MHz (Settings > CPU speed). The
system only allows that with an overclock plugin such as PSVshell; otherwise
the Vita stays at 444 MHz. A faster clock means a warmer Vita and shorter
battery life; choose "444 MHz (standard)" to go back.

**Super NES on a real Vita (1.3):** Super Mario World, Donkey Kong Country,
F-Zero, Chrono Trigger and Yoshi's Island (Super FX 2) run at full speed with
every frame shown; the game thread needs 5-8 ms of each 16.7 ms frame. The
SA-1 games Super Mario RPG and Kirby Super Star run at 59 of 60 frames a
second. Super FX games that keep the chip busy all the time now keep their
game speed too (Doom 59 of 60, Stunt Race FX 49.4 of 50): the chip gets the
time the Vita has, so their 3D view animates more slowly than on a SNES.

**Pokemon Unbound 2.1.1.1 on a real Vita:** about **60 emulated fps** and
**44 frontend-produced frames/s** across three samples of the opening beach
scene, at full-screen CPU xBRZ 3x.

[Full results, settings and screenshots](docs/PERFORMANCE.md).

## Bug Fixes Since 1.0

New in 1.3:

- **Mortal Kombat: Deadly Alliance** no longer sits on its logo for 40 seconds
  or covers its menus in green speckles.
- **Need for Speed: Underground** no longer hangs at its language screen.
- Engine fixes from GBAdhoc 4.5.0 for Golden Sun 2, Mario Golf, Mario Tennis,
  OpenLara and rare instructions.
- Saves from other emulators keep their clock data instead of being cut short.
- No more growing sound delay after the menu, fast forward or rewind, and no
  loud push from Game Boy games that switch a sound channel off.
- Games without a Wide view profile are never stretched.
- The right stick only rewinds if you turn it on.

From earlier versions:

- Recovery from a graphics error that could leave the picture frozen.
- Safer smoothing and staging textures, deferred GPU frees and fallback
  handling when texture allocation fails.
- Audio improvements from 1.1: persistent buffers, stereo routing, timing,
  fades and state-load cleanup.
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
| Cross / Circle | A / B (Circle is A on a Vita whose Enter button is Circle, or in Settings) |
| Hold Start + Select, or tap the game | Open the menu |
| Square | Fast forward |
| Select + Square | Rewind (right stick left too, if turned on in Settings) |
| Select + L / Select + R | Quick save / quick load |
| PS + Start | The Vita's own screenshot |
| Start on the home screen | Settings |

[All controls](CONTROLS.md) | [Detailed features and settings](docs/FEATURES.md)
| [Troubleshooting](KNOWN-ISSUES.md)

## Screenshots

All taken on a PS Vita running VitAdhoc 1.3.

| Super NES, in its own shape | Super NES with CRT scanlines |
| --- | --- |
| ![A SNES game filling the Vita's height in 4:3, with a soft glow at the sides](store/screenshots/vita-1.3/snes-original.png) | ![A SNES game with CRT scanlines](store/screenshots/vita-1.3/snes-crt.png) |
| **Split screen for two players** | **Your own cover pictures** |
| ![Two games side by side, player 2 on the touch screen](store/screenshots/vita-1.3/split-screen.png) | ![Choosing a cover picture for a game](store/screenshots/vita-1.3/covers.png) |
| **SNES picture settings** | **Super FX at full game speed** |
| ![The SNES player's Picture menu](store/screenshots/vita-1.3/snes-picture.png) | ![Doom running on the Super FX 2 chip](store/screenshots/vita-1.3/snes-superfx.png) |
| **A GBA game, never stretched** | |
| ![A GBA game at its own shape with the glow in the sides](store/screenshots/vita-1.3/gba-fit.png) | |

[Capture details](store/screenshots/vita-1.3/README.md). Pokemon Unbound at
full-screen CPU xBRZ 3x, from 1.2:
[ice cavern](store/screenshots/vita-1.2/unbound-intro-ice.png),
[bridge](store/screenshots/vita-1.2/unbound-intro-bridge.png).

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
code are in `gpsp/`. The SNES player (Snes9x 1.63 and its Vita frontend) is
in `snes/`, built as its own program.

## Credits

VitAdhoc builds on [GBAdhoc by ShoshinFauteux](https://github.com/ShoshinFauteux/GBAdhoc).
Thank you for the emulator, link-play work and original save formats this
project builds on. The GBA core is gpSP by Exophase and the libretro
contributors; GB/GBC uses TGB Dual; Super NES uses
[Snes9x](https://github.com/snes9xgit/snes9x) 1.63 by the Snes9x team, whose
licence allows non-commercial use only, so VitAdhoc is and stays free.
RetroAchievements uses [rcheevos](https://github.com/RetroAchievements/rcheevos).
Game descriptions are adapted from Wikipedia (CC BY-SA 4.0).

Built by AI agents (Claude), directed and play-tested by a human. See
[Credits](CREDITS.md) for the other projects and contributors.

An unofficial fan project, not affiliated with Nintendo, Sony, the Snes9x
team or RetroAchievements. No commercial games or proprietary BIOS are supplied.
