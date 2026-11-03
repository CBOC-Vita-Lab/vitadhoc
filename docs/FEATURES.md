# Features And Settings

VitAdhoc combines GB, GBC and GBA emulation with a Vita-native library,
display pipeline, touch input and motion controls. This is the detailed guide;
start with [Install](../INSTALL.md), [Controls](../CONTROLS.md) or
[Play Together](../PLAY-TOGETHER.md) for a specific task.

## Technical Summary

| Item | Implementation or limit |
| --- | --- |
| Platform | Native PS Vita application; title ID `GBAD00001`; 960 x 544 output |
| GBA | gpSP core and ARM dynamic recompiler; original game image 240 x 160 |
| GB / GBC | TGB Dual core; original game image 160 x 144; Super Game Boy border support |
| Rendering | GXM/libvita2d; GBA render worker and CPU upscaling workers |
| Input | Buttons, sticks, front touch, motion sensors; mappings and turbo settings |
| Game files | `.gba`, `.gb`, `.gbc`; supported stored/deflated `.zip` entries up to 32 MiB |
| Library | Up to 2,048 games per console; folders scanned two levels deep |
| Extra folders | Up to eight; Settings writes a list capped at 480 characters |
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
RetroFlow locations. Advanced users can set `extra_roms` in `vita.ini` or
`config.ini`. A list in `vita.ini` takes precedence; `extra_roms = 0` disables
the extra locations. Per-console `gba_roms`, `gb_roms` and `gbc_roms` keys are
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

Missing information can produce incomplete margins or the configured
fallback. Wide view leaves the game's save unchanged. Turn it off for a
game whose edges are incorrect.

### Borders, Overlays And Screenshots

Super Game Boy borders are composed by the GB core and cached for next time.
PNG screen overlays are separate: put a transparent image in
`overlays/<game stem>.png`, or use `gba.png`, `gb.png` or `gbc.png` as the
console-wide fallback. A 960 x 544 image matches the output; other sizes up
to 1024 x 1024 stretch to fit.

**Settings > Display > Screen overlay** controls overlays. **Grid strength**
adjusts the LCD/scanline effect from 1 to 10 (default 5). Screenshots use
**Settings > Interface > Screenshot folder**, or the default
`ux0:data/GBAdhoc/screenshots/`; the button chord is L + R + Select.

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
