# What's New

## Version 2.0

- **A state sent by the other player, or loaded from a file, can no longer crash or hang an emulator.** Starting a link loads the host's game
  state on the guest's console, and the same loader reads save state files and Share game data. A state made up on purpose (with its sizes
  and checksums repaired) used to be able to crash or freeze the Genesis core and others. Every core (Genesis / Master System / Game Gear /
  SG-1000, Super NES, GBA, Game Boy and Color, NES, WonderSwan) now checks the inside of a state before using it (indexes, banks, sizes,
  counts, timers) and refuses it, or brings impossible values back into range; and each was fuzzed on the PC build with a memory checker:
  no crash, no hang, no report. States the consoles themselves wrote, and older versions' states, still load and the game runs on exactly as
  if it had never been interrupted (all test games, 4 load points each). Not proven on a Vita; the GBA fuzzing ran the interpreter build.
- **The same looks on every console.** The main app's Settings > Display now has the rows the players have, with the same names and
  0..10 strengths (0 is off): **Ambient glow**, **Scanlines**, **LCD grid** (two rows now, each on its own; they used to be one Video filter
  with one strength) and **LCD ghosting** (was on/off) on the GBA, Game Boy, Game Boy Color and WonderSwan. **LCD colours** also for
  the WonderSwan, and **NTSC colours** for the NES. The GBA now starts with **GBA screen** colours, which look better on the Vita's screen
  than the raw ones (it is one row to change; judged on PC renders, not on a Vita). Old settings, per game too, are carried over once
  (the grid and scanline filters become their rows at the same strength). See docs/LOOKS.md.
- **NES: Ambient glow off now means black bars.** With the wide view on, the glow off still left blurred side bars.
- **Sega games no longer lose sprite collisions on frames that are not drawn.** Fast forward, Steady pacing's half mode and catching up
  after a long frame run frames without drawing them, and the Sega core took its sprite collision and overflow flags from the drawing:
  in 15 of 67 test games the game itself then ran differently. An undrawn frame now still runs the sprite pass of every line, and
  leaves the machine exactly as a drawn frame does (sound, state and memory equal on 105 test games, PC build).
- **Quick response for the Sega consoles.** Speed > Quick response (Off / Auto / On) shows a button press one frame sooner: the next frame is
  worked out ahead and shown, and the machine runs exactly as without it (sound, state, memory and battery save equal on 105 test games, PC
  build; a small test cartridge shows a press 1 frame after it plain and 0 frames after it with Quick response). Auto is the default on the
  Master System, Game Gear and SG-1000 and starts only while the game leaves the room; the Mega Drive is Off by default.
- **Sega states and rewind come back exactly.** A loaded state, a rewind landing and a quick load now continue exactly as the game that was
  never interrupted (first line of sprites, FIFO flag, PSG changes, EEPROM position, flash save chip included): 105 test games, 4 load
  points, with and without the VDP worker. The first game started after launch no longer differs from the same game started later. Old
  states still load.
- **No sprite flicker (Master System, Game Gear, SG-1000).** These consoles drew 8 sprites on a line (4 on the SG-1000) and games blinked
  the rest. Picture > No sprite flicker (on by default) draws them all. Only the picture changes: the console's overflow and collision
  flags are still set by the hardware's limit, so the game runs exactly the same (73 test games, PC build). Turn it off for a game that
  hides something behind the limit on purpose. The Mega Drive keeps its own limit.
- **Sega save states no longer carry stray memory.** A few unused bytes of a state file held whatever was in memory.
- **Frames that are not drawn leave every console's machine as it was.** Fast forward, Steady pacing's half mode and catching up run frames
  without drawing them. Checked, state, memory and sound compared between drawn and undrawn runs (PC build, one frame in two and one in three
  undrawn): Super NES 15 of 15 games (3000 frames; the one byte that differs is a drawing cache the next drawn frame refills), NES 263 of 263
  test cartridges (900 frames), Game Boy and Game Boy Color, GBA and WonderSwan games in the app (see docs/TESTING.md). No core needed a change.
- **Smooth 50 Hz for European games.** PAL games on the 60 Hz screen showed one picture twice every tenth of a second, a hitch in every scroll.
  Settings > PAL games speed (Super NES, Sega and, in the app, NES) has a new first choice, **Smooth 50 Hz**, on by default: the game, its sound
  and its input run exactly as before, but the frame the sixth refresh waited for is run one refresh early and that refresh shows an even mix
  of the two pictures; the next refresh shows the new one. The picture step per refresh is .5 .5 1 1 1 1 instead of 0 1 1 1 1 1 (the squared
  deviation from an even step falls from 0.83 to 0.33). Only that refresh differs, and only where something moves. True 50 Hz and Fast 60 Hz
  are as before. It is not used in a linked game.

- **Online Two players on a NES or WonderSwan game compares the games.** A room made from inside a NES or WonderSwan game carried an
  all-zero game number, so a joiner running another game was let in and only turned back when the link started, and the game list's
  "Join online" had no number to look for among its games. The room now carries the game's own number (its CRC) and the joiner's card
  says "Your friend is playing a different game. Add it to this Vita to join." at once.
- **Online relay: a joiner whose first relay request is refused asks again.** The service gives relay credentials only once both players have polled the room (403 `relay_not_allowed` before that). The app treated that first refusal as the end of the join ("could not reach your friend"); it now asks again every 3 seconds, up to 12 times, so the relay fallback works when the joiner is quicker than the host's next poll.
- **Wide view for the Sega consoles.** The Mega Drive / Genesis, Master System, Game Gear and SG-1000 fill the Vita's whole width like the Super NES
  and NES do: the console's picture stays pixel exact in the middle and the sides show more of the game's world where that can be shown truthfully, and a soft
  continuation of the picture's edge where it cannot. Picture > Wide view: Auto (on, and it narrows the real sides on a busy game before it would cost a
  frame), On, Off (the 4:3 picture back). Nothing the game does changes, with the view on or off.
  - **It remembers what you have seen.** On the Master System, the Game Gear and the Mega Drive the sides show the level you have already walked
    through (the part that scrolled off, and the part ahead when you turn back), not only a soft edge. Master System games have nothing beside their
    picture in the console itself, so there this is all of the real picture at the sides. Never a guessed tile: a remembered cell is shown only
    while its graphics are unchanged and the level is recognised; otherwise the soft side. (SG-1000 games keep the soft sides.)
  - **More of the Mega Drive picture is covered**: lines under a status window (the playfield behind it continues), and lines of games that scroll
    columns of the picture up and down, where the columns at that edge move together.
  - **It costs the game nothing**: the extra picture is drawn and judged on the picture thread while the game's own thread goes on, strips of a
    picture that stands still are reused, and the last step is shared between the helper threads. By the model the heaviest test programs keep the
    full real sides at 60 frames a second (not measured on a Vita; Auto still narrows the sides if a game ever needs the time).
  - The Master System and SG-1000 upscale with xBR 3x now (it steps down to 2x and sharp when the game needs the time); the Game Gear and the Mega
    Drive keep xBR 3x and 2x.
- **Super NES wide view: the same picture for about half the work.** At the Vita's full width the wide view's own work is roughly halved (its
  picture is bit for bit what it was): a side that shows the soft frame is no longer worked out on every frame, margin layers that never reach the
  screen are not drawn, and the rest is shared with a helper thread. Rewind's snapshot is now written while the game waits for the screen instead of
  before the frame is shown. By the estimates the heaviest games (Yoshi's Island, Kirby Super Star) can keep the full width more often; Super Mario
  World and Super Metroid may still narrow it. Not yet measured on a Vita.
- **Super NES wide view: the sides are drawn on a thread of their own.** The extra picture beside the game is now drawn by a separate thread
  (the picture thread drew it before), and the helpers that share the wide view's work moved to the two cores that have room (one of them sat on the
  core of the thread it was meant to help). The picture is bit for bit what it was (15 test games, 3000 frames each, PC build and ARM under an
  emulator; thread checker clean), and a background colour effect (the sky of Super Mario World and Yoshi's Island) is drawn faster, exactly. By the
  estimates every thread now fits a 60 fps frame at the full width on all 15 test games; on a console without the fourth core a heavy scene of
  Yoshi's Island, Super Mario World or Zelda may still make Auto narrow the margin. Not yet measured on a Vita. An option that would save more by
  counting the two sides on alternate frames exists but is off: it changes the sides' fade timing visibly in some games (Street Fighter Alpha 2).
- **The home screen appears at once.** The first frame no longer waits for
  the card to be searched for games: a card that takes a minute to read (a
  large card in a slow adapter) now shows "Finding games... N found" with the
  last game found, the games already known from the last visit stay in
  view, and Circle stops the search and shows what was found. The list is
  the same one the full search would have made; a search that was stopped is
  not remembered, so the next visit looks again. A missing card does not
  hold the start. The log line `boot_times` gives the first frame, the first
  response to a button and the time the list was ready.
- **A failed save no longer claims the card is full.** The message now says
  what is true: "ux0: is full (3 MB free)" when it is, "has 150 GB free; is the
  folder read-only?" when it is not, or that the card could not be checked.
  Free space is counted in 64-bit sizes for the card the game is on (ux0,
  uma0, imc0 or ur0), so a big card no longer reads as nearly empty.
- **Settings > Controls > Tap screen for menu** (On by default) turns the
  tap-the-game menu off, in the main app and in the Super NES and Sega players,
  for players whose thumbs touch the screen. Start + Select opens the menu on
  every console (the Super NES and Sega players also accept L + R + Start);
  the game hints now name Start + Select for all of them.
- **Game Boy and Game Boy Color games are told apart by the cartridge, not
  the file name.** A `.gb` file whose header says Game Boy Color now plays and
  is listed as a Color game, and a `.gbc` file that is a plain Game Boy
  cartridge as a Game Boy game. Saves and states stay where they are: they
  follow the file's own extension. To override a game, add a line `gbc <file
  name>` or `gb <file name>` to `gb_override.txt` in the VitAdhoc data folder
  (a file only, there is no menu for it yet).
- **Fast forward at normal pitch.** Settings > Fast-forward (and Keep sound in
  the Super NES and Sega players) has a third choice beside Off and Sped up:
  the game's sound plays at its normal pitch while the game runs fast, with
  the skipped stretches joined by a short fade so it does not crackle.
- **More covers found.** When a game's exact name has no picture, the cover
  search now also asks for the same title under the other regions, without
  its revision and language tags, and with a subtitle's dash as a space (at
  most seven more requests, once per game). On the full No-Intro lists the
  share of games with a cover available rose from 87.1% to 89.3% on Game Boy
  Advance and from 84.5% to 88.5% on Super NES.
- **Rewind, fast forward, patches and RetroAchievements in the Super NES and
  Sega players.** Hold the right stick left to rewind up to 60 seconds (sound
  off), right to fast forward 2x or 3x (with sound if you like; hold or toggle);
  both are off in two player games and rewind is off in Hardcore. Sega games take
  an IPS, BPS or UPS patch like the others. Signed-in players get achievement
  toasts, challenge, progress and leaderboard markers, and Hardcore rules
  (no rewind, cheats or state loads), with the account taken from the app.
- **Sega Genesis / Mega Drive, Master System, Game Gear and SG-1000.** Each is
  a console in the picker with its own shelf, folders (`ux0:ROMS/Genesis`,
  `SMS`, `GG`, `SG-1000` and RetroFlow's Sega folders), zip and `.smd` support,
  names and box art by checksum, Starred, Recent, play time, Continue, state
  shelf, cheats from the bundled database, RetroArch save import and export,
  crash protection and the Cover button, played in the separate `md.bin`
  program. Two players for them is written but switched off for now.
- **The console picker is alphabetical** by the console's displayed name, so
  any new console lands in place; the game counts stay.
- **A console picker instead of a row of console tabs.** The top bar names
  only the console in use. Triangle (or a tap on its name) drops a list of every
  console under it, with the number of games found in each, the one in use
  highlighted. Each further Triangle or Down moves along it, Up goes back, and
  the library behind follows at once. The list closes by itself after a
  moment, on any other button, or on a tap beside it; a tap on a row picks it.
  Adding a console to the app is one line in its table.
- **Online play.** Play together > Connection: Online. The host's screen shows
  a code of five characters; the friend types it on a code pad (d-pad or
  touch) from the game menu or the game list (Settings > Join online). The
  Vitas link directly when they can and through a Cloudflare relay when they
  cannot ("Connecting through the relay."); **Always use the relay** hides
  each player's address. Works for GBA Link Cable, GBA Wireless Adapter, Game
  Boy / Game Boy Color Link Cable and sharing. VitAdhoc finds your copy of
  the host's game by its cartridge code and file fingerprint and starts it, or
  says which game to add. Messages say who acts (code expired, a newer or older
  VitAdhoc on the other Vita, a wrong clock, a busy day). A connection quality
  message (Great, Good, Slow) appears when the link forms. Link delay Auto may use
  up to 12 frames online (8 elsewhere). No accounts; the room service stores a room
  only until the link forms or 10 minutes pass. See docs/ONLINE.md. Not yet
  tried on two real Vitas or the live service.
- **The typed-address "Internet" connection is now "Direct address"**, shown
  only with `link_direct = 1` in `vita.ini`; an Internet setting from 1.4
  becomes Online.
- **Game Boy colours.** Original Game Boy games are now coloured as a Game
  Boy Color coloured them: **GBC auto** (the default) picks the palette the
  Color's start-up chose for that game. Settings > Display > Game Boy colours
  also offers the twelve palettes you could hold buttons for at start-up
  (GBC Up - brown and the rest), the Super Game Boy's 32 palettes (1-A to
  4-H), and the plain Grey, DMG green and Pocket. The row shows the four colours as you
  choose, and "This game" can use its own. A colour you chose before 2.0 is kept (the old
  Auto becomes GBC auto).
- **Undo for save states, and slot lock, on every console.** In the save
  states page, R undoes the last save over a slot (the state that was there
  comes back, one step) or the last load (you go back to just before it).
  L locks a slot: a locked slot shows a padlock and cannot be overwritten or
  deleted, by a menu save or a quick save, until you unlock it. Not in Hardcore or while linked.
- **RetroAchievements shows more.** A small mark when an achievement is one
  step from unlocking, a progress count ("3/10") under the toast, and the live
  value of a leaderboard run at the corner of the screen (leaderboards run
  in Hardcore). Your rich presence is now logged when it is sent. Settings >
  Achievement indicators turns the marks off; unlock toasts stay.
- **Under the hood:** a TLS client of our own (Mbed TLS 3.6.8, eight root
  certificates, certificate checks always on) for the room service, STUN,
  hole punching and a TURN relay client on the link's own UDP socket, and the
  room service itself (`online/service/`, one Cloudflare Worker and one
  Durable Object). See docs/TECHNICAL.md and docs/BUILDING.md (building needs the
  Mbed TLS prefix).
- **GBA fixes from GBAdhoc 5.0.0-beta.6.** Top Gun: Combat Zones now has no
  save chip, as on the real cartridge, so its menu opens. Games that start
  without the BIOS begin on the screen line where the BIOS hands over (Polarium
  Advance no longer derails in its first frames). Rock 'N Roll Racing no longer
  breaks when a routine pushes the program counter. A game that probes for a
  link cable (Activision Anthology's two-player test) finds none instead of a
  ghost player. The built-in BIOS now plays the BIOS sound driver, so games that
  leave mixing to it (X-Men: Reign of Apocalypse) are no longer silent.

## Version 1.4

- **Your saves reach RetroArch** (GitHub #3, which also had the Game Boy
  tap-to-open-menu fix: a tap on the screen opens the menu on every console). A save made by an older version (or one that
  was never taken from RetroArch) is now also written, once, as a plain
  `Game.srm` where RetroArch looks, with one sentence saying where. A RetroArch
  file that is newer or different is never overwritten: both are kept and you
  are told which is which; an older one is backed up first. Odd files an older
  version left beside the game go to the app's folder. Settings > This game >
  Export save for other emulators makes a plain `.srm` or `.sav` on request.
  See docs/SAVES.md.
- **Game names** (GitHub #16; Settings > Library): show the game's title or its file name.
  Games that would show the same name, such as ROM hacks of one game, also
  show their file name under it.
- **NES and Famicom games.** A fifth shelf, NES, beside GBA, GB, GBC and SNES:
  `.nes`, `.fds` (Famicom Disk System, with your own `disksys.rom`), `.unf` and
  `.unif` games, zipped too. They play inside VitAdhoc like Game Boy games:
  ten state slots with pictures, Continue, fast forward, Game Genie cheats,
  patches, play time and Recent, per-game settings, RetroAchievements and the
  RetroArch save import. Hold Start + Select for the menu. Found in
  `ux0:ROMS/NES` and `ux0:roms/NES`, any folder inside `ux0:ROMS/`, RetroFlow's
  NES folder and your own folders; saves are `Game.srm` beside the game. Box
  art and title screens come by checksum or name like the other consoles.
  Rewind works as for Game Boy games; two-player link play is not available
  for NES games. PAL games run at their own 50 frames a second (five of six
  refreshes) instead of 20% fast.
- **WonderSwan and WonderSwan Color games.** A sixth shelf, WS, for `.ws`,
  `.wsc` and `.pc2` games (zipped too), found in `ux0:ROMS/WS`, `ux0:ROMS/WSC`,
  `ux0:roms/WS`, `ux0:roms/WSC`, RetroFlow's "Bandai - WonderSwan" folders and
  any folder inside `ux0:ROMS/`. They play inside VitAdhoc with states,
  Continue, rewind, fast forward, cheats (RAM codes `AAAAA:VV`), patches,
  play time, per-game settings, RetroAchievements and the RetroArch save
  import (Beetle WonderSwan folder); saves are `Game.srm` beside the game,
  the file RetroArch uses. Vertical games turn upright by themselves; the
  game's settings have **Rotation** (Auto, Horizontal, Vertical), **Vertical
  play** (Upright, or Turn the Vita) and **Palette** (Greyscale or the LCD's
  tint). The first time a game turns, a hint says where the buttons are.
  The console runs at 75 frames a second: each Vita refresh shows a blend
  of the frames it covers (Settings > Display > Smooth 75 Hz motion; on by
  default).
- **NES wide view** (Settings > Display, with the NES selected: Wide view
  Auto, On or Off; Auto by default). A scrolling NES game fills the 16:9
  screen: the original picture stays exactly as it is in the middle, the sides
  show the game's own picture where it is known (what the level has already
  shown, and a map of the level kept for the next visit), and fade where it is
  not. Nothing is guessed. Auto goes back to the plain picture when there is
  nothing to show at the sides.
- **Steady pacing** (Settings > Gameplay, on by default). When a busy moment
  cannot be drawn at the full rate, every second frame is shown, evenly,
  while the game keeps its true speed and sound; every frame comes back after
  about three seconds with room to spare. The same as the SNES player.
- **Smooth hand-over** (Settings > Gameplay, on by default). The picture is
  drawn and shown on a second core while the game runs on its own, so heavy
  picture options no longer slow the game down. Off in link play. Turn it off
  if the picture ever misbehaves.
- **The best look by default.** Image quality is Auto on every install (it
  was Original on a fresh one): xBRZ smoothing over sharp bilinear (no more
  uneven nearest pixels as the base filter), starting at a ceiling for each
  console (GBA, GB, GBC and WonderSwan xBRZ 3x, NES 2x; 4x only when you set
  it by hand). A game that cannot hold 60 with it is eased one small, least
  visible step at a time (the ambient glow's moving picture first, then the
  xBRZ size, MMPX 2x, the frame blend, and last sharp bilinear), gets the
  look back when there is room, and remembers what it needed. The wide view
  and the screen fill are never given up. The Image quality row then reads,
  for example, "Auto (xBRZ 3x, eased)". A row you set yourself is never
  touched. The wide view's margins now continue the picture's own edges by
  default instead of black bars.
- **MMPX 2x smoothing.** A new smoothing step between xBRZ 2x and sharp
  bilinear: it costs less than xBRZ and keeps pixel art crisp. GBA games that
  draw their whole picture every frame in software 3D (Need for Speed, Driver,
  SSX) start on sharp bilinear automatically. Every choice stays available in
  Settings.
- **Super NES, European games at their true speed.** PAL games run at 50 Hz
  (five frames per six refreshes) instead of 20 % fast. Menu row **PAL games
  speed**: True 50 Hz (default) or Fast 60 Hz.
- **Super NES wide view, on by default.** Every SNES game fills the 16:9
  screen without costing a frame: the real picture is kept in the middle, the
  sides show real picture where the game can afford it and fade into a soft
  extension elsewhere, and never a wrong tile. Per side, the side the camera
  came from can show real picture. Menu: Wide view Auto (adapts), On (full
  margin) or Off.
- **Super NES picture and sound.** Menu row **Image look** (Auto, Full,
  Light): Auto eases the blends only when a game cannot hold 60. A new audio
  pacer fixes crackle and uneven sound (GitHub #15). Snes9x fixes made after
  1.63 are in, plus cheats, ten state slots with pictures and a new in-game
  menu.
- **Super FX games run faster.** About 10 to 15 percent less work for each
  Super FX instruction, with identical pictures and sound on the 15 games
  tested; Doom, Stunt Race FX and the like get more game speed.
- **GBA stutter and picture fixes.** Games that rewrite their own code
  flush only the part that changed, and a dispatch cache cuts the work of
  jumping between translated blocks, so fewer stutters. Sprite priority is
  exact and several renderer bugs are fixed.
- **Reliability.** Waiting for a helper thread has a time limit, so a stalled
  helper can no longer hang the app; a hand-over self-check falls back by
  itself if the drawing thread misbehaves; saves and the log are written off
  the game thread; the fourth CPU core is used when the CapUnlocker plugin
  provides it. Link play waits 20 seconds before it gives a silent partner up
  and keeps the connection alive from its own thread. Achievements pause
  while two consoles are linked.
- **Menus.** Settings opens on the most useful section (Display in a game,
  Interface on the home screen) in a clearer order, and rows that do not
  apply to the current console are hidden. An always-visible button hint
  line sits under the home screen buttons, touch targets are bigger and
  messages are never cut off. Wide view's fill row is called Screen fill.
- **Settings for every default.** Each of the choices above has a row in
  Settings, so nothing here is hidden behind a file.
- **For testers.** `tools/vita-matrix` runs unattended test sessions on a
  Vita and puts everything back afterwards; the log has per-thread CPU and
  missed-refresh counts; switches in `vita.ini` bring back the 1.3 behaviour
  for comparison.
- **NES and WonderSwan cheat lists.** Cheats > the game's list now has
  ready-made codes for NES and WonderSwan games too (found by the game's
  checksum, then by its name), besides the codes you type yourself.
- **Export save for other emulators** (Settings > This game, in a game): X
  makes the save a plain file for RetroArch, or a `.sav` beside the game
  (LEFT / RIGHT choose), whatever Sync RetroArch saves says.
- **Top bar.** With six consoles the tabs are drawn smaller when they need
  to be, all of them before the search icon, and each tab can be tapped to go
  to its console (the fifth one was hidden under the search icon).
- **Settings fix.** In Settings > Library the names of Cover picture and Sync
  RetroArch saves were swapped: each row wore the other's name. They are the
  right way round now.
- **SNES menu hint.** The game page and the loading screen of a SNES game say
  to hold L + R + Start for the menu.

## Version 1.3.1

**Snappier menus and a new look.**
- **Switching consoles is instant.** Pressing Triangle through GBA, GB, GBC
  and SNES no longer freezes the screen for a moment each time: remembering
  the console and looking in `covers/` now wait until you stop on one, and
  only look at the memory card when there is something to do.
- **Touch feedback.** An on-screen button lights up while your finger is on
  it, and flashes when the tap goes through. Slide off to cancel, as before.
- **New bubble and LiveArea art,** with all four consoles in it.

## Version 1.3

**Super NES games.**
- **Play `.sfc` and `.smc` games** from the same home screen, under a new
  SNES tab. VitAdhoc finds them in `ux0:ROMS/SNES`, in any folder directly
  under `ux0:ROMS/` (a "SNES ROM Collection" folder, for one), in RetroFlow's
  folders and in the folders you add in Settings. Box art and title screens
  download like the other consoles, and your own covers work too.
- **Box art for loosely named games, on every console.** A game is now
  recognised by its contents, and by a loose match of its name ("Act Raiser",
  "7th Saga", "Pocket Dogs (U)"), so collections with short or old-style file
  names get their covers. Games that found none before are looked for again.
- **Accurate, and spread over the Vita's cores.** The SNES player is built on
  Snes9x 1.63 (the current release). Sound and picture drawing each run on a
  core of their own, with exactly the same result as running them one after
  the other. Super FX games (Yoshi's Island and Stunt Race FX among them) no
  longer rebuild their colours hundreds of times a frame.
- **Fills the screen without stretching.** The default shape is the SNES's
  own 4:3 at full height; the sides show a soft glow from the game's picture.
  Zoom, full screen and pixel-perfect are in the pause menu (L + R + Start),
  with sharp or smooth scaling, CRT scanlines and an LCD grid (1-10).
- **Up to four players.** A second controller is player 2; with the Super
  Multitap (automatic for games known to use it) players 3 and 4 join too.
- **Full speed with every frame shown** on a Vita: Super Mario World, Donkey
  Kong Country, F-Zero, Chrono Trigger and Yoshi's Island with room to spare,
  Super Mario RPG and Kirby Super Star (SA-1) at 59 of 60 frames a second.
  Nothing is ever dropped from the picture to save time.
- **Super FX games keep their speed.** When the Vita can't run the Super FX
  chip as fast as a SNES (Doom, the races of Stunt Race FX), the chip gets the
  time the Vita has instead of the whole game slowing down: the game, its
  music and its controls stay at full speed, and the 3D view animates as fast
  as the Vita can draw it. Doom went from 41 to 59 frames a second.

**Smoothing does less work.** The xBRZ scaler and the step that finds what
changed on screen do about a tenth less work, with exactly the same picture.

**Your saves are safe.**
- **A save is never cut short.** A save from RetroArch or another emulator
  that has extra bytes at the end (the clock data mGBA adds, a 131088-byte
  Pokemon save for one) used to lose them the first time VitAdhoc saved. They
  are now kept, for Game Boy and SNES saves too (a SNES `.srm` keeps any
  padding another emulator wrote after the cartridge RAM). ([#3](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/3))
- **One backup before anything is saved over a save from somewhere else.** The
  first time VitAdhoc uses a save it did not write itself, a copy goes to
  `ux0:data/GBAdhoc/backups/` (not beside your game).
- **RetroArch saves stay in sync both ways.** A save taken from RetroArch is
  now written back to RetroArch's file (same size, same layout) every time you
  save, so RetroArch can carry on from it. If RetroArch changed its file in
  the meantime, VitAdhoc never writes over it: at the next start the newer
  save is used and the other one is kept in `backups/`. Turn it off in
  Settings > Library > Sync RetroArch saves.

**Your game folders stay clean.**
- **Only the game and its battery save stay in your game folders.** Save
  states, their pictures, the Continue point and the play-time note now live
  under `ux0:data/GBAdhoc/states/` (in `gba`, `gb` and `gbc` folders), the way
  RetroArch keeps its files. Launchers such as RetroFlow no longer have to
  sort through `.rsm`, `.pt`, `.st0` and similar files.
  ([#3](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/3))
- **Cover thumbnail files moved out of `boxart/`.** The small `.t2.565` files
  and other picture copies VitAdhoc makes for speed are now kept in
  `ux0:data/GBAdhoc/cache/`. `boxart/` and the screenshot folder hold only
  your own pictures.
- **Your old states come along.** The first time a game is opened or shown,
  its old save states, pictures and Continue point are moved from beside the
  game to the new place. Nothing is overwritten, and if a file can't be
  moved it is still read from where it is.
- States are named after the game, as RetroArch does, so moving a game to
  another folder keeps its states.

**Games with a clock, and ROM hacks**
- **Pokemon clocks keep the right time after Continue.** The clock inside
  Pokemon Ruby, Sapphire and Emerald (and other games with one, such as
  Boktai) now follows the Vita's own clock. Save and quit, come back an hour
  or three days later, and the game shows the real time, so berries grow,
  daily events happen and the day and night change as they should. Loading a
  save state no longer winds the clock back, and fast-forward no longer
  pushes it ahead. Old save states still load.
- **Save type for ROM hacks.** In a game, Settings > This game has two new
  rows: **Save type** (Auto, SRAM, Flash 64 KB, Flash 128 KB, EEPROM 512 B,
  EEPROM 8 KB) and **Clock (RTC)** (Auto, On, Off). They are remembered for
  that game only and used the next time it starts. Pokemon Unbound and other
  hacks that were saving to the wrong size can be set here.
- **Auto is smarter.** A save file of 128 KB is now read as 128 KB flash,
  even when the hack's own label says 64 KB.

**Fast forward your way** (suggested by a player after 1.2)
- **2x fast forward**, for games where 3x is too fast. Settings >
  Fast-forward: 2x, 3x, Unlimited or Unlimited Smooth.
- **Fast forward with sound.** Choose "2x with sound" or "3x with sound" and
  the game's sound plays along, sped up, so a Pokemon battle still sounds like
  one. It's off by default; Unlimited stays silent.

**Smoother play with smoothing on**
- **Keep full speed** (Settings, on by default): a busy moment skips drawing a
  frame so the game and its sound keep their speed. The smoothing you chose is
  never lowered to get there.
  ([#7](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/7))

**Under the hood**
- **Parts we borrowed were checked for newer versions.** The Game Boy core,
  graphics code, image code and the RetroAchievements code were compared with
  their latest originals. Most were already current. Two small fixes came
  across: Game Boy Color games that switch video memory banks while copying
  graphics now behave as on a real console, and the achievements code no
  longer breaks on very large achievement sets or odd answers from the server.

**Sound.** Fixes for pops, drifting delay and loud pushes in Game Boy sound.

- **No more growing sound delay:** opening the menu, fast forward and rewind
  each used to count as a dropout and made the sound buffer 20 ms longer, up
  to 120 ms, until half a minute of calm play. Stopping the game on purpose no
  longer does that, and the sound starts clean when fast forward or the menu
  ends.
- **Game Boy games that "park" a sound channel:** a tone channel set above
  65 kHz (a trick games use to switch a channel off) played a loud constant
  push with a click on each side. It is now silent.

Real-console listening is still being checked; the numbers behind these are
in [Technical details](docs/TECHNICAL.md).

**Faster.**
- **Runs at 500 MHz when your Vita allows it.** Settings > CPU speed:
  "500 MHz (fast)" is the default; "444 MHz (standard)" is the old speed. A
  Vita needs an overclock plugin such as PSVshell for 500 (without one the
  app falls back to 444, and the row says so). With PSVshell installed,
  VitAdhoc sets up the profile for itself, never replacing one of yours
  without asking in Settings (your own is saved and put back if you switch to
  444). The first start after the change may need one more launch. A faster
  clock uses more battery and runs warmer.

**Your own cover pictures**
- **Set a cover for any game.** On the home screen press **Cover** (or
  Settings > Library > Cover picture) and choose a picture from your Vita's
  photos and screenshots, take the game's own screenshot, or go back to the
  downloaded cover with **Automatic**. You see the picture first, any shape
  works, and the new cover shows at once. It beats downloaded art everywhere,
  and downloads never replace it. Handy for custom games and ROM hacks.
- **Or just drop a file in.** A PNG or JPG in `ux0:data/GBAdhoc/covers/` named
  like the game (`Pokemon Unbound.png`) is used from the next time the list
  opens. Capitals and the `.gba` part of the name do not matter.

**Controls and first impressions**
- **A picture that is never stretched.** Games without a Wide view profile
  show at their own shape, with bars; only the games Wide view was made for
  (Pokémon Emerald, Ruby, Sapphire, FireRed, LeafGreen) get more map beside
  the picture. Screens with nothing more to show now keep black bars unless
  you pick soft edges or stretch in Wide view: menus. The sides of the screen
  are filled by a soft glow from the game's own picture (Settings > Display >
  Ambient bars; off under a screen overlay). Your saved choices stay.
- **Circle or Cross as A.** VitAdhoc follows your Vita's own Enter button, so
  on a Vita where Circle confirms, Circle is A and picks things in the menus.
  Settings > Controls > A and B buttons changes it.
- **Fewer surprises:** the right stick no longer rewinds unless you turn on
  Settings > Controls > Rewind with right stick (Select + Square still
  rewinds). L + R + Select no longer takes a screenshot (the Vita's PS + Start
  does); Settings > Controls > Screenshot button combo brings it back.
- **One hint, once.** The two tips at the start of your first games are now a
  single line in the first game you ever play.

**Fixes from GBAdhoc 4.x**

VitAdhoc's game engine started from GBAdhoc 4.1.1. These fixes come from GBAdhoc
4.5.0 (and the work behind it), checked against the Vita's own engine:
- **Need for Speed: Underground** could hang at its language screen: a timer
  that had just run out read 0 instead of its start value, and the game used
  that to size a huge memory clear. Fixed.
- **Mario Golf: Advance Tour, Mario Tennis: Power Tour and Golden Sun 2** use
  a small interrupt routine that the engine started one instruction late.
  Fixed. This may help the black and white screens reported on Mario Golf and
  Mario Tennis; we could not test those games.
- **Wrong code skipped or run**: a jump into the middle of a run of "only if"
  instructions ran the rest of the run even when the condition was false
  (seen in Mario Golf). Fixed.
- **OpenLara and other games that use the fast-interrupt mode**: the engine
  mixed up that mode's registers with the normal ones, which could send a game
  into a near-endless loop. Fixed.
- **Rare instruction fixes**: shifts by a register count in code that runs
  from graphics memory, returning from an interrupt with the register list
  form, and keeping the CPU flags when a game changes CPU mode.

**Mortal Kombat: Deadly Alliance** showed its Midway logo for over 40 seconds
and covered its title and name screens in green speckles. The built-in BIOS
read the wrong size when the game asked it to unpack data that starts halfway
through a 4-byte block. Fixed, for all of the BIOS's unpacking calls.

Not taken from 4.5.0: its Pokemon MonoMyth accuracy work, its speed work for
self-modifying code, and its PSP-only changes (they only exist for the PSP's
processor). Nothing here changes saves or save states: 1.2 states still load.
These fixes are tested with our own test programs. We also ran the games named
above, and Big Mutha Truckers, Smashing Drive and Street Racing Syndicate, for
a minute or more each on a PC build of the engine: they start and run in both
1.2 and 1.3, and we could not repeat the resets or the black and white screens
(Need for Speed: Underground and Mortal Kombat did change, as described). This
is not a Vita test; see [Game Compatibility](docs/COMPATIBILITY.md).

**Trading between versions**
- **Game Boy and Game Boy Color games link with each other.** A `.gb` game and
  a `.gbc` game can now be partners on one Vita (and between two Vitas, which
  is tested now), each running as its own kind of handheld. That is what
  Red, Blue and Yellow need with each other and with Gold, Silver and Crystal
  (Time Capsule trades). Before, Two players on this Vita only offered games
  with the same file ending, so Red (`.gb`) could not pair with Gold (`.gbc`).
- **Two Vitas can trade between versions of a Game Boy Advance game.** Ruby
  with Sapphire, FireRed with LeafGreen and Emerald with FireRed now link over
  Wi-Fi, the internet and ad hoc. The room list shows both games
  ("FireRed <-> Emerald") and no longer asks you to open the same game. The
  first time, each Vita sends the other its game (checked, with a progress
  bar, a time estimate and Cancel); a copy is kept in
  `ux0:data/GBAdhoc/cache/link/` under a hashed name, never in your game
  folders and never in the game list, so the next session starts at once.
  The cache holds 4 games or 96 MB. Each player's own save stays on their own
  Vita. Both players must own their games. Not yet confirmed on two real Vitas.

**Split screen for two players**
- **Two controllers, two equal pictures.** With a second controller (a PS TV
  with two DualShocks, or a handheld Vita with an external pad through a
  plugin) Two players on this Vita now always shows two equal pictures side by
  side, one pad each. Either player can open the menu with Start + Select on
  their own pad, and the menu stops both games without either player's held
  buttons leaking into a game when it closes.
- **A second pad is found on any port.** The second controller is accepted on
  any of the Vita's controller slots, a pad that is connected but untouched
  counts, and plugging one in or switching it off is noticed within a moment.
  A DualShock's L1 and L2 both act as L, and R1 and R2 as R.
- **No controller? Player 2 on the touch screen.** A new row in Two players on
  this Vita (Off by default, remembered): player 2's half of the front screen
  becomes a translucent D-pad with A, B, L, R, Start and Select, and both
  players can press at once.
- PLAY-TOGETHER.md says plainly what you need and which games this is for.

## Version 1.2 — October 30, 2026

**Bring your saves, your folders and your zips.** Plus graphics fixes and a
faster rewind.

**Saves and games**
- **Your old saves come with you.** Start a game that has no save yet and
  VitAdhoc takes in the one you already had: from `ux0:data/GBAdhoc/import/`,
  `Game.srm` beside the game (RetroArch), `Game.sav` beside a Game Boy game
  (most emulators' name), or RetroArch's save folder. It's copied, never
  moved, and a game that already has a save keeps it. A message says where it
  came from.
- **Zipped games.** `.zip` files are listed under the right console and played
  directly; the game is checked against the zip's checksum.
- **Choose your game folders in Settings** (Library > Game folders): type a
  folder to add it, type it again to remove it. `extra_roms` now also works in
  `config.ini`, and `gba_roms` / `gb_roms` / `gbc_roms` give one folder per
  console. ([#5](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/5))
- **Start a game from another app.** Launchers such as RetroFlow can open a
  game directly with `psgm:play?titleid=GBAD00001&param=<path>`. The path is
  used exactly as written, so names with `&`, `%` or `+` work.
  ([#3](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/3))
- **Ten save-state slots** (up from five), each with its picture; the pages
  scroll.
- **Accented names read properly:** "Pokémon" shows as "Pokemon", not
  "Pok??mon".

**Picture**
- **Screen overlays:** a PNG in `overlays/` (per console or per game) is drawn
  over the game, for bezels, shells or dot-matrix masks. Settings > Display >
  Screen overlay turns them on or off.
- **Adjustable LCD grid and scanlines:** Settings > Display > Grid strength,
  1 to 10.
- **Graphics fixes:** a smoothing filter could redraw a picture the graphics
  chip was still showing; a single graphics error could freeze the screen for
  the rest of the session; a failed memory allocation was retried every frame
  (or showed black); freeing a picture made the whole app wait for the
  graphics chip. All fixed.
- **Screenshots** can go to a folder of your choice (Settings > Interface >
  Screenshot folder).

**Speed**
- **Rewind:** recording each step does about half the work it did, and going
  back no longer throws away the emulator's translated game code each step.
- **Smoothing** (xBRZ / Scale3x) prepares each frame with fewer memory reads,
  and its scaler is compiled for speed.

**Settings**
- **Safer configuration saves:** if the memory card refuses a rename or
  write, the old settings and complete temporary copy are kept until the
  update succeeds; a failed replacement restores the old file.

[Technical details](docs/TECHNICAL.md) | [Vita performance results](docs/PERFORMANCE.md)

## Version 1.1 — October 25, 2026

**Sound fixes.** A rebuilt audio path for all three systems.

- **Steadier output:** fixed, aligned double buffers keep audio flowing
  through longer frames.
- **Correct stereo for the tone channels:** the Game Boy-style tone channels
  (square, wave and noise) play on the side the game asks for.
- **Smoother transitions:** fades and blends start from the last sample
  actually played, reducing abrupt jumps around pause, resume, state loads,
  underruns and audio-backlog trims.
- **Safer audio cleanup:** checked thread shutdown and channel release keep
  output buffers owned until the Vita has finished with them.
- **Right note lengths and fades:** the tone channels' length, volume
  envelope and sweep follow the real sample rate.
- **Clean GBA DirectSound FIFO resets:** queued samples and read/write
  positions reset together.
- **Expanded audio tests:** mixer, output and lifecycle regressions, plus a
  unified runner that also checks damaged save-state sound data.

Saves, save states, settings, ROMs and the BIOS are untouched, and the app
keeps the same ID, so 1.1 installs straight over 1.0.

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
- **Rendering on a second core** keeps graphics work off the main game loop.
- **Keep full speed:** catch-up frames help maintain game timing through
  short hitches.
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
