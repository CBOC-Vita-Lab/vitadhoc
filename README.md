# VitAdhoc

**Game Boy Advance, Game Boy and Game Boy Color, made for the PS Vita.**
VitAdhoc is a native Vita app, not a PSP app in a wrapper. It has a
touch-friendly home screen that fills itself with box art, full-speed play
with a picture you can make sharp or smooth, rewind and save states, and link
play between two Vitas. And now it has RetroAchievements, tilt and sunlight
games, ROM hacks, and a Continue button that drops you straight back where
you left off.

![The VitAdhoc home screen](store/screenshots/home.png)

**[Download the latest version](https://github.com/CBOC-Vita-Lab/vitadhoc/releases/latest/download/VitAdhoc.vpk)**
· [Install guide](INSTALL.md) · [Controls](CONTROLS.md) · [What's new](CHANGELOG.md)

You'll need your own games. No BIOS download is needed: an open one is built in.

## New In Version 1.0

The biggest update yet: seven new features, more games that boot, and a
library that feels like a console's own.

- **Continue where you left off.** Leave a game and the home screen offers
  Continue. One press and you're back in that exact moment, even if the Vita
  was closed from LiveArea.
- **RetroAchievements.** Sign in once from Settings and earn achievements in
  your GBA, GB and GBC games. Unlocks pop up as you play, and Hardcore mode
  keeps it honest.
- **Tilt, turn and sunlight games.** WarioWare: Twisted!, Yoshi Topsy-Turvy,
  Kirby Tilt 'n' Tumble and Boktai read the Vita's own motion sensor (or the
  right stick, if you prefer).
- **ROM hacks and translations.** Put a `.ips`, `.bps` or `.ups` patch next to
  a game and it starts patched. Your original file is never touched.
- **Recent games and play time.** See what you played last and for how long,
  and sort your library by name, last played or most played.
- **Super Game Boy borders.** Game Boy games made for the Super Game Boy appear
  inside their colourful borders.
- **A look for every game.** Give any game its own screen size, filter and
  colours, apart from the rest of your library.
- **More games boot.** Games that froze on a white screen at start, such as
  copies of Pokémon Pinball: Ruby & Sapphire with an added intro, now play.
  And picking a game starts it straight away.

## What To Expect

| | |
| --- | --- |
| **Speed** | The display's full 60 frames a second. On a Vita running 1.0, Pokémon Ruby held a steady 59.9 with xBRZ smoothing and the wide view on. |
| **Starting a game** | Pick a game and the loading screen is there at once, even before its artwork has loaded. |
| **Picture** | Sharp pixels at any size, or smooth xBRZ / Scale3x. Real GBA colours, frame blending, and a wide view for some games. |
| **Library** | Box art and title screens arrive by themselves over Wi-Fi, with a few lines about each game. Rest on a game and it plays back a few seconds of your own play. |
| **Saves** | Battery saves sit next to each game and reach the memory card within seconds. Five state slots per game, plus rewind. |
| **Two players** | Link Cable and Wireless Adapter games between two Vitas (Wi-Fi, internet or ad hoc), or two players on one Vita. |

Version 1.0 passed its full test suite and is being played on a Vita.
[Known Issues](KNOWN-ISSUES.md) lists what is still being confirmed.

## Install

**What you need:** a PS Vita with HENkaku or Ensō, VitaShell, and your own
`.gba`, `.gb` or `.gbc` games.

**Easy way: CBOC Vita Installer.** Open the
[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases)
on your computer, give it this page's link (or the VPK) and add your games if
you like. It finds the Vita and does the rest. See [Desktop Install](DESKTOP-INSTALL.md).

**Manual way:**

1. Copy `VitAdhoc.vpk` to your Vita (VitaShell, over USB or FTP) and install it.
2. Start VitAdhoc once. It creates `ux0:data/GBAdhoc/`.
3. Put your games in `ux0:data/GBAdhoc/roms/`, or leave them in `ux0:ROMS/`
   or your RetroFlow folders: those are found automatically.
4. Start it again and pick a game.

**Updating?** Just install the new VPK over the old one. Your games, saves,
states and settings stay where they are.

## Playing

| Button | Does |
| --- | --- |
| Cross / Circle | A / B |
| Hold Start + Select, or tap the screen | Open the menu: save states, link play, cheats, settings |
| Square | Fast forward |
| Right stick left, or Select + Square | Rewind |
| Select + L / Select + R | Quick save / quick load |
| Triangle (home screen) | Switch between GBA, GB and GBC |
| Select (home screen) | All games, Starred, Recent |

Press Start on the home screen for Settings. Every button can be changed in
Settings > Controls. [Controls](CONTROLS.md) has the full list and
[Play Together](PLAY-TOGETHER.md) the two-player setup.

| Continue | Achievements |
| --- | --- |
| ![Continue and Start over on the home screen](store/screenshots/continue.png) | ![This game's achievements](store/screenshots/achievements.png) |
| **Recent** | **Motion games** |
| ![The Recent tab with play time](store/screenshots/recent.png) | ![Tilt settings for a motion game](store/screenshots/motion.png) |

Screens from the app's computer test build.

## Download Contents

| File | What it is |
| --- | --- |
| `VitAdhoc.vpk` | The app. Install it on your Vita. |
| `VitAdhoc-install.zip` | The VPK plus the guides, checksums and complete source. |

## For Developers

| | |
| --- | --- |
| [Building](docs/BUILDING.md) | Build the VPK and run the tests |
| [Technical notes](docs/TECHNICAL.md) | How it works, switches, and each version's engineering changes |
| [Source record](docs/SOURCE.md) | What this release was built from |
| [Licensing](LICENSING.md) · [Assets](docs/ASSETS.md) · [Notices](notices/THIRD-PARTY.md) | Licences and third-party components |
| [Check a download](docs/CHECK-DOWNLOAD.md) | Verify files against `SHA256SUMS` |

The app is in `gpsp/vita/`; the emulator cores and shared frontend are in
`gpsp/`.

## Credits

Built by AI agents (Claude), directed and play-tested by a human. VitAdhoc
stands on [GBAdhoc by ShoshinFauteux](https://github.com/ShoshinFauteux/GBAdhoc),
gpSP by Exophase and the libretro contributors, TGB Dual,
[rcheevos](https://github.com/RetroAchievements/rcheevos) and the other
projects in [Credits](CREDITS.md).

An unofficial fan project, not affiliated with Nintendo, Sony or
RetroAchievements.
