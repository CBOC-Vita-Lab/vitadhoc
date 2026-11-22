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
  running exactly as without it. Game Boy, Game Boy Color, NES, WonderSwan and
  the Sega consoles have it (Settings, and per game); it is on Auto where a
  frame is cheap enough and off in linked and online play. Not offered for
  Game Boy Advance or the Super NES: see [Known Issues](KNOWN-ISSUES.md).
- **Names in every alphabet.** Japanese, Korean, accented Latin, Greek and
  Cyrillic names show as written in the app and in the Super NES and Sega
  menus, instead of `?`.

- **Wide view for the Sega consoles,** as on the NES and Super NES. The picture fills the Vita's full width:
  pixel exact in the middle, with the sides showing more of the level you have
  seen, or a soft continuation of the edge. Never a guessed tile.
- **Smooth 50 Hz for European games** removes the hitch PAL games had on the
  60 Hz screen. The game still runs at its true speed.
- **The same looks everywhere:** Ambient glow, Scanlines, LCD grid and LCD
  ghosting, each with a 0 to 10 strength, plus real Game Boy Color palettes
  for original Game Boy games.
- **Super FX games run on a much faster exact core** (Doom, Stunt Race FX),
  aiming to animate closer to the real console.
- **A console picker:** Triangle drops a list of every console with its game
  count; Circle cancels. **Undo and lock for save states** on every console.
- **Your library, quicker:** the home screen appears at once, even on a big
  card in a slow adapter, and more games find their cover.

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
