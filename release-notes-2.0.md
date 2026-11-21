## VitAdhoc 2.0: Ten consoles, one app, a friend anywhere

The biggest VitAdhoc yet. Four Sega consoles join the family, so one app now
plays **Game Boy Advance, Game Boy, Game Boy Color, NES, WonderSwan, Super NES,
Genesis / Mega Drive, Master System, Game Gear and SG-1000**, natively on your
PS Vita. And you can play two-player games with a friend on the other side of
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
  NES, WonderSwan, Super NES, Genesis / Mega Drive, Master System and
  SG-1000. The join finds your copy of the game for you. On a cable each of
  you can play your own game (Ruby with Sapphire).
- **Safe to join.** Every emulator checks a game state sent by the other
  player before using it.

**Picture and play**

- **Wide view for the Sega consoles.** The picture fills the Vita's full width:
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
  count. **Undo and lock for save states** on every console.
- **Your library, quicker:** the home screen appears at once, even on a big
  card in a slow adapter, and more games find their cover.

**Install or update**

Install `VitAdhoc.vpk` with VitaShell over the existing bubble (title ID
`GBAD00001`), or use the
[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest).
Games, saves and settings stay in place. For online play your friend installs
it too. Sega games go in any folder under `ux0:ROMS/` (for example `MD`, `SMS`,
`GG`, `SG-1000`).

`VitAdhoc-install.zip` contains the same VPK, full source, guides and
checksums. No commercial games or proprietary BIOS are included.

Full list: [CHANGELOG.md](CHANGELOG.md). An unofficial fan project, not
affiliated with Nintendo, Sega, Bandai or Sony. Built by AI agents (Claude),
directed and play-tested by a human.

SHA-256 `VitAdhoc.vpk`:
`0000000000000000000000000000000000000000000000000000000000000000`
