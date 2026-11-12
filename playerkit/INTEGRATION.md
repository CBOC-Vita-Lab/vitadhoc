# Adopting playerkit

playerkit gives the SNES and NES players, and the main app, one shared
behaviour: the same in-game menu, the same ten state slots with pictures, the
same remapping, one launch protocol. This file lists the exact steps. Nothing
here edits `gpsp/`; the main app side is described as the places the app has
to call, in the app's own language and code.

Rules that keep the licences clean (see `LICENSE`):

- `playerkit/` is MIT. It contains nothing from `gpsp/` (GPL) or xBRZ. The GBA
  menu was read for how it looks and behaves; every line here is new.
- The SNES player links Snes9x, which is not GPL compatible, so it may link
  `playerkit/` and nothing from `gpsp/`. The same holds for the NES player.
- playerkit's only dependencies are the C++11 standard library and, on the
  console, vita2d behind `pk::Gfx`.

## 0. Build

Add to the player's Makefile (the files are listed in `playerkit/Makefile`
as `LIB_SRC`, and `make -C playerkit vita` builds `libplayerkit.a` for the Vita):

```make
PK := ../playerkit
PK_SRC := $(PK)/src/pk_util.cpp $(PK)/src/pk_text.cpp $(PK)/src/pk_font_data.cpp \
          $(PK)/src/pk_icons.cpp $(PK)/src/pk_states.cpp $(PK)/src/pk_launch.cpp \
          $(PK)/src/pk_patch.cpp $(PK)/src/pk_input.cpp $(PK)/src/pk_notice.cpp \
          $(PK)/src/pk_menu.cpp $(PK)/vita/pk_gfx_vita.cpp
CXXFLAGS += -I$(PK)/src -I$(PK)/vita
```

`pk_gfx_soft.cpp` is the Linux test renderer; leave it out on the Vita. The
built-in fonts (Inter and Outfit, OFL) are inside `pk_font_data.cpp`, so no
font file has to be packed.

## 1. Common setup in a player (SNES and NES alike)

```cpp
#include "pk_launch.h"
#include "pk_menu.h"
#include "pk_input.h"
#include "pk_states.h"
#include "pk_notice.h"
#include "pk_gfx_vita.h"

static pk::LaunchRequest g_req;       // what the app asked for
static pk::SessionTracker g_session;  // what we tell the app
static pk::Mapper g_mapper;
static pk::Profile g_profile;
static pk::VitaGfx g_gfx;
static pk::Text *g_text;
static pk::Menu g_menu;
static pk::States *g_states;
```

Start-up, in this order:

1. `pk::launch_read(root, "snes", g_req)`. When it fails (an older app, or
   started from a shell), fall back to the argument the player gets today:
   `g_req.game_path = argv[last]`, everything else default. This keeps old and
   new apps working together. If `g_req.newer_version` is set, carry on: unknown
   keys are ignored.
2. `g_session.begin(g_req)`.
3. Apply the picture settings from `g_req` (shape, quality, per-game overrides:
   `g_req.override_int("scanlines", -1)` and so on) before the first frame.
   The player's own `picture.ini` and `games/<name>.ini` stay as fallback when
   there is no launch file.
4. Patch: if `g_req.patch_path` is not empty, read it and the ROM and call
   `pk::patch_apply_files()`. On failure show `result.text()` as an error
   notice, report it in `session.txt`, and start the unpatched game only if
   the user chose that; never start a half-patched ROM.
5. RetroArch save: if `g_req.retroarch_save` is newer than `g_req.save_path`,
   copy it over through `pk::atomic_write` (the old `.srm` is kept as `.old`
   until the new one has been read back).
6. States: `pk::StateLayout l{root, "snes", pk::path_stem(game_path)}`;
   `g_states = new pk::States(l); g_states->ensure_dirs();`
   Then `pk::recover_old()` on the `.srm` as well.
7. Controls: `pk::ProfileStore store(root, "snes", name);`
   `store.load_effective(pk::model_snes(), g_req.enter_is_cross, g_profile);`
   `g_mapper.configure(pk::model_snes(), g_profile, pk::MapperConfig());`
8. Menu: fill a `pk::MenuConfig` (game name, console label, `caps`, accent,
   `enter_is_cross`), implement a `pk::MenuHost` (below), then
   `g_menu.configure(cfg, &host, g_states); g_menu.attach_input(&g_mapper, &store, &pk::model_snes(), &g_profile);`
   and set the picture rows once with `g_menu.set_value(pk::ROW_PIC_SHAPE, ...)` etc.
9. Start mode: `START_CONTINUE` loads `States::load_resume()` once the core is
   running (check `resume_patch(g_req.patch_key) == PATCH_SAME`);
   `START_SLOT` loads `g_req.slot` the same way. A damaged file produces a
   notice (`state_status_text()`), never a crash.

Each frame:

```cpp
pk::PadSample pads[pk::MAX_PLAYERS];  // from sceCtrlPeekBufferPositiveExt2 / touch
pk::InputResult in;
g_mapper.update(pads, now_ms, emulated_frame, &in);
if (in.menu || tapped_picture) { open_menu(); }
for (int i = 0; i < 4; i++) core_set_pad(i, in.pad[i]);
if (in.fired & pk::FIRE_QSAVE) quick_save();   // see step 4 below
...
g_session.tick_running(now_ms);                // only while the game runs
```

Opening the menu: `g_session.pause(); g_mapper.menu_opened(); g_menu.open(in.menu_player);`
Inside it, call `g_menu.update(menu_input)` once per frame, draw the frozen
game picture, then `g_menu.draw(&g_gfx, g_text)`. Handle the returned event:
`MENU_RESUME` (close; call `g_mapper.menu_closed(pads)` so a still-held chord
does not open it again), `MENU_GAME_LIST` and `MENU_EXIT` (leave, see below),
`MENU_RESET_DONE`. Before the `Gfx` is destroyed call `g_menu.release_textures()`.
A tap on the game picture opens the menu as well; the chord to open is
START + SELECT on every console, and the SNES player's old L + R + Start keeps
working (it is on in `MapperConfig::legacy_chord` and in `model_snes()`).

The `MenuHost` the player implements is small:

| callback | what to do |
| --- | --- |
| `capture_state(out)` | the core's save state bytes (`snes_host_state_*` into a buffer) |
| `restore_state(data, n)` | load them back; false if the core refuses |
| `current_frame(&f)` | the last picture as RGB565 (`pk::Frame`), for thumbnails |
| `play_seconds()` | `g_session.current().seconds_played` |
| `on_change(row, value)` | store a picture or speed value in the player's own settings and apply it |
| `on_reset()` | reset the core |
| `on_state_touched(slot)` | `g_session.note_state_touched(slot)` |
| `list_cheats` / `set_cheat` | when the player has cheats; otherwise leave `CAP_CHEATS` out and the row is greyed with a reason |
| `on_controls_changed(rc)` | `rc != W_OK` -> `notices().push(pk::write_result_text(rc), NOTICE_ERROR)` |

`Caps` tells the menu what exists: a missing bit greys the row and shows the
reason, so a feature that is not wired yet (`CAP_WIDE`, `CAP_REWIND`) simply is
not set. Exiting: on `MENU_EXIT` or `MENU_GAME_LIST`, or when the game ends
for any reason:

```cpp
g_states->save_resume(state, n, frame, g_session.current().seconds_played, g_req.patch_key);
g_session.set_resume_present(g_states->resume_present());
pk::session_write(root, g_session.finish(pk::EXIT_NORMAL /* or EXIT_GAME_LIST, EXIT_ERROR */));
return_to_vitadhoc();
```

Also write the resume point about once a minute while playing
(`g_states->resume_due(now_ms)` then `resume_mark(now_ms)`), and on suspend
(`EXIT_SUSPEND`), so a battery that runs out loses at most a minute.

## 2. SNES player: what `snes/vita/main.cpp` hands over

Everything below is replaced, not extended. File and line numbers are from the
current tree.

| today (`snes/vita/main.cpp`) | becomes |
| --- | --- |
| `read_port()` (≈568) maps buttons and a hard-coded stick threshold to `SNES_*` | the player only fills a `pk::PadSample` from `sceCtrlPeekBufferPositiveExt2` (L1/R1 folded into L/R as now; the two analog sticks and the rear panel go into the sample); mapping and sticks are `pk::Mapper::update()` |
| `read_pads()` (≈600) | builds the four `PadSample`s, then `Mapper::update()` once per emulated frame; the `masks[]` are `InputResult::pad[]` (the out bits equal `SNES_*`) |
| the `(L1+R1+Start)` test in the main loop (≈1127) | `InputResult::menu`, plus a tap on the picture |
| `kMenu[]`, `pause_menu()` (≈787, 868), `nav_poll()` (≈795), `menu_frame()` (≈809) | `pk::Menu` (`update`, `draw`) |
| `picture_menu()` (≈816), the `kShape`/`kMulti` tables, `level_text()` | the Picture page; `on_change` writes the value into the existing `pic_settings` |
| `settings_load()` / `settings_save()` (≈756, 765) | stay for `picture.ini`/`games/<game>.ini`; the controls move to `pk::ProfileStore`. Per-game "this game / all games" stays with `ROW_PIC_APPLY` |
| the state slot code (≈889: `DATA_ROOT "/states/%s.st%d"`, three slots by status line) | `pk::States` and the Save states page. States move from `states/<game>.stN` to `states/snes/<game>.stN` (`FORMATS.md`); on first run, if a file is in the old place, rename it (`pk::fs().rename_file`) |
| `snes_host_state_save/load(path)` | wrap into `capture_state` / `restore_state` (they take a path today: write to a temp file in `ux0:temp` and read it, or add buffer versions to `host.h` — the second is cleaner and is the only change outside the player) |
| `srm_write_sync()` / `srm_writer()` (≈633-700) | keep the thread; replace the file write inside it by `pk::atomic_write()`, call `g_session.note_save_written()` on success and `note_save_error()` plus a notice on failure (today a failed write is only logged) |
| `return_to_vitadhoc()` (≈724) | call `pk::session_write()` first (above), then the existing `sceAppMgrLoadExec` |
| `g_confirm_is_cross` (SceAppUtil) | `g_req.enter_is_cross` (the app already knows; the player reads the system value only when there is no launch file) |
| the `--from-snes` / `rom|savepath` argument (≈1021-1030) | still accepted as a fallback; `LaunchRequest::game_path` / `save_path` replace it |

A "Multitap" entry (`ps.multitap`, `multitap_wanted()`) is a SNES-only row.
Keep it as a row of the Controls page by passing it as an extra row through
`set_row_note`/a host-owned choice if wanted; until then it stays in
`picture.ini` and `multitap_wanted()` remains as is.

## 3. NES player: `nes/vita/main.cpp`

Same table, with `model_nes()`. The NES player's line numbers: `read_port`
(≈514), `read_pads` (≈548), `srm_write_sync` (≈606), `settings_load/save`
(≈674, 683), `return_to_vitadhoc` (≈698), `nav_poll` (≈724),
`picture_menu` (≈753), `pause_menu` (≈807), the state slots (≈843).
Differences:

- no legacy chord (`model_nes().legacy_chord` is false); START + SELECT only;
- the NES A button is on the Enter button (`default_phys` / `default_phys_circle`
  in `model_nes()`), the SNES layout is fixed because the console's pad is;
- `CAP_PLAY_TOGETHER` stays unset (the card is greyed with
  `play_together_reason`);
- `console` in `StateLayout` and `launch_read()` is `"nes"`.

## 4. Shortcuts and turbo (both players)

`InputResult::fired` carries `FIRE_QSAVE`, `FIRE_QLOAD`, `FIRE_SHOT` once on
the frame the chord completes, and `FIRE_FF_HELD/PRESS`, `FIRE_REWIND_HELD/PRESS`.
Quick save is slot 1 through the same `do_save` as the menu (so thumbnails and
the session report are right) and shows a one-line notice; quick load asks
nothing and keeps the undo copy. Fast-forward honours the Speed page: factor
(`ROW_SPD_FF`: 2x or 3x), sound on/off, hold or toggle. Turbo is counted in
emulated frames inside the mapper, so it follows fast-forward.

## 5. The main app (`gpsp/vita/main_vita.c`, `gpsp/vita/menu_files.c`)

The app is GPL; playerkit is MIT and may be linked into it (MIT is compatible
with GPL), but there is no need: the app only has to write and read the two
small text files, so the app may also keep its own tiny reader/writer. What it
must do:

1. **Before starting a player** (`snes_launch()`, ≈593, and the call site at
   ≈5341; the NES equivalent beside it): write
   `<root>/launch/<console>.ini` (`pk::launch_write`, or the same keys by hand,
   see `FORMATS.md`): `game` (the unzipped path for a zipped game), `save`
   (replaces the `rom|save` argument), `shape`, `quality`, `cpu_mhz`,
   `enter_button`, `language`, `start` (`continue` for the Continue tile,
   `slot`+`slot` for a state chosen on the shelf), `patch` and `patch_key`,
   `retroarch_save`, `wide_supported`, `ovr.*` for the per-game settings, and a
   fresh `session_id`. Call `pk::session_clear(root)` first so an old report is
   not read. Keep passing the game path as the program argument too, so an old
   player still starts.
2. **When it comes back** (the `--from-snes` start, the same place that returns
   from a GBA game): `pk::session_read(root, rep)`; ignore it unless
   `rep.valid` and `rep.session_id` matches. Then:
   - add `rep.seconds_played` to the game's play time and update **Recent**
     exactly as a GBA session does;
   - if `rep.notice` is not empty show it with the app's notice (`app_notice`),
     as an error when `rep.save_error`;
   - if `rep.save_changed`, refresh the game's save sync status (cloud/backup
     badge) as after a GBA session;
   - `rep.states_touched` says which shelf slots to re-read (or simply rescan
     `states/<console>/` as the shelf does for GBA);
   - `rep.resume_present` turns the Continue tile on or off without touching the
     disk.
   A missing or invalid report (the player crashed or was killed) is not an
   error: use the wall-clock time since the launch as an estimate of play time,
   and show nothing.
3. **Shelf:** nothing special. The player writes the same folders and picture
   formats as the GBA side (`states/snes/<name>.stN`, `.thumb`, `.shot`,
   `.rsm`, `scenes/<name>.shot`), so the code that lists, previews, loads and
   deletes GBA states works for SNES and NES by passing the console folder. One
   addition: when the app deletes a state, also delete the companion
   `<name>.stN.meta`.
4. **Controls and settings:** the per-game and global control profiles are plain
   ini (`controls/<console>.ini`, `controls/<console>/<game>.ini`), so the app
   can offer "Controls" in its settings for the players and reset them by
   deleting the file.

## 5b. Rules from the UX review (sections 3.1 to 3.3), as playerkit does them

- The cursor is remembered when a sub-page is left (`Menu` keeps a cursor stack).
- X on an empty slot saves; Square saves over a filled slot (asks first); Triangle deletes.
- A notice clears itself (2.2 s, errors 5 s), errors have a red dot and never green, and a new one replaces the old.
- Hints are 24 px (`F_BODY`), the secondary text colour is `#8F99B3` or lighter, and the header always shows the buttons with real glyphs for the page.
- Rows and cards that cannot apply are **hidden** (a console without cheats or a link has no such card; unwired Wide view, Rewind, Fast-forward rows are not listed). A row is greyed only when a reason helps: the host calls `set_row_note(row, reason)`.
- Reset game is a card of the main menu; Game list and Exit ask before leaving.
- Words: "starred", "game file", never "ROM" or "favourites"; hosts should pass their own game name without the region tag if they want it stripped.
- Battery RAM: do not write the save before opening the menu. Open the menu, draw it, then call `srm_write_sync()`; the menu never waits for it.

## 6. Testing the adoption

- `make -C playerkit test` (unit tests with ASan and UBSan, plus the scripted
  menu walk) must pass before and after.
- `make -C playerkit shots` renders every page to PNG for a visual check.
- `make -C playerkit vita` proves the library and the vita2d binding compile
  for the console; on the device, check the touch targets and the first
  frame after opening the menu.
