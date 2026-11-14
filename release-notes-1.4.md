## VitAdhoc 1.4: NES, WonderSwan and a steadier picture

The biggest update since Super NES. Everything below is verified on the PC
test builds and an emulator; it has not been measured on a real Vita yet, so
no speed is promised for the new parts.

**New consoles**

- **NES and Famicom.** A new shelf for `.nes`, `.fds` (Famicom Disk System;
  needs your own `disksys.rom`), `.unf` and `.unif` games, zipped too. Box
  art, Continue, ten state slots with pictures, rewind, fast forward, cheats
  and patches work as for the Game Boy games. Saves are `Game.srm` beside the
  game. Two-player link play is not available for NES.
- **WonderSwan and WonderSwan Color.** A WS shelf for `.ws`, `.wsc` and
  `.pc2` games with the same features. Tall games turn upright by
  themselves, or play them with the Vita on its side (Rotation and Vertical
  play, in the game's settings). The console's 75 frames a second are blended
  into the Vita's 60 for smooth scrolling.

**Picture**

- **Wide view for NES and Super NES.** The real picture stays untouched in
  the middle of the 16:9 screen and the sides continue it softly. Nothing is
  guessed. Wide view is Auto, On or Off in Settings. GBA, GB, GBC and
  WonderSwan fill the screen's height.
- **Auto picture quality, on by default.** The best smoothing the console can
  hold at a steady 60, eased down a little only when a game needs it, and
  brought back when there is room. A new MMPX 2x smoothing sits between xBRZ
  and sharp. What you set by hand is never changed.
- **Steady pacing and smooth hand-over.** Drawing runs on its own core, and a
  scene that is too busy shows an even rhythm instead of uneven drops.

**Super NES**

- European (PAL) games run at their true 50 Hz speed instead of 20 % fast.
- A new audio pacer cures the crackle and uneven sound (GitHub #15).
- Cheats, ten state slots with pictures and a new in-game menu.
- Super FX games (Doom, Stunt Race FX) get more game speed.

**Saves and library**

- **Saves are backed up before anything is written over them,** checked
  before the change, in `backups/`.
- **RetroArch can see your older saves,** and **Settings > This game >
  Export save for other emulators** makes a plain `.srm` or `.sav` on request
  (GitHub #3). A tap on the screen now opens the menu on every console
  (GitHub #3).
- **Game names** in Settings > Library shows the title or the file name
  (GitHub #16).
- **Cheats on every console,** with ready-made lists for NES games.

**Also**

- Fewer stutters in GBA games that rewrite their own code.
- Link play waits 20 seconds before giving a silent partner up, and
  achievements pause while two consoles are linked.
- Settings opens on the part you most likely want and hides rows that do not
  apply; the home screen has an always-visible button hint line and bigger
  touch targets.

**Not in 1.4:** internet play by room code (it needs a relay server) and
hardware anti-aliasing. See [Known Issues](KNOWN-ISSUES.md): NES has not
been tried on commercial games yet and the WonderSwan cheat list has one
game.

### Install or update

Install `VitAdhoc.vpk` with VitaShell over the existing bubble (title ID
`GBAD00001`), or use the
[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest).
Games, saves and settings stay in place. The installer may need a newer
version before it offers NES and WonderSwan folders; VitaShell works now.

`VitAdhoc-install.zip` contains the same VPK, full source, guides and
checksums. No commercial games or proprietary BIOS are included.

SHA-256 `VitAdhoc.vpk`:
`0000000000000000000000000000000000000000000000000000000000000000`
