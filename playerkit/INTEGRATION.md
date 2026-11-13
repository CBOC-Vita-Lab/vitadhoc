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
          $(PK)/src/pk_menu.cpp $(PK)/src/pk_pace.cpp $(PK)/src/pk_handoff.cpp $(PK)/vita/pk_gfx_vita.cpp
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
| `play_seconds()` | `g_session.seconds_played()` |
| `on_change(row, value)` | store a picture or speed value in the player's own settings and apply it |
| `on_reset()` | reset the core |
| `on_state_touched(slot)` | `g_session.note_state_touched(slot)` |
| `list_cheats` / `set_cheat` | when the player has cheats; otherwise leave `CAP_CHEATS` out and the row is greyed with a reason |
| `on_controls_changed(rc)` | `rc != W_OK` -> `notices().push(pk::write_result_text(rc), NOTICE_ERROR)` |
| `queue_state_write(job)` / `wait_state_writes()` | run `States::commit(job)` on the writer thread, then `Menu::state_write_done(slot, report)` on the game thread; wait for the writer |
| `add_cheat()` / `delete_cheat(i)` | with `CAP_CHEAT_EDIT`: ask for a code with the keyboard, add it, `Menu::refresh()`; remove one |

`Caps` tells the menu what exists: a missing bit greys the row and shows the
reason, so a feature that is not wired yet (`CAP_WIDE`, `CAP_REWIND`) simply is
not set. Exiting: on `MENU_EXIT` or `MENU_GAME_LIST`, or when the game ends
for any reason:

```cpp
g_states->save_resume(state, n, frame, g_session.seconds_played(), g_req.patch_key);
g_session.set_resume_present(g_states->resume_present());
pk::session_write(root, g_session.finish(pk::EXIT_NORMAL /* or EXIT_GAME_LIST, EXIT_ERROR */));
return_to_vitadhoc();
```

Also write the resume point about once a minute while playing
(`g_states->resume_due(now_ms)` then `resume_mark(now_ms)`), and on suspend
(`EXIT_SUSPEND`), so a battery that runs out loses at most a minute.

## 2. SNES player: what `snes/vita/main.cpp` hands over

**Done (Nov 2026):** `snes/vita/player.cpp` (portable, shared with the Linux twin `snes/twin/player_twin`)
does all of the table below; `snes/vita/main.cpp` is the console around it. See `snes/README.md`.

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

## 7. Frame hand-over (`pk_handoff`)

What it is: the emulating thread builds a picture and passes it through a small mailbox to a drawing thread that filters, composes, draws and presents it. Emulation and sound stay at true speed; a picture or two "in flight" soak up wobble when the drawing work sits near one refresh. The SNES player already uses it (`snes/vita/pipe.*`, `hd_*` in `snes/vita/main.cpp`, row "Smooth hand-over", default on). This section is the exact recipe for the main app (`gpsp/vita/*`), which has not been changed. Every number quoted in `make -C playerkit model` is a model number; nothing here has run on a console.

### 7.1 Build and links

- Add `pk_handoff.cpp` to the app's playerkit sources (section 0). The class is C++; the gpsp side is C, so add a small C++ file (`handoff_glue.cpp`, with `extern "C"` functions) holding the `Handoff`, a `HandoffSys` backed by sceKernel semaphores (`sceKernelCreateSema`, `WaitSema` with a timeout in microseconds, `SignalSema`, `DeleteSema`; copy `VitaHandoffSys` from `snes/vita/main.cpp`) and the draw thread. No `std::mutex`, no `thread_local`, no pthread TLS (`pk::SpinLock` is used inside).
- Give the draw thread its own core and a priority just below the emulating thread's; the SNES player uses the core the audio thread does not use. Pick on the console and log it.

### 7.2 Who owns what

The draw thread becomes the only thread that calls `vid_*`, `vb_*`, `osd_draw`, `vid_swap`, the upscalers and any GXM call. The emulating thread keeps the core, sound, pad, menus' logic and writes only into `HandoffFrame` buffers. Everything else that touches the picture must either `flush()` first or run on the draw thread (7.6).

### 7.3 Calls, in game loop order (`main_vita.c`)

Per `game_loop` pass, with the existing code around them:

1. `pace_frame()`: keep the absolute vcount wait. Replace the drain wait (the `vid_present_state` mask loop, at most two vblank waits, `g_sw.drain`) with nothing when hand-over is active: the mailbox gate is the back-pressure now. Keep the drain code for hand-over off, link sessions and `link_bench`.
2. Read the pad once (`pspcompat_pad`), then, for every pad read (see 7.7), `PadLatch::feed`.
3. `ho_set_pace(steady_half())` then `ho_set_force_drop(g_ff || rewinding)`: `Handoff::set_pace_mode`, `set_force_drop`.
4. Poll finished costs: `while (h.poll_cost(&c)) steady_add(c);` and stop feeding `steady_add` with `plat_time_us() - t_drawn` while hand-over is active. The cost is the larger of the emulating work and the drawing work, which is what the model fed the pacer.
5. Run the owed frames (`g_catchup`, `CATCHUP_PER_FRAME`, `HORIZON`) exactly as now: owed frames run undrawn and are never staged. After them, `h.note_emulated(n)` with the count of ALL frames run in the pass (drawn or not).
6. `sink_video` of the frame to show: replace the body by `HandoffFrame *f = h.begin_write(); if (!f) { old inline path } else { copy g_show.pix (or the sgb frame) into f->data; set w, h, pitch, seq, emu_us, user (sgb/border flags); h.commit(f); }`. `begin_write` is the only place the emulating thread waits (full mode, in-flight at depth). In the steady half path (`runs = 2`) stage only the drawn one, which is the second.
7. `frame_present` (`osd_draw` + `vid_swap`) moves into the draw thread loop: `f = h.take(); draw; osd; swap; h.release(f, draw_us)`. `draw_us` is the draw thread's own work including the wait in `vid_swap`. The OSD text and toasts live in values the emulating thread publishes under a `pk::SpinLock` (copy, then draw; see `hd_publish_notices`).
8. Once a second beside `vita_win`: `if (h.census_line(now_us, buf, sizeof buf)) fe_evt("%s", buf);`. `now_us` must come from the same clock as `HandoffSys::now_us`.

What to remove or guard: the `drain` wait and its auto-off counter (when on), the `plat_time_us() - t_drawn` cost feed (when on), any "present skipped because the swap queue is full" path (the draw thread blocks in `vid_swap`; the emulating thread does not).

### 7.4 What stays

`render_thread` (GBA line renderer on another core) and the upscale threads stay as they are, but they are driven from the draw thread when they belong to the filter stage (xBRZ, compose) and from the emulating thread when they belong to the core. Do not call `rt_frame_end` from the draw thread: the picture is complete before `commit`. `g_blit_suppress` is checked before `begin_write`, so a suppressed picture is not staged.

### 7.5 Default depth and adaptation

`HandoffConfig` defaults are the model's choice: depth 2 in flight at most, 1 at least, adaptive (window 24 pictures, shrink at 100% of the refresh held 1 s, restore at 88% held 1 s and 4 s after the shrink, cool-down doubling to 32 s on a bounce). Do not tune them without rerunning `make -C playerkit model ARGS=...`: the table shows depth 2 gives the frame rate depth 3 and 4 give, with less delay (about 13 ms for every extra picture). The governor never changes the speed of the game, so a wrong cost estimate costs delay at worst.

### 7.6 Flush, discard and off

| situation | call |
| --- | --- |
| menu opens, pause, "go to game list", exit | `flush()` before the menu draws; at exit `shutdown()`, join the thread |
| save-state thumbnail, `clip_frame`, screenshot, `vid_readback` | `flush()`, then read `last_shown()` (it stays valid until the next release) |
| state load, rewind step, reset | `discard()` |
| menu closed or game resumed | `time_baseline()` so stopped time is not counted as lost time |
| fast-forward, rewind held | `set_force_drop(true)` (nothing waits, newest wins) |
| link session, local play, `link_bench`, anything lockstepped | `set_enabled(false)` (flushes, then `begin_write` returns NULL and the old inline path runs) |
| the Settings row switched off | the same as link |

### 7.7 Pad latching

Add one `pk::PadLatch` per player port. The pad is sampled by the emulating thread at least once per pass; if a pass is long, also feed it from a faster poll. At each EMULATED frame call `take()` to get the sample that frame sees, so a tap shorter than the gap between reads is kept (rising edges are ORed in until taken). The test in `tests/test_handoff.cpp` shows the loss without it. The SNES player does not use it yet.

### 7.8 The settings row and config

Gameplay page: "Smooth hand-over", on by default, help line "Draws each picture on its own thread, so heavy picture options do not slow the game down. Turn it off if the picture ever misbehaves." Config key `smooth_handoff` (default 1), and a per-launch override of the same name. Log it with the title line (`handoff=%d`).

### 7.9 Tests to add

- A twin proof like `snes/twin/handoff_proof.cpp`: the same game and pad script run plain and through the mailbox with a slow consumer; every delivered picture must be bit-identical to the plain run's picture for the same frame number, in order, including a flush and a disable/enable in the middle.
- A census gate: the app's selftest must fail when hand-over is default on and the log has no `handoff:` line with `shown` above zero (lessons D: a feature that logs nothing has not been proven to run).
- A weak-symbol check: `nm` of the linked binary shows no ` w ` entries for the `pk::Handoff` or `pk::HandoffSys` methods (the console linker accepts unresolved weak symbols silently).
- `make -C playerkit test` (unit, ThreadSanitizer and model-agreement tests) stays green.

## 8. Look governor (`pk_look`)

Purpose: every game starts with the best look the console can show; the governor takes a sacrifice only when THIS game, in THIS scene, cannot hold 60, takes the least visible one first, and gives it back when there is room. It never lowers a picture "to be safe". Everything below is a model checked on the host (`tests/test_look.cpp`); nothing has run on a real console yet.

### 8.1 The ladder (a table, once per console)

`LookRung{name, row, est_saving, visibility}`, least visible first (visibility must not decrease down the ladder; `set_ladder` rejects a ladder that does). `row` is the settings row id the rung belongs to. At most `LOOK_MAX_RUNGS` (12). The per-console ladders are in `docs/DEFAULTS-1.4.md`.

### 8.2 Calls (main app and SNES player alike)

1. At start: `set_ladder(...)`, then for every row the player has set by hand `set_user_row(row, true)` (a row on Auto or at its default is not by hand), then `load_memory(text, n, cpu_mhz)` with the game's `.look` file if there is one (a rejected file is not an error), then `start()`. The look to show is `applied(i)` / `steps_on_row(row)`; poll `generation()` and re-apply the rows when it changed.
2. Once per second of play (not while a menu, pause or load is up; call `reset_window()` when one stops the clock): fill `LookSecond` and call `second()`:
   - `emu`, `draw`, `filter`: the average time per emulated frame of the emulating thread, of drawing/presenting without the filter, and of the filter's critical path, as fractions of the frame budget (1.0 = 16.7 ms at 60). Take them from the counters the pacer already has (`pace_steady.h` / `pk::Pacer` drawn-cost sums); do not add timers to the game thread.
   - `refreshes`, `missed`: refreshes in that second, and refreshes that repeated a picture because the new one was NOT ready (the hand-over mailbox's `late` counter; planned half-mode repeats do not count).
   - `parallel`: hand-over active (load = the larger of emu and draw+filter, else their sum).
   - `pace_half`: `pacer.mode() == PACE_HALF`.
3. Safe points: `safe_point(LOOK_SAFE_START|SCENE|BLANK|MENU)` where a change cannot be seen as a flicker: the first frame, a scene change the core reports (the main app: a game's blank/fade frames; SNES: the screen-off or forced blank bit), the frame after the menu closes. A step down with no safe point is applied after `force_s` (12 s); a step up never is. A frontend that can cross-fade sets `crossfade`.
4. The menu: a row shows `Auto` plus, while `eased(row)`, the look in force (`look_auto_label(current, eased)`); choosing a value calls `set_user_row(row, true)` and the governor never touches that row again; choosing Auto calls `set_user_row(row, false)`.
5. Log: after each `second()` write `census_line()` if it returns true (a `look:` line; the selftest gates on it like the `handoff:` line, see section 7), and `summary()` at the end of the game with the `events()` reasons.
6. Memory: at the end of the game and when the menu opens (writer thread, safe write, see `FORMATS.md` "Look memory"): `memory_action()` is `LOOK_MEM_WRITE` (write `serialize(mhz)`), `LOOK_MEM_DELETE` (delete the file) or `LOOK_MEM_NONE`.

### 8.3 Coupling that must hold

- `LookConfig::pace_recover` (0.80) is the load above which a second counts as bad while `pk_pace` is in half mode. It must equal `PaceConfig::recover_pct / 100`: pacing only leaves half mode when every frame is under that share, so a governor that parks the look at a higher load leaves the game in half mode for good (found in the model, fixed by `down_target` = `up_load` = 0.80).
- The governor only replaces the disabled xBRZ governor of the main app where it keeps a locked 60: a game that holds 60 at the full look is never touched.
- An "Auto" row that the governor eased shows what it did; the same wording on every console.

### 8.4 Main app (`gpsp/vita/`, owned by the main-app work)

Wire in `main_vita.c`'s per-second block (where the `handoff:` census is made): fill `LookSecond` from the pacer and hand-over counters; rows `ui_psp.c` Smoothing (xBRZ factor), Frame hand-over depth and, last, Smoothing on/off; re-apply in `video_vita.c`/`upscale_vita.c` when `generation()` changes, at the safe point only (the xBRZ factor change reallocates the target: not mid-frame). Memory path `<root>/look/<console>/<xx>/<name>.look`.

### 8.5 SNES player (`snes/vita/player.cpp`)

Not wired in 1.4: it needs the per-second cost sums from `main.cpp`'s drawing code and a row for hires/interlace blend on the Picture page; that is more than the small change allowed there, and the SNES ladder is short (blend off, then wide view off) with the player already steady-paced. The recipe is the same as 8.2 with the ladder in `docs/DEFAULTS-1.4.md`. Snes9x is not GPL compatible: the SNES ladder has no xBRZ rung.

### 8.6 Tests

`make -C playerkit test` runs `tests/test_look.cpp` (20 cases: ladder order, protected rows, multi-rung drop, cost learning, peak memory, cool-down doubling, no flapping at a steady load, the real `pk::Pacer` leaving half mode, memory round trip and rejection of damaged/other-ladder files). An adopter adds: a twin proof that a script of loads gives the same `mask()` sequence, and the census gate.

## 9. Fourth core (`pk_cores`)

What it is: the Vita has four CPU cores; a user app gets three (affinity masks `0x10000`, `0x20000`, `0x40000`). The fourth, the "system" core (`0x80000`), is usable by an app only when the owner runs a kernel plugin such as CapUnlocker. `pk_cores` probes it once, plans where every thread goes with and without it, watches the threads on it, and falls back to three cores without a hitch. Without the plugin nothing changes: the three-core layout is exactly today's (frozen by a golden test). Everything below is checked on the host with a fake kernel (`tests/test_cores.cpp`, 33 cases, plus `make -C playerkit tsan`); NOTHING has run on a console. The user-facing text is `docs/FOURTH-CORE.md`.

Kernel behaviour this design ASSUMES (measured by our other ports, not proven here): (a) with the plugin, a thread pinned to `0x80000` gets about all of a core; without it `sceKernelCreateThread`/`ChangeThreadCpuAffinityMask` fails, or the thread gets almost no CPU; (b) the system may take the core back at any time (the thread then starves, it is not killed); (c) `sceKernelChangeThreadCpuAffinityMask(thid, mask)` on a starved thread works and puts it on a user core; (d) priorities 0x40..0xBF are all valid for user threads, the "default" `0x10000100` is 160 = 0xA0; (e) a mask of 0 means the user cores only, never the system core.

### 9.1 Build

Add `pk_cores.cpp` to the player's playerkit sources (section 0; the SNES Makefile already has it). It is plain C++11 with `std::atomic`; no mutex, no `thread_local`, no pthread (checked with `nm` on the Vita object). The gpsp side is C: add a C++ glue file next to `handoff_glue.cpp` with `extern "C"` wrappers (`core4_init(mode)`, `core4_plan_mask(role, idx)`, `core4_prio(role, idx)`, `core4_beat(slot)`, `core4_busy(slot, b)`, `core4_poll(now)`, `core4_census(buf, n)`, `core4_status()`).

### 9.2 Calls

```cpp
pk::Cores cores(pk::vita_core_sys(), mode /* CORE4_AUTO | CORE4_OFF from vita.ini core4 */);
cores.set_log(fn, ctx);                 // lines "cores: probe N of 6: share X % ..." go to the log
cores.probe();                          // ONCE, before any worker thread exists; 10 ms with the plugin, up to 300 ms without
pk::Request rq(pk::CP_APP);             // or CP_SNES / CP_NES; rq.with(role)... for the roles the player really has
rq.upscale_n = 3;
pk::Plan plan = cores.plan(rq);         // 3-core layout unless the core is proven and not lost
const pk::Slot *s = plan.find(pk::R_RENDER);   // s->mask, s->prio, s->kprio, s->on_core3
```

- Create every thread with `s->mask` and `s->kprio ? s->kprio : s->prio` (kprio is the exact value today's call site passes; 0 means "do not change"). Mask 0 means "leave to the system" (`SCE_KERNEL_CPU_MASK_USER_ALL` for an apply).
- For each thread the plan puts on core 3, register a watchdog slot: `int w = cores.watchdog().add("name")`, `watch(w, slot->on_core3)`, `set_busy(w, true)` while it owes progress (false while it legitimately waits on a semaphore), `beat(w)` once per unit of work. The emulating thread calls `cores.watchdog().ref_beat()` and `cores.poll_watchdog(now_us)` once per frame (about 17 ms; stall limit 150 ms, the reference must have moved 3 beats, so a paused game or an open menu never trips it).
- When `poll_watchdog` or `mark_lost` returns true: `plan = cores.plan(rq)` (now three-core), and for every thread whose mask changed call `pk::apply_slot(thid, *newslot)` (`sceKernelChangeThreadCpuAffinityMask` + `ChangeThreadPriority`; for a thread whose priority comes from an ini file call the affinity function only). Log `census_line`.
- After a resume from suspend, at a quiet point: `if (cores.on_resume()) { replan; apply; }` (hysteresis: a usable core stays usable on a share of 40 % or more). When lost: at a quiet point (menu open, pause) `if (cores.try_recover(now_us)) { replan; apply; }`; it waits 30 s after the loss and gives up after 2 recoveries a session.
- Never call `probe()`, `reprobe()`, `on_resume()` or `try_recover()` from inside a frame: they block 10 to 300 ms.

### 9.3 SNES player (done: `snes/vita/main.cpp`)

`core4_start()` runs before the writer, audio and draw threads exist (`threads.ini`: `core4=auto` default, `core4=off`). Only the audio thread moves: `audio_init` creates it with the plan's mask (core 2 exactly as before without the plugin, core 3 with it), so the sound-chip thread has core 2 alone and the PPU thread core 1 (the draw and writer threads still share core 1 as today). With the extra core the audio thread has a watchdog slot (beat per audio grain); the emulation loop does `ref_beat` + `poll_watchdog` once per pass and `core4_fallback()` puts the thread back on core 2. Log lines: `cores: probe N of 6 ...`, the census line at start and at exit, `audio thread on the extra core (core 3)` / `audio thread back on core 2 rc=...`. Not wired (a recipe, not done, because they are more than a small change): the draw thread to core 3 (`hd_start`: `plan.find(R_DRAW)`; the plan puts it on core 3 with the audio thread when asked), the writer (stays on core 1), the wide composer (`R_WIDE_COMPOSE`, core 3 once the compose is split off), and the re-probe on resume (the watchdog covers a core lost over a suspend; in `power_cb` set a flag on `SCE_POWER_CB_RESUME`, and at the top of the next frame outside the menu call `on_resume()`).

### 9.4 Main app (`gpsp/vita/`, owned by the main-app work): the exact recipe

Settings: `vita.ini` key `core4 = auto|off` in the test-switch block (`g_sw`, next to `main_cpu`): `g_sw.core4 = fe_ini_get_str(ini, "core4", "auto")` starts `auto`; `off` skips the probe and leaves every thread as today. Default `auto` is safe: the layout changes only when the probe proves the core.

1. `plat_vita.c`, one new function `plat_cores_init(int off)` called from `main_vita.c` right after the switches are parsed and before the first thread is created (just above `plat_thread_watch("main", 0)` where `main_cpu` is pinned). It builds the request `CP_APP`: `R_EMU_MAIN`, `R_RENDER`, `R_UPSCALE` (n = 3), `R_AUDIO_OUT`, `R_WRITER`, `R_NET_IO` (+ `R_DRAW` once the hand-over of section 7 is in), calls `probe()`, stores the plan, and logs `census_line` with `fe_evt`.
2. `plat_vita.c` thread creation points. The audio thread (`plat_audio_thread_start`, mask 0, `PRIO_AUDIO` 0x50), the io worker (`PRIO_WORKER` 0xB0), the bg and net threads (`PRIO_BG` 0xB8) are planned as floating in both layouts: NO change to their calls (they never get the system core; mask 0 is user cores only). `plat_thread_pin(core)` stays for `main_cpu` (core 0 in the plan; an explicit `main_cpu` in vita.ini still wins).
3. `render_thread.c` `rt_start()`: replace the mask `SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2` by `core4_plan_mask(R_RENDER, 0)` (same value without the core; `SCE_KERNEL_CPU_MASK_SYSTEM` with it), priority `0x60` unchanged. The render thread owns required work (the emulator waits for it in `rt_wait()`), so give it a watchdog slot (`busy` from `rt_post` until the job is done, `beat` at the end of each job) AND bound the wait: `rt_wait()` becomes `sceKernelWaitSema(g_done, 1, &timeout_50ms)`; on timeout call `core4_mark_lost("render thread stalled", now)`, apply the new plan (the render thread is the only one that has to move at once: `apply_slot(g_th, newslot)`), then wait again without a timeout.
4. `upscale_vita.c` `pool_start()`: `#define UP_HELPERS 3`; create helper `i` with the plan's mask for `R_UPSCALE, i` (cores 1 and 2 exactly as now; helper 2 only when the plan has it, so without the plugin `g_pool` is 2 and nothing changes) and `0x10000100` as now. Helper 2 is a pure work-stealer (`claim_and_run`/`upscale_help` already make a late or absent helper cost only speed). One real hazard: a helper that has claimed a stripe and then starves on a lost core keeps `S->inside` above 0, and `slot_wait()` (unbounded `nap()` loop) would never return; a stripe cannot be re-run (it consumes `dirty` and `pend`). So `slot_wait` gets a clock: after 10 ms of waiting with `inside` still above 0, call `core4_mark_lost("upscale helper stalled", now)` and `apply_slot(g_th[2], ...)` (moves the starved thread to a user core, where it finishes its stripe). Give helper 2 a watchdog slot (`busy` between waking and `work_all` returning, `beat` per stripe).
5. Hand-over draw thread (section 7): create it with the plan's `R_DRAW` mask (core 2, priority 0x68 without the extra core: a new thread, so no old behaviour to keep; the plan keeps it where the upscale helper 1 is, lower priority, as the rules require).
6. Per frame in `main_vita.c` (beside the `handoff:` census block): `core4_ref_beat(); if (core4_poll(now)) core4_reapply();`. Once a second with the other census lines: nothing; the census line is logged at start, on every change, and at exit.
7. Settings > About (or the menu's system page): one line from `status_text()`: `Extra CPU core: in use`, `Extra CPU core: not available (needs the CapUnlocker plugin)`, `Extra CPU core: off (setting)`, `Extra CPU core: not available (the system took it back)`. No setting row; the choice is the plugin and the ini.
8. NES player (`nes/vita/main.cpp`, same shape as the SNES change): audio thread mask from `R_AUDIO_OUT`, compose and scout threads from `R_WIDE_COMPOSE` / `R_WIDE_SCOUT`, draw from `R_DRAW`, writer from `R_WRITER`. In the 4-core plan the NES keeps compose on core 1 and scout on core 2 (a core each, because the audio thread has left core 2) and puts audio, draw and writer on core 3; the scout and writer are optional work and lowest priority. The compose thread owns required work for the wide picture: if it stalls the wide view must fall back to the plain picture (the NES wide view already does when the compose is late), the watchdog moves it only if it is on core 3.

### 9.5 Hardware test procedure (the owner, once, on a console WITH CapUnlocker and once without)

Log lines to read (`log.txt`, or the app's event log for the main app):

- With the plugin: `cores: probe 1 of 6: share 98 % of a 10 ms spin (usable)`, then `cores: probe 1 of 6, share 98 %, decision usable, layout 4-core, watchdog trips 0, recoveries 0`, then (SNES) `audio thread on the extra core (core 3)`. A first line of `share 3 % ... (too low, retry)` followed by `probe 2 of 6 ... (usable)` is the retry doing its job (note it, it is the case the probe exists for).
- Without the plugin: either `probe 1 of 6: affinity refused (rc ...), final` (decision `unusable (affinity refused)`) or six lines `share N % ... retry` ending in `decision unusable (share too low)` and `layout 3-core`; the SNES audio line is absent and `main thread to core 0` etc. are as before.
- Settings > About shows the matching sentence.

A/B: set `core4=off` (`threads.ini` for the SNES player, `vita.ini` for the app) and play the same scene; compare `A t=` audio lines (`ur=` underruns, `agap=`, `stall=`) and the per-second frame lines. A good result with the plugin: `ur=0/0`, `agap` flat, frame times at or below the off run in a scene where the sound-chip thread was busy (SNES: Super Mario RPG, Star Fox with the Super FX; app: xBRZ 4x with wide view).

Loss test: with the plugin and a game running, make the system take the core back if the plugin allows it (some builds have a toggle; otherwise put the console to sleep and wake it with the plugin disabled, if the plugin can be unloaded). Expected: within about 0.2 s `cores: watchdog: 'snes_audio' made no progress; core 3 marked lost, back to three cores (trip 1)`, then `audio thread back on core 2 rc=00000000`; one audio glitch at most, no freeze, no crash. If the glitch is longer than half a second report `agap` and the frames around the line.

What each line proves and what it cannot: a `usable` verdict proves a thread pinned there ran at about full speed for 10 ms at start; it does not prove the plugin gives the core for the whole session (that is what the watchdog is for), nor that the clock (444 vs 500 MHz, PSVshellPlus or an overclock plugin) is the same on that core.

### 9.6 Tests

`make -C playerkit test` (33 `cores_*` cases): the probe against a fake kernel (100 %; 3 then 97; 3 x6; affinity refused; flapping 50; 59.9 versus 60 %; off; configurable tries), the spin measure against a fake clock, resume with and without hysteresis, the planner (golden three-core tables for the SNES player, the main app and the NES; four-core tables; every role combination of every profile checked for: priorities in 0x40..0xBF, core 0 alone for the emulating thread, no two whole-core roles on a core, audio never with a spinner, optional helpers at the bottom, no system core in a 3-core plan, only the listed roles move and never change priority), the watchdog (quiet while alive, trip timing, no trip while the game is paused, idle versus dead, per-slot limits, recovery, recovery cap, outside evidence). `make -C playerkit tsan` runs the watchdog with real threads under ThreadSanitizer.
