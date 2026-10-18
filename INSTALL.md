# Install And Update

For the shortest path, open [Start Here](START-HERE.txt).

## Requirements

- A homebrew-enabled PS Vita and VitaShell. The application ID is `GBAD00001`.
- Storage for the app and your own legally usable `.gba`, `.gb` and `.gbc` files.
  Subfolders are scanned to two levels.
- Wi-Fi only for optional artwork downloads and network features.
- An open GBA BIOS is built in. Nintendo's BIOS, ROMs and saves are not supplied.

No separate shader compiler is needed for the default renderer. Leave the
experimental GPU option off for a normal installation.

## First Install

This private candidate has no published release yet. Use the matching build-21
package, not an unrelated older VPK.

1. Extract the install ZIP on your computer and [check the download](CHECK-DOWNLOAD.md).
   Leave `VitAdhoc-build21.vpk` intact. Copy it through VitaShell USB or FTP
   to `ux0:VPK/` (create that folder if needed), then end the transfer.
   In VitaShell, highlight the VPK, use the confirm button (usually Cross),
   and confirm installation.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`, then exit normally.
3. Copy your games to `ux0:data/GBAdhoc/roms/`, or use an existing folder below.
4. Reopen VitAdhoc, choose GBA, GB or GBC with Triangle, then select a game
   and press Cross.

Keep the readable licenses/notices from the install ZIP with your copy. They
are also included in the VPK under `licenses/` and `notices/`.

## Existing ROM Folders

The local `roms/` folder is scanned first. Without an `extra_roms` setting,
existing folders are checked in this order:

```text
ux0:ROMS/GBA
ux0:ROMS/GB
ux0:ROMS/GBC
ux0:roms/GBA
ux0:roms/GB
ux0:roms/GBC
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy
ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Color
```

Case-equivalent paths are deduplicated. Missing folders are ignored. Hidden
files, including macOS `._Game.gba` files, are not treated as games.

To select your own folders, add this to `ux0:data/GBAdhoc/vita.ini`:

```ini
extra_roms = ux0:ROMS/GBA; ux0:Games/GB
```

Up to eight roots are accepted, separated by semicolons. Each must begin
with its device prefix, such as `ux0:`. An explicit list replaces the
automatic external list, not the primary `roms/` folder. `extra_roms = 0`
or an empty value disables external roots.

For large libraries and same-name games, see [Technical Notes](TECHNICAL.md)
and the save advice below.

## Data And Saves

```text
ux0:data/GBAdhoc/
  roms/                 your games and their adjacent saves/states
  boxart/               optional <game>.png covers
  titles/               optional downloaded title images
  hero/                 optional wide artwork
  clips/                locally recorded browser previews
  cheats/               optional per-game .cht files
  screenshots/          screenshots taken by the app
  log/                  diagnostic logs
  config.ini            normal settings and remapping
  vita.ini              optional Vita switches and extra_roms
  gba_bios.bin          optional BIOS from your own lawful dump
```

Battery saves and five state slots live **beside the ROM**, including ROMs
outside the app folder. For GBA, `Game.gba` uses `Game.sav` and `Game.st0`
through `Game.st4`; GB/GBC names retain `.gb`/`.gbc` before the save suffix.
State previews use `.thumb` and `.shot`. Move or back up the whole group.

Artwork, cheat files, recorded previews and some per-game settings use the
game's basename. Two same-name games can share these. Favourites use the full
ROM path and can distinguish them. Give different games distinct names when
these collisions matter. A second local player's separate save may use
`<game>.p2.sav`; see [Play Together](PLAY-TOGETHER.md).

The app prefers your optional BIOS in the data root, then any private
package's `app0:gba_bios.bin`, then its built-in open BIOS. The shared package
does not contain the original BIOS. If an older private VPK kept your BIOS
inside the app, preserve your own copy in the data root before updating.
Build-specific testing notes are in [Known Issues](KNOWN-ISSUES.md).

## Optional Artwork And Previews

In the browser, Wi-Fi artwork fetching uses the libretro thumbnail service;
your own existing images take priority. It does not run during gameplay or
ad hoc mode. Settings > Interface shows progress and lets you retry missing
covers. Use `art = 0` in `vita.ini` to disable downloading, or `art_titles = 0`
for title images only. Downloaded artwork is not part of this distribution.

Game previews record recent play locally. They are not uploaded. Disable them
in Settings > Interface > Game previews, or force them off with `clips = 0`
in `vita.ini`. Fast-forward, rewind and link sessions are not recorded.

## Update Or Remove

| Item | What To Do |
| --- | --- |
| `VitAdhoc-build21.vpk` | Install with VitaShell; no manual app-folder replacement. |
| Games and their adjacent saves/states | Keep, including those in external folders. |
| `config.ini` and `vita.ini` | Keep; no starter config is bundled. |
| Your own BIOS, covers and previews | Keep in their existing data locations; see the BIOS note above. |
| Source archive, guides and notices | Keep on your computer; no Vita copy step. |

1. Exit VitAdhoc and back up `ux0:data/GBAdhoc/` plus all external ROM folders'
   saves/states. Do not assume an app-folder backup covers external saves.
2. Install the new VPK with the same application ID. Do not delete the data
   folder or rename ROMs as an update step.
3. Relaunch, check the game list, load a backed-up test save and confirm your
   settings. Keep the backup until this succeeds.

To remove the app, remove its bubble only after backing up. Data outside the
app is separate; deleting `ux0:data/GBAdhoc/` removes its settings, art and
any games/saves stored there. External saves require their own deliberate cleanup.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Empty list | Correct console tab, extension and device-prefixed root; restart to rescan. Read `rom_roots` / `rom_scan_root` in the log. |
| Save seems missing | Look beside the exact ROM; check whether it was moved or renamed. Never overwrite the only copy. |
| A game fails with the open BIOS | Try your own valid BIOS dump in the data root and report the exact game revision. Do not download an unofficial BIOS bundle. |
| Slow or broken experimental display | Return to a normal scale/filter; disable GPU xBRZ and experimental wide view. |
| Cover not found | It is optional. Check Wi-Fi, retry Covers in Settings or supply permitted local artwork. |

When reporting a fault, include the build, steps and a short relevant log
excerpt. Review logs and screenshots for personal filenames, addresses and
save details before sharing them. Do not upload your ROM or save by default.
