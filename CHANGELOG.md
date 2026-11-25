# What's New

## Version 2.0 — <date TBD at release>

The everything update: eleven consoles in one app, and a friend anywhere in the
world a five-character code away. Everything here was checked on PC test builds
and in emulators; none of it has run on a real Vita yet (see
[Known Issues](KNOWN-ISSUES.md)), so speeds are aims, not promises.

**Four new consoles**

- **Sega Genesis / Mega Drive, Master System, Game Gear and SG-1000.** Each has
  its own shelf in the console list, with box art, names, descriptions,
  Starred, Recent, play time, Continue, ten state slots with pictures, cheats,
  patches and RetroArch save import, like the Super NES. They play in their
  own program (`md.bin`).
- **Sega CD / Mega CD** is its own console in the list ("Sega CD"), played by
  the same Sega program. Discs: `.cue` + `.bin`, `.iso` and `.chd`, with audio
  tracks in raw, wav or Ogg; they are read from storage as the game runs. It
  needs your own Sega CD BIOS (see INSTALL), which VitAdhoc cannot include.
  Continue, ten states, rewind, fast forward, cheats, RetroAchievements, wide
  view, smoothing and Two players work as on the Genesis. Saves (internal
  backup RAM and the RAM cartridge: 512 KB by default, a setting for other
  sizes or none) are kept per game, and the cartridges of the other sizes you
  tried are kept in the same file. A disc whose region has no BIOS on the
  Vita says which BIOS it uses instead of leaving a black screen. Reading
  and unpacking the disc runs on a helper thread (tested on the PC only).
- **Every Sega game gets the full treatment**: rewind, fast forward, the same
  picture choices as every other console, RetroAchievements (with Hardcore),
  and a remappable pad (Genesis A / B / C on Square / Cross / Circle, X / Y / Z
  on L / Triangle / R).
- **A few lines of history for NES, WonderSwan and Sega games**, like the
  Game Boy and Super NES have: from Wikipedia, built in (about 850 NES, 670
  Genesis, 210 Master System, 115 Game Gear, 27 SG-1000 and a few WonderSwan
  games). A description now appears only when the article is about that very
  game on that console: not another console's game of the same name, not a
  series, not a remake on another system. This also removed about 460 Game Boy,
  Game Boy Color, Game Boy Advance and Super NES descriptions that described
  another game; those games show none. Sega CD games use the same list.
- **No sprite flicker** on the Master System, Game Gear and SG-1000: all the
  sprites on a line are drawn, not every other frame. The game itself runs
  exactly the same.
- **Quick response** shows a button press one frame sooner, with the game
  running exactly as without it. Game Boy, Game Boy Color, NES, WonderSwan,
  the Sega consoles and the Super NES have it (Settings, and per game); it is
  on Auto by default where a frame is cheap enough and off in linked and
  online play, in fast forward and in rewind. The Super NES player keeps the
  machine with a light copy of its own, so the picture thread and the caches
  go on as they were (heavy Super NES games have no room for it on a Vita, and
  Auto then does not start). Not offered for Game Boy Advance: see Known
  Issues.

**Cheat search**

- **Search memory on every console's Cheats page**, as RetroArch and mGBA have
  it: choose the size of the number, start a new search, then narrow it with
  Equal to, Not equal to, Greater than or Less than a number you type, or
  Changed, Unchanged, Increased or Decreased since the last step. With 50
  results or fewer they are listed with their value, and one choice makes a
  cheat that keeps it, in each console's usual code and saved like a typed
  one. Game Boy Advance, Game Boy, Game Boy Color, NES, WonderSwan, Super NES,
  Genesis, Sega CD, Master System, Game Gear and SG-1000, in the app and in
  both players, with the same words. It searches work RAM, only while the menu
  is open, and is off while linked and in Hardcore like the other cheats.
  Tested on the PC only. On the Game Boy, NES, WonderSwan, Super NES and
  Master System family the Size row also offers "2 bytes, any address" and "4
  bytes, any address" for numbers kept at odd addresses. The Game Boy's
  one-console two-player mode says Search memory is not available there (the
  game's memory cannot be read); a suspended Cheats page no longer offers
  START search; the note after a step no longer covers the help line.

**A library you can organise**

- **Collections.** Make lists of your own (Road trip, Two at once, Backlog):
  press Select + Square on a game (the Home screen also has a Lists button) to
  add it to a collection, take it out, make a new one with the on-screen
  keyboard, rename one or delete one. A collection holds games of every console;
  each console's list shows the ones it has. Select steps through them after
  Games, Starred and Recent, as soon as it is pressed; Select + Square takes
  that step back and opens the page. A `collections.txt` saved with a byte
  order mark is read; one that was not read whole says it cannot be changed
  safely, rather than that it is too big. They are kept in `collections.txt` beside
  `config.ini`, written whole at every change: a write that fails leaves the
  old file as it was and says so.
- **Most played** (the longest first) and **Not played yet** appear once
  something has been played, from the play time VitAdhoc already keeps. There
  is no "Two players" view: none of the bundled data says how many players a
  game has, and a guess would be wrong.
- **RetroArch and Emu4Vita++ playlists** (`.lpl` files in
  `ux0:data/retroarch/playlists` and `ux0:data/EMU4VITAPLUS/playlists`, and on
  `uma0:`) show as read-only collections. Their games are matched to yours by
  path, then file name, then checksum (the bundled No-Intro table names the
  checksum; no game file is read). VitAdhoc never writes to them.

**Smoother play**

- **Sega sound on its own core.** The Genesis / Mega Drive's FM chip (the
  YM2612) makes its sound on a separate core in the Sega player, as the Super
  NES sound chip does, so the main core has less to do in music-heavy games.
  The sound, the picture and the saved game are exactly the same as before
  (checked bit for bit over 30 sample games, with saves, rewind, Quick response
  and the Sega CD). If the helper falls behind, the main core finishes the work
  itself, so the sound never drops. On by default (`fm_thread=0` in
  `md/threads.ini` turns it off). Measured on a computer, not yet on a Vita.

- **Faster Super NES Mode 7 and wide view.** Games with a rotating or
  scaling floor (Super Metroid's intro, F-Zero, Pilotwings) draw that floor
  with fewer instructions, and a wide-view margin that did not change since
  the last frame is not drawn again. The pictures, the sound and the game
  state are identical to before (checked on 15 games, 3000 frames each).
  Measured on a computer, not yet on a Vita.

- **Faster pixel-art scaling in the Super NES and Sega players.** The Enhanced
  looks (xBR 2x and 3x, MMPX 2x) now run in NEON: xBR takes about half the
  instructions (counted exactly in an ARM emulator on test pictures; MMPX
  somewhat fewer). The pictures are identical to before, byte for byte
  (checked on many generated pictures under an ARM emulator). Not yet timed
  on a Vita.

- **Smooth 50 Hz for European games.** PAL games on the Vita's 60 Hz screen
  no longer show one picture twice every tenth of a second. The game, its sound
  and its input run at their true speed; scrolling just looks even. On by
  default for Super NES, Sega and NES. True 50 Hz and Fast 60 Hz stay as
  choices.
- **Faster Super FX games** (Doom, Stunt Race FX): the Super FX chip
  now runs on a much faster exact interpreter, so the 3D view should animate
  closer to the real console. Results are identical to the old one on every
  game tested.
- **Skipped frames can't change a game.** Fast forward, Steady pacing and
  catching up after a long frame now leave every console exactly as a drawn
  frame would, including the Sega sprite flags that games read.
- **The home screen appears at once**, even on a big card in a slow adapter.
  It shows "Finding games... N found" while it looks, and Circle stops the
  search.

**Better picture**

- **Wide view for the Sega consoles.** The Genesis, Master System, Game Gear and
  SG-1000 fill the Vita's whole width like the Super NES and NES. The real
  picture stays pixel exact in the middle; the sides show more of the world
  you have already seen, or a soft continuation of the edge. Never a guessed
  tile. Auto, On and Off in the menu.
- **Smoothing for the Super NES and the Sega consoles**: xBR upscaling like
  the other consoles, chosen for you by Image quality Auto.
- **The same looks on every console.** Ambient glow, Scanlines, LCD grid and
  LCD ghosting all have their own 0 to 10 strength, with the same names
  everywhere. WonderSwan gets LCD colours and the NES gets NTSC colours. GBA
  games start with GBA screen colours, which suit the Vita's screen better.
- **Screen size and the other picture rows read the same in the app and in the
  Super NES and Sega players**: Fit (it was called Original in the players),
  Zoom, Stretch (was Full screen in the players), Pixel perfect; values start
  with a capital letter everywhere (Off, On, Sharp bilinear, Bilinear, Soft
  edges). Image quality starts at Auto and Wide view is on in every console;
  the Super NES and Sega players call their on-by-default Wide view setting
  Auto (On forces the full margin).
- **Screen colours (Settings > Display > Screen, also in both players).**
  Auto, OLED (PCH-1000), LCD (PCH-2000) or PS TV: a mild correction for the
  screen you play on, the same for every console and kept in one setting. LCD
  and PS TV leave the picture as it is. OLED takes a little saturation off
  (92%) and a touch of contrast (curve 1.06), which are estimates, not
  measurements. Auto picks PS TV on a PS TV and LCD otherwise (the Vita cannot
  tell the two handhelds apart, so owners of the first model choose OLED by
  hand). It is folded into the colour table the picture already uses, so it
  costs no extra pass; GBA screen colours still apply first.
- **CRT mask.** A new row next to Scanlines, 0 (off, the default) to 10: a
  fine red, green and blue stripe pattern like an aperture grille, with
  stripes that follow the size of the picture on screen. One extra drawn
  rectangle per frame (a small stripe texture, multiplied in); no shaders.
  Higher values darken the picture a little (about 30% at 10), so raise the
  brightness if you like it strong. In the app and in both players.
- **Text size and High contrast (the first two rows of Settings > Interface
  in the app, and Settings > Interface in both players).** Both are global.
  High contrast pushes pale text to white and dark text to black and gives
  the app a pure black or white page. Large makes text 20% bigger where the
  page has been laid out for it: every page of the players' menus; in the
  app, the home screen, the console list, the in-game menu, Settings (there
  a value too long for its row ends in "..." and a help text too long for
  two large lines stays at the normal size), Save states, Cheats (with Add a
  code and Search memory) and the Play together page (a hint line too wide at
  the larger size stays at the normal size). The app's other pages (Button
  mapping, Button test, Cover picture, the pages Play together opens and the
  plain list view) keep the normal size for now, apart from their title and
  pop-up messages: larger letters ran over each other and off the bottom edge
  there (see KNOWN-ISSUES).
- **Game Boy colours.** Original Game Boy games are coloured the way a Game
  Boy Color coloured them (GBC auto), with the twelve start-up palettes, the
  Super Game Boy's 32 and the plain greys to choose from.
- **Game Boy and Game Boy Color games are told apart by the cartridge**, not
  the file name.

**Clips and updates**

- **Save clip.** A new card in the in-game menu saves the last ten seconds of
  the game as an animated PNG in `clips/<game>-<date>-<time>.png`, at the
  game's own size, every frame exactly as drawn. It reuses the recording the
  home screen's previews already keep, so it costs no extra memory while you
  play, and it is encoded on a thread of its own: the game is not held up.
  A toast says when it is saved. Game Boy, Game Boy Color, Game Boy Advance,
  NES and WonderSwan (the consoles the app plays itself); the Super NES and
  Sega players do not keep a recording yet. In the in-game menu the cards are
  a little narrower to make room.
- **Check for updates.** Settings > About: Daily (default) or Off. Once a day
  at most, while the home screen is idle and Wi-Fi is up, the app asks GitHub
  for the latest release and, if it is newer, says so once on the home screen
  ("VitAdhoc X.Y is out: get it with the CBOC Vita Installer or from
  GitHub"). Nothing is downloaded or installed, nothing about you is sent, and
  a failure is silent. X on the row checks now (the footer says so). The
  notice is said once for each new version, not on every start, and with
  Wi-Fi off the check looks again every few seconds, not every frame. The HTTPS client gained four
  Sectigo root certificates (the ones GitHub's certificates chain to), trusted
  for `api.github.com` only: online play's connections accept what they did.

**Controls and gameplay**

- **A console picker.** Triangle drops a list of every console under the top
  bar, with its game count; Triangle or Down moves along it. A tap on a row
  picks it.
- **Undo for save states, and slot lock, on every console.** R undoes your
  last save or load; L locks a slot so nothing overwrites it.
- **Rewind, fast forward, patches and RetroAchievements in the Super NES
  player too.** Hold the right stick left to rewind up to about a minute, right to
  fast forward.
- **RetroAchievements shows more:** a mark when an achievement is one step
  away, a progress count ("3/10") and the live value of a leaderboard run.
- **`.7z` games everywhere `.zip` works.** Every console lists, unpacks and
  plays a game kept in a `.7z` (LZMA and LZMA2): the first game file in the
  archive, checked against its checksum, unpacked once, with saves and states
  named after the archive. A game over 32 MB, or a solid archive whose block is
  over 64 MB, is refused with a notice that says so instead of running out of
  memory. Encrypted archives are not read. Checked on the PC only; how long a
  big archive takes to unpack on a Vita is not measured.
- **Sega CD games on several discs.** A `.m3u` list names the discs of one game:
  the library shows the list once and hides its discs, saves and saved moments
  are kept under the list's name, and Settings > Speed > Change disc in the
  game opens the disc tray, swaps the disc and closes it, as a console would.
  A saved moment remembers which disc was in. A list with no disc line (a
  music playlist, another system's list) is not listed as a game. A game
  that was played from its first disc before the list existed takes that
  disc's save and saved moments over on its first start (copied, the first
  disc's files stay). Checked on the PC with synthetic discs; no real
  multi-disc game has run.
- **Fast forward at normal pitch**, with the sound kept low and smooth.
- **Tap screen for menu** can be turned off for thumbs that touch the screen;
  Start + Select always opens the menu.
- **Rear touch panel in the main app** (Game Boy Advance, Game Boy, Game Boy
  Color, NES and WonderSwan), as the Super NES and Sega players already had it.
  Settings > Controls > Rear touch panel (Off by default) makes the panel's four
  corners inputs that Button mapping can give to any button, shortcut or turbo
  button; Settings > This game can switch it on or off for one game. On a
  WonderSwan a corner bound to a game button presses that button.
- **The NES Zapper on the touch screen.** Duck Hunt, Hogan's Alley, Wild Gunman,
  Gumshoe and the other games that use the light gun (the game's own database
  says which) take the touch screen as the gun: touch the picture to aim, and
  the trigger is pulled while your finger is down. A tap no longer opens the
  menu in those games (Start + Select does), and nothing is drawn for the gun.
  Settings > This game > Port 2 (Auto, Zapper, Controller) forces the Zapper on
  for any game or turns it off. Checked on PC with test cartridges, not with the
  real games, and not on a Vita.
- **Button test** (Settings > Controls): shows every button, both sticks, the
  touch screen and the four corners of the rear panel as you press them, with
  what each one does for the console you are on, so you can check a worn button
  or a mapping. Hold Start + Select to leave (or a finger on the bottom line,
  if one of the two is the worn button). The Super NES and Sega players
  have the same page in their menu's Controls.
- **More covers found** (a few percent more of the full game lists on Game Boy
  Advance and Super NES), and a failed save now says what is really wrong
  instead of "the card is full".
- **GBA fixes** for Top Gun: Combat Zones, Polarium Advance, Rock 'N Roll
  Racing, Activision Anthology and X-Men: Reign of Apocalypse.

- **Game Boy Camera.** The Pocket Camera cartridge boots, takes pictures and
  keeps its 128 KB of photos. The picture comes from the Vita's front camera
  (mirrored, like a mirror), its back camera, or a `camera.png` you put in the
  data folder (Settings > This game > Camera: Front / Back / Picture file); a
  Vita without a camera, or one that cannot start it, uses the file or a test
  picture. Exposure, gain, edge enhancement and the dither pattern the game
  sets are followed; a real camera's sensor noise and infrared glow are not.
  The Vita's camera runs only while the game is taking pictures and is
  switched off after about two seconds without one, so a Camera game left in
  its menus does not drain the battery. The gain steps follow a smoother
  curve than the sensor's decibel table (a picture differs from earlier test
  builds), and the cartridge's RAM reads 0 during a capture, as on hardware.
- **Game Boy Printer.** Games that print (Game Boy Camera, Pokemon's Pokedex,
  Zelda DX's photos, ...) find a printer on the cable when nobody is linked.
  A print is saved as a PNG in `prints/` (`<game>-<date>-<n>.png`) in the
  game's colours, grey by default, and a toast says "Printed: saved to prints".
  Long pictures printed in bands are one file, and the file is written while
  the game keeps running. Settings > This game > Game Boy Printer turns it
  Off; it never answers during a link.

**Online and multiplayer**

- **Online play by room code.** One of you hosts and gets a code of five
  characters; the other types it. No router settings, no accounts. The Vitas
  link directly when they can and through a relay when they cannot.
- **Every two-player console.** GBA Link Cable and Wireless Adapter, Game Boy
  and Game Boy Color Link Cable, save and state sharing, and now Two players
  for NES, WonderSwan, Super NES, Genesis / Mega Drive, Sega CD, Master
  System and SG-1000. The join finds your copy of the game for you. On a cable each of
  you can bring your own game (Ruby with Sapphire).
- **Safe to join.** Every emulator now checks a game state sent by the other
  player, or read from a file, before using it.
- Both Vitas need VitAdhoc 2.0.

**GBA sprite order**

- **Sprites that overlap are drawn in the real console's order**, so a sprite
  no longer peeks through a menu in Golden Sun and Mario Golf. The rule was
  already in 1.4; 2.0 checked it line by line against a reference renderer on
  both games and added a pixel-by-pixel test cartridge. Host results; see
  docs/COMPATIBILITY.md.

**Game Boy accuracy**

- **Game Boy and Game Boy Color games follow the real console more closely.**
  The Game Boy core was measured against the public test ROMs (blargg's
  suites, dmg-acid2 and cgb-acid2) and the failures that games can notice were
  fixed: the flags after PUSH AF, DAA, ADD SP and LD HL,SP+n; the ten sprites
  per line limit and which sprite is on top where they overlap; the HALT bug;
  the 20 clocks an interrupt takes; and sound registers reading back their
  unused bits as 1. The CPU test ROM (cpu_instrs) went from 2 of 11 passing to
  all 11, and dmg-acid2 and cgb-acid2 now draw the reference picture. Some
  games draw a line differently when more than ten sprites share it, as on a
  real Game Boy. Host results; see docs/COMPATIBILITY.md.
- **Game Boy sound registers are exact.** The last sound tests that failed
  (blargg dmg_sound and cgb_sound: 8 and 9 of 13 passing) pass now, 13 of 13
  each. The sweep of square channel 1 writes its new pitch back where the next
  write to its registers finds it; channel 3 has its own clock, so reading or
  writing wave RAM while it plays gives what the real console gives, and a
  retrigger on a Game Boy corrupts the start of wave RAM as it does there.
  Ordinary games sound the same (a sound register write now takes effect a few
  clocks later, well under one sample). Old save states load. Host results.

**Names in every alphabet**

- Japanese (kana and kanji), Korean, fullwidth forms, accented Latin, Greek and
  Cyrillic names now show as written in the app and in the Super NES and Sega
  players' menus, instead of `?`. Names are never cut in the middle of a
  character, and long ones wrap at sensible places.

**Under the hood**

- A TLS client, STUN, hole punching and a relay client of our own, and the
  small room service they talk to.
- New shared kits, `playerkit` (menus, pacing, look governor, link) and
  `widekit` (wide view), so every console behaves the same way.
- A fuzz-tested state loader on every core, a test gate, and a lot more
  testing than any earlier version.

Full engineering list: [docs/TECHNICAL.md](docs/TECHNICAL.md#version-20-changes).

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
- **Super NES picture and sound.** Menu row **Image quality** (Auto,
  Original, Enhanced 2x, Enhanced 3x, MMPX 2x): Auto eases the scaler and the
  blends only when a game cannot hold 60. A new audio
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
