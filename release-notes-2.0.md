## VitAdhoc 2.0: Eleven consoles, one app, a friend anywhere

The biggest VitAdhoc yet. Five Sega consoles join the family, so one app now
plays **Game Boy Advance, Game Boy, Game Boy Color, NES, WonderSwan, Super NES,
Genesis / Mega Drive, Sega CD, Master System, Game Gear and SG-1000**,
natively on your PS Vita. And you can play two-player games with a friend on the other side of
the world: one of you types a code of five characters, and you are linked.

Everything below was checked on PC test builds and in emulators. None of it
has run on a real Vita yet, so no speed is promised: where we say "aims for",
that is what we are going for. See [Known Issues](KNOWN-ISSUES.md).

**New consoles**

- **Sega Genesis / Mega Drive, Master System, Game Gear and SG-1000.** Each has
  its own shelf in the console list with box art, descriptions, Starred,
  Recent, play time, Continue, ten state slots with pictures, cheats, patches
  and RetroArch save import. They get the same picture and pacing features as
  every other console, and aim for a smooth 60 (50 for European games).
- **Sega CD / Mega CD** is its own shelf, played by the same Sega program:
  `.cue` + `.bin`, `.iso` and `.chd` discs, read from storage as the game
  runs. It needs your own Sega CD BIOS (VitAdhoc cannot include one); saves
  (backup RAM and the RAM cartridge) are kept per game. Continue, ten states,
  rewind, fast forward, cheats, RetroAchievements, wide view and Two players
  work as on the Genesis. It is the least tested console in 2.0: see
  [Known Issues](KNOWN-ISSUES.md).
- **Rewind, fast forward and RetroAchievements** on all of them, with a
  remappable pad (Genesis A / B / C on Square / Cross / Circle, X / Y / Z on
  L / Triangle / R).
- **No sprite flicker** on the Master System, Game Gear and SG-1000: every
  sprite on a line is drawn, and the game runs exactly the same.

**Online play**

- **Play with a friend anywhere.** Host a game, share the five-character code,
  and your friend types it. No router settings, no accounts. The Vitas link
  directly when they can and through a relay when they cannot.
- **Every two-player console.** GBA Link Cable and Wireless Adapter, Game Boy
  and Game Boy Color Link Cable, save and state sharing, and Two players for
  NES, WonderSwan, Super NES, Genesis / Mega Drive, Sega CD, Master System
  and SG-1000. The join finds your copy of the game for you. On a cable each of
  you can play your own game (Ruby with Sapphire).
- **Safe to join.** Every emulator checks a game state sent by the other
  player, or read from a file, before using it; the state loaders were fuzz
  tested on the PC build. The room service limits guessing of codes and
  protects the relay. Both Vitas need VitAdhoc 2.0.

**Picture and play**

- **Quick response** shows a button press one frame sooner, with the game
  running exactly as without it. Game Boy, Game Boy Color, NES, WonderSwan,
  Super NES and the Sega consoles have it (Settings, and per game); it is on
  Auto where a frame is cheap enough and off in linked and online play. Not
  offered for Game Boy Advance: see [Known Issues](KNOWN-ISSUES.md).
- **Names in every alphabet.** Japanese, Korean, fullwidth forms, accented
  Latin, Greek and Cyrillic names (plus Simplified Chinese GB 2312 hanzi) show
  as written in the app and in the Super NES and Sega menus, instead of `?`.

- **Wide view across the library:** GBA Wide view defaults to `On + any game`
  with Soft edges so the catalogue gets full-width play out of the box (turn it
  off per game if edges misbehave). On NES, Super NES and the Sega consoles,
  the real picture stays pixel exact in the middle, with the sides showing more
  of the level you have seen or a soft continuation of the edge (never a guessed
  tile). NES Wide View (`424×224`/`240`) supports full 2x upscaling (`xBRZ 2x`,
  `Scale2x`, `MMPX 2x`) and Sharp Bilinear (`1344×720` render target), Master
  System and SG-1000 automatically step `xBR 3x` down to `xBR 2x` when needed
  within the `1024×576` scaler budget, and Game Gear composes LCD and Screen
  colour profiles in a single LUT pass.
- **Smooth 50 Hz for European games** removes the hitch PAL games had on the
  60 Hz screen. The game still runs at its true speed.
- **The same looks everywhere:** Ambient glow, Scanlines, LCD grid and LCD
  ghosting, each with a 0 to 10 strength, plus real Game Boy Color palettes
  for original Game Boy games.
- **Super FX games run on a much faster exact core** (Doom, Stunt Race FX),
  aiming to animate closer to the real console.
- **A console picker & full PlayerKit menu parity:** Triangle drops a list of
  every console with its game count; Circle cancels. **Undo and lock for save
  states** (`L` lock/unlock, persisted `R` undo for both loads and overwrites),
  **unified Cheats buttons** (`Triangle` = Add, `Square` = Delete), **Start**
  to resume the Super NES and Sega pause menus, **Right-stick Rewind** enabled
  by default across all eleven consoles, **`Settings > Controls > Left stick`**
  (`D-pad` / `Custom` / `Off`), an in-game **Achievements** viewer, synced
  **Light theme** & **Tap screen for menu** via `display.ini`, and mappable PNG
  **Screenshots** in the Super NES and Sega players across all eight languages.
- **Your library, quicker and smarter:** the home screen appears at once, even
  on a big card in a slow adapter; cover lookup handles `.zip`/`.7z` entries,
  GoodTools tags and ROM hack protection; RetroArch and Emu4Vita++ `.lpl`
  playlists support 6-line and archive formats and filter out unsupported
  systems; rear touch uses a unified 12% outer edge grip inset + center
  deadzone across all eleven consoles; and WonderSwan keeps unswapped physical
  buttons (`Circle = A, Cross = B`) even on Asian Vitas.
- **Core, online and FAT32 hardening:** Online 2.0 adds STUN fallback, TURN
  and worker budget hardening, an in-app cartridge picker for mismatched
  link-cable games, and host gameplay-progress preservation if a link partner
  disconnects; GBA ARMv7/Thumb-2 dynarec (7th cached guest register `r10`,
  `ubfx`/`cbnz` fast-path stubs, ARM literal pool folding, single-instruction
  Thumb `NEG` and IWRAM `POP`/`LDMIA sp` fastpath), Super NES Mode 7, Sega
  VDP/FM/CD read-ahead, `.smd` netplay normalization, in-place Mega Drive
  region timing updates, Gambatte zero-initialized savestates, NES and
  WonderSwan cores carry exact scheduling and memory refinements; and saves and
  archives use safe two-step `.old` file replacement on Vita FAT32 (`APP_VER`
  `02.00` in LiveArea).

**Install or update**

Install `VitAdhoc.vpk` with VitaShell over the existing bubble (title ID
`GBAD00001`), or use the
[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest).
Games, saves and settings stay in place. For online play your friend installs
it too. Sega games go in any folder under `ux0:ROMS/` (for example `MD`, `SMS`,
`GG`, `SG-1000`, `SegaCD`).

`VitAdhoc-install.zip` contains the same VPK, full source, guides and
checksums. No commercial games or proprietary BIOS are included.

Full list: [CHANGELOG.md](CHANGELOG.md). An unofficial fan project, not
affiliated with Nintendo, Sega, Bandai or Sony. Built by AI agents (Claude),
directed and play-tested by a human.

SHA-256 `VitAdhoc.vpk`:
`0000000000000000000000000000000000000000000000000000000000000000`
