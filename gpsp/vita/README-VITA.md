# VitAdhoc

Game Boy Advance, Game Boy and Game Boy Color on the PS Vita, with two-player link play: a
native Vita app built from GBAdhoc 4.1.1 (the PSP emulator), with its own look. Written with an
AI assistant (Claude) and tested on a real Vita; GPL-2.0, like GBAdhoc and gpSP.

## What it looks like
- **Home.** Your games as a row of tiles. The focused one is large, with its name beside it,
  and behind everything is that game's picture: its box art or hero art if you added some,
  otherwise the last scene you saw in it. VitAdhoc saves that scene each time you leave a game.
  Left and right move along the row, Cross plays. Down lifts the page and shows the game's save
  states as cards: pick one and press Cross to continue from it. Square stars a game, Select
  shows only starred games, Triangle switches between GBA, GB and GBC, Start opens Settings.
  The time and the battery sit top right. The old list layouts are still in Settings > Game list.
- **Previews.** Rest on a game for about a second and the picture behind it starts to move: the
  last ten seconds you played of it, real frames of your own game, a little like a streaming
  service's trailer. It plays, fades back into the still picture (its own last frame), and
  starts again a moment later. If you stood still for a while before leaving (a pause screen,
  a dialogue left open), the clip ends a second after things went still instead. VitAdhoc
  records while you play (every second frame, squeezed in memory; a typical clip is 0.2 to
  2 MB) and writes `clips/<game>.clip` when you leave the game. Nothing is recorded during fast
  forward, rewind or a link session. A game played for under three seconds keeps its old
  clip. Turn it off in Settings > Interface > Game previews.
- **Box art comes by itself.** When the console is on Wi-Fi and the game list is showing, every
  game without a picture in `boxart/` gets its cover from the libretro thumbnail server (the
  one RetroArch uses), and its title screen as well (`titles/`), which is the home screen's
  picture for a game you have not played yet. The game is found by its file name when that is the usual No-Intro name
  ("Pokemon - Emerald Version (USA, Europe).gba"), otherwise by the code in a GBA cartridge's
  header, otherwise by the file's checksum, so renamed files still find their cover. The
  picture is saved as `boxart/<game>.png` (and `titles/<game>.png`) and replaces the tile's
  card a moment later. Games the server has no picture for (homebrew, hacks with unusual
  checksums) are listed in `boxart/.missing.txt` (and `titles/.missing.txt`) and not asked
  for again. **Settings > Interface > Covers** shows how far it got ("looking: 12 of 40", then
  "38 of 40 games have one"); press Cross there to look again for the missing ones. Nothing is
  downloaded while a game runs, without Wi-Fi, or in ad hoc mode. Your own pictures in
  `boxart/` always win. Turn it all off with `art = 0` in `vita.ini`, or only the title
  screens with `art_titles = 0`.
- **A few lines about the game.** Under the focused game's name: the first sentences of its
  English Wikipedia article (about 1,300 GBA, GB and GBC games), shown with a "From Wikipedia"
  chip. They are built into the app (`gamedb.txt`, under 1 MB), so they need no network.
- **In a game**, hold Start + Select (or tap the picture). The game stays on screen, dimmed,
  and a sheet rises with seven cards: Resume, Save states, Play together, Cheats, Settings,
  Game list, Exit. The line above the cards says what the focused one does: how many slots are
  used, whether you are linked, which cheats are on.
- **Every page** uses the same navy, the same rounded selection and the same header (page
  title, the game, the console and room chips, the time).

The app's data folder is still `ux0:data/GBAdhoc/` and its title ID is still GBAD00001, so
installing this over GBAdhoc keeps every game, save, state and setting.

**Status: runs on a real Vita.** Build 20 plays Pokémon Emerald on the console at the display's
full rate (59.94 frames a second) with sound, even with every picture option on at once:
stretched to the full screen, the wide view, xBRZ smoothing, the GBA's colours and frame
blending (6.6 ms of each 16.7 ms frame). Build 20 brings in what GBAdhoc added up to 4.1.1 and gpSP's September 2026 fixes:
turbo buttons, Fast link, sharing saves and states between two Vitas, Mystery Gift from a
phone, Game Boy links between two different cartridges, and title screens fetched with the box
art. Some features have so far been checked only on computer stand-ins and in the Vita3K
emulator, real Wi-Fi between two Vitas above all ("What was tested" at the end says which is
which).

## Install
1. Copy `VitAdhoc.vpk` to the Vita and install it with VitaShell (HENkaku / Enso).
2. Start it once. It creates `ux0:data/GBAdhoc/`.
3. Copy games to `ux0:data/GBAdhoc/roms/` (`.gba`, `.gb`, `.gbc`; sub-folders are fine), or
   leave them where they are: see "Games in other folders" below.
   A PSP GBAdhoc folder (roms, saves, states, box art) can be copied over as it is.
4. Optional: box art in `boxart/`, wide banners in `hero/`, your own cheat files in `cheats/`.

No BIOS file is needed: a free replacement is built in. With the original GBA BIOS as
`ux0:data/GBAdhoc/gba_bios.bin` the app uses that instead (bring your own dump: no BIOS is
part of the source or of any package shared publicly).

## Games in other folders
The game list shows `roms/` first and then the games of any other folder you name, without
copying anything. Same rules as `roms/`: the file types of the console on the list, sub-folders
down to two levels, A to Z by file name (a game with the same name in two folders is listed
twice).
- **Which folders.** `extra_roms` in `ux0:data/GBAdhoc/vita.ini`: up to eight folders, a `;`
  between them, e.g. `extra_roms = ux0:ROMS/GBA; ux0:Games/GB`. Each has to start with its
  drive (`ux0:`). With that line, those folders are the list; `extra_roms = 0` (or nothing
  after the `=`) turns the other folders off altogether.
- **Without that line** the app looks for these, in this order, and uses the ones that exist:
  `ux0:ROMS/GBA`, `ux0:ROMS/GB`, `ux0:ROMS/GBC`, `ux0:roms/GBA`, `ux0:roms/GB`,
  `ux0:roms/GBC`, and RetroFlow's `ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance`,
  `... - Game Boy` and `... - Game Boy Color`. (The card does not tell `ROMS` from `roms`, so a
  folder is never listed twice; `roms/` of the data folder itself is never taken a second
  time.) A card with none of them is read exactly as before.
- **Hidden files are ignored**, in every folder: any file whose name starts with `.`. A card
  written by a Mac holds a 4 KB `._Game.gba` beside every `Game.gba`; those are not games.
- **Saves and states go next to the game**, in its own folder (`Game.sav`, `Game.st0` ...), as
  they do in `roms/`. Nothing is copied, and nothing is written to `roms/` for such a game. Box
  art, cheats and the home screen's picture follow the game's file name, as they do for a game
  in a sub-folder of `roms/`; so do Link mode (Settings) and the partner chosen for two players
  on one console (two games with one name share them). A star (favourites) follows the whole
  path, so the same name in two folders can be starred separately.
- The list holds 2048 games. If you have more, the list says how many it shows; the extra
  folders are cut first, never `roms/`.
- The log's `rom_roots` line (at start) says which folders were taken and why any other was
  not; `rom_scan_root` lines say how many games each had.

## Controls (all can be changed in Settings > Controls)
| Vita | Does |
|---|---|
| D-pad / left stick | D-pad |
| Cross / Circle | A / B |
| L / R, Start, Select | the same |
| **Tap the game's picture**, or hold **Start + Select** | open the menu |
| Square | fast forward |
| Select + Square, or right stick held left | **rewind** |
| Right stick held right | fast forward |
| Right stick down | cheat button (for button-activated codes) |
| Triangle | next video preset (2x, 3x, fit, stretch, 4x crop; sharp or LCD filter) |
| Select + L / Select + R | quick save / quick load |
| Select + Triangle | pause screen |
| L + R + Select | screenshot (`ux0:data/GBAdhoc/screenshots/`) |
| Select + Down | Mystery Gift: start / stop listening for a phone (GBA games) |
| (none until you set them) | four **turbo buttons** |

**Turbo buttons** (Settings > Controls > Turbo buttons): Turbo A, Turbo A (2), Turbo B and
Turbo B (2), each on a button of your choice, at 10, 15 or 30 presses a second (counted in
game frames, so fast forward speeds them up too), either while held or switched on and off
with a press. None is set at first. They rest during link play and with two players on one
Vita, and stop when the menu opens, a state is loaded or the game is left.

The menus also work by touch: tap a row, tap the hints in the footer, flick lists.
Start and Select reach the game a moment late (a tenth of a second), so that a shortcut such as
Select + L does not also press Select in the game; `chord_guard = 0` in `config.ini` switches
that off.

## The picture (Settings > Display, kept per console: GBA, GB, GBC)
- **Video scale** -- 2x, 3x, fit (the whole height, 3.4x), stretch, 4x crop.
- **Video filter** -- nearest, bilinear, sharp bilinear, LCD grid, scanlines, and two that redraw
  the pixel art: **Scale3x** (rounds stair-steps, light) and **xBRZ smooth** (rebuilds curves
  and diagonals; the heaviest). Those two run beside the game on the Vita's other cores, redraw
  only what changed and recognise scrolling, so the game itself never waits for them. Build 17
  made scrolling nearly free for them: the smoothed picture is kept as a ring that a scroll
  only turns, instead of being moved and re-sent to the GPU every frame. On the console,
  walking in Emerald with xBRZ at fit went from 56 frames a second with the sound breaking up
  to a steady 59.94 with none; with the wide view on as well, from 52 to 59.7. Build 19: a
  picture that changes whole (a fade, a flash, a new map) costs xBRZ a full redraw (20 to 33 ms
  at 4x), so during fades every other smoothed picture was skipped, and the smoothing then
  stepped down to 3x and back up, each time making new textures on the emulator's thread (12
  to 30 ms: the frames the game had to catch up). Now such pictures are smoothed at 2x until
  the screen settles, inside the same 4x textures, and nothing is made anew.
  **xBRZ on the GPU was tried in build 19 and is off:** the same xBRZ rules as GPU programs
  (from libretro's "xbrz-freescale", in three passes: the points where four GBA pixels meet,
  each pixel's corners, then every screen pixel at its final size), compiled by the Vita
  itself with the shader compiler in `ur0:data/libshacccg.suprx`. The picture is right, but
  on the console the GPU took 31 ms over each frame with it (2.6 ms with the CPU's xBRZ),
  half of it for the last pass's 444,000 pixels: it cannot keep 60 frames a second, so xBRZ
  stays on the CPU's cores. `vita.ini gpu_xbrz = 1` turns it on for experiments (the first
  time, the compiler takes about 26 s; the programs are then kept in
  `ux0:data/GBAdhoc/shaders/`); a guard measures the GPU as the game runs and gives xBRZ back
  to the CPU when a frame's GPU work takes over 10 ms twice in a row
  (`gpu_xbrz off reason=slow`).
- **Colours** -- as the game sends them, or as a GBA's (or Game Boy Color's) own screen showed
  them: muted, what the art was drawn for. **Motion blend** mixes each frame with the last,
  which is what games that flicker to fake transparency rely on.
- **Wide view** (GBA) -- instead of black bars beside the picture, more of the game:
  - *on* (the default): in games the app has a profile for. That is **Pokémon Emerald
    (USA/Europe)**, tested, and -- written from the same games' published symbol tables but
    not yet run, because only Emerald was at hand -- Ruby, Sapphire, FireRed and LeafGreen
    (USA/Europe, every revision). In the overworld the map is drawn 21 columns further on each
    side, at the same size, filling the screen. In a battle the field reaches both edges (the
    Pokémon and trainers come in from further out) and the text panel spans the screen; the
    wipe into a battle covers the full width. The app reads the game's own map to do this; the
    game, the save, cheats and link play are not touched, and the 240 columns in the middle
    are exactly the console's picture. A profile checks itself against the picture every
    frame; if its reading of the map does not match what the game shows (a wrong guess for one
    of the untested four), it shows nothing rather than something wrong.
  - *on + 3D tracks*: also in any game while it shows a rotated/scaled floor ("Mode 7", the
    racers). Not tried with a commercial racer yet; sprites a game parks just off-screen may
    show.
  - *on + any game* (experimental): for games without a profile. The app remembers every
    piece of background a game draws and where in the level it belongs, and shows it again
    when it lies beside the screen: the side you came from, and everywhere you have been. For
    what the game has not drawn yet it sends a **scout**: a second, invisible copy of the
    running game that runs a little ahead of you in spare time and reports what the level
    looks like there. Walking sideways, it holds your direction. When the picture moves up or
    down (a map seen from above) there is nothing new beside the screen for the game to show,
    so the scout steps to one side and then goes your way, a turn for the left and a turn for
    the right. The game you play is not touched by it (its pictures and sound are the same
    with and without). Where neither has seen anything -- a room just entered, the far side of
    a wall, the first moments of walking into new ground -- that part of the sides falls back
    to the *menus* setting below; a line is either the real thing or not drawn. It never
    guesses: checked against Emerald's profile, every tile it showed was the right one. The
    scout costs time (it is a second game running) and it is only as good as it is fast; it
    rests while two players are linked, and whether a Vita has the time to spare is not
    measured yet.
  - **Wide view: menus** -- what the sides show where a game has no more to give (a menu, the
    bag, a title screen): *soft edges* (the screen's own edge colours continue outward, dimmed;
    the default), *stretch* (the screen is drawn across the whole width), or *bars*.
  - The margins need room: they show at fit, 2x and 3x; at stretch and 4x the console's
    picture already takes the width.
  - Known limits in Emerald: people at the very edge appear a few pixels late (the game does
    not draw them further out), weather may not reach the margins, and the picture behind the
    app's own menu is the plain one.

## Speed and sound (Settings > Gameplay > Keep full speed)
- **Keep full speed** (on by default) -- when the Vita falls behind for a moment (the emulator
  meeting new game code at a scene change, the wide view filling in), the frames it owes are
  run without being drawn, two at most before each drawn one, so the game keeps the display's
  pace and its sound has no gap. You may see a frame skipped; you will not hear one. A stall
  of more than eight frames (loading) is let go rather than caught up. Off, the game slows
  down with the Vita and the sound breaks up instead.
- **A frame starts once the last picture is on the screen.** The display holds two pictures in
  a queue; a frame that hands it one while the last is still waiting used to wait there, and
  from then on every picture reached the screen a refresh late. Now a frame waits for the
  screen before it reads the buttons, so the queue stays empty: no wait in the queue, and up
  to one refresh (17 ms) less between a press and the picture.
- **The sound's cushion grows when it needs to.** It starts at 60 ms; each time it runs dry
  (with Keep full speed off, or a stall longer than the cushion), it grows by 20 ms, up to
  120 ms, and shrinks back by 5 ms after each half minute without a gap.
- Measured on the owner's Vita (build 18, Emerald from his save, the same scripted walk across
  a scene change): with xBRZ at fit, 59.94 frames a second in every ten-second window after
  loading and no gap in the sound; with the wide view too, the same (10 and 20 frames caught
  up in the two windows with a scene change; build 17: 59.7). The same walk with the wide view
  and Keep full speed off: 57.6 frames a second in the slowest window and 11 gaps in the sound.
- Not affected: fast forward, and two players (linked or on one Vita), where both games must
  stay frame for frame together.
- **The GBA's picture is drawn on another core** (build 19). gpSP draws the picture line by
  line while it emulates, and that was nearly half of the emulator thread's time (2.2 ms of
  4.8 ms a frame in Emerald). Now the emulator only notes what each line needs (the PSP
  version's Media Engine renderer did the same), and a thread on another core draws the frame
  from those notes while the emulator goes on with the vertical blank; the frame waits for it
  before it is shown, so what you see and when is unchanged. Checked on the computer copies on
  every frame of four test cartridges (the picture the same to the pixel, including games that
  rewrite graphics memory while the screen is being drawn). With the wide view, its margins
  are still drawn on the emulator's thread (they come from the live game, line by line) and
  the 240 pixels in the middle on the other core; the lines are put together at the end of the
  frame. Only VRAM pages written since the last frame are handed over (usually none to two of
  96). Two players (linked or on one Vita) draw as before.
- **Fast forward at 3x** no longer draws the two frames in each three that are never shown.
- **500 MHz**: with the PSVshell plugin, VitAdhoc has its own profile
  (`ur0:data/PSVshell/profiles/GBAD00001`) that runs the processor at 500 MHz instead of 444.
  Measured: about a tenth less time a frame. The app's log shows the measured clock
  (`mhz_measured` in the `switches2` line); the system tells the app 444 either way.

## Cheats
Open the menu > **Cheats**. The app ships with a cheat database (about 2,190 games); a game is
recognised when it boots and its known cheats are listed, all off. Switch on what you want.
- Triangle adds a code of your own (GameShark / Action Replay / CodeBreaker on GBA, GameShark /
  Game Genie on GB and GBC). The page tells you whether the code is understood before you save it.
- Some games need the entry called "Enable Code (Must Be On)" switched on first.
- Entries marked *unsupported* are code types the engine cannot run (about 5 % of the database).
- Cheats switch themselves off while you are linked to another console, and come back afterwards.

## Play together
Menu > **Play together**. Pick the **Connection** once:
- **Wi-Fi** — both Vitas on the same Wi-Fi network (or one phone's hotspot). One player chooses
  *Host a game*, the other *Join a game* and picks the room from the list, which fills by itself.
- **Internet** — the host's page shows an address; the other player types it in. If the host's
  router supports UPnP the port is opened automatically and the public address is shown;
  otherwise forward UDP port 19019 to the host's Vita.
- **Ad hoc** — no router at all. The Vita's own connection screen appears first.

Three kinds of link are handled: GBA Link Cable, GBA Wireless Adapter, and the GB / GBC Link
Cable. For a GBA game both players need the same game open (for Link Cable games, the same ROM
file). A **Game Boy / Game Boy Color** link works between two *different* cartridges (Red with
Blue, Gold with Silver) even when neither Vita has the other's: each one sends its cartridge to
the other for the session, in memory only (checked, never written to the memory card).
Vita-to-Vita only: a Vita and a PSP running GBAdhoc cannot link to each other.

- **Fast link** (Settings > Wireless; GBA Link Cable games): two frames of
  buttons in each packet instead of one, for a game that runs below full speed while linked.
  Off by default; the host's setting is the one used. On a strong connection the link is also
  quicker than before: the answer to each frame rides on the next one.
- **Share game data** (Settings > Wireless, opened from the game list; Wi-Fi only): send a save file or a save state
  to another Vita. One player chooses *Share game data* and picks what to offer; the other
  chooses *Join a share* on the same network, sees the list and takes what they want (a
  state goes into the slot they pick). A save it replaces is kept as `<game>.sav.bak`. Both
  need build 20 or later.
- **Mystery Gift** (Pokémon FireRed, LeafGreen and Emerald): a phone running
  [Mystery Gift Station](https://github.com/ShoshinFauteux/MysteryGiftStation) stands in for
  Nintendo's distribution station, and the game receives a real Wonder Card through its own
  MYSTERY GIFT menu; nothing is written into the save by the emulator.
  1. Open Mystery Gift Station on the phone and pick a gift. It starts a Wi-Fi network named
     **Mystery Gift**.
  2. On the Vita, join that network in the system's Settings > Network > Wi-Fi Settings (a Vita
     app cannot pick a network itself, as the PSP version does).
  3. Start the game, then press **Select + Down** (or menu > Play together > Mystery Gift). The
     Vita listens; the page says when the gift has arrived.
  4. In the game, choose MYSTERY GIFT on the title screen and receive the card as usual.
  Select + Down again stops it. Mystery Gift and a link session cannot run at the same time.

**Two players on this Vita** runs both Game Boys on one console, side by side:
- The second Game Boy starts from a copy of your save, or from its own save file
  (`<game>.p2.sav`) — or with a **different cartridge** (Ruby with Sapphire, Red with Blue),
  using that game's own save. This is how you trade with yourself.
- Flick the right stick sideways, or tap the other picture, to change which one you control;
  tap the one you already control (or hold Start + Select) for the menu.
- With a second controller, each player has their own. On a PS TV pair a DualShock normally. On
  a handheld Vita you need a controller plugin; use **ds34vita with reVita's "Emulate DS4"**
  (or two controllers) so the pad is a separate player rather than a copy of the Vita's buttons.

Bluetooth between two Vitas is not possible (the system's Bluetooth only serves controllers,
headsets and keyboards); ad hoc is the no-router option.

## Continue, Recent and play time
- **Continue.** Leaving a game keeps the moment you left it (`<rom>.rsm`, beside the save), and
  the point is renewed every two minutes of play and just after the game saves itself, so a
  Vita closed from LiveArea still continues. On the Home page **X Continue** puts the game back
  there; **Up** (or the Start over button) plays from the battery save instead. A patch put
  beside the game after the point was made starts it from the save.
- **Recent.** Once something has been played, Select on the Home page goes All games >
  Starred > **Recent** (latest first). The game page says when a game was last played and for
  how long (menus and a sleeping Vita not counted).
- **Game order** (Settings > Library, from the game list): by name, last played first, or most
  played first. The record is `playlog.txt` in the data folder.

## Patches (ROM hacks and translations)
Put the patch beside the game with the game's name: `Game.gba` + `Game.ips` (or `.bps`, `.ups`).
The game is started patched; the original file is never changed (the patched copy is made once,
in `patched/`, and made again when the patch changes). Saves and states keep the original's
name. BPS and UPS patches are checked against the game they were made for and refused for
another version; the game then starts unpatched and a message says why. The game page shows
*Patched*. For link play both players need the same patch: the link compares the patched games.

## Motion and light sensor cartridges
Recognised by themselves: **Boktai 1-3** (light sensor), **WarioWare: Twisted!** (gyro),
**Yoshi Topsy-Turvy** and **Koro Koro Puzzle** (tilt) and, on Game Boy Color, **Kirby Tilt 'n'
Tumble**. Their rows appear in Settings > **This game** while one is running:
- **Motion controls**: *tilt the Vita* (its motion sensor) or *right stick*.
- **Motion strength** (low / normal / high) and **Direction** (normal / reversed, if a turn or
  tilt feels backwards).
- **Level**: hold the Vita the way level should be and press X.
- **Sunlight** (Boktai): none to full sun in ten steps, or *follows the clock* (daylight by the
  Vita's time of day).
The reading is part of each frame, so rewind replays it. In a link session the other machine
sees a still console in a dark room.

## A look of its own (Settings > This game)
**Display settings: just this game** gives the running game its own screen size, quality,
filter, colours and palette; the Display rows then edit that game's look (the section's chip
says *This game*). Back to *all GBA games* (or GB, GBC) and it follows the console again. Kept
in `games/<rom>.ini`, with the game's sensor settings.

## Super Game Boy borders
Game Boy games made for the Super Game Boy (Pokemon Red, Blue and Yellow, Donkey Kong, Kirby's
Dream Land 2, ...) are shown inside the border they send, in its colours. The border is
kept (`borders/<rom>.sgb`), so it is there from the first frame next time, and after a loaded
state. Settings > Display > **SGB border** (Game Boy only) turns it off.

## RetroAchievements
Earn [RetroAchievements](https://retroachievements.org) as you play. Settings > **RetroAchievements**
(from the game list) > **Account**: X asks for your user name, then your password (shown as
dots). The password is sent once and never kept: the app keeps the name and the server's token
in `retroachievements.txt` and signs in with those when it starts. On the Account row, X twice
signs out.
- When a game starts, its file's fingerprint (MD5) is looked up; a game the site knows shows
  "N of M achievements". An unlock is a message at the top of the screen and is sent at once,
  or later if the Vita is offline. A patched game is looked up as patched (the site lists hacks
  and translations separately).
- In game, Settings > **Achievements** lists them, earned or not, with their points.
- **Hardcore** (from the next game started): no loading of states (slots, the shelf, Continue),
  no rewind and no cheats, as the site's rules ask; fast forward stays. A game the site does not
  know is not restricted.
- Achievements pause during link play and start over afterwards, as after a loaded state.
- It needs Wi-Fi (not ad hoc). The connection is HTTPS through the Vita's own secure-connection
  library; if that fails on your firmware, the log (`log/gbadhoc.log`, lines `ra_http`) says why.

## Save data
Battery saves and save states live beside the ROM, with the PSP version's names
(`<rom>.sav`, `.st0`–`.st4`), and the Continue point (`<rom>.rsm`). Five state slots, with previews, in the menu and on the game list.
For a game kept in another folder (see "Games in other folders") that is its own folder, not
`roms/`: back up, or move, the game and its save files together.

## If something goes wrong
Send back `ux0:data/GBAdhoc/log/` (the newest file). Useful switches, in
`ux0:data/GBAdhoc/vita.ini` (create it; one `key = value` per line):
- `extra_roms = ux0:ROMS/GBA; ux0:Games/GB` — the folders besides `roms/` the game list also
  reads (see "Games in other folders"); `extra_roms = 0` — none
- `drc = 0` — run without the dynamic recompiler (slow, but tells us if a crash is in it)
- `pace_mode = 1` — pace by timer instead of the display's vertical blank
- `io_thread = 0` — write saves and the log on the main thread
- `pad_trace = 40` — log the first 40 changes of the buttons as the console reports them (did a
  press arrive at all?)
- `keys_script = 1` — give the first game the buttons in `ux0:data/GBAdhoc/keys.txt`, frame by
  frame (`wait N`, `hold START 6`, `dump` = a screenshot, `evt TEXT` = a line in the log): a run
  that reaches the same place every time with nobody holding the console
- `clips = 0` — record no previews, whatever Settings say (the log's `clip_save` line says what
  recording cost: `enc_us_avg` is the time to squeeze one frame, on a thread of its own)
- `art = 0` — fetch no box art; `art_url = http://host:port` — fetch it from another server
  with the same layout (the log's `art_fetch` lines say what was asked and what came)
- `up_slide = 1` — follow scrolling the way builds 16 and before did (to compare); `up_slide =
  0` — not at all
- `main_cpu = 0` — keep the emulator's thread on the first core (the default lets the system
  choose; measured: no difference)
- `tex_mem = 1` — keep the textures rewritten every frame in main memory instead of the GPU's
  (measured: no faster, and slow to create; for experiments)
- `render_thread = 0` — draw the GBA's picture on the emulator's thread, as before build 19;
  `render_thread = 2` — draw it both ways and compare every frame (`vita_render` lines:
  `checked`, `bad_frames`, `bad_px`; a `render_check` line for the first frames that differ;
  also `render_us` the other core's time, `copy_us`/`pages` the hand-over, `wait_us` how long
  the frame waited for the picture)
- `fade_drop = 1` — smooth a fade's pictures at 3x instead of 2x; `0` — at 4x, as before
- `gpu_xbrz = 1` — draw xBRZ on the GPU (too slow on a Vita, see above; the log's `gpu_xbrz`
  line says whether the GPU took it and how its programs were made: `compiled`, `cached`, or
  why not, e.g. `no_compiler` or the compiler's first error; `gpu_xbrz_time` lines are the
  guard's samples of the GPU's time over a frame; `vita_gpu` counts the pictures)
- `gpu_time = 1` — time the GPU on every frame (each present waits for it; for measuring)
- `gpu_xbrz_view = 1` — the screen shows GPU xBRZ's corner flags instead of the picture (red
  top left, green top right, blue the bottom two: coloured specks along every edge mean the
  GPU is working the passes out); `2` the first pass's. `gpu_probe = 1` compiles test
  programs at start and logs what the compiler said (`gpu_probe` lines); `gpu_probe = N` (2
  or more) reads the first two passes' targets back at the Nth GPU picture (`gpu_rt` lines;
  Vita3K never writes them back, so there it always reads zeros)
- `core_phase = 2` — the core's split of each frame in the log (`core_phase` lines: `cpu` the
  game's code, `vid` the picture, `blt` handing it over, ...); `3` also the wide view's parts
  (`vita_wide_prof`). It costs a little time itself.
- `drain = 0` — start a frame without waiting for the last picture to reach the screen (builds
  17 and before); `drain = 2` — wait only until the display has taken it. If the display never
  says a picture was shown, the app gives the wait up by itself (`drain off` in the log)
- The log's `vita_draw` line has the slowest of each part of drawing a picture in the window,
  `up_factor` lines say when the smoothing's factor changed (2 during a fade), and
  `vita_upscale` counts its late pictures by factor (`late_f`) and its steps down (`stepped`).
- The log's `vita_win` line (every ten seconds) says how the frames went: `late` (frames that
  fell a refresh or more behind), `forgiven` (times the game gave the time up: loading, a long
  stall), `caught` / `caught_us` (frames run without being drawn to catch up, and their cost),
  `swap_us` (average/longest time handing a picture to the display: near 150 µs when the queue
  is empty) and `q` (pictures already taken / already on screen when a frame ended, and the
  refreshes waited for them). `vita_audio` has the sound's `level`, its `target` cushion (in
  32768ths of a second) and `under`, the gaps so far.
- The log has a `vita_threads` line every ten seconds: for each of the app's threads, the core
  it last ran on, the microseconds it ran and how often it was interrupted. `vita_upscale`
  says how long the smoothing took, how often a picture had to be skipped (`dropped`) and
  what made the slow ones slow (`late=` count/slid/percent redrawn).

## What was tested, and what was not
**On a real Vita** (the owner's, build 3): boot, the game list, Pokémon Emerald from his own
save at full speed with sound, the menus by touch, video scales, the smoothing filter as it
was then (too slow: it ran inside the frame), two players on one console (playable without the
filter). Builds 12 to 14, driven over Wi-Fi: Emerald at 3x with xBRZ holds the display's rate
(59.9 frames a second in every ten-second window after the first, no filter frame dropped) on
the title screens, into the overworld and walking there, with the wide view's check of every
map tile finding none wrong; the scroller test cartridge with the automatic mode and the scout
(the scout's cost measured there led to build 13's larger budget).

**In the Vita3K emulator on a Mac** (the real package, the real ARM code): every build since --
Cross as A, the menu by tap, colours, the filters beside the game, and the wide view. In
Emerald: the overworld (600 frames a window, every map tile on screen compared with the app's
reading of the map: none differed), a state made on a computer loaded on the "Vita", a
trainer battle from the wipe to the action menu at full width, and the renderer's self-check
through all of it (4 million pixels a window, none wrong). With the scroller test cartridge:
the automatic mode filling both sides, the scout sent out and reporting. An emulator says
nothing about a console's speed.

**On Linux stand-ins that run the same app code** (one of them on the Vita's own ARM code
generator under an ARM emulator): the game list and menus, save states, fast forward, rewind,
cheats (every code in the database was parsed; RAM, ROM and Game Genie codes were checked in
running games), Wi-Fi rooms / typed address / ad hoc between two running copies including
packet loss and dropped connections, two players on one console, the filters (pictures equal to
a whole-picture scale; the threaded hand-over under ThreadSanitizer), and the wide view: three
test cartridges -- a "Mode 7" floor, a side-scroller that streams its level in as it goes,
and a map seen from above -- whose margins are compared, pixel for pixel, with what a console
shows when everything is moved 21 pixels sideways (walking into new ground with the scout
ahead, sideways and up and down; walking back; standing; after a jump to another place). With the owner's Emerald (not in
the tests that ship): walking across two map borders, into and out of a building, a trainer
battle, the menus, states and rewind; and the automatic mode judged by Emerald's profile
(walks in all four directions, through a building and across map borders: more than a
million tiles shown, none wrong; with the scout at three of its frames to one of the
game's, nine tenths of the margins filled while walking up and down, all of them sideways
and standing).

**Build 17 on the owner's Vita** (driven over Wi-Fi, Emerald from his save, the same walk
each time): xBRZ at fit 59.94 frames a second while walking, no sound underruns (build 16:
56, 38 underruns, a smoothed frame skipped one time in three); with the wide view too, 59.7
(was 52, 70 underruns); nearest and 3x unchanged; the cover for Emerald downloaded and shown
on the home screen with its description, the test cartridges correctly found to have none.
Build 18, the same walk: 59.94 in every window after loading with xBRZ at fit and with the wide
view as well, no gap in the sound, a picture handed to the display in about 0.14 ms (it waited
9 ms whenever the queue was full before); with Keep full speed off, 11 gaps.
Build 19 also takes four fixes from the upstream gpSP (libretro/gpsp, August 2026): a save
type guessed as EEPROM from a stray library string in the cartridge gives way to the SRAM or
flash the game really uses; a game reading open bus while running from open bus no longer
overflows the stack; and two kinds of code the compiler was free to get wrong (a rotate by 0,
and a negative branch offset shifted left, in two places).
Build 19 so far (the same walk): with the fades smoothed at 2x, the frames caught up in the two
windows with scene changes went from 3 and 8 to none (xBRZ at fit) and from 13 and 24 to 0-2
(with the wide view), the slowest frame from 37 ms to 13 ms; at 500 MHz, a tenth less time a
frame. The wide view's map rows are made once per row of tiles instead of on every line
(its self-check: 4 million pixels a window, none wrong). Drawing the picture on another core,
measured on the console (500 MHz, the same walk): checked frame by frame against the core's own
picture (`render_thread = 2`), not one pixel different in any window, with and without the wide
view; the emulator's time a frame went from 4.9 to 3.9 ms with xBRZ at fit (the other core
draws in 1.9 ms; the frame's end waits 0.7 ms for it) and from 6.8 to 5.5 ms with the wide view.
Everything on at once (stretch to the full screen, the wide view, xBRZ, the GBA's colours and
frame blending): 6.9 ms a frame, 59.94 frames a second in every window, no frame late, no gap
in the sound; stretch or 4x crop with the wide view and xBRZ: 5.4 ms.

**Build 20 on the owner's Vita** (driven over Wi-Fi, Emerald from his save, the same walk,
500 MHz): his settings 59.94 frames a second in every window after loading, no frame late or
caught up, no gap in the sound, the emulator's time a frame 3.6 ms (build 19: 3.9); everything
on at once 59.94 in every window, 6.6 ms a frame (build 19: 6.9); the picture drawn on the other
core checked against the core's own on every frame (`render_thread = 2`, 3,600 frames): not one
pixel different. Turbo: with Turbo A on Square, the test cartridge's A lit and unlit by turns
while Square was held, and stayed unlit once it was let go. Mystery Gift: Select + Down started
listening in Emerald ("Mystery Gift" chip on screen), a Wonder Card parcel from Mystery Gift
Station's own files sent from a computer on the same Wi-Fi was accepted (answer "accepted",
handed to the emulated distribution cart), Select + Down stopped it; the in-game half was not
tried (it writes the card into the save). The home screen fetched Emerald's title screen by
itself and marked the test cartridges as having none. His save was the same, byte for byte,
after all of it.

**Games in other folders, on the Linux stand-ins only:** the game list with a second folder
(a game picked from it starts, its save written beside it, a state saved there, a star kept
and read back), hidden `._` files left out, lists of 1,500 and 2,200 games sorted and complete
(the second one cut at 2,048, the extra folder first), the `extra_roms` list read as written
and the built-in folder names found, the other-game picker for two players on one console
reading such a folder, and a partner's Game Boy cartridge found in one over the cable. Not
yet tried on a real Vita.

**Build 20, on the Linux stand-ins:** every suite on the x86 stand-in (app, links, two players
on one console, rewind, the network transport, Mystery Gift's protocol), the app suite on the
ARM stand-in, and a test for each new part -- turbo buttons pressing at their
rate (Hold at 10 a second, Toggle at 30) and stopping when let go or pressed again; a Fast
link session and the riding answers; a save shared between two running copies over Wi-Fi, byte
for byte, and over a save of the receiver's own, which was kept as `.sav.bak`; Mystery
Gift's network half (Mystery Gift Station's own parcels, taken from its repository, sent to the
app, answered, and handed to the emulated distribution cart; the cart itself is GBAdhoc's,
with its own tests, because the in-game half needs FireRed, LeafGreen or Emerald's Mystery Gift
menu); a Game Boy link between two different cartridges, each console sent the other's; the
title screens fetched and shown on the home screen; and one test cartridge for each of gpSP's
September 2026 fixes (I/O registers read through the right handler, code run from video
memory, and the flags after a few instructions the recompiler handled wrongly).

**Not tested:** real Wi-Fi between two Vitas and the ad hoc connection screen, second-controller
ports, the on-screen keyboard on hardware; the Wireless Adapter mode was only brought up and
torn down, never used by a game; no commercial game but Emerald was run (so: the Ruby,
Sapphire, FireRed and LeafGreen profiles, the automatic mode in a real platformer, and "3D
tracks" in a real racer are untested); the home screen's previews on a console (recorded, saved and played only on the computer
stand-ins so far, with the scroller test cartridge and with Emerald); Mystery Gift from a real
phone (on the console the parcel came from a computer) and its in-game half; sharing between
two real Vitas.

Credits for the built-in game list (`gamedb.txt`): cartridge names and checksums from the
No-Intro catalogues as published in libretro-database; which Wikipedia article is about which
game from Wikidata (CC0); the descriptions are the opening sentences of English Wikipedia
articles, CC BY-SA 4.0 (shortened, accents written plainly). Box art and title screens are
fetched from thumbnails.libretro.com when the app runs and are not part of the package.
Mystery Gift, sharing, turbo and Fast link are GBAdhoc's (ShoshinFauteux); the phone app is
Mystery Gift Station, from the same author.

Not in this build: sharing over ad hoc (no router) or the internet, one-cartridge "multiboot"
multiplayer, Game Boy "Xploder" cheat codes, 3–4 player links.
