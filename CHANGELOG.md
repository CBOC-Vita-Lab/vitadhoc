# What's New

## Version 1.1 — October 25, 2026

**Sound fixes.** A rebuilt audio path for all three systems.

- **Steadier output:** audio now plays from two fixed, aligned buffers that
  take turns, so there are no more gaps or clicks when a frame runs long.
- **Correct stereo:** left/right routing of the Game Boy Advance's direct-sound
  channels is fixed, so effects and music come out of the right sides.
- **Cleaner start, stop and pause:** a short fade at each edge removes the pop
  when you open a game, pause, resume or load a state, and the sound
  channel is closed properly when you leave a game.
- **Right pitch and tempo at any speed:** square, wave and noise channels now
  follow the real sample rate, so music keeps its pitch and tempo when the
  output rate or emulation speed changes.
- **Fresh start after loading a state:** leftover sound from the previous game
  or state no longer carries over.
- **Safer against bad data:** damaged save-state sound data is rejected instead
  of producing noise.

Saves, save states, settings, ROMs and the BIOS are untouched, and the app
keeps the same ID, so 1.1 installs straight over 1.0. The sound changes were
checked with new automated tests (including memory-error checking); they have
not yet been confirmed on every game on a Vita.

## Version 1.0 — October 24, 2026

The first release. Game Boy Advance, Game Boy and Game Boy Color, built as a
native PS Vita app from GBAdhoc's emulator.

**Your library**
- **A home screen like a console's own:** big tiles, the game's picture behind
  them, and touch or buttons to move around.
- **Box art and title screens download by themselves** on Wi-Fi, matched by
  name, cartridge code or checksum. Your own pictures always win.
- **A few lines about each of 1,300 games**, from Wikipedia, built in.
- **Previews:** rest on a game and the last ten seconds you played move behind it.
- **Continue where you left off.** Leaving a game keeps the moment you left it,
  refreshed every two minutes and right after the game saves, so even a Vita
  closed from LiveArea continues. Cross continues; Up starts from your battery
  save instead.
- **Recent tab and play time,** with sorting by name, last played or most played.
- **Starred and Find:** Find matches every word in any order and ignores accents
  and punctuation ("zelda minish" finds The Minish Cap).
- **Your existing folders:** games in `ux0:ROMS/` or RetroFlow's folders are found
  without moving them.

**The picture**
- **Original:** crisp, even pixels at any size, including the full height.
- **xBRZ and Scale3x** smoothing on the Vita's spare cores.
- **Wide view:** more of the game instead of black bars, made for Pokémon
  Emerald and its siblings, with options for 3D racing tracks.
- **Real handheld colours,** motion blend, LCD grid and scanlines, five sizes.
- **Super Game Boy borders,** kept for next time.
- **A look for every game:** Settings > This game > Display settings.

**Speed**
- **A steady 60 frames a second,** with the picture drawn on a second core.
- **Keep full speed:** short hiccups are caught up without a gap in the sound.
- **Low lag:** up to a frame less between a press and the picture.
- **Games start the moment you pick them.**
- Rewind (60 seconds), fast forward, five save-state slots, turbo buttons and
  screenshots.

**Play together**
- **GBA Link Cable and Wireless Adapter games** between two Vitas, over Wi-Fi,
  the internet or ad hoc.
- **Game Boy and Game Boy Color links,** even between different cartridges.
- **Two players on one Vita,** side by side.
- **Share saves and states,** and receive **Mystery Gift** cards from a phone.

**More to play**
- **RetroAchievements.** Sign in from Settings; your password is sent once and
  never stored. Unlocks are sent again later if you were offline. Hardcore mode
  turns off state loading, rewind and cheats.
- **Tilt, turn and sunlight cartridges:** WarioWare: Twisted! (gyro), Yoshi
  Topsy-Turvy and Koro Koro Puzzle (tilt), Kirby Tilt 'n' Tumble (GBC tilt) and
  Boktai 1–3 (sunlight). Tilt the Vita or use the right stick.
- **ROM hacks and translations:** `Game.ips`, `Game.bps` or `Game.ups` next to
  `Game.gba` is applied as the game starts; a patch made for another version of
  the game is refused.
- **Cheats** for about 2,190 games, plus your own codes.
- **An open GBA BIOS built in.**

**Safe saves**
- Game Boy and Game Boy Color saves reach the memory card within about two
  seconds, and a save that can't be written (a full card) tells you so.
- Favourites, two-player saves and shared saves are written so that an
  interrupted write can't lose them.

**Under the hood**
- gpSP with the latest upstream fixes, plus VitAdhoc's own: games whose start-up
  code froze on a white screen now boot, and many fixes from independent code
  reviews, all covered by a large automated test suite. Engineering details are
  in [Technical notes](docs/TECHNICAL.md#version-10-changes).
