# Known Issues

Found something else? Please tell us in
[GitHub issues](https://github.com/CBOC-Vita-Lab/vitadhoc/issues), with the
game, its region and what you did.

## Version 2.0: Online Play Is Not Yet Tested On Real Consoles

Online play was verified on the PC test builds against simulated home routers
(several kinds of NAT, packet loss, delay) and a test relay, and against a
local copy of the room service. It has not been run on two real Vitas or
against the live service. Please report what you see, with the lines that
start `online_` in `ux0:data/GBAdhoc/log/gbadhoc.log` (they contain addresses:
read them before sharing).

- **Two Vitas on two different home networks is the meaningful test.** Two
  Vitas on one router only prove the same-router shortcut. How many real
  routers and phone hotspots need the relay is not known.
- **TLS on the console is new.** The app checks the service's certificate with
  its own list of eight root certificates. If Cloudflare serves a chain that
  needs another root, Online says "The online service could not be reached"
  and the log names the certificate. A Vita whose clock was reset (it shows
  2011) cannot check certificates: set the date and time.
- **The relay's hour.** Relay credentials last one hour and are not renewed. If
  Cloudflare refuses to extend a relayed session past it, such a session would
  end as "link lost" after about an hour. A direct link never does. Only the
  live relay can say.
- **Delay and the Great / Good / Slow limits** (80 and 120 ms, delay up to 12
  frames) are first guesses to be tuned on real internet paths.
- **A GBA cable link between two different games sends a whole cartridge once**
  (up to 32 MB); over the relay that is slow. It is kept in the cache for
  later sessions.
- **The service limits itself** (a day's requests, rooms at once, relay use per
  day and month). "Online play is busy for today" is that limit working, not a
  fault; Same Wi-Fi never needs the service.
- **Zipped games** may not be found by the join's library search if the file
  cannot be read without unpacking it; the message then says to add the game.
- **Two players (NES, WonderSwan, Super NES)** runs one shared game on both
  Vitas, so both need the very same game file (a patched copy does not match an
  unpatched one). The WonderSwan's own link cable is not emulated. Super NES
  has no Nearby (ad hoc) option.

## Version 1.4: Not Yet Tested On A Console

Everything new in 1.4 was verified on the PC test builds and in an emulator.
The physical-Vita test is pending, so no 1.4 speed figure exists yet and the
speeds in the [performance report](docs/PERFORMANCE.md) are those of 1.3. The
first things to watch on a console: how large a smoothing step each console
holds at a steady 60 (Image quality Auto starts at a ceiling per console and
steps down by itself), NES sound and the WonderSwan, the Super NES wide view
and the drawing-on-its-own-core hand-over. Each can be turned off in Settings
if it misbehaves (Smooth hand-over, Steady pacing, Wide view, Image quality).

- **Internet play by room code was not in 1.4;** it arrived in 2.0 (above).
- **Hardware anti-aliasing was dropped:** a game picture is one flat rectangle,
  so there is no polygon edge for it to smooth; Smoothing and Image quality do
  that job.
- **The CBOC Vita Installer** does not yet offer NES or WonderSwan folders;
  copy those games with VitaShell (`ux0:ROMS/NES`, `ux0:ROMS/WS`).
- **Link Vitas on the same version.** Link play changed in 1.4 (waiting time,
  keepalive); update both Vitas.

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

## Sega: Genesis / Mega Drive, Master System, Game Gear, SG-1000

- None of this has run on a real Vita; the app side was checked on the PC test
  twin (library, launch file, play-time read-back, crash protection), the
  player on PC and emulated ARM.
- A `.bin` is listed as a Genesis game when its header says SEGA (or it is a
  Super Magic Drive file), also when it is inside a zip. `.68k` (raw Genesis) and
  `.sc` (SC-3000 cartridges, played as SG-1000) files are listed too.
- A `.sms` file that is really a Game Gear game is played as a Master System
  game; save and states stay together under Master System.
- Join online does not know these games yet.
- RetroAchievements badges in the library are not shown for them.
- **Wide view (Sega).** On by default, Off in the menu gives the 4:3 picture back. The **learned map is not done** for any Sega console (the Super NES
  player has it): on the Mega Drive the real picture beside the lead side of a scroll reaches only as far as the name table lets widekit trust it (a few to a few
  tens of pixels), on the Master System and SG-1000 there is nothing beside the picture (the tile map is exactly the screen wide) so only the soft extension
  is shown, and the Game Gear's 256 column band shows what the VDP drew there with the same fade. Lines with a window plane, per 2-cell vertical
  scroll or interlace get the soft extension. The Game Gear band was tested with a test program written for the purpose and Master System ROMs forced to the
  Game Gear: no real Game Gear or SG-1000 ROM was available. The cost ladder (halve the strips, then none) is by the model; nothing has run on a Vita.

## Super NES

- **Wide view at full width on the heaviest games** (Yoshi's Island, Super Mario World, Kirby Super Star): by the model in
  docs/PERFORMANCE.md the draw thread does not fit a 60 fps frame at the full margin, and at margin 41 only at IPC 1.0. The player measures the real
  time on the Vita and narrows the margin only when it must; the compose was made 10 to 28 % cheaper in 2.0 but the passes that draw the margin inside the
  core (merging batches across lines, one pass for the strip, caching it between frames) are not done. Nothing has run on a Vita yet.
- **Rewind** captures a state every fourth frame on the game thread; it now copies about 300 KB (plain cartridge) to 580 KB (SA-1) instead of 823 KB,
  which is estimated at 0.5 to 1.2 ms on the Vita for the heaviest games, not yet measured.

- **Super FX games that keep the chip busy** (Doom, the races of Stunt Race
  FX) run at full game speed, but their 3D view gets less chip time than on a
  SNES, so it animates more slowly than on the console. Yoshi's Island runs
  the chip at full speed. Super Mario RPG and Kirby Super Star dip a frame
  now and then. The SNES player writes its speed to
  `ux0:data/GBAdhoc/snes/log.txt` (`superfx clock` lines show the chip's share).
- The SNES player has its own pause menu (hold **Start + Select**; L + R + Start also works) with ten
  state slots and cheats. Rewind, fast forward, patches and RetroAchievements
  are in it too, as in the Sega player (see "Rewind, fast forward and
  achievements in the Super NES and Sega players" below).
- **Rewind, fast forward and achievements in the Super NES and Sega players.**
  Rewind keeps up to 60 s (30 s or 60 s, 4 to 24 MB, `rewind_mb`), off in two
  player games and in Hardcore. A Super NES state is about 800 KB, so a rewind
  capture there copies that much on the game thread (about 0.1 ms on a PC, a
  few milliseconds on the Vita by our estimate, once every few frames, never
  every frame); the Sega state is 145 KB and costs far less. The right stick
  starts rewind and fast forward one refresh after it moves, and stops one
  refresh after it is let go. Unlimited fast forward is not offered in these
  players. The players have no RetroAchievements settings: sign in and choose
  Hardcore in the app. Achievements and link play: see "Achievements pause
  during link play" under Gameplay. No SG-1000 game was available to test
  achievements.
- **Wide view** is on by default and built never to cost a frame: where a
  game is busy, the real picture at the sides is narrowed and fades into a
  soft extension. It was checked on 15 games for 3000 frames each, not on a
  Vita; if a game looks wrong at the sides, set Wide view to Off in the menu.
- **PAL (European) games** now keep their true 50 Hz speed; **PAL games
  speed** in the menu returns them to the old fast 60 Hz.
- **Players 3 and 4** need the Super Multitap (Auto turns it on for games
  known to use it) and extra controllers. Controllers beyond the first have
  not been tried on a real Vita yet.

## NES And Famicom

- **New in 1.4, and the least tested console here.** NES has run on the PC
  build with test cartridges only: no commercial NES game has been tried
  yet, and none has been played on a real Vita. Nothing is measured, and no
  speed is promised. The ready-made cheat list covers about 2,000 NES games,
  found by the game's checksum or name; looking one up from the menu has not
  been tried on a console.
- **Famicom Disk System games** need your own `disksys.rom` (the system's BIOS,
  which cannot be included). Put it beside the game or in
  `ux0:data/GBAdhoc/nes/`. Without it the game says so and returns.
- NES games use the same menu, state slots, Continue, fast forward, cheats
  (Game Genie codes and RAM codes), patches and RetroAchievements as the Game
  Boy games, rewind too. NES games link one way, Two players (Play together
  in the menu). A few cheat codes of the "RAM" kind change what the game
  reads but not what it saves.
- **Box art** finds a NES game by its contents or its name. A UNIF game
  (`.unf`, `.unif`) is found by name only, so a file with an unusual name may
  stay without a cover until you give it one (the Cover button).
- Game descriptions are written for GBA, GB, GBC and SNES games first; a NES
  game may show none.
- **Wide view** is new and measured only on the PC build. Some boards (MMC5,
  MMC2/MMC4 and a few more) show soft edges only. The look-ahead that would
  show the level before it scrolls in is not used yet.

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
  `ux0:data/GBAdhoc/cheats/` or typed in the Cheats page. The ready-made
  list for WonderSwan games has a single game (the source data is that small);
  use RetroArch's `.cht` files or type codes in for the rest.
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
- **Achievements pause during link play** and start counting again afterwards.
  In the Game Boy Advance, Game Boy and NES games that happens inside the game, as
  after loading a state. In the Super NES and Sega players a linked game is a
  whole session of its own, so nothing is counted in it, and achievements count
  again from the next game you start on its own.
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
