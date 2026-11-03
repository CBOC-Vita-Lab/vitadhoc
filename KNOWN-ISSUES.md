# Known Issues

Found something else? Please tell us in
[GitHub issues](https://github.com/CBOC-Vita-Lab/vitadhoc/issues), with the
game, its region and what you did.

## Still Being Confirmed On A Vita

Historical version 1.0 measurements cover specific games and settings, not
every cartridge or ROM hack. Version 1.2 has source and host-test evidence;
current physical-Vita measurements and their limits are recorded in the
[performance report](docs/PERFORMANCE.md). These still need more time on a
real console:

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

- **Pokemon Unbound 2.1.1.1:** the tested opening beach scene ran at about
  60 emulated fps with full-screen CPU xBRZ 3x, but frontend output was lower
  and audio underruns were recorded. Exploratory setup showed additional
  scene-dependent slowdown. This is not full-game acceptance; see the
  [measurements and exact conditions](docs/PERFORMANCE.md).
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
- The same game in two folders (for example `ux0:data/GBAdhoc/roms/` and
  `ux0:ROMS/GBA/`) shows up twice, and each copy has its own save. If a save
  seems missing, check for the other copy of the game.
- Each console's list shows up to 2,048 games.
- **Zipped games:** the first game in the zip with a `.gba`, `.gb` or `.gbc`
  name is the one played. Patches, and two-player play with a different game
  for each player, need the game unzipped. Encrypted and ZIP64 archives are
  not read.
- **Starting from a launcher** works when VitAdhoc is closed; close it first.
- **Sharing states between two Vitas** covers slots 1 to 5 (so a Vita on
  1.1 can take them too).
- **Names in other alphabets** (Japanese, for example) show as `?` in the
  list: the menus' font has Latin letters only. The games still play.

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
