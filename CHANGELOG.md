# What's New

## Version 1.3

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
- A game in another folder keeps its own states, even when two games share a
  name.

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
