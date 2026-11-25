# Using md_host in a player (md.bin)

`md/frontend/host.h` is the whole interface. `md/core` and `md/frontend/host.c` are built as ONE relocatable object, `mdcore.o`
(`make -C md/core vita`), whose only exported symbols are `md_host_*` (`make -C md/core check` fails on anything else). The
player links that object and nothing else of the core. `md.bin` must not link the GBAdhoc-derived GPL files
(`gpsp/netdrv/netdrv.c`, `fe_gblink.c`, `fe_evt.c`, `fe_util.c`): the core's licence is non-commercial, not the GPL.

## Frame loop

```c
md_host_init();
md_host_set_seed(seed);                       /* any value; the same seed makes a reset reproducible */
md_host_set_region(MD_REGION_AUTO);           /* AUTO reads the header's region field at 0x1F0; USA / EUROPE / JAPAN_NTSC / JAPAN_PAL force it */
if (md_host_load_mem(rom, n, "game.md", MD_SYS_AUTO, err, sizeof err)) { /* show err */ }
md_host_sram_write(saved, saved_n);           /* right after load, if a .srm exists */
for (;;) {
    md_host_set_pad(0, mask0); md_host_set_pad(1, mask1);
    md_host_run_frame();                      /* one frame: 1/59.9227 s NTSC, 1/49.7014 s PAL (true 50 Hz timing) */
    int w, h, pitch; const uint16_t *px = md_host_video(&w, &h, &pitch);   /* RGB565 */
    md_video_info vi; md_host_video_info(&vi);
    int16_t snd[2400 * 2]; int n = md_host_audio_take(snd, 2400);          /* 48 kHz stereo, about 800 / 960 frames per video frame */
}
```

- `md_host_fps()` gives the exact frame rate (59.9227 or 49.7014); pace video and audio on it. The audio is produced at exactly
  48000 Hz by the core's own resampler, `md_host_audio_take` drains an internal one second ring.
- `md_host_set_framebuffer(buf, pitch_px)` lets the core draw into your own RGB565 buffer instead (it is stable between frames).

## Pad

`md_host_set_pad(port, mask)`, bits `MD_UP .. MD_MODE`. Owner mapping: A = Square, B = Cross, C = Circle, X = L, Y = Triangle,
Z = R, Start = Start, Mode = Select. The in-game menu stays Start + Select held, so the player takes both away from the game while
the menu is held. Pad type: `md_host_set_pad_type(port, MD_PAD_AUTO / MD_PAD_3 / MD_PAD_6)`; AUTO uses a 6 button pad when the
header's peripheral field has the "6". On Master System / Game Gear / SG-1000 button 1 is `MD_B` (Cross) and button 2 is `MD_C`
(Circle); Start is Start (Game Gear Start, Master System Pause).

## Picture for the upscalers

`md_video_info` is valid after every `md_host_run_frame` and says what the player needs to choose a scaler and a ceiling:

| field | meaning |
|---|---|
| `w`, `h` | size of the picture returned (interlaced Mega Drive frames are double height, option `interlace`) |
| `native_w`, `native_h` | the console's own resolution this frame before doubling: 320 or 256 wide, 224 / 240 / 192 / 144 tall |
| `system`, `h40`, `interlace`, `pal` | which console, 320 wide, 0 / 1 / 2 (interlace mode 1 / 2), 50 Hz |
| `left_mask` | Master System: the left 8 pixel column is blanked by the VDP; `crop_left` / `crop_right` say how many pixels the core cut (option `sms_border`) |
| `gg_window` | Game Gear: the 160 x 144 window of the 256 x 192 VDP picture |
| `dar`, `par` | display aspect and pixel aspect for this frame |
| `fps` | exact frame rate |
| `serial` | increases whenever the geometry changes (H32 / H40 switch in game, interlace, left blank): rebuild the scaler on a change |

## Saves, states, achievements, cheats

- Save RAM: `md_host_sram_size/read/write/dirty/mark_saved`. The `.srm` is the raw RAM bytes (RetroArch compatible).
- Save state: `md_host_state_size` (about 145 KB for the Mega Drive), `md_host_state_save_mem`, `md_host_state_load_mem`. A state is
  tied to the same build; a damaged one is refused. Rewind keeps states in memory the same way.
- RetroAchievements: `md_host_ra_console()` (1 Mega Drive, 11 Master System, 15 Game Gear, 33 SG-1000) and `md_host_ra_read(addr, out, n)`
  with the rcheevos memory map (Mega Drive: 0x0000-0xFFFF the 68000 work RAM in true byte order, 0x10000-0x1FFFF save RAM; Master
  System / Game Gear: 0x0000-0x1FFF work RAM, 0x2000-0x9FFF save RAM; SG-1000: the windows of rcheevos' `consoleinfo.c`).
- Cheats: `md_host_cheat_add("PART+PART")` takes the decoded parts of the cheat database, never raw Game Genie text. The rules are
  `gpsp/vita/tests/cheats/SEGA-CODES.md`, summarised at the cheat block of `host.h`; at most 150 per game;
  `md_host_cheat_reset` undoes the ROM patches.

## Wide view (picture beside the picture)

A player that shows the console's picture as wide as its screen asks the core for more than the console drew; the emulation does not change (the
picture in the original window, the sound, the state and the RAM are bit for bit the same with it on or off, with the VDP worker thread too).

- `md_host_set_framebuffer(canvas + MD_WIDE_MAX, MD_WIDE_PITCH)`: a buffer with `MD_WIDE_PITCH` (512) pixels a row, the picture at column `MD_WIDE_MAX` (96), room for strips on both sides.
- `md_host_set_wide_margin(m)`: Mega Drive: the VDP draws `m` (0..96) more pixels on each side of every line it can (mode 5, not interlaced, display on): the planes and
  the sprites beyond the edge, at columns -m..-1 and w..w+m-1 of the row. On the side of a line the window plane takes, the strip is drawn without plane A (plane B and
  the sprites go on; the window plane never does) and only where the window's two cells at that edge are empty. With per 2-cell vertical scroll a side gets its strip
  only where the three columns next to that edge scroll alike; the strip scrolls like the edge column. `md_wide_line.side` says per line which strips were drawn and how.
  The 8 bit systems: any `m > 0` keeps the per line records the analysis needs (their tile map is exactly the screen wide, so there is nothing to draw beside the picture).
- `md_host_set_wide_planes(mask)`: bit 0 plane A, bit 1 plane B (default 3): a plane left out is not drawn into the strips (it is transparent there); `md_wide_frame.opq` says which strip pixels are opaque.
- `md_host_wide_run(fn, user)`: after `md_host_run_frame()`, on the emulating thread: `fn(user, &frame)` gets what the analysis needs (per line what the VDP scrolled by,
  where its name tables are and which strips it drew (`line[]`), the opaque masks, the VRAM, VSRAM, CRAM, the registers, the 8 bit palette of line 0, and the list of the
  32 byte VRAM blocks written since the last call). With the VDP worker running `fn` runs on the worker thread and the call returns at once: the emulating thread does
  not pay for the analysis; nothing of the next frame is drawn before `fn` has returned; `md_host_wide_join()` (and the end of the next frame) wait for it. Without the
  worker `fn` runs before the call returns. What `fn` sees is the same either way.
- `md_host_wide_row(plane, row, addr, words, n)` / `md_host_wide_rows_clear()` (inside `fn` only): learned rows. The strips of the next frames are drawn from `words` instead
  of the VRAM row at `addr` for as long as the game leaves that VRAM row as it is; a line drawn from one says so (`MDS_OVR_A/B`). This is how cells the player has seen
  before come back beside the screen through the VDP's own renderer (priorities, the other plane, sprites and shadow / highlight as on the screen).
- `md_host_wide_cache(on, check)`, `md_host_wide_cache_stat(out)`: the worker renderer copies a line's strips from the last frame while nothing they are drawn from has
  changed (on with the worker); `check` draws anyway and counts differences (0 on every test ROM: `selftest.sh` 11b).
- Option `gg_band` (Game Gear): the picture is 256 x 144 (square pixels, the LCD window in columns 48..207) instead of the 160 x 144 window; the same machine.
- `md/frontend/wide.h` (`mdw::Wide`) is the adapter over widekit: `frame()` on the emulating thread after the frame hands it to the analysis (`md_host_wide_run`),
  `compose()` (or `compose_begin` / `compose_lines` / `compose_end` with the lines shared between threads) on the drawing thread, which waits for the analysis if it has
  not finished; md/vita/main.cpp and md/twin/wide_pipe_proof.cpp show the hand-over. `analyse()` is `frame()` + wait for callers with one thread.

## Options (`md_host_set_option(name, value)`)

The full list, ranges and defaults are in the comment above `md_host_set_option` in `host.h`. Highlights: `fm` 0 = discrete YM2612
(default; 1 ASIC, 2 enhanced, 3 and 4 Nuked, never a default), `interlace` (default 1: double height), `sprite_limit` (default 1:
the hardware's limits), `sms_border`, `gg_extra`, `overscan`, `lowpass`, `vdp_mode`. Defaults are RetroArch's. Options that change
what a game can see belong in the compat word of a two player session.

## Worker threads

The core has no thread code. Before `md_host_load_mem` a player may call `md_host_set_threads(&ops, MD_THREAD_VDP)` (add
`MD_THREAD_VDP_SMS` to use the worker on the 8 bit systems too) with a `md_thread_ops` (`core/thread_ops.h`): counting semaphores,
`spawn(id, fn, arg)` / `wait_end(id)`, `yield`, and an optional `now_us` for the census. The worker is bit-identical to the single
thread path; `md_host_vdp_census()` returns its counters and `md_host_vdp_debug()` turns on the per frame raw-state check and a join
after every line (twin only). `md/vita/main.cpp` shows the sceKernel version, `md/twin/main.cpp` the pthread one.

`MD_THREAD_FM` (worker id `MD_WORKER_FM`) puts the YM2612's FM synthesis on its own thread (`core/fm_thread.h`): the 68000 and the Z80 side
records the chip's register writes, resets and "run n samples" calls in a log and the worker feeds them to the unchanged chip; the status register's
timer flags, the BUSY flag and the FM clock stay on the emulating thread, which keeps a small copy of the chip's timers (`YM2612Shadow*`). The mixer
joins at the end of every frame; the wait is bounded (`FMW_BOUND_US`), after which the emulating thread finishes the queue itself with the same code,
so the sound never drops and never differs. Sound, state and picture are bit-identical with it on and off. It does not start for the Nuked
YM2612 / YM3438 options or for the YM2413 of the 8 bit systems (they run inline). `md_host_fm_census()` returns its counters, `md_host_fm_debug()`
turns on the timer check and the fallback / queue-full paths (twin only; `md_twin --fm-thread | --fm-check | --fm-stress | --fm-inline | --fm-tiny`,
and `MD_TWIN_FM=1|check|stress|inline|tiny` for player_twin and link_twin).
