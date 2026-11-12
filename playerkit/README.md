# playerkit

The shared part of VitAdhoc's separate game players (SNES, NES, and whatever
comes next): one in-game menu, one set of save-state files, one remapping
model, one launch protocol, ROM patching, and on-screen notices. It is written
so the experience is the same on every console: the same labels, the same
order, the same buttons, the same touch behaviour.

## Licence

**MIT** (see `LICENSE`). This is our own new code.

playerkit contains **no code copied or adapted from `gpsp/` (GPL) and none from
xBRZ**. The SNES player links Snes9x, whose licence is not GPL compatible, so
nothing GPL may be linked into it. The Game Boy Advance menu was read for how it
looks, what it is called and how it behaves, and the formats it writes were
matched by looking at the files; every line in this folder was written fresh.
The built-in fonts are the OFL-licensed Inter and Outfit, rasterised once by
`tools/gen_font.cpp` into `src/pk_font_data.cpp` (the generator uses
`stb_truetype`, public domain, and is not part of the library).

Dependencies: the C++11 standard library, and thin platform bindings behind
`pk::Gfx` (`vita/pk_gfx_vita.cpp` over vita2d on the console; the software
renderer `src/pk_gfx_soft.cpp` on Linux for tests).

## What is in it

| module | files | job |
| --- | --- | --- |
| util | `pk_util` | CRC32/Adler32, paths, `key=value` files, file operations behind a table (so tests can break the card), the safe write (temp, read back, swap, never lose the old file) |
| gfx, text, icons | `pk_gfx.h`, `pk_text`, `pk_icons`, `pk_font_data`, `pk_gfx_soft`, `vita/pk_gfx_vita` | the small drawing interface and what draws on it |
| menu | `pk_menu` | the control centre: Resume, Save states, Play together, Cheats, Settings (Picture, Controls, Speed), Reset game, Game list, Exit; pad with repeat, touch with 80 px-tall targets |
| states | `pk_states` | ten slots with pictures, resume point, undo of a load; same folders and picture format as the GBA side (`FORMATS.md`) |
| input | `pk_input` | Vita buttons, sticks and the rear panel to console buttons for four players, profiles, turbo, shortcuts, the START + SELECT chord with the chord guard |
| launch | `pk_launch` | the protocol between the app and a player: `launch/<console>.ini` in, `launch/session.txt` out |
| patch | `pk_patch` | IPS, BPS and UPS applied to a ROM in memory, with checksums |
| notice | `pk_notice` | one-line toasts |

Documents: `FORMATS.md` (the files), `INTEGRATION.md` (exact steps for the SNES
player, the NES player and the main app).

## Building and testing

```
make -C playerkit test      # unit tests + scripted menu walk, ASan + UBSan, on Linux
make -C playerkit shots     # renders every menu page to PNG (SHOTS=dir to choose where)
make -C playerkit vita      # compiles the library for the PS Vita (vitasdk), not run
make -C playerkit regen-font
make -C playerkit clean
```

`PK_TMP` chooses where the tests make their temporary folders (default
`build/tmp`). The tests run with `TZ=UTC` so time stamps are reproducible.

## How the menu is used

```cpp
pk::Menu menu;
menu.configure(cfg, &host, &states);          // host: callbacks; caps: what exists
menu.attach_input(&mapper, &store, &pk::model_snes(), &profile);
menu.open();
// every frame while it is open:
pk::MenuEvent ev = menu.update(input);        // pads, touch, dt
/* draw the frozen game picture, then */ menu.draw(&gfx, &text);
```

Rows the host has not wired are shown greyed with the reason, never hidden, so
a feature that arrives later lights up without changing the layout.
