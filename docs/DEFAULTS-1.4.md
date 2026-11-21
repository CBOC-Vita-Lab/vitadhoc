# Defaults for 1.4

The rule: a first start shows the best picture, sound and feel the console can hold at a locked 60, on every
console the same way. A game that cannot hold 60 at the best look is eased one small step at a time by the look
governor (`playerkit/src/pk_look.*`, `playerkit/INTEGRATION.md` section 8), the least visible sacrifice first, and
gets the look back when there is room. A setting the player changes by hand is never touched again.

Numbers below are labelled: a console measurement (one, in section 5), a host measurement (instruction counts under
qemu-arm, host twin), or an estimate. Section 6 lists what still has to be measured on a console.

**The order (owner decisions, 13 Nov), for every default and for the governor:**
1. A locked, smooth 60 (no missed refreshes, no hitches) beats everything.
2. Wide view / screen fill stays ON, and hand-over stays on. The governor has no rung for them.
3. The best look that fits the budget. The scaler is therefore the first thing the governor gives up and the last it
   gets back: xBRZ 3x, 2x, MMPX 2x, then sharp bilinear, which is a normal floor whenever MMPX 2x cannot hold 60.

The governor starts low and climbs only with measured headroom, never into a miss. What it trades is the scaler's
resolution (the 3x GBA ceiling, 2x on NES). A row the player sets by hand is never touched.

## 1. Anti-aliasing, in plain words

There is no "anti-aliasing 4x" row, and hardware multisampling (MSAA) is not used in 1.4. MSAA smooths the edges of
the shapes a GPU draws (a triangle's border against what is behind it). A game picture here is one flat rectangle
holding a finished image; its four edges are the screen edges, so there is no polygon edge for MSAA to smooth, and
the pixels inside the rectangle are not touched by it. The smooth look people mean comes from how the picture is
scaled, and that already has two parts under the existing "Smoothing" / "Video filter" rows:

1. **xBRZ factor.** xBRZ redraws the game at 2x, 3x or 4x its size and rounds off stair steps on curves and
   diagonals while keeping the pixel art's colours (main app only, see licence below).
2. **How the result is fitted to the screen.** The Vita screen is 960 x 544. A GBA picture is 240 x 160, so xBRZ 4x
   gives 960 x 640 which is then shrunk by about 1.2 (under 2, so a bilinear read of it averages the 4 texels
   under each screen pixel, which is area sampling: the same effect a supersampled image gets from MSAA). The
   non-xBRZ path, "sharp bilinear", draws the game with point sampling up to a whole-number size first and then
   bilinear for the last fraction, so edges are crisp and the small final stretch is smooth. Both already fit the
   picture without a visible seam; neither needs extra work in 1.4.

Therefore the single user-visible row is Smoothing, and Auto means "the best of xBRZ, MMPX 2x, sharp bilinear that
holds a locked 60". The Super NES player cannot use xBRZ (xBRZ is GPL, the SNES core is not GPL compatible); its
smoothing is sharp bilinear plus the two blend rows below.

## 2. Main app (GBA, GB, GBC, NES, WonderSwan): table

Storage: per console in `config.ini` (`scale_<c>`, `filter_<c>`, `quality_<c>`, `ambient_<c>`, `wide_<c>`,
`blend_<c>`, ...), a game can have its own copy. "Cost" is the model/estimate of extra work.

| Setting (menu row) | Storage | Current default | Proposed default | Reason |
| --- | --- | --- | --- | --- |
| Image quality / Smoothing (`SET_QUALITY`) | `quality_<c>` | Original on a fresh install, Auto only when an older `scale`/`filter` key exists (`config_psp.c` `pdisp_load`) | Auto, where Auto means the governed xBRZ ladder (4x, 3x, 2x, sharp bilinear); Original stays a choice | A fresh install currently never sees the finished improvement, and two installs of the same version look different. Gated on the governor (below): until it is wired, keep Original |
| Video filter (`SET_FILTER`) | `filter_<c>` | Nearest (Auto follows it); row shows "sharp" under Original | Sharp bilinear stored as the base filter | Auto should never fall back to nearest (uneven pixels at 960 x 544) |
| Scale (`SET_SCALE`) | `scale_<c>` | Fit (fills the height at the right aspect) | Fit (no change) | Right; no stretch |
| Ambient glow (`SET_AMBIENT`) | `glow_<c>` | 5 of 10 (2.0: was three modes) | no change | Already on; first (least visible) rung of the ladder |
| Wide view GBA (`SET_WIDE`) | `wide_gba` | 1: games with a profile | no change | Already on where it is correct. Name the values the same as NES/SNES (Auto/On/Off) |
| Wide view NES (`SET_NESWIDE`) | `nes_wide` | 0 = Auto (sides filled where known) | no change | Already on |
| Wide fill (`SET_WFILL`) | `wide_fill_gba` | 0 black bars | 1 (its own edges continued softly) | A black bar is the look to avoid; the soft edge is finished. Needs a look at a menu screen before release |
| LCD ghosting (`SET_BLEND`) | `ghost_<c>` (2.0: 0..10; was `blend_<c>` on/off) | 0 | Off | It changes what flickering games look like; the right default is the console's own |
| WonderSwan smoothing (`SET_WSBLEND`) | `ws_blend` | On | On | Consistent |
| Colour correction (`SET_COLOR`) | `color_<c>` | GBA 1 (GBA screen, 2.0), GBC from its old key | no change | See docs/LOOKS.md |
| Scanlines / LCD grid (`SET_GRID`, `SET_LCDGRID`) | `scan_<c>`, `grid_<c>` | 0 / 0 | 0 / 0 | A taste |
| Steady pacing (`SET_STEADY`) | `steady_pacing` | On | On | Keeps a locked 60 / locked 30 instead of uneven |
| Smooth hand-over | hand-over key | On | On | Frees the game thread from drawing |
| Fast-forward multiplier (`SET_FFMULT`) | `ff_mult_x10` | 3.0x | 3.0x | |
| Rewind (`SET_REWIND`) | `rewind`, `rewind_mb` | 60 s / 24 MB on Vita | no change | |
| Show fps (`SET_FPS`) | `show_fps` | Off | Off | |
| Full speed (`SET_SPEED`) | `full_speed` | On | On | |

## 3. Super NES player: table

Storage: `<root>/snes/picture.ini` (and a per-game copy when "own settings" is on), read by `pic_load`.

| Setting | Key | Current default | Proposed default | Reason |
| --- | --- | --- | --- | --- |
| Picture shape | `shape` | Original (fills the height) | Original | Right |
| Pixel shape | `pixel_square` | 8:7 | 8:7 | |
| Quality | `quality` | Sharp (sharp bilinear) | Sharp | Already the finished filter |
| Ambient glow | `glow` | 5 of 10 | 5 | On, with a 0 to 10 scale |
| Scanlines / LCD grid | `scanlines`, `lcd_grid` | 0 | 0 | Tastes |
| Blend hi-res columns | `hires_blend` | Off | **On** | Removes the dither shimmer of pseudo-hi-res games; applied in this build (`snes/vita/player.cpp`) |
| Blend interlace lines | `interlace_blend` | Off | **On** | Removes interlace flicker; applied in this build |
| Wide view | `wide_view` | 0 = Auto | Auto | Already on unless pacing is thinning frames |
| Multitap | `multitap` | 0 = Auto | Auto | |
| Steady pacing, smooth hand-over | player.h | On, On | On, On | |

Known limit: a `picture.ini` that already contains `hires_blend=0` or `interlace_blend=0` keeps it (it was written
by an earlier version, we cannot tell a choice from a default). A fresh install, and a game without its own file,
get the new defaults.

## 4. Inconsistencies found

1. A fresh Vita install of the main app gets Original, an upgraded one gets Auto: two looks for one version.
2. "Auto" smoothing follows a hidden stored filter (nearest) that the menu shows as "sharp" under Original.
3. Wide view value names differ per console (GBA 0..3 by mode, NES and SNES Auto/On/Off).
4. Glow is a 0 to 10 scale in the SNES player but a 3-way choice (off, art, art or frame) in the main app: the
   owner's rule is a 1..10 scale where the setting has a degree.
5. The old xBRZ governor in the main app is disabled; the Smoothing row has no automatic protection, so a default
   of xBRZ would be unprotected until the new governor is wired.
6. SNES blend rows defaulted off while the same kind of effect (WonderSwan blend) defaults on.

## 5. Governor ladders (least visible first)

Visibility is a 1..100 judgement, not a measurement. Savings are the governor's first guesses, from the output
pixels each quality step writes; it replaces them with what it learns on the console.

Where Auto starts is the console's ceiling, and it is a starting point, not a promise: the governor starts a game
there, steps down in the first bad seconds, remembers where the game settled, and climbs back only with measured
headroom and never into a miss. The ceilings follow from output pixels, using the real width of the picture (the
GBA's wide view is 282 wide). The only console measurement is xBRZ 3x of a GBA picture, 345,600 output pixels a
frame, which held 59.7 emulated fps but drew 44 frames a second on a heavy game (Pokemon Unbound, one physical Vita,
`docs/PERFORMANCE.md`, "Pokemon Unbound 2.1.1.1 On VitAdhoc 1.2"): a console measurement, not a model. 4x of the
GBA picture writes 1.78 times the pixels of 3x, so it is not an Auto start and not a climb target, only a hand-set
choice.

| Console (picture) | Auto starts at | output pixels | quality steps down the ladder |
| --- | --- | --- | --- |
| GBA (282x160 with the wide view, 240x160 without) | xBRZ 3x | 406,080 (345,600) | xBRZ 2x, MMPX 2x, sharp |
| GB, GBC (160x144) | xBRZ 3x until measured (4x is 368,640 but nothing has run) | 207,360 | xBRZ 2x, MMPX 2x, sharp |
| WonderSwan (224x144) | xBRZ 3x | 290,304 | xBRZ 2x, MMPX 2x, sharp |
| NES (256x240) | xBRZ 2x | 245,760 | MMPX 2x, sharp (3x would be 720 lines, taller than the 544-line panel) |
| any other | xBRZ 2x | | MMPX 2x, sharp |

Rungs (GBA shown; every console has the same kinds, with its own quality steps):

| # | Step | vis | first guess of the saving |
| --- | --- | --- | --- |
| 0 | Ambient glow: blurred game frame to still art | 8 | 3 % of a refresh |
| 1 | xBRZ 3x to 2x | 25 | output pixels: 345,600 to 153,600 |
| 2 | xBRZ 2x to MMPX 2x | 35 | same size, about a third of xBRZ 2x's cost |
| 3 | Frame blend off (where it is on) | 40 | 5 % |
| 4 | MMPX 2x to sharp bilinear (the floor) | 55 | MMPX's whole cost |

The guesses are `output pixels x cost per pixel`: xBRZ is anchored at 0.45 of a refresh for 345,600 pixels (a
whole 3x picture redrawn is about 11-19 ms on the two helpers, `upscale_vita.c` UP_HOT_HOLD; a game's typical
picture is about half new), and MMPX at 0.65 of xBRZ 2x's cost per output pixel (measurements below). The old flat guesses (12 % for 4x to 3x) are gone.

MMPX 2x measured (240x160 RGB565 frames dumped from the private GBA games on the host twin; the output is
bit-identical to the authors' reference on all of them, 50 frames from 17 games, and on synthetic odd sizes):

| frame | MMPX 2x | xBRZ 2x | xBRZ 3x |
| --- | --- | --- | --- |
| Big Mutha Truckers (bitmap mode) | 10.3 M instr | 15.7 M | 17.1 M |
| NfS Porsche (bitmap mode) | 9.0 M | 12.7 M | 14.0 M |
| Golden Sun | 9.0 M | 12.4 M | 13.6 M |
| Minish Cap | 7.6 M | 9.6 M | 10.7 M |

Instructions are ARMv7 under qemu-arm (single-stepped count, one scale of the whole picture, tables built
outside); the same frames on the host x86 take 0.8-1.3 ms for MMPX 2x against 1.2-2.1 ms for xBRZ 2x and 1.4-2.2 ms
for xBRZ 3x. So MMPX 2x is about 0.65-0.8 of xBRZ 2x and 0.55-0.65 of xBRZ 3x: cheaper, not "much cheaper", and
it needs no float and no 2 MB distance table, which should favour it on a Cortex-A9's caches (not measured). Note
what the counts also say: xBRZ's work is mostly per source pixel (3x costs only 8 % more instructions than 2x), so
the output-pixel guesses above flatter the 3x to 2x step if the console is compute-bound and fit it if it is
bound by memory writes and the texture upload (the earlier console finding); the governor learns which.

Wide view and hand-over have no rung: they stay on. The old last rung (wide view off) was removed.

Software-3D GBA games: a game that keeps the video in a bitmap mode (3, 4 or 5) while the upscaler had to rescale at
least 70 % of the picture, in 7 of any 10 seconds, redraws its whole picture every frame (Need for Speed, Driver, SSX), so the scaler's "only what changed" and scroll
savings never apply. The governor starts such a game at sharp bilinear (no scaler holds 60 on a whole picture every frame until a console
says so) and remembers it; the usual climb gives MMPX 2x or xBRZ back with measured headroom. Seen in the host twin on the private GBA set: Big Mutha Truckers and Need for Speed
Porsche Unleashed sit in bitmap modes from the start (see the `look:` lines' `mode=` and `bm=` fields).

The check is skipped for a game that already has a memory file, and `vita.ini` `look_bitmap = 0` turns it off.

The game's memory (`look/<console>/<xx>/<name>.look`) keeps the settled look and the learned costs, so the next
start begins where the last one settled; a game that held 60 at the full look stores nothing.

Why this order: the first rungs change what the player cannot see on a 5 inch screen at arm's length; the last
rungs change what the player picked the game's presentation for. A row set by hand is skipped.

## 6. Not measured (needs a console)

- Cost of each xBRZ factor and of MMPX 2x per console and per game on the console (all the savings above are
  guesses from output pixel counts and qemu-arm instruction counts; no clocks).
- Whether the bitmap-mode check (7 of 10 seconds) is the right threshold for every software-3D game.
- Whether the ladder holds 60 on a real Vita for the heavy case, and how long the first drop takes.
- The look of Wide fill soft edges at menu screens; the visibility order.
- Real hand-over counters feeding `LookSecond` (only simulated loads have been tested).
