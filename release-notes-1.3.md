## VitAdhoc 1.3: Super NES, split screen and your own covers

Game Boy Advance, Game Boy, Game Boy Color and now Super NES, native on PS
Vita. One home screen with box art for all four, previews of your own play,
Continue, smoothing, rewind, RetroAchievements and link play, built on the
work of [GBAdhoc](https://github.com/ShoshinFauteux/GBAdhoc), gpSP, TGB Dual
and [Snes9x](https://github.com/snes9xgit/snes9x).

### New in 1.3

- **Super NES games.** `.sfc` and `.smc` games (zipped too) appear on a new
  SNES shelf, found in `ux0:ROMS/` (any folder inside it), RetroFlow's folders
  and your own folders, with box art.
- **Accurate and spread over the Vita's cores.** Built on Snes9x 1.63: sound
  and picture each run on a core of their own, with exactly the same result as
  one core. Super FX games no longer rebuild their colours hundreds of times a
  frame.
- **Fills the screen without stretching.** The SNES's own 4:3 shape at full
  height with a soft glow at the sides, plus sharp scaling, CRT scanlines and
  an LCD grid (1-10). Up to four players with the Super Multitap.
- **Split screen for two players** on one Vita or PS TV: two equal pictures,
  a second controller on any port, or player 2 on the touch screen.
- **Trade between versions.** Red, Blue and Yellow link with Gold, Silver and
  Crystal, and two Vitas can trade between Ruby and Sapphire or FireRed and
  LeafGreen.
- **Your own cover pictures.** The Cover button, or a PNG or JPG in `covers/`
  named like the game. Your picture always wins over downloaded art.
- **Safer saves.** Saves from other emulators keep their clock data, RetroArch
  saves stay in sync both ways, and one backup is kept before anything is
  written over a save from elsewhere. ROM hacks can choose their save type.
- **Cleaner game folders.** Only the game and its battery save stay beside it;
  states and thumbnails move to `ux0:data/GBAdhoc/` on first open.
- **500 MHz** when an overclock plugin such as PSVshell allows it.

### Fixes

- Mortal Kombat: Deadly Alliance no longer sits on its logo for 40 seconds or
  covers its menus in green speckles. Need for Speed: Underground no longer
  hangs at its language screen.
- Engine fixes from GBAdhoc 4.5.0 (Golden Sun 2, Mario Golf, Mario Tennis,
  OpenLara, rare instructions).
- No growing sound delay after the menu, fast forward or rewind; no loud push
  from Game Boy games that switch a sound channel off.
- Games without a Wide view profile are never stretched; Pokemon clocks keep
  the right time after Continue; the right stick only rewinds if you turn it on.
- A busy moment skips drawing a frame instead of lowering the smoothing or
  picture options you chose.

### Install or update

Install `VitAdhoc.vpk` with VitaShell over the existing bubble (title ID
`GBAD00001`). Games, saves, settings and artwork stay in place. Two Vitas
linking together both need 1.3.

`VitAdhoc-install.zip` contains the same VPK, full source, guides and
checksums. No commercial games or proprietary BIOS are included. The SNES
player uses Snes9x, whose licence allows non-commercial use only: VitAdhoc is
and stays free.

[What's new in detail](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/CHANGELOG.md)
| [Install and save import](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/INSTALL.md)
| [Known issues](https://github.com/CBOC-Vita-Lab/vitadhoc/blob/main/KNOWN-ISSUES.md)

SHA-256 `VitAdhoc.vpk`:
`935d938cd0fa9293ed6fa5847bb83c72e0a779814e4083f9b6d64c0f6e3586b2`
