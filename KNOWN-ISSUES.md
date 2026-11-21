# Known Issues

Thanks for trying VitAdhoc 2.0. This page is the honest list of what we know
is rough, what is new and still untried, and how to switch things off if one
misbehaves. Found something else? Please tell us in
[GitHub issues](https://github.com/CBOC-Vita-Lab/vitadhoc/issues), with the
game, its region and what you did.

## Version 2.0: Not Yet Tested On A Console

Everything new in 2.0 was verified on PC test builds, in emulators and by
models of the Vita's speed. **None of it has run on a real Vita yet**, so no 2.0
speed figure exists and nothing here is promised: every console aims for a
locked 60 frames a second (50 for European games), and that is what the first
tests on a Vita will check. Current measured results are those of earlier
versions, in the [performance report](docs/PERFORMANCE.md).

The first things to watch: the Sega consoles, online play on real networks,
the Super NES wide view and Super FX games, and how large a smoothing step each
console holds at a steady 60 (Image quality Auto starts at a ceiling per
console and steps down by itself). Each can be turned off in Settings if it
misbehaves: Smooth hand-over, Steady pacing, Wide view, Image quality, Quick
response, No sprite flicker.

- **Hardware anti-aliasing was dropped:** a game picture is one flat
  rectangle, so there is no polygon edge for it to smooth; the smoothing
  upscalers and Image quality do that job.
- **The console picker** (Triangle on the home screen) is new and was checked
  on the PC test twin only.
- **The CBOC Vita Installer** may not offer the NES, WonderSwan or Sega
  folders yet; copy those games with VitaShell (for example `ux0:ROMS/NES`,
  `ux0:ROMS/MD`).

## Online Play

Online play was verified on PC test builds against simulated home routers
(several kinds of NAT, packet loss, delay), a test relay and a local copy of
the room service. It has not run on two real Vitas or against the live service.
Please report what you see, with the lines that start `online_` in
`ux0:data/GBAdhoc/log/gbadhoc.log` (they contain addresses: read them before
sharing).

- **Two Vitas on two different home networks is the meaningful test.** Two
  Vitas on one router only prove the same-router shortcut. How many real
  routers and phone hotspots need the relay is not known.
- **The online service limits itself** (a day's requests, rooms at once, relay
  use per day and month). "Online play is busy for today" is that limit
  working, not a fault; try tomorrow or use Same Wi-Fi, which never needs the
  service.
- **TLS on the console is new.** The app checks the service's certificate with
  its own list of eight root certificates. If the service serves a chain that
  needs another root, Online says "The online service could not be reached"
  and the log names the certificate. A Vita whose clock was reset (it shows
  2011) cannot check certificates: set the date and time.
- **The relay's hour.** Relay credentials last one hour and are not renewed.
  If the relay refuses to extend a session past it, a relayed session would
  end as "link lost" after about an hour. A direct link never does.
- **Delay and the Great / Good / Slow limits** (80 and 120 ms, delay up to 12
  frames) are first guesses, to be tuned on real internet paths.
- **A GBA cable link between two different games sends a whole cartridge once**
  (up to 32 MB); over the relay that is slow. It is kept in the cache for
  later sessions.
- **Two players (NES, WonderSwan, Super NES, Genesis / Mega Drive, Master
  System, SG-1000)** runs one shared game on both Vitas, so both need the very
  same game file (a patched copy does not match an unpatched one). The
  WonderSwan's own link cable is not emulated, the Game Gear has no two-player
  mode, and the Super NES and Sega consoles have no Nearby (ad hoc) option.
  Both Vitas need VitAdhoc 2.0.
- **Zipped games** may not be found by the join's library search if the file
  cannot be read without unpacking it; the message then says to add the game.
- **State loading was hardened on the PC test build, not on a Vita.** When a
  link starts, the host's game state is loaded on the guest's console, and the
  same loader reads save state files and Share game data. Every core now
  checks the inside of a state, and each was fuzzed with a memory checker
  without a crash, hang or report. Two limits: the GBA fuzzing ran the
  interpreter build (the Vita runs the dynamic recompiler, which a memory
  checker cannot watch; the state loader is the same code), and Genesis
  cartridges with unusual hardware (EEPROM or flash saves, bank mappers) were
  fuzzed only through the public test games available here.

## Sega: Genesis / Mega Drive, Master System, Game Gear, SG-1000

- **New and untried on a Vita.** The app side was checked on the PC test twin
  (library, launch file, play-time read-back, crash protection) and the player
  on PC and emulated ARM, with homebrew and test cartridges written for the
  purpose: no commercial Sega ROM, and no real Game Gear or SG-1000 ROM, was
  available. Cost figures are a model.
- **Files.** A `.bin` is listed as a Genesis game when its header says SEGA (or
  it is a Super Magic Drive file), also inside a zip. `.68k` (raw Genesis) and
  `.sc` (SC-3000 cartridges, played as SG-1000) are listed too. A `.sms` file
  that is really a Game Gear game plays as a Master System game; its save and
  states stay together under Master System.
- **Not there yet:** RetroAchievements badges in the library for these games.
  No SG-1000 game was available to test achievements.
- **No sprite flicker** (Master System, Game Gear, SG-1000; on by default) was
  checked with homebrew only. A game that hides something behind the console's
  sprite limit on purpose would show it: turn the row off for that game. It is
  not offered for the Mega Drive, which keeps its own limit.
- **Quick response** shows what you press one frame sooner by working out the
  next frame ahead; the game runs exactly the same and states, rewind and saves
  are unaffected. It costs about twice a plain frame, so Auto is the default on
  the Master System, Game Gear and SG-1000 (it runs only while the game leaves
  the room, and stops when frames run late) and Off on the Mega Drive, where
  the heavy games do not fit (On there forces it and can drop frames). It is
  off in linked play, in fast forward, in rewind and in Steady pacing's half
  mode.
- **Save states.** A state holds a small extra block for what the core never
  saved (state version 2); a state from before still loads, and the frames
  right after it can differ from the uninterrupted game by the missing block
  (a sprite flag, a half-written EEPROM command). A cartridge with a flash chip
  (the SGDK save-manager type) has states 256 KB larger.
- **Wide view** is on by default, and Off in the menu gives the 4:3 picture
  back. Beside the picture you see real picture only where it can be shown
  truthfully, a soft continuation of the edge elsewhere:
  - The **learned picture** (what the game already showed at that place) is
    kept for the play session only; a state load, a rewind or a reset forgets
    it. A level you have not walked through yet has the soft side on the side
    you walk towards.
  - **Master System:** the sides are learned picture only (the console has
    nothing beside its 256 pixels). A game that masks the first 8 columns
    keeps its border colour there on lines whose left side is not known.
    Games in the 224 / 240 line modes, and games that scroll the right 8
    columns apart, get the soft sides.
  - **SG-1000:** soft sides only (these games have no scroll registers).
  - **Mega Drive / Genesis:** lines under a window plane show the picture
    behind it where the window is empty at that edge; lines whose columns
    scroll apart vertically get a strip only where the three edge columns
    scroll together. Interlaced pictures get the soft sides.
  - A tile the game changes while it is off screen (a broken block, an opened
    door) can show its old look beside the picture until the screen reaches
    it again; the same limit as on the Super NES and NES.
  - The cost ladder (halve the strips, then none, in Auto) is the safety net
    if a game needs the time.
- **Smooth 50 Hz** (PAL games speed, on by default) is judged by arithmetic and
  the twin, not by eye on a Vita. If the mixed refresh looks wrong in a game,
  choose True 50 Hz.

## Super NES

- **Wide view at full width on the heaviest games** (Yoshi's Island, Super
  Mario World, Kirby Super Star, Zelda, Chrono Trigger, Super Metroid) is a
  model, not a measurement. By the model every thread fits a 60 fps frame on
  the 15 test games, but the three cores have little room left at the
  pessimistic end: on a console without the fourth core a heavy scene may make
  Auto narrow the margin (On keeps the full width and may drop frames). If a
  game looks wrong at the sides, set Wide view to Off. A side that shows the
  soft frame is refreshed every second frame; a rare short fade of the old
  picture can be left out there (it did not happen once in 90,000 test
  frames).
- **Super FX games that keep the chip busy** (Doom, the races of Stunt Race
  FX) run at full game speed, but their 3D view gets less chip time than on a
  SNES, so it may animate more slowly. The chip runs on a new, faster exact
  interpreter (`gsu_fast`, on by default, results identical to the old one on
  Doom, Stunt Race FX, Yoshi's Island and 58 test programs, x86 and ARM under
  emulation). By the model Doom's game thread comes down to about 10 to 14 ms
  of a 16.7 ms frame; that is not measured on a Vita. Not tested on Star Fox,
  Winter Gold, Vortex or Dirt Trax FX (no ROM was available). Super Mario RPG
  and Kirby Super Star may dip a frame now and then. The player writes its
  speed to `ux0:data/GBAdhoc/snes/log.txt`.
- **Rewind** captures a state every fourth frame at best (less often in a busy
  game) and costs an estimated 0.5 to 1.2 ms on the Vita; it is off in two
  player games and in Hardcore. Not measured.
- **PAL (European) games** keep their true 50 Hz speed. PAL games speed in the
  menu chooses Smooth 50 Hz (default), True 50 Hz or the old fast 60 Hz. It is
  off in a linked game and with the wide view's split capture.
- **Players 3 and 4** need the Super Multitap (Auto turns it on for games
  known to use it) and extra controllers, not tried on a real Vita yet.
- The Super NES and Sega players have no RetroAchievements settings: sign in and
  choose Hardcore in the app.

## NES And Famicom

- **New in 1.4 and still the least tested console here.** NES has run on the PC
  build with test cartridges only: no commercial NES game has been tried, and
  none has been played on a real Vita. The ready-made cheat list covers about
  2,000 games by checksum or name; looking one up from the menu has not been
  tried on a console.
- **Famicom Disk System games** need your own `disksys.rom` (the system's BIOS,
  which cannot be included). Put it beside the game or in
  `ux0:data/GBAdhoc/nes/`. Without it the game says so and returns.
- A few "RAM" cheat codes change what the game reads but not what it saves.
- **Box art** finds a NES game by its contents or its name; a UNIF game
  (`.unf`, `.unif`) by name only, so a file with an unusual name may stay
  without a cover until you give it one (the Cover button). Game descriptions
  are written for GBA, GB, GBC and SNES games first; a NES game may show none.
- **Wide view** is measured only on the PC build. Some boards (MMC5,
  MMC2/MMC4 and a few more) show soft edges only.

## WonderSwan

- **New in 1.4, untested on a real Vita.** It has run on the PC build with test
  cartridges and one game. A busy scene may need two console frames in one
  Vita refresh; if that is too slow, turning off Smooth 75 Hz motion does not
  make it lighter.
- **Rotation** follows what the game shows (its LCD icons, the buttons it
  reads, the cartridge). A game that gives no sign keeps the way its cartridge
  says; set Rotation by hand in the game's settings.
- **Buttons.** Held sideways, a game's second cross is not on a button (the
  right stick fast-forwards and rewinds). Upright, the d-pad and the four face
  buttons are the two crosses, L is A and R is B. Button mapping does not apply
  to WonderSwan games.
- **Cheats** are RAM codes (`AAAAA:VV`, hex) from the game's `.cht` file in
  `ux0:data/GBAdhoc/cheats/` or typed on the Cheats page. The ready-made list
  has a single game; use RetroArch's `.cht` files for the rest.
- Pocket Challenge V2 (`.pc2`), WonderWitch and games with a clock are untested.
  No descriptions for WonderSwan games yet; names and covers come from the
  No-Intro catalogue.

## Games

How we checked the games people reported, and what is still unchecked, is in
[Game Compatibility](docs/COMPATIBILITY.md). Not fixed: sprites in the wrong
order in Golden Sun, Mario Golf and The Minish Cap (OAM order), and any
flicker or missing instruments, which we could not judge without a Vita.

**Still being tested on real hardware:**

- **Motion games:** whether turning and tilting go the right way and feel
  strong enough. If one feels backwards, use Settings > This game >
  Direction; Motion strength changes how much a tilt counts.
- **RetroAchievements:** signing in over the Vita's own secure connection. If
  sign-in fails, the reason is in the log (lines starting `ra_http`).
- **Two Vitas linking two different GBA games** (Ruby with Sapphire, FireRed
  with Emerald): tested on the PC build with test cartridges and a short
  session of two real Pokemon games, not on two Vitas. Not measured: the memory
  with two 16 MB games at once. A full trade between versions has not been
  played through.
- **Mystery Gift** from a phone, all the way into the game's save.

## Performance

- **500 MHz needs an overclock plugin** such as PSVshell. Without one the Vita
  stays at 444 MHz (Settings > CPU speed says "Needs an overclock plugin"). If
  the app cannot write PSVshell's profile folder it tries once, then leaves it
  to you.
- **Pokemon Unbound 2.1.1.1:** audio underruns were recorded at full-screen CPU
  xBRZ 3x. See the [measurements and settings](docs/PERFORMANCE.md).
- The heaviest picture settings (xBRZ at full height plus the wide view) are the
  most demanding. If a game slows down, try Image quality: Original.
- The GPU version of xBRZ (`gpu_xbrz = 1`) is an experiment and too slow for
  normal play. Leave it off.

## Gameplay

- **Hardcore mode** applies from the next game you start, and only to games
  RetroAchievements knows.
- **Achievements pause during link play** and count again afterwards. In the
  GBA, Game Boy and NES games that happens inside the game, as after loading a
  state. In the Super NES and Sega players a linked game is a whole session of
  its own, so nothing is counted in it.
- **Game clocks follow the Vita's clock.** A game saved by an older version
  while its clock ran ahead can see the date go backwards once; the clock then
  waits until the real time catches up. Save type and Clock (RTC) can only be
  changed inside a running game, and apply the next time it starts.
- **Wide view** for the GBA is made for Pokemon Emerald and its siblings. In
  other games the edges can miss people or weather. Games without a profile are
  never stretched.
- Some cheats need their "Enable Code (Must Be On)" entry first; a few code
  types are marked unsupported. Cheats switch off while linked and in Hardcore.
- Three- and four-player links and single-cartridge multiboot are not
  supported.
- Two games with the same file name share box art, cheats and previews. Give
  different games different names. The same game in two folders shows up twice,
  each copy with its own save; if a save seems missing, look for the other copy.
- Each console's list shows up to 2,048 games.
- **Zipped games:** the first game in the zip with a known extension is the one
  played. Patches, and two-player play with a different game for each player,
  need the game unzipped. Encrypted and ZIP64 archives are not read.
- **Starting from a launcher** works when VitAdhoc is closed; close it first.
- **Sharing states between two Vitas** covers slots 1 to 5.
- **Names in other alphabets** (Japanese, for example) show as `?` in the list:
  the menus' font has Latin letters only. The games still play.

## Install

- Back up `ux0:data/GBAdhoc/` and any saves next to games in other folders
  before a big update.
- A `Game.ips` that sits next to both `Game.gb` and `Game.gba` is not applied to
  either. Name it after the whole file instead, for example `Game.gba.ips`.
- Game Boy and Game Boy Color games are told apart by the cartridge, not the
  file name. To override a game, add a line `gbc <file name>` or `gb <file
  name>` to `gb_override.txt` in the VitAdhoc data folder (a file only, there is
  no menu for it).

## If Something Goes Wrong

- **Turn off the automatic box art:** add `art = 0` to
  `ux0:data/GBAdhoc/vita.ini`.
- **Turn off previews:** Settings > Interface > Game previews, or `clips = 0`
  in `vita.ini`.
- **Go back to the old picture or timing:** Settings > Display > Wide view: Off,
  Image quality: Original, PAL games speed: True 50 Hz, Quick response: Off.

The log is `ux0:data/GBAdhoc/log/gbadhoc.log`. It can contain game names and
network addresses, so read it before sharing.
