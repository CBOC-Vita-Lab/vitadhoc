# Looks: what every console offers

Everything below is in the in-app menus; nothing needs a file edit. Wording is the
same everywhere: **Ambient glow**, **LCD colours**, **LCD ghosting**, **Scanlines**,
**LCD grid** (in the main app: **Scanlines / grid strength**).

| Look | Main app (GBA, GB, GBC, NES, WonderSwan) | Players (SNES, Genesis / Mega Drive, Master System, Game Gear, SG-1000) |
|---|---|---|
| Colour correction ("LCD colours") | GBA: vivid / GBA screen / GBA screen, dark; GBC: vivid / GBC screen; GB: its own palette choice (no colour correction); NES and WonderSwan: none | Game Gear: On/Off; the others show their own console's colours unchanged |
| LCD grid | Video filter "LCD grid", strength 1..10 (one value for all consoles) | LCD grid 0..10 |
| CRT / scanlines | Video filter "Scanlines", same strength | Scanlines 0..10 |
| Ambient glow | Off / art / art, else game (per console) | 0..10 |
| LCD ghosting | On/Off (50 % blend of two frames) | Game Gear 0..10 |

Defaults: Ambient glow on, Scanlines and LCD grid off, LCD colours off (GBC: GBC screen),
ghosting off.

## Known differences (not closed)

- In the main app Scanlines and LCD grid are one exclusive Video filter with one
  strength, not two independent 0..10 sliders; ambient glow is three modes, not 0..10;
  ghosting is On/Off.
- No LCD colour correction for WonderSwan (and NES, which has none by nature).
- GBA colours stay raw by default.
- Players have no LCD colours choice except Game Gear.

## Screenshots

Pictures for the owner are produced off-repo. Colour, ambient and blend shots come
from the app's host twin (host x86). The twin has no GPU, so the grid, scanline and
player band looks are SIMULATED from the formulas in the renderers and are marked
in the file name; they show the intended look, not a Vita capture. Nothing here has
run on a Vita.
