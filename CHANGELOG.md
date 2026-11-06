# What's New

## Version 1.3

**Smoothing does less work.** The xBRZ scaler and the step that finds what
changed on screen do about a tenth less work, with exactly the same picture.

**Your saves are safe.**
- **A save is never cut short.** A save from RetroArch or another emulator
  that has extra bytes at the end (the clock data mGBA adds, a 131088-byte
  Pokemon save for one) used to lose them the first time VitAdhoc saved. They
  are now kept, for Game Boy saves too. ([#3](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/3))
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

**Smoother play with smoothing on**
- **Smoothing gives way when the game is busy.** With Keep full speed on (the
  default), if xBRZ or Scale3x smoothing keeps making frames miss the screen
  for a few seconds, VitAdhoc quietly uses a lighter smoothing (one step at a
  time, down to the screen's own filter), and goes back up once there is room
  again. It tries again slowly, so it does not flip back and forth. The game
  menu shows one line when smoothing is lighter than you chose. Turn Keep full
  speed off to always get the full smoothing.
  ([#7](https://github.com/CBOC-Vita-Lab/vitadhoc/issues/7))
- **Keep full speed** (Settings) now covers both: busy moments skip drawing a
  frame so the game and its sound keep their speed, and smoothing eases off.

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

Not taken from 4.5.0: its Pokemon MonoMyth accuracy work, its speed work for
self-modifying code, and its PSP-only changes (they only exist for the PSP's
processor). Nothing here changes saves or save states: 1.2 states still load.
These fixes are tested with our own test programs and one Pokemon game. They
are not tested on the games named above, nor on Big Mutha Truckers, Street
Racing Syndicate or Mortal Kombat: Deadly Alliance.

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
