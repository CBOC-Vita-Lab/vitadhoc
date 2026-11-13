# Defaults for 1.4

The rule: a first start shows the best picture, sound and feel the console can hold at a locked 60, on every
console the same way. A game that cannot hold 60 at the best look is eased one small step at a time by the look
governor (`playerkit/src/pk_look.*`, `playerkit/INTEGRATION.md` section 8), the least visible sacrifice first, and
gets the look back when there is room. A setting the player changes by hand is never touched again.

Every number below is a host measurement, a model or an estimate. Nothing in this document has been measured on a
real console; section 6 lists what has to be.

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

Therefore the single user-visible row is Smoothing, and Auto means "xBRZ at the highest factor that holds 60, else
sharp bilinear". The Super NES player cannot use xBRZ (xBRZ is GPL, the SNES core is not GPL compatible); its
smoothing is sharp bilinear plus the two blend rows below.

## 2. Main app (GBA, GB, GBC, NES, WonderSwan): table

Storage: per console in `config.ini` (`scale_<c>`, `filter_<c>`, `quality_<c>`, `ambient_<c>`, `wide_<c>`,
`blend_<c>`, ...), a game can have its own copy. "Cost" is the model/estimate of extra work.

| Setting (menu row) | Storage | Current default | Proposed default | Reason |
| --- | --- | --- | --- | --- |
| Image quality / Smoothing (`SET_QUALITY`) | `quality_<c>` | Original on a fresh install, Auto only when an older `scale`/`filter` key exists (`config_psp.c` `pdisp_load`) | Auto, where Auto means the governed xBRZ ladder (4x, 3x, 2x, sharp bilinear); Original stays a choice | A fresh install currently never sees the finished improvement, and two installs of the same version look different. Gated on the governor (below): until it is wired, keep Original |
| Video filter (`SET_FILTER`) | `filter_<c>` | Nearest (Auto follows it); row shows "sharp" under Original | Sharp bilinear stored as the base filter | Auto should never fall back to nearest (uneven pixels at 960 x 544) |
| Scale (`SET_SCALE`) | `scale_<c>` | Fit (fills the height at the right aspect) | Fit (no change) | Right; no stretch |
| Ambient glow (`SET_AMBIENT`) | `ambient_<c>` | Hero art, else blurred game frame | no change | Already on; first (least visible) rung of the ladder |
| Wide view GBA (`SET_WIDE`) | `wide_gba` | 1: games with a profile | no change | Already on where it is correct. Name the values the same as NES/SNES (Auto/On/Off) |
| Wide view NES (`SET_NESWIDE`) | `nes_wide` | 0 = Auto (sides filled where known) | no change | Already on |
| Wide fill (`SET_WFILL`) | `wide_fill_gba` | 0 black bars | 1 (its own edges continued softly) | A black bar is the look to avoid; the soft edge is finished. Needs a look at a menu screen before release |
| Frame blend (`SET_BLEND`) | `blend_<c>` | Off | Off | It changes what flickering games look like; the right default is the console's own |
| WonderSwan smoothing (`SET_WSBLEND`) | `ws_blend` | On | On | Consistent |
| Colour correction (`SET_COLOR`) | `color_<c>` | GBA 0, GBC from its old key | no change | A taste, not a quality; leave |
| LCD grid (`SET_GRID`) | profile | Off | Off | A taste |
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
| Side glow | `glow` | 5 of 10 | 5 | On, with a 0 to 10 scale |
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

Visibility is a 1..100 judgement, not a measurement. Savings are estimates the governor replaces with what it
learns on the console.

| # | Main app (all consoles) | vis | est. saving |
| --- | --- | --- | --- |
| 0 | Ambient glow: blurred game frame to still art | 8 | 3 % of a refresh |
| 1 | xBRZ 4x to 3x | 15 | 12 % |
| 2 | xBRZ 3x to 2x | 25 | 10 % |
| 3 | Frame blend off (where it is on) | 40 | 5 % |
| 4 | xBRZ 2x to sharp bilinear | 55 | 10 % |
| 5 | Wide view off (GBA, NES; last, it changes the composition) | 80 | 12 % |

| # | Super NES player (no xBRZ) | vis | est. saving |
| --- | --- | --- | --- |
| 0 | Blend interlace lines off | 30 | 4 % |
| 1 | Blend hi-res columns off | 45 | 4 % |
| 2 | Wide view off | 70 | 10 % |

Why this order: the first rungs change what the player cannot see on a 5 inch screen at arm's length; the last
rungs change what the player picked the game's presentation for. A row set by hand is skipped.

The only xBRZ evidence on file is one heavy full-screen game that showed 44 fps at xBRZ 3x on a Vita-class
model run (see `docs/performance/`). So xBRZ cannot be the default without the governor, and with it the heavy
game starts at the best look, drops to the first factor that holds 60 within a few seconds, and remembers it.

## 6. Not measured (needs a console)

- Cost of each xBRZ factor per console and per game (all the savings above are estimates).
- Whether the ladder holds 60 on a real Vita for the heavy case, and how long the first drop takes.
- The look of Wide fill soft edges at menu screens; the visibility order.
- Real hand-over counters feeding `LookSecond` (only simulated loads have been tested).
