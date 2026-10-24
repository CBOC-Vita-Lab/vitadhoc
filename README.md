# VitAdhoc

**Game Boy Advance, Game Boy and Game Boy Color, built for the PS Vita.**
Your whole library on a home screen that fills itself with box art. A picture
that is pin-sharp or beautifully smooth, and wide enough to fill the Vita's
screen. Full speed, with rewind, save states and a Continue button that drops
you back exactly where you left off. Link play with a friend, achievements,
tilt games and ROM hacks. All of it native: no PSP mode, no emulator inside an
emulator.

![The VitAdhoc home screen](store/screenshots/home.png)

**[Download the latest version](https://github.com/CBOC-Vita-Lab/vitadhoc/releases/latest/download/VitAdhoc.vpk)**
· [Install guide](INSTALL.md) · [Controls](CONTROLS.md) · [What's new](CHANGELOG.md)

You'll need your own games. No BIOS download is needed: an open one is built in.

## Why VitAdhoc?

[GBAdhoc](https://github.com/ShoshinFauteux/GBAdhoc) is a great PSP emulator
whose big idea is link play: trade and battle in Pokémon over Wi-Fi, just like
with a link cable. On a Vita, though, a PSP app runs inside the Vita's PSP
mode: a 480×272 picture blown up to fit, the power of a PSP, and no touch
screen or motion sensor.

VitAdhoc keeps GBAdhoc's heart (its emulator, its link play and its save
formats) and rebuilds everything around it as a real Vita app:

| | A PSP app on a Vita (PSP mode) | VitAdhoc |
| --- | --- | --- |
| **Screen** | The PSP's 480×272, scaled up | The Vita's own 960×544: crisp text, art and pixels |
| **Power** | One PSP processor | All of the Vita's cores: smoothing and drawing run beside the game |
| **Touch** | Not available | Tap, drag and flick through the home screen and menus |
| **Motion sensor** | Not available | Tilt and turn the Vita in motion games |

Then it adds a lot more: everything below. Your GBAdhoc folder, games, saves
and states carry straight over.

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

## Everything It Does

### A library that feels like a console's own
- **Home screen.** Your games as big tiles, with the game's own picture behind
  them. Tap, drag and flick, or use the buttons.
- **Box art by itself.** On Wi-Fi, covers and title screens download on their
  own, found by name, cartridge code or checksum, so renamed files still match.
  Your own pictures always win.
- **A few lines about every game.** About 1,300 GBA, GB and GBC games come with
  the opening of their Wikipedia article, built in, no network needed.
- **Previews of your own play.** Rest on a game and the last ten seconds you
  played start moving behind it, like a streaming service's trailer.
- **Continue, Recent and play time.** One press back into the exact moment you
  left, a Recent tab, and sorting by name, last played or most played.
- **Starred and Find.** Star favourites; Find matches words in any order and
  ignores accents and punctuation.
- **Uses the games you already have.** `ux0:ROMS/` and RetroFlow's folders are
  found without moving anything.

### A picture made for the Vita's screen
- **Original:** crisp, even pixels at any size, including the full height.
- **Smooth:** xBRZ and Scale3x redraw the pixel art with clean curves. They run
  on the Vita's spare cores and only redraw what changed, so the game never
  waits for them.
- **Wide view:** instead of black bars, more of the game. Made for Pokémon
  Emerald and its siblings (Ruby, Sapphire, FireRed, LeafGreen): the map really
  continues to the screen's edges, and battles span the full width. Options
  widen 3D racing tracks too.
- **Real handheld colours** (the muted look the art was drawn for), motion
  blend for games that flicker, LCD grid and scanline looks, five screen sizes,
  and **Super Game Boy borders**.
- **A look for every game**, kept apart from the rest of your library.

![Left: the console's picture. Right: the wide view filling the screen](store/screenshots/wide-view.png)

### Smooth, fast and responsive
- **The full 60 frames a second**, with the GBA's picture drawn on a second core
  while the game runs on the first.
- **Keep full speed:** if the Vita falls behind for a moment, the game catches
  up without a gap in the sound.
- **Low lag:** each frame starts once the last one is on screen, up to a frame
  less between a press and the picture.
- **Rewind** the last 60 seconds, **fast forward**, five **save-state slots**
  with pictures, **turbo buttons** and **screenshots**.
- An optional **500 MHz** profile for the PSVshell plugin.

### Play together
- **Link Cable and Wireless Adapter games** between two Vitas: trade and battle
  in Pokémon as the games intended.
- **Three ways to connect:** the same Wi-Fi, over the internet (the port opens itself
  on routers with UPnP), or ad hoc with no router at all.
- **Game Boy and Game Boy Color links**, even between two different cartridges
  (Red with Blue): each Vita lends the other its cartridge for the session.
- **Two players on one Vita**, side by side, with a second controller on a PS TV.
- **Share saves and states** between two Vitas, and receive real **Mystery
  Gift** cards from a phone.

### More to play
- **RetroAchievements**, with Hardcore mode.
- **Tilt, turn and sunlight cartridges:** WarioWare: Twisted!, Yoshi
  Topsy-Turvy, Koro Koro Puzzle, Kirby Tilt 'n' Tumble and Boktai 1–3.
- **ROM hacks and translations:** IPS, BPS and UPS patches, applied as the game
  starts; your original file is never changed.
- **Cheats** for about 2,190 games built in, plus your own GameShark, Action
  Replay, CodeBreaker and Game Genie codes.
- **No BIOS hunt:** an open GBA BIOS is built in (your own dump works too).

### A sharper emulator
VitAdhoc runs gpSP, GBAdhoc's emulator, with the latest upstream fixes, and
adds its own, found on real games and real Vitas: games that froze on a white
screen at start now boot, Game Boy saves reach the memory card within two
seconds, a full card is reported instead of silently losing a save, and many
more from independent code reviews. Details are in the
[technical notes](docs/TECHNICAL.md).

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
