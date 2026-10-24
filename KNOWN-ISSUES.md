# Known Issues

Found something else? Please tell us in
[GitHub issues](https://github.com/CBOC-Vita-Lab/vitadhoc/issues), with the
game, its region and what you did.

## Still Being Confirmed On A Vita

Version 1.0 passed its full test suite and plays at full speed on a Vita.
These still need more time on a real console:

- **Motion games:** whether turning and tilting go the right way and feel
  strong enough. If one feels backwards, use Settings > This game >
  Direction; Motion strength changes how much a tilt counts.
- **RetroAchievements:** signing in over the Vita's own secure connection.
  If sign-in fails, the reason is in `ux0:data/GBAdhoc/log/gbadhoc.log`
  (lines starting with `ra_http`).
- **Two Vitas linked together** (Wi-Fi, internet and ad hoc), sharing saves,
  and Wireless Adapter games.
- **Mystery Gift** from a phone, all the way into the game's save.

## Performance

- The heaviest picture settings (xBRZ at full height plus the wide view) are
  the most demanding. If a game slows down, try Image quality: Original.
- The GPU version of xBRZ (`gpu_xbrz = 1`) is an experiment and too slow for
  normal play. Leave it off.

## Gameplay

- **Hardcore mode** applies from the next game you start, and only to games
  RetroAchievements knows.
- **Achievements pause during link play** and start counting again afterwards,
  as after loading a state.
- **Wide view** is made for Pokémon Emerald and its siblings. In other games
  the edges can miss people or weather.
- Some cheats need their "Enable Code (Must Be On)" entry first; a few code
  types are marked unsupported. Cheats switch off while linked and in Hardcore.
- Three- and four-player links and single-cartridge multiboot aren't supported.
- Two games with the same file name share box art, cheats and previews. Give
  different games different names.
- Each console's list shows up to 2,048 games.

## Install

- Back up `ux0:data/GBAdhoc/` and any saves next to games in other folders
  before a big update.
- A `Game.ips` that sits next to both `Game.gb` and `Game.gba` is not applied
  to either. Name it after the whole file instead, for example `Game.gba.ips`.

## If Something Goes Wrong

- **Turn off the automatic box art:** add `art = 0` to `ux0:data/GBAdhoc/vita.ini`.
- **Turn off previews:** Settings > Interface > Game previews, or `clips = 0`
  in `vita.ini`.

The log is `ux0:data/GBAdhoc/log/gbadhoc.log`. It can contain game names and
network addresses, so read it before sharing.
