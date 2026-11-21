# VitAdhoc

**One app, ten consoles, native on your PS Vita.** Game Boy Advance, Game Boy,
Game Boy Color, NES, WonderSwan, Super NES, Genesis / Mega Drive, Master
System, Game Gear and SG-1000 live together on one home screen with box art, a
little history for every game and previews of your own play. Pick up where you
left off, fill the Vita's whole width with the wide view, smooth the pixels or
keep them crisp, rewind your mistakes, earn RetroAchievements, and play
two-player games with a friend anywhere in the world by typing a code of five
characters. No emulator front-end to wrestle with: it is a Vita app, built for
the Vita.

<!-- Screenshot placeholder: a 2.0 home screen with the new console picker, to be
captured on a Vita (the existing store/screenshots are from 1.3 and show the
old tab bar). -->

**[Download the latest version](https://github.com/CBOC-Vita-Lab/vitadhoc/releases/latest/download/VitAdhoc.vpk)**
· [Install guide](INSTALL.md) · [Controls](CONTROLS.md) · [What's new](CHANGELOG.md)

You'll need your own games. An open-source GBA BIOS is built in, so no BIOS
download is required.

## New In Version 2.0

The biggest update yet: four Sega consoles, online play, and a lot of polish
for every console. Nothing in 2.0 has been tried on a real Vita yet, so these
are what we built and aim for, not measured results.

- **Four Sega consoles.** Genesis / Mega Drive, Master System, Game Gear and
  SG-1000 each get their own shelf, with box art, Continue, ten state slots,
  cheats, rewind, fast forward and RetroAchievements.
- **Sega CD / Mega CD** as its own shelf, with box art, Continue, ten state
  slots, rewind, fast forward, cheats, RetroAchievements and two players over
  the link. Needs your own Sega CD BIOS; plays `.cue`, `.iso` and `.chd` discs.
- **Play together, anywhere.** Host a game, share a five-character code, and
  your friend joins from any home network. No router settings, no accounts.
- **Two players on every two-player console.** GBA and Game Boy link cables,
  plus Two players for NES, WonderSwan, Super NES, Genesis / Mega Drive,
  Master System and SG-1000, over Wi-Fi or online.
- **Full-width wide view for the Sega consoles,** as on the NES and Super NES:
  the real picture in the middle, more of your level beside it.
- **A better-looking picture everywhere.** Smoothing upscalers (xBRZ, MMPX,
  xBR), Ambient glow, Scanlines, LCD grid and LCD ghosting with 0 to 10
  strengths, and real Game Boy Color palettes for original Game Boy games.
- **Smoother motion.** Smooth 50 Hz removes the hitch in European games, no
  sprite flicker on the 8-bit Sega consoles, and a much faster exact Super FX
  core for Doom and Stunt Race FX.
- **A console picker, undo and lock for save states,** and a home screen that
  appears at once, even on a big card.

Full list in the [changelog](CHANGELOG.md).

## What To Expect

Everything in 2.0 was checked on PC test builds and in emulators. We do not
have measured Vita numbers for 2.0 yet; they will go here after hardware tests.

| | What to expect |
| --- | --- |
| **Gameplay** | GBA, Game Boy, Game Boy Color and Super NES games were measured on a real Vita in earlier versions ([results](docs/PERFORMANCE.md)). NES, WonderSwan and the four Sega consoles are newer and untried on a Vita. Every console aims for a locked 60 frames a second (50 for European games), eased down by itself only when a game needs it. |
| **Heavy scenes** | Wide view and the biggest smoothing steps cost the most; Image quality Auto and Wide view Auto step down before a frame is lost, and can be set by hand. Doom and other Super FX games are the busiest. |
| **Online** | Verified on PC test builds against simulated routers; not yet on two real Vitas or the live service. Expect to hear what works from you. |
| **Menus and library** | Browse by buttons or touch. The home screen aims to appear at once. Box art needs Wi-Fi. |
| **Picture and saves** | Every overwrite of a save is backed up first. Saves are plain `.srm` / `.sav` files other emulators can read. |

Details, and what we are still checking, are in [Known Issues](KNOWN-ISSUES.md).
If something runs slowly, Settings > Display > Image quality: Original is the
lightest picture.

## Install

You need a homebrew-enabled PS Vita, VitaShell and your own games.

**Easy way: CBOC Vita Installer.** Drop the VPK on the
[desktop installer](DESKTOP-INSTALL.md); it finds your Vita and does the rest.

**Manual way:**

1. Copy `VitAdhoc.vpk` to your Vita with VitaShell (USB or FTP) and install it.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`.
3. Put your games in `ux0:data/GBAdhoc/roms/`, or in any folder under
   `ux0:ROMS/` (for example `GBA`, `SNES`, `NES`, `MD`, `SMS`, `GG`, `SG-1000`).
   RetroFlow's folders work too.
4. Start VitAdhoc again, pick a game and press Cross. Triangle opens the
   console list.

**Updating?** Just install the new VPK over the old one. Games, saves and
settings stay where they are. See the [full install guide](INSTALL.md).

## Playing

| Button | Default action |
| --- | --- |
| Cross / Circle | A / B (Circle is A on a Vita whose Enter button is Circle, or in Settings) |
| Hold Start + Select, or tap the game | Open the menu |
| Square | Fast forward |
| Select + Square | Rewind (right stick left too) |
| Select + L / Select + R | Quick save / quick load |
| Triangle (home screen) | Console list |
| Start (home screen) | Settings, including RetroAchievements |

Genesis games use Square, Cross and Circle for A, B and C, and L, Triangle and
R for X, Y and Z. [All controls](CONTROLS.md) · [All features](docs/FEATURES.md)
· [Play together](PLAY-TOGETHER.md) · [Online play](docs/ONLINE.md)

## Download Contents

| File | Contents |
| --- | --- |
| `VitAdhoc.vpk` | The app to install on a Vita. |
| `VitAdhoc-install.zip` | The same VPK, guides, checksums and complete source. |

`cboc-release.json` and `SHA256SUMS` come with each release for the desktop
installer and for checking a download.

## For Developers

| | |
| --- | --- |
| [Building and tests](docs/BUILDING.md) | [Technical notes](docs/TECHNICAL.md) |
| [Source record](docs/SOURCE.md) | [Check a download](docs/CHECK-DOWNLOAD.md) |
| [Licensing](LICENSING.md) | [Third-party notices](notices/THIRD-PARTY.md) |

The app is in `gpsp/vita/` (GBA, Game Boy, NES and WonderSwan cores run inside
it). The Super NES player (`snes/`) and the Sega player (`md/`) are separate
programs, kept apart because of their non-commercial licences. The shared MIT
kits are `playerkit/` and `widekit/`, and the online room service is in
`online/service/`.

## Credits

Built by AI agents (Claude), directed and play-tested by a human. VitAdhoc
builds on [GBAdhoc by ShoshinFauteux](https://github.com/ShoshinFauteux/GBAdhoc)
and on these projects: gpSP (GBA), TGB Dual (Game Boy and Game Boy Color),
[FCEUmm](https://github.com/libretro/libretro-fceumm) (NES), Beetle WonderSwan
from Mednafen, [Snes9x](https://github.com/snes9xgit/snes9x) (Super NES),
[Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX) (the Sega
consoles), [rcheevos](https://github.com/RetroAchievements/rcheevos)
(RetroAchievements) and [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (online
play). The player kits `playerkit` and `widekit` are ours. Snes9x and Genesis
Plus GX allow non-commercial use only, so VitAdhoc is and stays free. Game
descriptions are adapted from Wikipedia (CC BY-SA 4.0). See
[Credits](CREDITS.md) for everyone else.

An unofficial fan project, not affiliated with Nintendo, Sega, Bandai or Sony.
No commercial games or proprietary BIOS are supplied.
