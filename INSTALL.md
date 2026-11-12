# Install And Update

Use the [CBOC Vita Installer](DESKTOP-INSTALL.md) from a computer, or follow
the VitaShell steps below. This guide covers first install, safe updates,
existing saves and the advanced folder setup. [Features](docs/FEATURES.md)
explains display, achievements, patches and link play in more detail.

## What You Need

- A PS Vita with HENkaku or Ensō, and VitaShell. The app's ID is `GBAD00001`.
- Your own `.gba`, `.gb`, `.gbc`, `.sfc`, `.smc` and `.nes` games (also `.fds`, `.unf`,
  `.unif`), or supported `.zip` archives.
  Subfolders are fine, two levels deep.
- Wi-Fi only for box art, RetroAchievements and playing online.
- No separate BIOS is required: an open replacement is built in. An optional
  BIOS dump you are entitled to use stays on your own Vita, not in the VPK.

## Updating From 1.0 Or 1.1

1. Close VitAdhoc. Back up `ux0:data/GBAdhoc/` and any saves or states beside
   games in other folders.
2. Download `VitAdhoc.vpk` from the
   [official release](https://github.com/CBOC-Vita-Lab/vitadhoc/releases/latest).
   The title ID remains `GBAD00001`.
3. Install the VPK over the existing bubble in VitaShell. Do not delete the
   bubble or data folders first.
4. Start VitAdhoc and check a familiar game and its save before removing a
   backup. Your ROM folders, battery saves, states, settings and artwork do
   not need to be moved.

The install ZIP contains the same VPK plus source and documentation. Extract
the whole ZIP before running its [download checker](docs/CHECK-DOWNLOAD.md).
The checker verifies listed bytes; it does not test game compatibility.

## First Install

1. Copy `VitAdhoc.vpk` to `ux0:VPK/` with VitaShell over USB or FTP (make the
   folder if needed). In VitaShell, select the VPK, press Cross and confirm.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`, then close it.
3. Copy your games to `ux0:data/GBAdhoc/roms/`, or use a folder you already
   have (below).
4. Start VitAdhoc again. Triangle switches between GBA, GB, GBC, SNES and NES; Cross plays.

### Transfer With VitaShell

**USB (usually simplest):**

1. Open VitaShell on the Vita and press Start. Set **SELECT button** to USB,
   then return to the file list.
2. Connect the USB cable and press Select. The Vita's storage appears as a
   drive on your computer. Open it and make a `VPK` folder if there is none.
3. Copy `VitAdhoc.vpk` into `VPK`. Eject the drive on the computer before
   disconnecting USB storage on the Vita.
4. In VitaShell, open `ux0:VPK/`, select the VPK, press Cross and confirm.

**FTP (no cable):** put the computer and Vita on the same Wi-Fi. In VitaShell,
set **SELECT button** to FTP and press Select. Connect an FTP client on the
computer to the address and port shown on the Vita, then copy the VPK into
`ux0:VPK/`. Stop the transfer, return to VitaShell's file list and install it.
Use the address actually shown on your device; an old address may not work.

After first launch, copy your games the same way into
`ux0:data/GBAdhoc/roms/`. `ux0:` means the Vita storage root, not a folder
you need to create inside the computer's mounted drive. No game downloads
are provided by VitAdhoc or the installer.

## Games You Already Have

VitAdhoc reads `roms/` first, then any of these that exist, without moving
anything:

```text
ux0:ROMS/GBA, ux0:ROMS/GB, ux0:ROMS/GBC, ux0:ROMS/SNES, ux0:ROMS/NES
ux0:roms/GBA, ux0:roms/GB, ux0:roms/GBC, ux0:roms/SNES, ux0:roms/NES
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Color
ux0:data/RetroFlow/ROMS/Nintendo - Super Nintendo Entertainment System
ux0:data/RetroFlow/ROMS/Nintendo - Nintendo Entertainment System
ux0:ROMS/WS                  ux0:ROMS/WSC
ux0:roms/WS                  ux0:roms/WSC
ux0:data/RetroFlow/ROMS/Bandai - WonderSwan
ux0:data/RetroFlow/ROMS/Bandai - WonderSwan Color
```

Every other folder directly inside `ux0:ROMS/` or `ux0:roms/` is read too
(a `SNES ROM Collection` or `NES Games` folder, for example). The file ending
decides the console, not the folder's name: `.gba`, `.gb`, `.gbc`, `.sfc` or
`.smc`, `.nes`, `.fds`, `.unf` or `.unif`, `.ws`, `.wsc` or `.pc2`. NES and
Famicom games go in `ux0:ROMS/NES/`, WonderSwan games in `ux0:ROMS/WS/`, or
in any of these folders.

To choose your own folders, open **Settings > Library > Game folders** in the
game list, press Cross and type the folder (for example `ux0:ROMS/GBA`). Type
a folder that is already listed to remove it. The list is scanned again
straight away.

Or write the list yourself in `ux0:data/GBAdhoc/vita.ini` (or `config.ini`):

```ini
extra_roms = ux0:ROMS/GBA; ux0:Games/GB
```

Up to eight folders, separated by `;`, each starting with its drive (`ux0:`).
`extra_roms = 0` turns the other folders off. One folder per console works
too: `gba_roms = ux0:ROMS/GBA`, `gb_roms = ...`, `gbc_roms = ...` (once
`vita.ini` has an `extra_roms` line, which Settings writes, that line is the
whole list).

**Settings > Library > Game names** picks what the list calls each game: "Game
title" (the default, region and hack tags cut) or "File name" (the file's name
without its extension). Games that would show the same name, such as a ROM hack
and its parent, always show their file name under it. Saves and covers are not
affected.

## Bringing Saves From Another Emulator

VitAdhoc picks up your old save by itself the first time you start a game
that has no save here yet. It looks, in this order:

1. In `ux0:data/GBAdhoc/import/`: a file named after the game, ending in
   `.sav` or `.srm` (`Pokemon Emerald.sav`, `Pokemon Red.srm`). Once taken in,
   the file is renamed `.imported`.
2. Next to the game: `Game.srm` (RetroArch's name), and for a Game Boy or
   Game Boy Color game also `Game.sav` (unless a `Game.gba` sits beside it).
3. RetroArch's save folder, `ux0:data/retroarch/savefiles/` (and its folders
   per core, such as `mGBA` or `Gambatte`).

The save contents are copied, and a game that already has a save keeps it.
Files beside a game or in RetroArch's folder are left alone; a file in
VitAdhoc's `import/` drop folder is renamed `.imported` after a successful
import. A message says where it came from.

Save states from other emulators can't be used: make an in-game battery save
there first. Import accepts files up to 256 KiB, but an unusual save format
may still need conversion. Back up the original. If VitAdhoc finds a backup
or temporary save (`.bak`, `.tmp` or `.old`), it does not import over that
recovery situation. Do not delete those files just to force an import.

Beside each game, VitAdhoc keeps only its battery save:

| Console | Battery save |
| --- | --- |
| GBA | `Game.sav` |
| Game Boy | `Game.gb.sav` |
| Game Boy Color | `Game.gbc.sav` |
| Super NES | `Game.srm` (the same name RetroArch and Snes9x use) |
| NES / Famicom | `Game.srm` (the same name RetroArch uses) |
| WonderSwan / Color | `Game.srm` (the same name RetroArch uses) |

Everything else VitAdhoc makes for a game goes under `ux0:data/GBAdhoc/`:

| Console | States | Continue point |
| --- | --- | --- |
| GBA | `states/gba/Game.st0` … `Game.st9` | `states/gba/Game.rsm` |
| Game Boy | `states/gb/Game.gb.st0` … `Game.gb.st9` | `states/gb/Game.gb.rsm` |
| Game Boy Color | `states/gbc/Game.gbc.st0` … `Game.gbc.st9` | `states/gbc/Game.gbc.rsm` |

SNES games keep their four state slots and picture settings in
`ux0:data/GBAdhoc/snes/` (`states/Game.st0` … `Game.st3`). Before VitAdhoc
first writes over a SNES save it did not make, a copy goes to
`ux0:data/GBAdhoc/backups/snes/`.

NES and WonderSwan games keep their states under
`ux0:data/GBAdhoc/states/nes/` and `states/ws/`, like the
other consoles, and VitAdhoc keeps a copy of a save before it first writes over
it.

**Famicom Disk System games** (`.fds`) need the disk system's BIOS, which VitAdhoc
cannot include: put your own `disksys.rom` next to the game or in
`ux0:data/GBAdhoc/nes/` (create the folder).

Each GBA or Game Boy state also has a `.thumb` and a `.shot` picture next to it. States are
named after the game wherever it is kept, as in RetroArch, so moving a game to
another folder keeps its states. Games from older versions are moved
over by themselves the first time you open or view them.

A game kept in a `.zip` uses the archive's stem and the console's suffix:
`Game.zip` becomes `Game.sav` for GBA, `Game.gb.sav` for GB, or
`Game.gbc.sav` for GBC.

## Starting A Game From Another App

Launchers such as RetroFlow can start a game directly:

```text
psgm:play?titleid=GBAD00001&param=ux0:ROMS/GBA/Pokemon Emerald.gba
```

Everything after `param=` is the game's path, exactly as written (it is not
URL-decoded, so names with `&`, `%` or `+` work). A name without a drive is
looked up in `roms/`. The extension picks the console: `.gba`, `.gb`,
`.gbc`, `.sfc`/`.smc` or `.nes`/`.fds`/`.unf`/`.unif` (a `.zip` works too). Close VitAdhoc before launching another game this way.

## Zipped Games

Games kept in `.zip` files work too: VitAdhoc looks inside each zip and lists
it under its console. On start, the game is unpacked once into
`ux0:data/GBAdhoc/unzipped/` (only the game you're playing is kept there) and
checked against the zip's checksum. Saves and states sit beside the `.zip`,
named after it. The first game entry (`.gba`, `.gb`, `.gbc`, `.sfc`, `.smc`, `.nes`, `.fds`,
`.unf` or `.unif`) is used; keep one
game per zip to avoid surprises. Stored and deflated entries up to 32 MiB
are supported; encrypted, multi-disk and ZIP64 archives are not.

Patches and two-player play using a different cartridge for each player need
the games unzipped. If you unpack or rename a game later, back up its saves
and check the new expected save name before continuing.

## Screen Overlays

Put a PNG in `ux0:data/GBAdhoc/overlays/` and it is drawn over the game, with
its own transparency: a bezel, a Game Boy shell, a dot-matrix mask.

- `overlays/gba.png`, `gb.png`, `gbc.png`, `snes.png`, `nes.png`: for every game of that console.
- `overlays/<game name>.png` (the game's file name without its extension):
  for that game only.

960×544 matches the Vita's screen pixel for pixel; other sizes (up to
1024×1024) are stretched to fit. **Settings > Display > Screen overlay** turns
them on or off, and **Grid strength** (1 to 10, 5 is the default) makes the LCD
grid and scanline looks lighter or darker.

## ROM Hacks And Translations

Put the patch next to the game with the same name: `Game.gba` and `Game.ips`
(or `.bps` / `.ups`). The game starts patched; your original file is never
changed, and the save keeps the original's name. If `Game.gb` and `Game.gba`
sit in the same folder, name the patch after the whole file: `Game.gba.ips`.

Use the base-ROM revision specified by the patch author. BPS and UPS check
the source/result checksums; IPS has no source checksum and cannot confirm
that you chose the right base. Unzip a game before using a sidecar patch.
ROM hacks do not automatically inherit the original game's wide-view profile,
achievements or performance; those need to be checked for the exact hack.

## CPU Speed (500 MHz)

VitAdhoc asks for 500 MHz. The Vita only allows that with an overclock plugin
such as PSVshell; without one it runs at 444 MHz and Settings > CPU speed
says so. With PSVshell installed, VitAdhoc writes the profile it needs
(`ur0:data/PSVshell/profiles/GBAD00001`) the first time, if you have none for
it; a profile of your own is left alone until you switch the row in Settings,
and is saved and put back when you switch to "444 MHz (standard)". The new
profile applies the next time the app starts. A faster clock uses more
battery and runs warmer.

## Where Things Are Kept

A game's battery save lives **next to the game**, wherever it is (names in the
table above). Save states and the Continue point live in `states/`. Back up
the game together with its `.sav`, and `states/` if you want your states.
Moving a game to another folder (outside `roms/`) starts it with no states.

```text
ux0:data/GBAdhoc/
  roms/                  your games (and their battery saves)
  states/                save states and Continue points, by console
  cache/                 pictures VitAdhoc can rebuild (safe to delete)
  boxart/  titles/       covers and title screens, downloaded or your own
  covers/                pictures to use as covers, named like the games (above)
  hero/                  optional wide artwork
  clips/                 the home screen's previews of your play
  games/                 each game's own settings ("This game")
  borders/               Super Game Boy borders
  patched/               patched copies of games (made again when needed)
  cheats/                your own cheat files
  screenshots/           screenshots you take (Settings > Interface > Screenshot folder puts them elsewhere)
  import/                saves from other emulators to take in (above)
  overlays/              your screen overlays (above)
  unzipped/              the zipped game you last played, unpacked
  playlog.txt            what you played, when and for how long
  retroachievements.txt  your RetroAchievements name and sign-in token
  config.ini  vita.ini   settings
  gba_bios.bin           optional: your own BIOS dump
  log/                   the app's log
```

Your password is never stored. Delete `retroachievements.txt` (or sign out in
Settings) to forget the account.

## Update

Install the new VPK over the old one, following
[Updating From 1.0 Or 1.1](#updating-from-10-or-11). Everything above stays.

To remove VitAdhoc, delete its bubble. `ux0:data/GBAdhoc/` stays until you
delete it yourself, and with it any games and saves kept there.

## Your Own Cover Pictures

Give any game a cover you like: a custom game, a ROM hack, or just a better
picture than the downloaded one. Yours always wins over a download, and the
downloader never replaces it.

**From the app.** On the home screen, press **Cover** (touch it), or press
START and choose **Library > Cover picture**. Then pick one:

- **Choose a picture...** lists the pictures on your Vita (`ux0:picture/`,
  including `SCREENSHOT/` and `CAMERA/`), your VitAdhoc screenshots, and the
  `covers/` folder below, newest first. Move down the list to see each one.
  Press X and you see it larger; press X again to use it.
- **Use a screenshot of the game** takes the game's newest screenshot, or the
  picture it left when you last played, or a save-state picture, or its title
  screen.
- **Automatic (download)** goes back to the downloaded cover.

PNG and JPG work, in any shape. The picture is copied (shrunk to 512 pixels on
its long side, never stretched) and shown whole on the tile, with bars if it
is not square. Your original is not touched.

**From a computer.** Copy a PNG or JPG into `ux0:data/GBAdhoc/covers/` and name
it like the game, without the extension: `Pokemon Unbound.png` for
`Pokemon Unbound.gba`. Capitals do not matter, and `Pokemon Unbound.gba.png`
works too. VitAdhoc takes it up when the game list opens, or at Settings >
Library > Refresh library.

A file in `covers/` is never changed or deleted. If you choose **Automatic**
for a game, its file in `covers/` stays unused until you put in a new or changed
one. To get rid of it for good, delete it from `covers/`.

Behind the scenes the cover is `boxart/<game>.png`, with a small
`boxart/<game>.own` note that it is yours, and the downloaded one is kept as
`boxart/<game>.dl` so Automatic can bring it back. A picture you put in
`boxart/` yourself, named like the game, also wins over downloads.

## Box Art And Previews

On Wi-Fi, the home screen fetches covers and title screens from the libretro
thumbnail server; [your own pictures](#your-own-cover-pictures) always win. Nothing downloads
during a game or in ad hoc mode. Settings > Interface > Covers shows progress
and looks again for missing ones. `art = 0` in `vita.ini` turns downloading off;
`art_titles = 0` skips title screens.

Previews are recorded on your Vita while you play and never uploaded. Turn them
off in Settings > Interface > Game previews.

## Troubleshooting

| Problem | Try |
| --- | --- |
| No games listed | Check the console tab (Triangle) and the file extension, then restart the app. |
| A save seems missing | Look next to the game's file, in case it was moved or renamed. A Game Boy save is `Game.gb.sav`, a Color one `Game.gbc.sav`. To bring one from another emulator, see above. |
| A game fails with the built-in BIOS | Put your own BIOS dump at `ux0:data/GBAdhoc/gba_bios.bin`. |
| The picture is slow or odd | Settings > Display: Image quality Original, and leave GPU xBRZ off. See the [performance report](docs/PERFORMANCE.md) for measured settings, not a universal speed promise. |
| A zip will not open | Unzip it and try the game directly; check that it has one supported game and is not encrypted or ZIP64. |
| No cover for a game | Covers are optional. Check Wi-Fi, or set your own: Cover on the home screen, or a picture in `covers/` ([above](#your-own-cover-pictures)). |
| RetroAchievements won't sign in | Check Wi-Fi and the Vita's date and time. The log's `ra_http` lines say why. |
