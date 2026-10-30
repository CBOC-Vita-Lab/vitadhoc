# Install And Update

The quickest route is the [CBOC Vita Installer](DESKTOP-INSTALL.md). This page
is the manual way, with every folder explained.

## What You Need

- A PS Vita with HENkaku or Ensō, and VitaShell. The app's ID is `GBAD00001`.
- Your own `.gba`, `.gb` and `.gbc` games. Subfolders are fine, two levels deep.
- Wi-Fi only for box art, RetroAchievements and playing online.
- No BIOS: an open replacement is built in.

## First Install

1. Copy `VitAdhoc.vpk` to `ux0:VPK/` with VitaShell over USB or FTP (make the
   folder if needed). In VitaShell, select the VPK, press Cross and confirm.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`, then close it.
3. Copy your games to `ux0:data/GBAdhoc/roms/`, or use a folder you already
   have (below).
4. Start VitAdhoc again. Triangle switches between GBA, GB and GBC; Cross plays.

## Games You Already Have

VitAdhoc reads `roms/` first, then any of these that exist, without moving
anything:

```text
ux0:ROMS/GBA, ux0:ROMS/GB, ux0:ROMS/GBC
ux0:roms/GBA, ux0:roms/GB, ux0:roms/GBC
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Color
```

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

The file is copied, never moved, and a game that already has a save keeps it.
A message says where the save came from. Save states from other emulators
can't be used: make an in-game save there first.

VitAdhoc's own save names, beside each game:

| Console | Battery save | States | Continue point |
| --- | --- | --- | --- |
| GBA | `Game.sav` | `Game.st0` … `Game.st9` | `Game.rsm` |
| Game Boy | `Game.gb.sav` | `Game.gb.st0` … `Game.gb.st9` | `Game.gb.rsm` |
| Game Boy Color | `Game.gbc.sav` | `Game.gbc.st0` … `Game.gbc.st9` | `Game.gbc.rsm` |

A game kept in a `.zip` saves under the zip's name (`Game.zip` → `Game.sav`).

## Starting A Game From Another App

Launchers such as RetroFlow can start a game directly:

```text
psgm:play?titleid=GBAD00001&param=ux0:ROMS/GBA/Pokemon Emerald.gba
```

Everything after `param=` is the game's path, exactly as written (it is not
URL-decoded, so names with `&`, `%` or `+` work). A name without a drive is
looked up in `roms/`. The extension picks the console: `.gba`, `.gb` or
`.gbc` (a `.zip` works too). Close VitAdhoc before launching another game this way.

## Zipped Games

Games kept in `.zip` files work too: VitAdhoc looks inside each zip and lists
it under its console. On start, the game is unpacked once into
`ux0:data/GBAdhoc/unzipped/` (only the game you're playing is kept there) and
checked against the zip's checksum. Saves and states sit beside the `.zip`,
named after it. Patches and two-game link play need the game unzipped.

## Screen Overlays

Put a PNG in `ux0:data/GBAdhoc/overlays/` and it is drawn over the game, with
its own transparency: a bezel, a Game Boy shell, a dot-matrix mask.

- `overlays/gba.png`, `gb.png`, `gbc.png`: for every game of that console.
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

## Where Things Are Kept

Saves and states live **next to each game**, wherever it is (names in the
table above). Move or back up a game together with these.

```text
ux0:data/GBAdhoc/
  roms/                  your games (and their saves)
  boxart/  titles/       covers and title screens, downloaded or your own
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

Install the new VPK over the old one. Everything above stays.

Before a big update, back up `ux0:data/GBAdhoc/` and the saves next to games in
other folders.

To remove VitAdhoc, delete its bubble. `ux0:data/GBAdhoc/` stays until you
delete it yourself, and with it any games and saves kept there.

## Box Art And Previews

On Wi-Fi, the home screen fetches covers and title screens from the libretro
thumbnail server; your own pictures in `boxart/` always win. Nothing downloads
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
| The picture is slow or odd | Settings > Display: Image quality Original, and leave GPU xBRZ off. |
| No cover for a game | Covers are optional. Check Wi-Fi, or add your own picture to `boxart/`. |
| RetroAchievements won't sign in | Check Wi-Fi and the Vita's date and time. The log's `ra_http` lines say why. |
