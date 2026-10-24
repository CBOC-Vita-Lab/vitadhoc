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

To choose your own folders instead, add a line to `ux0:data/GBAdhoc/vita.ini`:

```ini
extra_roms = ux0:ROMS/GBA; ux0:Games/GB
```

Up to eight folders, separated by `;`, each starting with its drive (`ux0:`).
`extra_roms = 0` turns the other folders off.

## ROM Hacks And Translations

Put the patch next to the game with the same name: `Game.gba` and `Game.ips`
(or `.bps` / `.ups`). The game starts patched; your original file is never
changed, and the save keeps the original's name. If `Game.gb` and `Game.gba`
sit in the same folder, name the patch after the whole file: `Game.gba.ips`.

## Where Things Are Kept

Saves and states live **next to each game**, wherever it is: `Game.sav`, state
slots `Game.st0` to `Game.st4`, and the Continue point `Game.rsm`. Move or back
up a game together with these.

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
  screenshots/           screenshots you take
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
| A save seems missing | Look next to the game's file, in case it was moved or renamed. |
| A game fails with the built-in BIOS | Put your own BIOS dump at `ux0:data/GBAdhoc/gba_bios.bin`. |
| The picture is slow or odd | Settings > Display: Image quality Original, and leave GPU xBRZ off. |
| No cover for a game | Covers are optional. Check Wi-Fi, or add your own picture to `boxart/`. |
| RetroAchievements won't sign in | Check Wi-Fi and the Vita's date and time. The log's `ra_http` lines say why. |
