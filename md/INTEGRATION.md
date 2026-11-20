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

## Options (`md_host_set_option(name, value)`)

The full list, ranges and defaults are in the comment above `md_host_set_option` in `host.h`. Highlights: `fm` 0 = discrete YM2612
(default; 1 ASIC, 2 enhanced, 3 and 4 Nuked, never a default), `interlace` (default 1: double height), `sprite_limit` (default 1:
the hardware's limits), `sms_border`, `gg_extra`, `overscan`, `lowpass`, `vdp_mode`. Defaults are RetroArch's. Options that change
what a game can see belong in the compat word of a two player session.
