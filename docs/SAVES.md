# Saves

Where each game's save lives, what VitAdhoc writes for RetroArch and other
emulators, when, and how to get a plain file by hand.

## The short version

- The game's own save is **one file beside the game**: `Game.sav` (GBA),
  `Game.gb.sav` (Game Boy), `Game.gbc.sav` (Game Boy Color), `Game.srm`
  (SNES, NES, WonderSwan). That file is the one the game reads and writes.
  Nothing else of VitAdhoc's is kept in the game's folder.
- Since 1.4 every save also gets **a plain copy where RetroArch looks**,
  `Game.srm`, once per game and then kept up to date each time the game saves
  (Settings > Library > Sync RetroArch saves, on by default).
- Saves are never overwritten without a verified backup in `backups/`.

## Where the save is, by version

The ROM's folder before 1.3 also held states, `.rsm` (Continue), `.rsm.pt`,
pictures; from 1.3 those live under the app's data folder
(`states/<console>/`, in 1.4 in 256 small folders named by two hex digits).

| | 1.0, 1.1 | 1.2 | 1.3, 1.3.1 | 1.4 |
|---|---|---|---|---|
| GBA save | `Game.sav` beside the game, 128 KiB image | same | same | same |
| GB / GBC save | `Game.gb.sav` / `Game.gbc.sav`: cartridge RAM, and for a clock cartridge a 48-byte clock trailer | same | same, extra bytes after the RAM kept | same |
| SNES / NES / WS | `Game.srm` beside the game (SNES: raw SRAM) | same | same | same |
| import from other emulators | none | copy (never move) into the save, when the game had no save: from `import/` (file renamed `*.imported`), `Game.srm` beside the game, `Game.sav` beside a Game Boy game, or RetroArch's savefiles folder (and a core's folder in it) | same; and only a save taken from RetroArch's `.srm` is **linked** to that file | same, plus the link is made for every save (below) |
| export to RetroArch | none | none | only to a linked file | a plain copy for every save |
| records | none | none | `saves-sync.txt` (one row per save; a row names RetroArch's file or `-`) | same, plus a flag column |
| backups | none | none | one verified copy before a save from elsewhere is written over | same, and before any other file is replaced |
| leftovers beside the game | `.st0`-`.st9`, `.thumb`, `.shot`, `.p2`, `.rsm`, `.rsm.pt`, `.old` | same | moved to `states/` the first time the game starts or its detail page opens; a file whose twin already exists there is left (`kept`) | the same, and a left-over twin is moved to `leftovers/` (verified copy) |

None of 1.0-1.2 ever wrote a file RetroArch could read: RetroArch only reads
`Game.srm` (its savefiles folder, or beside the game when it is set to save
there), never `Game.sav`, `Game.gb.sav` or `Game.gbc.sav`. 1.3 added the
write-back only for a save that had been **taken from** RetroArch while the
game had no save at all. A save made in 1.0-1.2 (or any game that already had
a save when 1.3 first started it) was therefore never linked, and never
written anywhere RetroArch looks.

## File layouts

Nothing is converted on export: the file is the game's save, byte for byte.
It already is the layout of the cores.

- **GBA**: the 128 KiB gpSP image (the same bytes gpSP in RetroArch writes);
  a longer file (a trailer another emulator put after it) keeps it. mGBA
  expects the size of the game's save type (32 KiB SRAM, 64 or 128 KiB flash,
  512 B or 8 KiB EEPROM) and a 16-byte clock trailer when the game has a
  clock; it reads a 128 KiB file of a game that names its save type in the
  ROM. A linked RetroArch file keeps ITS size and trailer: only the bytes
  both files have are written, and nothing is written if the save holds data
  past the end of a smaller file.
- **Game Boy / Color**: cartridge RAM, then, for a cartridge with a clock
  (MBC3), the 48-byte trailer mGBA, SameBoy and VBA-M write (VBA-M also
  reads 44 bytes). Gambatte keeps cartridge RAM in `Game.srm` and the clock in
  `Game.rtc`: it reads the RAM from the front of the file and ignores the
  trailer, so a Gambatte player starts the clock again. A cartridge without a
  clock has no trailer.
- **SNES / NES / WonderSwan**: raw battery RAM, as Snes9x, FCEUmm and Beetle
  WonderSwan write it.

Same save, different file sizes (a 32 KiB copy of the 128 KiB image, a
trailer of 44, 48 or no bytes, other clock bytes): treated as the same save.
Bytes that differ: a different save.

## What is exported where, and when

Once per game, at the game's start (and after the first save of a new game),
if Sync RetroArch saves is on. One `save_census` line in the log every start
says what was found, what was written, the CRC, whether a backup was made,
and why anything was skipped. The player is told once, in one sentence:
"Save is now also at `<place>`, RetroArch can use it".

1. **The record names a RetroArch file that is still there**: nothing to do
   (1.3's sync keeps it up to date). A named file that is gone is not made
   again (the player may have deleted it).
2. **A RetroArch file of this game exists** (its configured folder, the
   default folder, a core's folder in it, or `Game.srm` beside the game):
   - same save: linked, nothing written, the file is not touched;
   - different, and RetroArch's is **newer or has the same time**: both are
     kept as they are and backed up, nothing is overwritten, the player is told
     which is which. Use "Export save for other emulators" to choose;
   - different, and RetroArch's is **older**: its bytes are kept in
     `backups/` first (size and CRC read back), then the save goes over it in
     its own size and layout.
3. **No such file**: a new one, where RetroArch's `retroarch.cfg` says:
   1. `savefiles_in_content_dir = "true"`: `Game.srm` beside the game;
   2. `savefile_directory` set to a folder: there;
   3. RetroArch is on the card: `ux0:data/retroarch/savefiles/Game.srm`
      (in a core's folder when `sort_savefiles_enable` is on and one exists);
   4. RetroArch is not on the card: the plain `Game.sav` beside the game (a
      Game Boy or Color game gets `Game.sav` next to `Game.gb.sav`; a GBA
      game's own save already is that; a `Game.gba` or `Game.zip` of the same
      name beside it keeps the name, then `Game.srm`).
   It is written to `<file>.import`, read back (size and CRC), then renamed.
   A write that fails or is cut short leaves the save and RetroArch's folder
   as they were and is tried again at the next start; a leftover `.import` or
   a replaced file waiting as `.old` is cleaned up or put back first.

After that, every complete write of the battery file is written to the linked
file too, but only while that file still is what was last written (RetroArch
changing it first means the newer one is used at the next start, the other kept
in `backups/`).

**Sync RetroArch saves off**: nothing is exported or written back, and the
census line says so. 1.3 and 1.4 write the setting only when the player
changes it, and the default is on, so an "off" in `vita.ini` is a choice:
it is respected. (An install cannot be told apart from a player who turned it
off by themselves. If it was off by mistake, turn it on: the copy is made at
the next start of each game.)

## Export save for other emulators (by hand)

Settings > This game > Export save for other emulators (the menu row calls
`ui_host_export_save` in `main_vita.c`):

- *For RetroArch*: the save as `Game.srm` where RetroArch looks (the rules above,
  or the file RetroArch already has);
- *Plain .sav beside the game*: `Game.sav`. For a GBA game that is the game's own
  save. A Game Boy game gets `Game.sav` next to its `Game.gb.sav` unless a
  GBA game owns the name.

It ignores the sync setting. A file it replaces is backed up first.

To do it with no menu: copy the game's save file, rename it to the name the
other emulator wants (`Game.srm` for RetroArch and most others, `Game.sav` for
mGBA/BGB/SameBoy), and put it where that emulator looks.

## Backups and restoring one

`backups/` in the app's data folder holds `<file name>.<8 hex digits of its
CRC>.bak`: whole copies, named by their own CRC, so the same bytes are only
kept once. A backup is made before a save from elsewhere is first written
over, before the app replaces a different file, and when both sides changed.
It only counts if it is read back whole (size and CRC); if it cannot be made,
nothing is overwritten and the reason is shown. To restore: close the game,
copy the `.bak` over the game's save (or the RetroArch file), and remove the
`.bak.<crc>` part of the name. Other leftovers are in `leftovers/` (old state
files that were beside the game, moved after a verified copy).

## In the log

`save_census`, `save_export`, `save_backup`, `save_sync_out`,
`save_sync_conflict`, `save_tidy`, `save_import`.
