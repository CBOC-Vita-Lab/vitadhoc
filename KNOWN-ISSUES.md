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
- **The NES Zapper on the touch screen** was tried on PC with hand-made test
  cartridges only, never with Duck Hunt or any real game, and not on a Vita.
  Where the shot lands depends on the touch panel's calibration; the gun's
  tolerance is a six-pixel target. A tap does not open the menu in a Zapper
  game: use Start + Select. If a game that uses the Zapper is not in the core's
  database, set Settings > This game > Port 2 to Zapper.
- **The rear touch panel** (main app and players) has no hold time or grip
  area: the Vita's grips can touch it. Where its halves meet comes from the
  report range other Vita software uses (y 108 to 889), not from a measurement
  on a console. It is Off until you turn it on, and
  nothing is bound to a corner until you bind it. On the WonderSwan a corner
  can press the buttons the Vita's own buttons press (it follows the game's
  turn: the d-pad is the Y buttons in an upright game), not a WonderSwan
  button by name.
- **The Button test** shows the WonderSwan's buttons for a game held
  sideways only.
- **Text size Large does not reach every page of the app yet.** It applies to
  the home screen, the console list, the in-game menu and Settings, and to
  every menu page of the Super NES and Sega players. Save states, Cheats,
  Button mapping, Cover picture, Play together and the plain list view keep
  the normal size, apart from their title and pop-up messages: their rows are
  built for it. High contrast applies everywhere.
- **CRT mask** is drawn with a blend the app did not use before (the picture
  multiplied by a stripe texture). It was checked on the PC test twin and
  compiled for the Vita, not run on one. If it misbehaves, set it to 0 (Off,
  the default): nothing extra is drawn then.
- **Screen: OLED** uses estimated numbers, not a measurement of the panel, and
  Auto cannot tell the first Vita from the second: owners of a PCH-1000 choose
  OLED by hand. Screenshots are taken with the correction applied.
- **The CBOC Vita Installer** may not offer the NES, WonderSwan or Sega
  folders yet; copy those games with VitaShell (for example `ux0:ROMS/NES`,
  `ux0:ROMS/MD`).

- **Game Boy Camera and Game Boy Printer** were checked with test cartridges
  made for the purpose (the PC test twin), never with the real Camera cartridge
  or a real Vita camera: the sensor's exposure scale and the printer's margin
  length are estimates, a real camera's pictures will differ in detail, and the
  Vita's camera code (the CDRAM buffer, the non-waiting read) has only been
  compiled. A rewound or loaded state keeps the live camera picture. A game
  that prints more than one sheet is saved once. The printer does not answer
  during a link, and a state loaded in the middle of a print leaves the game
  and the printer out of step until the next initialise.

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
  No real SG-1000 or Game Gear game was available to test achievements: the
  tests use small cartridges made for them (`md/twin/tools/sg_gen.py`).
- **No sprite flicker** (Master System, Game Gear, SG-1000; on by default) was
  checked with homebrew only. A game that hides something behind the console's
  sprite limit on purpose would show it: turn the row off for that game. It is
  not offered for the Mega Drive, which keeps its own limit.
- **Fast forward in the Super NES and Sega players** has the app's choices
  (2x, 3x, Unlimited, Unlimited Smooth). Unlimited runs as many frames as fit in
  one refresh (up to 30) and stays tied to the display, where the app lets the
  screen run free; it is silent and draws a picture every second refresh
  (Unlimited) or every refresh (Unlimited Smooth). Those numbers come from a
  budget of 84 % of the display's refresh (about 14 ms) less the time the main
  thread spends on a refresh besides emulating (never slower than 2x), and
  from measured frame costs, not from a Vita. Keep sound is listed for 2x and
  3x only, as the Unlimited choices are silent. The players
  have no Enhanced 4x in Image quality (no 4x scaler), and their Rewind length
  is Short (about 30 s) or Medium (about a minute); the app's rewind has no
  length row yet.
- **Quick response** shows what you press one frame sooner by working out the
  next frame ahead; the game runs exactly the same and states, rewind and saves
  are unaffected. It costs about twice a plain frame (1.2 to 1.7 times on the
  Game Boy, NES and WonderSwan test games, host counts), so Auto is the default
  on Game Boy, Game Boy Color, NES, WonderSwan, Master System, Game Gear and
  SG-1000 (it runs only while the game leaves the room, and stops when frames
  run late) and Off on the Mega Drive, where the heavy games do not fit (On
  there forces it and can drop frames). It is off in linked and online play,
  in fast forward, in rewind and in Steady pacing's half mode. A game that
  already answers a button in the same frame it reads it gains nothing from it.
  It is not in the Super NES player in 2.0 (so that player has no Quick
  response row). It was assessed: on the PC build a Super NES state save costs
  0.1 to 0.2 ms and a load 0.25 to 0.5 ms next to a 1.0 to 2.0 ms frame (host
  counts), but Snes9x's state load resets the machine and makes the picture
  thread wait and redo its whole video memory, so a look-ahead frame is expected to
  cost too much for Auto to start (an estimate: not measured on a Vita); it needs a
  lighter way back in the core first. It is not offered for Game Boy Advance:
  a state is 416 KB and loading one
  throws away the translated code that ran from RAM (see docs/PERFORMANCE.md,
  numbers measured on the PC build only), and a frame costs more than half of
  what the Vita can do. On WonderSwan one step is one console frame, and the
  console runs 75 Hz on a 60 Hz screen, so it gains one console frame, not
  always one screen refresh. On NES the wide-view scout is part of the frame, so
  a game that needs it runs closer to the limit.
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
- **Sega CD: the least tested thing in 2.0.** No Sega CD BIOS or commercial
  disc was available, so no real game has booted. Checked on the PC twin and
  emulated ARM with synthetic discs and a stand-in BIOS made for the tests:
  disc readers (cue, iso, chd, audio), save RAM, states, achievements memory
  and that ARM equals PC. Not checked: a real game, real CD loading speed on
  a Vita, Ogg audio accuracy, and a state of about 1 MB sent over a real
  link. A game that needs the real BIOS timing may misbehave.
  - Disc reading and CHD decoding run on a helper thread that reads ahead of
    the game (like the other consoles' helpers). It is checked on the PC
    twin only (threads, a stress mode that delays every read, the same bytes
    with and without it); how well it shares the Vita's cores, and whether
    it keeps a game at full speed while a `.chd` is unpacked, is not known.
    A `.chd` costs more CPU than a `.cue` with raw tracks (a `.chd` is
    unpacked on the fly, with LZMA, zlib, zstd or FLAC); if a game stutters
    from a `.chd`, a `.cue` + `.bin` of the same disc is the safe choice.
    The game's wait for the helper is bounded (about 2 ms, then it reads the
    bytes itself, counted as `timeouts` in the log); the bound was only
    exercised on the PC twin with an artificially stalled helper, so how often
    a real Vita meets it is not known.
    A close of the disc waits for the helper at most about a quarter of a
    second: if the helper is stuck in a read longer than that, the disc is
    closed anyway, the helper is left to finish and free what it was on, and
    there is no read-ahead (direct reads, slower) for the rest of that
    player run; the next start of the player gets it back. A
    stuck helper leaks its thread handle. Tested on the PC twin with a helper
    stalled for 500 ms (close, disc swap; raw and `.chd`); not on a Vita, and
    not under qemu (no ARM toolchain was available for this check).
  - Rewind keeps a picture of the machine every 4 frames on a disc (every 2
    on a cartridge): a state is about 1 MB. The step back is coarser, and a
    quiet scene keeps the full rewind length, a busy one a few seconds less.
    The cost is measured on the PC only.
  - A game saved with a RAM cartridge of one size and then played with
    another keeps the first size's data: the save file holds the cartridge of
    each size you used (a few hundred KB each), and switching back finds it.
  - The Two players id of a `.chd` game is not the id of the same disc as
    `.cue` + `.bin`: both players must hold the same kind of image. A `.chd`
    written without its SHA-1 gets an id from its size, header and first map
    bytes instead (checked on synthetic headers on the PC only).
  - You need your own BIOS (INSTALL lists the names). No BIOS: a message and
    back to the library.
  - A disc with only a data track shows 99 audio tracks (the core's own
    behaviour). A lone `.bin` without `.cue` is not listed; use `.iso`. No zip.
  - Games on several discs use an `.m3u` list (up to 8 discs). Change disc
    works through the drive's tray handling like Genesis Plus GX's own disc
    swap; a game that does not check the tray may not notice the new disc until
    it reads it. RetroAchievements follows the first disc of the list. Not
    offered in Two players (both consoles must keep the same disc). Only
    tested with synthetic discs; no real multi-disc game has run.
  - Saves are not RetroArch `.brm` files; they cannot be exchanged.
  - Cheats change main work RAM only; no ROM patches for discs.
  - In Two players the region and RAM cartridge are fixed (USA/auto, 512 KB) on both sides.
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
- **Mode 7 speed-up and the wide view's strip cache** (new in this build) give
  the same picture, sound and state as before on all 15 test games, but were
  only timed on a computer: the gain on a Vita is not measured. Scrolling
  games get nothing from the strip cache and pay about 0.7 % for it. The
  second background of Mode 7 (EXTBG), mosaic and hi-res Mode 7 are not sped
  up.
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
  come from Wikipedia and are shown only when the article is about that game
  on that console (about 850 of 2,985 NES games); the rest show none.
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
  Only a few WonderSwan games (about 11) have a description: Wikipedia has
  articles for few of them. Names and covers come from the No-Intro catalogue.

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
- **Collections and playlists:** matching a RetroArch playlist item by
  checksum uses the bundled No-Intro names, so a hack or a game the database
  does not know is matched by path or file name only; compressed (rzip)
  playlists are skipped, and there is no "Two players" view because no bundled
  data has a player count. Limits: 24 collections, 4,096 games in them, 16
  playlists. Tested on the PC build; the Vita's folders (`uma0:` included) and
  the on-screen keyboard have not been used on a console.

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
- Search memory looks at work RAM only (not cartridge save RAM or banked Game
  Boy Color RAM), at unsigned whole numbers of 1, 2 or 4 bytes on aligned
  addresses; a value kept in two places or stored as a decimal-coded number
  needs more than one search. Tested on the PC only.
- Three- and four-player links and single-cartridge multiboot are not
  supported.
- Two games with the same file name share box art, cheats and previews. Give
  different games different names. The same game in two folders shows up twice,
  each copy with its own save; if a save seems missing, look for the other copy.
- Each console's list shows up to 2,048 games.
- **Zipped games:** the first game in the zip with a known extension is the one
  played. Patches, and two-player play with a different game for each player,
  need the game unzipped. Encrypted and ZIP64 archives are not read.
- **`.7z` games:** read with the LZMA SDK (LZMA, LZMA2, BCJ, Delta; no
  passwords, PPMd or BCJ2). The archive's block that holds the game is
  unpacked in memory at once, so a game over 32 MB or a solid block over 64 MB
  is refused (the notice says which); pack big games without "solid" or on
  their own. A Genesis `.bin` in a `.7z` is unpacked once to check its header
  when the library lists it, which can be slow for a big solid archive. The
  library does not show the room-finder's checksum for a `.7z` (same as a
  deflated zip). Tested on the PC with archives made by p7zip; the unpacking
  time on a Vita is not measured.
- **Starting from a launcher** works when VitAdhoc is closed; close it first.
- **Sharing states between two Vitas** covers slots 1 to 5.
- **Chinese-only characters** (simplified or traditional hanzi that Japanese
  does not use) show as `?`: the menu font covers Japanese kana and kanji,
  Korean, fullwidth forms, and Latin, Greek and Cyrillic letters. The games
  still play.

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
