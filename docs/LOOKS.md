# Looks: what every console offers

Everything below is in the in-app menus; nothing needs a file edit. The wording is the
same everywhere: **Ambient glow**, **LCD colours**, **LCD ghosting**, **Scanlines**,
**LCD grid**. Every strength is a 0..10 scale (0 is off, 10 is the strongest) and the
same number looks the same in the main app and in both players (the scales are built the
same way; nothing here has run on a Vita).

| Look | Main app (GBA, GB, GBC, NES, WonderSwan) | Players (SNES, Genesis / Mega Drive, Master System, Game Gear, SG-1000) |
|---|---|---|
| Ambient glow | 0..10 (0 black bars, 5 default) | 0..10 (5 default) |
| LCD colours | GBA: vivid / GBA screen / GBA screen, dark; GBC: vivid / GBC screen; GB: its own palette choice; WonderSwan: vivid / WonderSwan screen; NES: "NTSC colours" (vivid / NTSC) | Game Gear: On/Off; the others show their console's colours unchanged |
| LCD ghosting | 0..10 on GBA, GB, GBC and WonderSwan | Game Gear 0..10 |
| Scanlines | 0..10, independent of the grid | 0..10 |
| LCD grid | 0..10, independent of the scanlines | 0..10 |

Defaults (improvements on, tastes off): Ambient glow 5, LCD colours "GBA screen" for the GBA
and "GBC screen" for the GBC (the other consoles raw), LCD ghosting 0, Scanlines 0, LCD grid 0.

Scanlines and LCD grid used to be one exclusive Video filter in the main app with one
strength. They are two rows now. Old settings are carried over once: filter LCD grid became
LCD grid at the old strength (the old 1..10 scale kept, so the look is the same), filter
Scanlines became Scanlines the same way, and the filter became Sharp bilinear. The old
ambient modes became glow 5 (art or game picture) or 0 (off); the old ghosting On
became 5. Per-game files carry over the same way. The art-only / art-else-game choice of the glow
is no longer a menu row (it stays a config key).

## GBA colours default (decision)

The GBA's own screen was unlit and dim, so its games were drawn with saturated colours that look
washed out and too bright on a modern screen. The GBA colour setting "GBA screen" mutes
and shifts them back. gpSP's own default is off, but its libretro option is a taste choice; the Vita's
screen is a small OLED/LCD held close, where raw GBA colours are noticeably harsh. On
the twin renders of the test game (host x86, not a Vita) the raw picture is the more
glaring one, so the default is "GBA screen". Changing it is one row (Settings > Display >
LCD colours). The decision rests on those renders and on what was found about the
frontends; no number from a Vita backs it.

## Screenshots

Pictures for the owner are produced off-repo. The main app's shots come from the app's host
twin (host x86), whose software backend really draws the grid, scanlines, ghosting, colours
and glow with the same formulas. Player shots are SIMULATED (the same formulas applied to a test
frame) and marked in the file name; they show the intended look, not a Vita capture.
