# Features And Settings

VitAdhoc combines GB, GBC, GBA, Super NES, NES and WonderSwan emulation with a Vita-native library,
display pipeline, touch input and motion controls. This is the detailed guide;
start with [Install](../INSTALL.md), [Controls](../CONTROLS.md) or
[Play Together](../PLAY-TOGETHER.md) for a specific task.

## Technical Summary

| Item | Implementation or limit |
| --- | --- |
| Platform | Native PS Vita application; title ID `GBAD00001`; 960 x 544 output |
| GBA | gpSP core and ARM dynamic recompiler; original game image 240 x 160 |
| GB / GBC | TGB Dual core; original game image 160 x 144; Super Game Boy border support |
| Super NES | Snes9x 1.63 in its own program (`snes.bin`, non-commercial licence); sound and picture drawing on their own cores; up to four players with the Super Multitap |
| NES / Famicom | FCEUmm inside the app (GPL-2.0-or-later); NES, Famicom Disk System (your own `disksys.rom`) and UNIF games |
| WonderSwan / Color | Beetle WonderSwan (Mednafen) inside the app (GPL-2.0-or-later); 224 x 144 at 75.47 frames a second, blended into each 59.94 Hz refresh; horizontal and vertical games |
| Rendering | GXM/libvita2d; GBA render worker and CPU upscaling workers |
| Input | Buttons, sticks, front touch, motion sensors; mappings and turbo settings |
| Game files | `.gba`, `.gb`, `.gbc`, `.sfc`, `.smc`, `.nes`, `.fds`, `.unf`, `.unif`; supported stored/deflated `.zip` entries up to 32 MiB |
| Library | Up to 2,048 games per console; folders scanned two levels deep |
| Extra folders | Up to 24; Settings writes a list capped at 480 characters |
| State slots | Ten per game (`.st0` to `.st9`); sharing protocol exposes the first five |
| Rewind | Up to 60 seconds, subject to mode and available history |
| Soft patches | IPS, BPS and UPS; original file unchanged; no soft-patching zipped games |
| Open BIOS | Built in; an optional personal dump can be supplied on the Vita |
| Network features | Art, RetroAchievements, supported link modes, local save sharing and Mystery Gift receiver |

See [Performance](PERFORMANCE.md) for Vita measurements and display settings.

## Library And Continue

The home screen has console tabs, All/Starred/Recent views, sorting and
play time. Find matches words in any order and ignores accents and
punctuation. Latin accents are folded for the menu font; unsupported scripts
may display as `?` without preventing the game from loading.

**Settings > Library > Game names** chooses what the list calls a game: "Game
title" (the default) cuts the bracketed tags, so `Quest (Hack v2).gba` reads
"Quest"; "File name" shows the file's name as it is, without the extension.
Whatever the setting, when two or more games in the list would show the same
name, each of them also shows its file name under it, so a ROM hack and its
parent can be told apart. Only the words on screen change: stars, covers, play
time and saves stay with the file.

On Wi-Fi, covers and title screens can be matched by name, cartridge code or
checksum. Your own artwork takes priority. Optional hero images and short
clips appear behind the selected game. Clips record your own play, remain on
the Vita and can be disabled. Wikipedia-derived descriptions are bundled
with attribution; this does not bundle the games or their cover collections.

Continue uses a resume state kept when leaving a game, periodically during
play and after an in-game save. On the home screen, Cross continues and Up
starts from the battery save. A resume point is separate from the ten manual
state slots. Changed patches can invalidate a resume point; keep an ordinary
in-game save too.

Games with the same filename share name-based artwork, cheats and previews.
Copies in different folders can appear twice with separate battery saves.
Use distinct filenames and back up games with their neighbouring save files.

## Saves, Imports And State Slots

Battery saves and states live beside the game, not in the application bubble.
GBA uses `Game.sav`; GB uses `Game.gb.sav`; GBC uses `Game.gbc.sav`. The manual
has the [complete naming table](../INSTALL.md#bringing-saves-from-another-emulator).

Before loading a game without a save, import checks the `import/` drop
folder, compatible sidecar saves and RetroArch's savefiles folders. An
existing save or recovery file prevents overwriting it. The accepted file is
copied; only an `import/` source is renamed `.imported` afterwards. Save
states from other emulators are not portable battery saves. Import size is
limited to 256 KiB and compatibility still depends on the save layout.

State menus show five entries at a time and scroll through all ten. Quick
save/load uses the selected slot. State sharing remains limited to five slots
for compatibility with earlier peers. In Hardcore mode, state loading,
Continue and rewind are restricted for games recognised by RetroAchievements.

## Game Folders, Zips And Launchers

Use **Settings > Library > Game folders**. Enter a folder to add it; enter
the same folder again to remove it. The library rescans after the change.

Built-in discovery includes the data-root `roms/`, common `ux0:ROMS/` and
RetroFlow locations (`ux0:ROMS/NES`, `ux0:roms/NES` and RetroFlow's "Nintendo
- Nintendo Entertainment System" among them). Every folder directly inside
`ux0:ROMS/` (a "SNES ROM Collection" or "NES Games" folder, say) is scanned
too; the file ending picks the console, never the folder's name, so a `.sfc`
in a folder called NES is still a SNES game. Advanced users can set `extra_roms` in `vita.ini` or
`config.ini`. A list in `vita.ini` takes precedence; `extra_roms = 0` disables
the extra locations. Per-console `gba_roms`, `gb_roms`, `gbc_roms`, `snes_roms` and
`nes_roms` keys are
also accepted unless `vita.ini` already supplies the complete list.

The zip reader selects the first supported game entry, validates its CRC-32
and caches the unpacked game in `unzipped/`. Saves, states and settings keep
the archive's name. No encrypted, ZIP64 or multi-disk archives; use one game
per zip. Patches and local play with a different second cartridge require
unzipped games.

Launchers can pass a raw cartridge path through
`psgm:play?titleid=GBAD00001&param=...`. The path is not URL-decoded. It is read
at application startup, so close VitAdhoc before launching a different game.
The [install guide](../INSTALL.md#starting-a-game-from-another-app) has an example.

## Super NES

SNES games open in their own player and return to the home screen when you
quit. **L + R + Start** opens its pause menu (the game page and the loading
screen of a SNES game say so): Resume, Picture, Multitap, four
save-state slots, Reset and Quit. Battery saves are `Game.srm` beside the game.

Picture (saved for all SNES games, or for one with "Apply to: this game"):
Original shape (4:3 at full height, the default), Zoom (blank rows cropped, not
stretched), Full screen or Pixel perfect; Sharp or Smooth scaling; side glow,
CRT scanlines and LCD grid, each Off or 1-10; optional blending for hi-res and
interlaced pictures. A PNG in `overlays/` named like the game (or `snes.png`)
is drawn as a bezel. Every frame is shown with every picture option on; none
is skipped or simplified to save time.

## NES And Famicom

NES games run inside VitAdhoc, as Game Boy games do: the same menu (Start +
Select), ten state slots with pictures, Continue, fast forward, Game Genie and
RAM cheats, patches, play time, per-game settings, RetroAchievements and the
RetroArch save import (FCEUmm, Nestopia, Mesen folders). Battery saves are
`Game.srm` beside the game, the file RetroArch uses. Famicom Disk System
games (`.fds`) need your own `disksys.rom` (put it beside the game or in
`ux0:data/GBAdhoc/nes/`); VitAdhoc cannot include it. UNIF files (`.unf`,
`.unif`) play too. Box art and title screens come from the libretro
thumbnails for the NES and the Famicom Disk System; a NES file is recognised by
its contents without the 16-byte header (and the 512-byte trainer some files
carry), a UNIF file by its name only. Rewind works as for Game Boy games; link
play is not available for NES games. PAL and Dendy games run at their own 50
frames a second (five of every six refreshes).

## WonderSwan

WonderSwan and WonderSwan Color games (`.ws`, `.wsc`, `.pc2`) share one WS
tab and run inside VitAdhoc: the same menu, ten state slots, Continue,
rewind, fast forward, RAM cheats (`AAAAA:VV`), patches, play time,
per-game settings, RetroAchievements (console 53) and the RetroArch save
import (Beetle WonderSwan folder). Saves are `Game.srm` beside the game,
RetroArch's layout (VitAdhoc adds the console's own settings memory at the
end, which RetroArch ignores).

- **Rotation.** The game decides: VitAdhoc follows its LCD icons, the
  buttons it reads and the cartridge. A vertical scene is turned upright on
  a level Vita (Vertical play: Upright, the default) or shown full size for a
  Vita turned on its side (Vertical play: Turn the Vita). The picture and
  the buttons change in the same frame; the first time, a hint shows the
  layout. Rotation: Auto, Horizontal or Vertical is kept per game.
- **Buttons.** Sideways: d-pad, Circle = A, Cross = B, Start. Upright: the
  d-pad is the game's cross turned with the picture, the face buttons its
  second cross, L = A, R = B.
- **75 frames a second on a 60 Hz screen.** Each refresh runs the one or two
  console frames it covers and shows their blend, weighted by time
  (Settings > Display > Smooth 75 Hz motion, on by default), so scrolling
  stays even. Off shows the newest frame only.
- **Palette** (game settings): greyscale or the original LCD's tint for black
  and white games.
- Box art and title screens: libretro thumbnails "Bandai - WonderSwan" and
  "Bandai - WonderSwan Color", by the file's checksum or name.

## Cheats on every console

The Cheats page works the same for every console: the list for the game (from
the bundled database for GBA, GB and GBC games, or the game's `.cht` file in
`ux0:data/GBAdhoc/cheats/` in RetroArch's format), codes typed by hand with
a name, a switch per code, delete, and the choices kept per game. Codes are
off during a link session and in RetroAchievements Hardcore. Formats: GBA
(GameShark, Action Replay, CodeBreaker, raw), GB/GBC (GameShark, Game Genie),
NES (Game Genie 6 or 8 letters, Pro Action Replay, `AAAA:VV`, `AAAA?CC:VV`),
WonderSwan (`AAAAA:VV`: a RAM address, 00000-0FFFF the console's memory,
10000 on the cartridge's save memory, and a byte).

## Display

Choose a screen size and filter per console, or keep a game-specific look in
**Settings > This game**. Sizes include integer scales, fit, stretch and
crop; filters include nearest, bilinear, sharp bilinear, LCD, scanlines,
Scale3x and CPU xBRZ. Colour correction and motion blending are independent
choices. Demanding combinations can cost more time than Original pixels.

CPU xBRZ and Scale3x use worker cores and changed regions; the GBA picture
can be drawn by a separate render worker. **GPU xBRZ is an experiment, not
the recommended faster option.** It is off by default, was slower in earlier
Vita measurements and needs a separately supplied shader compiler module.
No proprietary compiler is distributed. Its licensing/provenance caveats are
recorded in [Licensing](../LICENSING.md).

Keep full speed can run up to two undrawn catch-up frames to maintain game
timing through short hitches.

### Wide View

Wide view draws outside the original GBA viewport using game profiles, Mode 7
support or experimental background-cache/scout behaviour. Profiles target
the original Pokemon Emerald family, not every game with a similar name.
ROM hacks can change the layout or rendering enough to break a profile.
The [performance report](PERFORMANCE.md) includes Pokemon Unbound measurements.

By default Wide view is on only for games that have a profile; every other
game is shown at the console's own shape (240 x 160 at 816 x 544) with bars,
never stretched. Where a profiled game has nothing more to show (a menu), the
default is black bars; **Wide view: menus** can fade the picture's edges or
stretch it instead. Wide view leaves the game's save unchanged. Turn it off
for a game whose edges are incorrect.

### Borders, Overlays And Screenshots

Super Game Boy borders are composed by the GB core and cached for next time.
PNG screen overlays are separate: put a transparent image in
`overlays/<game stem>.png`, or use `gba.png`, `gb.png` or `gbc.png` as the
console-wide fallback. A 960 x 544 image matches the output; other sizes up
to 1024 x 1024 stretch to fit.

**Settings > Display > Screen overlay** controls overlays. **Grid strength**
adjusts the LCD/scanline effect from 1 to 10 (default 5). Screenshots use
**Settings > Interface > Screenshot folder**, or the default
`ux0:data/GBAdhoc/screenshots/`. The L + R + Select chord is off until
**Settings > Controls > Screenshot button combo** turns it on; the Vita's own
PS + Start always works.

## Audio

Version 1.1 rebuilt the Vita audio output used by GBA, GB and GBC. Fixed,
aligned double buffers keep playback steadier through longer frames. Fades
and blends start from the last sample played, smoothing transitions around
pause, resume, state loads, underruns and audio-backlog trims. Leaving a
game uses checked audio-thread shutdown and channel cleanup.

GBA's Game Boy-style square, wave and noise channels use the game's stereo
routing and sample-rate-based length, volume-envelope and sweep timing.
DirectSound FIFO resets clear queued samples and reset their read/write
positions together. These improvements are all included in 1.2.

Fast forward is silent unless you choose **2x with sound** or **3x with
sound** in **Settings > Fast-forward** (1.3): then the game's sound plays
along, sped up and higher, without a break when fast forward starts or stops.
Unlimited and Unlimited Smooth stay silent.

## Patches And Cheats

Place `Game.ips`, `Game.bps` or `Game.ups` beside `Game.gba` (or GB/GBC).
Where extensions share a stem, name the full file, for example
`Game.gba.bps`. A cached patched copy is used; the source ROM stays unchanged.

BPS and UPS verify source/result checksums. IPS does not identify the correct
base revision. Wide-view profiles and achievement sets need to match the
patched game. Use the same game and patch on both Vitas when linking.

The bundled cheat catalogue covers about 2,190 games; custom codes are
supported too. Some codes require an enable code, and some code types are
unsupported. Cheats suspend during link play and in recognised Hardcore
games. Back up a battery save before experimenting.

## RetroAchievements

Sign in through Settings. The password is sent for authentication, not stored;
the account name and token are kept in `retroachievements.txt`. Sign out or
delete that file to remove them. Do not publish it with logs or backups.

Recognition uses the game file's hash, including soft-patched content.
Hardcore restrictions apply to recognised games from the next launch;
unsupported games do not gain an achievement set by enabling the option.
Achievement processing pauses during link play and resets progress after
state loading, rewind or link sessions. Network retries do not establish
that every offline unlock has been accepted by the service.

For sign-in troubleshooting, see [Known Issues](../KNOWN-ISSUES.md).

## Sensors And Controls

The implementation covers gyro in WarioWare: Twisted!, tilt in Yoshi
Topsy-Turvy, Koro Koro Puzzle and Kirby Tilt 'n' Tumble, and sunlight in
Boktai. **Settings > This game** selects motion or right-stick input, strength,
direction and a level calibration. Physical direction and sensitivity need
game-specific checking; do not assume all cartridges use the same axes.

Controls can be remapped. Turbo A/B variants support hold or toggle and
10, 15 or 30 presses per second; they rest during link play and reset when a
menu opens or a state loads. The menu shortcut remains reachable. See
[Controls](../CONTROLS.md) for the defaults and two-player input switching.

## Link Play And Sharing

| Mode | Scope and limits |
| --- | --- |
| GBA Link Cable | Two Vitas; matching game files; Wi-Fi, internet or ad hoc transport |
| GBA Wireless Adapter | Supported Wireless Adapter games |
| GB / GBC cable | Compatible cartridges can differ; cartridge bytes are exchanged in memory for the session |
| Two players on one Vita | Side-by-side handhelds; switch control or use a supported distinct second pad |
| Save/state sharing | Local Wi-Fi; first five state slots; receiver should retain backups |
| Mystery Gift | Receiver for the supported station protocol; full phone-to-game/save flow still needs validation |

Vita-to-PSP, Bluetooth as a Vita-to-Vita transport, three/four players and
single-cartridge multiboot are not supported. Internet hosting needs a
reachable UDP port 19019; UPnP is not guaranteed. Controller/plugin setups
and two-device reliability need broader hardware confirmation. No plugins
or clock profiles are installed by the app.

Use [Play Together](../PLAY-TOGETHER.md) for setup and supported modes.

## Advanced Configuration And Diagnostics

Most settings belong in the UI. The following keys in
`ux0:data/GBAdhoc/vita.ini` are useful when diagnosing an issue:

| Key | Purpose |
| --- | --- |
| `art = 0` | Disable automatic artwork downloads |
| `art_titles = 0` | Skip automatic title-screen images |
| `clips = 0` | Disable gameplay previews |
| `grid_strength = 5` | LCD/scanline strength, 1 to 10 |
| `overlays = 0` | Disable PNG screen overlays |
| `screenshots = ux0:...` | Select a screenshot folder |
| `gpu_xbrz = 0` | Keep experimental GPU smoothing disabled |
| `render_thread = 2` | Diagnostic comparison mode; not a general performance recommendation |
| `drc = 0` | Diagnostic interpreter mode, not a recommended speed setting |

Close the app and back up configuration files before manual edits. Changes
made through Settings are preferred because they can be checked and saved
through the app's configuration layer.

Logs are in `ux0:data/GBAdhoc/log/gbadhoc.log`. They may contain game paths
and network addresses; review before sharing. See [Performance](PERFORMANCE.md),
[Technical Notes](TECHNICAL.md) and [Known Issues](../KNOWN-ISSUES.md) for
measurements, implementation details and troubleshooting.
