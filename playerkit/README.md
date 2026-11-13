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
| notice | `pk_notice` | one-line toasts (two lines when a reason is long) |
| pace | `pk_pace` | steady frame pacing: when drawn frames cannot keep up, draw every second frame evenly; back to every frame after 3 s with headroom (a pure decision function plus a small history keeper) |
| handoff | `pk_handoff` | frame hand-over: a bounded mailbox that lets one thread emulate while another draws and presents, with an adaptive depth and a once-a-second census line (`INTEGRATION.md` section 7) |

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

## Changes made while wiring the SNES player (Nov 2026)

Found by integrating `snes/vita` (and its Linux twin, `snes/twin/player_twin`, which runs the player's
own code with the software renderer):

- **Card work per frame (bug).** The control centre called `States::used_count()` from `card_help()` and
  `draw_top()` on every drawn frame: ten `info()` calls, about 30 file lookups and up to ten `.meta` reads a
  frame on the memory card. The count is now taken once when the slot list is read.
- **States are read once.** `States` keeps the slot list in memory: one folder listing (`FsOps::list_dir`,
  new) and the `.meta`/`.thumb` of the slots in use, then every save, delete and load keeps it current. The
  menu reads no file to open or to show the Save states page (only the big picture of the chosen slot).
- **Fewer card operations per save** (`lessons-audit` 1d). `atomic_write` takes what the caller knows about
  the file (`Exists` hint) and no longer removes the temp file first or looks the file up (a lookup of a
  missing name walks the whole folder on the Vita's card): 5 operations for a replaced file, 3 for a new one.
  The pictures and the note beside a state are written in place (they are never the only copy of anything:
  a cut picture fails its size check, a cut note has no `end=1` and is ignored), and the old note is emptied
  before the state is swapped, so a stale note can never refuse a good state. Measured with `strace`: 9 file
  operations per state save and no failed lookups, against about 42 operations, 15 `mkdir` and 11 failed
  lookups before (`snes/README.md`).
- **Saves on another thread.** `States::prepare()` (game thread: copies the bytes, makes the two pictures)
  and `States::commit()` (any thread: the files). The menu hands a save to the host with
  `MenuHost::queue_state_write()` and shows the slot at once; the host reports with
  `Menu::state_write_done()`; `MenuHost::wait_state_writes()` is called before a load or delete. The
  cache is guarded by `SpinLock` (std::mutex is a no-op on the Vita: no pthread is linked).
- **Undo of a load is kept in memory** for the session, not as a `.undo` file (a 400 KB file write on every
  load).
- **Host rows and limited choices.** `Menu::add_host_row()` adds a console's own pickers to Picture, Controls
  or Speed (SNES: pixel shape, the two blend rows, Multitap, Steady pacing); `set_choices_allowed()` leaves
  out a value the player cannot do (SNES has no separate "Fit": Original already fills the height). The
  Speed link is hidden when its page would be empty.
- **Cheats can be added and deleted** (`CAP_CHEAT_EDIT`): "Add a cheat" row (`MenuHost::add_cheat()`; the
  host asks with the system keyboard and calls `Menu::refresh()`), Triangle or the row's Delete button asks
  and calls `MenuHost::delete_cheat()`.
- **Notices** on the pages sit at the foot of the screen (they overlapped the hints and the panel's edge),
  and a long reason wraps to a second line instead of being cut.
- **Remap capture texts** named the buttons for Enter = Cross even when Enter is Circle (bug).
- **`SessionTracker::seconds_played()`** (new): `current().seconds_played` is only filled in by `finish()`,
  so the thumbnails' "played" time read 0 when a host followed INTEGRATION.md.
- `init_shared()`: makes the CRC table and the file table before a second thread can race to make them.
