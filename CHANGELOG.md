# What's New

## Version 1.0 — October 23, 2026

The big one: seven new features, and a library that feels like a console's own.

**New ways to play**
- **Continue where you left off.** Leaving a game keeps the moment you left it.
  The point is refreshed every two minutes and right after the game saves, so
  even a Vita closed from LiveArea continues. On the home screen, Cross
  continues and Up starts from your battery save instead.
- **RetroAchievements.** Sign in from Settings > RetroAchievements; your
  password is sent once and never stored. Achievements unlock as you play, are
  listed in the in-game Settings, and are sent again later if you were
  offline. Hardcore mode turns off state loading, rewind and cheats.
- **Tilt, turn and sunlight cartridges.** WarioWare: Twisted! (gyro), Yoshi
  Topsy-Turvy and Koro Koro Puzzle (tilt), Kirby Tilt 'n' Tumble (GBC tilt) and
  Boktai 1–3 (sunlight). Settings > This game: tilt the Vita or use the right
  stick, strength, direction, set level, and how much sun Boktai sees.
- **ROM hacks and translations.** `Game.ips`, `Game.bps` or `Game.ups` next to
  `Game.gba` is applied as the game starts. Saves keep the original's name, and
  a patch made for another version of the game is refused.
- **Super Game Boy borders**, kept for next time. Settings > Display > SGB
  border turns them off.

**Your library**
- **Recent tab and play time.** Select on the home screen cycles All games,
  Starred and Recent. Each game shows when you last played and for how long.
- **Game order:** by name, last played or most played (Settings > Library).
- **Find** matches every word in any order and ignores accents and punctuation:
  "zelda minish" finds The Minish Cap.
- **A look for every game:** Settings > This game > Display settings.
- **Faster browsing:** one card scan fills all three console tabs, and box art,
  descriptions and previews load in the background.

**Better picture**
- **Image quality: Original** draws crisp, even pixels at any size, including
  the full-height fit.
- **Smoother frames** with xBRZ and the wide view: each picture reaches the
  screen sooner, and smoothing works less on fades and cut scenes.

**Safer saves**
- Game Boy and Game Boy Color saves reach the memory card within about two
  seconds.
- A save that can't be written (a full card) now tells you so.
- Favourites, two-player saves and shared saves are written so that an
  interrupted write can't lose them.

**Fixes**
- **Picking a game starts it straight away**, even before its background art
  has loaded. Before, the press could wait several seconds with no sign it
  was taken.
- **Games that froze on a white screen at start now boot**, such as copies of
  Pokémon Pinball: Ruby & Sapphire with an added intro. Two things the real
  console does, and those intros rely on, now work the same way.

**Under the hood**
- Many fixes from independent code reviews, and a much larger automated test
  suite. Engineering details are in
  [Technical notes](docs/TECHNICAL.md#version-10-changes).

## Earlier versions

- **Build 21 (October 2026):** finds games in other folders, including
  `ux0:ROMS/` and RetroFlow's, without moving them; complete licence notices;
  the GBAdhoc maintainer's licence permission recorded.
- **Build 20 and before:** the native Vita app with its home screen, automatic
  box art, previews, wide view, xBRZ, rewind, turbo buttons, cheats, Mystery
  Gift and link play between two Vitas or two players on one Vita.
