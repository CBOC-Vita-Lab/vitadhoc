# Known Issues

Found something else? Please tell us in
[GitHub issues](https://github.com/CBOC-Vita-Lab/vitadhoc/issues), with the
game, its region and what you did.

## Testing In Progress

Current Vita results are in the [performance report](docs/PERFORMANCE.md).
Testing continues on:

- **Motion games:** whether turning and tilting go the right way and feel
  strong enough. If one feels backwards, use Settings > This game >
  Direction; Motion strength changes how much a tilt counts.
- **RetroAchievements:** signing in over the Vita's own secure connection.
  If sign-in fails, the reason is in `ux0:data/GBAdhoc/log/gbadhoc.log`
  (lines starting with `ra_http`).
- **Two Vitas linked together** (Wi-Fi, internet and ad hoc), sharing saves,
  and Wireless Adapter games.
- **Two Vitas linking two different GBA games** (Ruby with Sapphire, FireRed
  with Emerald): tested on the PC build with test cartridges and with two real
  Pokemon games (a short session), but not yet on two Vitas. Not measured: the memory with two
  16 MB games at once, and the time to send a game over the internet. A
  full trade between versions has not been played through.
- **Mystery Gift** from a phone, all the way into the game's save.

## Super NES

- **Super FX games that keep the chip busy** (Doom, the races of Stunt Race
  FX) run at full game speed, but their 3D view gets less chip time than on a
  SNES, so it animates more slowly than on the console. Yoshi's Island runs
  the chip at full speed. Super Mario RPG and Kirby Super Star dip a frame
  now and then. The SNES player writes its speed to
  `ux0:data/GBAdhoc/snes/log.txt` (`superfx clock` lines show the chip's share).
- The SNES player has its own pause menu (hold **L + R + Start**) with four
  state slots. Rewind, fast forward, Continue, cheats, patches and
  RetroAchievements are for GBA and Game Boy games only for now.
- **Players 3 and 4** need the Super Multitap (Auto turns it on for games
  known to use it) and extra controllers. Controllers beyond the first have
  not been tried on a real Vita yet.

## NES And Famicom

- **New in 1.4, and the least tested console here.** NES has run on the PC
  build with test cartridges; no game has been played on a real Vita yet.
  Nothing is measured, and no speed is promised.
- **Famicom Disk System games** need your own `disksys.rom` (the system's BIOS,
  which cannot be included). Put it beside the game or in
  `ux0:data/GBAdhoc/nes/`. Without it the game says so and returns.
- NES games use the same menu, state slots, Continue, fast forward, cheats
  (Game Genie codes and RAM codes), patches and RetroAchievements as the Game
  Boy games, rewind too. Link play is not available for NES games; the link
  card says so. A few cheat codes of the "RAM" kind change what the game
  reads but not what it saves.
- **Box art** finds a NES game by its contents or its name. A UNIF game
  (`.unf`, `.unif`) is found by name only, so a file with an unusual name may
  stay without a cover until you give it one (the Cover button).
- Game descriptions are written for GBA, GB, GBC and SNES games first; a NES
  game may show none.

## WonderSwan

- **New in 1.4, untested on a real Vita.** It has run on the PC build with
  test cartridges and one game; nothing is measured. A busy scene may need
  two console frames in one Vita refresh; if that is too slow on the Vita,
  turning off Smooth 75 Hz motion does not make it lighter (it changes only
  how the picture is shown).
- **Rotation** follows what the game shows (its LCD icons, the buttons it
  reads, the cartridge). A game that gives no sign keeps the way its
  cartridge says; set Rotation by hand in the game's settings.
- **Buttons.** Held sideways: Circle is A, Cross is B; a game's second cross
  is not on a button there (the right stick fast-forwards and rewinds). Upright: the
  d-pad and the four face buttons are the two crosses, L is A and R is B; there
  Triangle and Square are the game's, so use the right stick for fast forward.
  Button mapping in Settings does not apply to WonderSwan games.
- **Cheats** are RAM codes (`AAAAA:VV`, hex), from the game's `.cht` file in
  `ux0:data/GBAdhoc/cheats/` or typed in the Cheats page. No cheat list for
  WonderSwan or NES games is included yet: use RetroArch's `.cht` files.
- Pocket Challenge V2 (`.pc2`), WonderWitch and games with a clock are
  untested. No descriptions for WonderSwan games yet; names and covers come
  from the No-Intro catalogue.

## Games

How we checked the games people reported, and what is still unchecked, is in
[Game Compatibility](docs/COMPATIBILITY.md). Not fixed: sprites in the wrong
order in Golden Sun, Mario Golf and The Minish Cap (OAM order), and any
flicker or missing instruments, which we could not judge without a Vita.

## Performance

- **500 MHz needs an overclock plugin** such as PSVshell. Without one the Vita
  stays at 444 MHz (Settings > CPU speed says "Needs an overclock plugin").
  If the app cannot write PSVshell's profile folder it tries once, and then
  leaves it to you (create the profile in PSVshell's own menu).

- **Pokemon Unbound 2.1.1.1:** audio underruns were recorded at full-screen
  CPU xBRZ 3x. See the [measurements and settings](docs/PERFORMANCE.md).
- The heaviest picture settings (xBRZ at full height plus the wide view) are
  the most demanding. If a game slows down, try Image quality: Original.
- The GPU version of xBRZ (`gpu_xbrz = 1`) is an experiment and too slow for
  normal play. Leave it off.

## Gameplay

- **Hardcore mode** applies from the next game you start, and only to games
  RetroAchievements knows.
- **Achievements pause during link play** and start counting again afterwards,
  as after loading a state.
- **Game clocks follow the Vita's clock.** A game saved by an older version
  while its clock ran ahead can see the date go backwards once; the clock then
  waits until the real time catches up. Two Vitas in link play share one
  repeatable clock for the session. Save type and Clock (RTC) can only be
  changed inside a running game, and apply the next time it starts.
- **Wide view** is made for Pokémon Emerald and its siblings. In other games
  the edges can miss people or weather.
- Some cheats need their "Enable Code (Must Be On)" entry first; a few code
  types are marked unsupported. Cheats switch off while linked and in Hardcore.
- Three- and four-player links and single-cartridge multiboot aren't supported.
- **1.3 links only with 1.3.** Engine fixes changed the check two Vitas make
  before linking, so update both Vitas.
- Two games with the same file name share box art, cheats and previews. Give
  different games different names.
- The same game in two folders (for example `ux0:data/GBAdhoc/roms/` and
  `ux0:ROMS/GBA/`) shows up twice, and each copy has its own save. If a save
  seems missing, check for the other copy of the game.
- Each console's list shows up to 2,048 games.
- **Zipped games:** the first game in the zip with a `.gba`, `.gb`, `.gbc`,
  `.sfc`, `.smc`, `.nes`, `.fds`, `.unf` or `.unif` name is the one played. Patches, and two-player play with a different game
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
