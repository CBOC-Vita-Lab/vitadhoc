# Local changes to the vendored TGB Dual core

The files in this directory are MasterBoy 2.02's `gbcore/`
(PSP-Archive/MasterBoy commit `320bb5e1388df27f65f1044e366d77e7bd3cad4f`)
**plus the changes below**. The pristine import is its own commit
("gbcore: vendor the TGB Dual core from MasterBoy 2.02 (pristine)", 8f84303),
so the whole local diff is one command away:

```
git diff 8f84303 -- gbcore/tgbdual
```

Local changes carry `GBAdhoc:` comments at the affected blocks. `tgb_port.h`,
`tgb_shared.c`, `apu_timing.h`, `state_validate.h`, `README.md` and this file
are new; `cheat.c` is unchanged and not built.

## Import normalisation (no code change)

CRLF line endings became LF (`.gitattributes` requires it for `*.c`/`*.h`);
trailing whitespace, trailing blank lines and space-before-tab indentation
were removed so `tools/lint.sh` accepts the files. Comment bytes stay in
their original encodings (Shift-JIS, some CP1252). `diff -w -B
--strip-trailing-cr` of the pristine commit against upstream is empty.

## Removing MasterBoy (the frontend around the core)

| File | Change |
|---|---|
| `gb.h`, `gb.c`, `cpu.c`, `lcd.c`, `apu.c` | `../psp/pspcommon.h`, `../renderer.h`, `../syscall.h`, `../lib/zlib.h` and `<oslib/oslib.h>` replaced by `tgb_port.h` (via `gb.h`). The port header supplies `MAKE_COLOR`, `pad_state`, the DMG palette and the adapter callbacks (`gbe_*`, `set_gb_type`, `tgb_port_serial`), all implemented in `../gbcore_tgbdual.c`. |
| `gb.h` | `CHEAT_SUPPORT` only with `TGB_CHEATS` (off): it put a 64 KiB `cheat_map` lookup on every emulated write. `inline` dropped from ten prototypes (the definitions keep it; an inline declaration warns in every other file that includes the header). Declarations for the buffer-based state functions; externs of removed arrays deleted. |
| `gb.c` | `gb_save_state`/`gb_restore_state` work on memory only (MasterBoy's `VIRTUAL_FILE` path removed); `gb_save_state(NULL)` measures, `gb_restore_state` is bounds-checked. `vframe_mem` (128 KiB .bss) removed: the adapter allocates `vframe` so GBA sessions do not pay for it. `exiting_lcdc()` (ColorIt palette-file lookup) removed. |
| `cpu.c` | `gblCpuCycles` (MasterBoy's "Z80 clock" percentage) removed. GBC infrared bridge to the PSP's `irda0:` removed (`cpu_io.h`): the port reads "no light". |
| `lcd.c` | MasterBoy's ColorIt colourisation (tile palettes, VRAM CRC, `*_colored` renderers) compiled only with `TGB_COLORIT` (off). The DMG palette comes from `tgb_dmg_palette[]` (BGR555, set by the adapter) instead of OSLib colours from palette files. The SGB path's `oslGetCachedPtr`/`oslUncacheData` pair removed (the frame is ordinary cached memory). |
| `apu.c` | Output rate `snd_rate` (set by the adapter) replaces the literal 44100 in the four generators. `snd_render_orig`'s menu-volume scaling replaced by a fixed gain `TGB_SND_GAIN` with int16 saturation (see *Volume* below). The register-write queue is `SND_QUE_SIZE` (16384) entries allocated by the adapter instead of a 768 KiB .bss array. |
| `rom.c` | `sram_space` (128 KiB .bss) removed; the adapter passes its own cartridge RAM. |
| `sgb.c` | The SGB border image (112 KiB) and its renderer compiled only with `TGB_SGB_BORDER` (off): borders are not presented. Border packets are still parsed, since palettes ride along with them. |

## Portability (host tests and modern GCC)

| File | Change |
|---|---|
| `gb_types.h` | `dword` is `uint32_t`, not `unsigned long` (64-bit on LP64 hosts; the sound phase accumulators rely on 32-bit wrap). |
| `cpu.c` | `GenZF` keeps its MIPS `sltiu/sll` under `__psp__`, with a C fallback elsewhere. |
| `lcd.c` | `horizflip` keeps Allegrex `bitrev`/`wsbw` under `__psp__`, with a C fallback. The PSP asm no longer writes its input operand (`bitrev %1,%1`), which GCC's asm contract forbids; it reads `src` and writes only the output. |
| `cpu_io.h` | `_memcpy4x`/`_memcpy40` copy `dword`s, not `unsigned long`s (on LP64 every OAM DMA copied twice the span). |

## Bugs fixed (present in upstream TGB Dual / MasterBoy)

| File | Bug | Fix |
|---|---|---|
| `cpu.c` | Reads and writes of 0xFEA0-0xFEFF indexed `spare_oam[]` with `adr-0xFFA0`: a negative subscript that wrote before the array. Found by `-Warray-bounds`. | `adr-0xFEA0`. |
| `lcd.c` | `gbc_recreate_colors`, `gb_recreate_colors` and `gb_invalidate_all_colors` walked 2-D arrays as flat ones (`inval_col_pal[0][i]`, `i` up to 63; `m_pal16[0][0][i]`, `i` up to 11). That is undefined, and GCC warns (`-Waggressive-loop-optimizations`) that it may cut the loops to four iterations -- which would leave most CGB palettes and both DMG sprite palettes unconverted. | Flat pointers; `gbc_invalidate_color(i>>2, i&3)`. |
| `sgb.c` | `sgb_reset` cleared 2048 bytes of the 4096-byte `sgb_palette_memory`. | `sizeof`. |
| `rom.c` | A RAM size code above 5 indexed past `tbl_ram`/`ram_size_tbl`. | Treated as 0. |
| `rom.c` | Bank masks come from the header size code, so a file larger than its header claims lost its upper banks. | The code grows to cover the file (the adapter pads the buffer to match). |
| `apu.c` | `freq<<16` overflowed `int` for square frequencies above 32767 Hz. | Unsigned shift. |
| `op_def.h` | Header guard `OP_DEF_` defined `OP_DEF`. | Defines `OP_DEF_`. |
| `apu.c` | The wave channel's `sample * volume << 1` shifts a negative value (undefined; UBSan reports it on every real cartridge). | `* 2`: the same value on every compiler we use, and the single-player trace is bit-identical to 3.0.0 (`gbcore/tests/bitident.c`). |
| `gb.c` | HDMA from WRAM formed `ram-0xC000` from the array itself (`-Warray-bounds`). | Same pointer via `cpu_get_ram()`. |

## Behaviour changes

- **Serial port (the link cable).** MasterBoy had compiled out serial
  completion, so an internal-clock transfer never finished. `cpu_io_write_02`
  now offers SB to the adapter (`tgb_port_serial`) at every transfer start;
  GBAdhoc exchanges one whole byte with the peer over ad-hoc WiFi
  (`netdrv/gb_link.c`, `frontend-common/fe_host.c`). Internal clock: the
  received byte (0xFF with no cable) lands in SB with the serial interrupt
  after eight bit times (`seri_occer`, checked in `cpu_exec`). External
  clock: with a peer the byte lands at once; without one nothing happens, as
  on hardware. The DMG transfer time is corrected from 512 to 4096 clocks.
  New global `seri_rx`.
- **Game Boy Color colours.** `gbc_recreate_colors` (`lcd.c`) converts a
  CGB palette entry through the GBC LCD model (`cgb_lcd_color`: SameBoy's
  measured response curve and green/blue mix, tables generated into
  `_cgb_lcd_table.h` by `tools/gen_cgb_lcd_table.py`) instead of the plain
  5-to-8-bit expansion, unless the adapter's `gbcore_set_color_correction(0)`
  turns it off. The tables and the switch (`tgbshared_cgb_lcd`) are defined
  once in `tgb_shared.c`. DMG and SGB colours are unchanged. Why:
  `docs/GB-PALETTE-FIXES.md`.
- **`gb_run` frame boundary.** Unchanged in the core; the adapter ends a
  frame at VBlank (LY 144) rather than at LY 0, so a presented frame is
  lines 0-143 of one frame.

### The Super Game Boy border on the Vita (2026-10-23)

`sgb.c`: with `TGB_SGB_BORDER` (the Vita's builds, `vita/corebuild.mk`;
the PSP still leaves it out) the border's pixels are stored in the
frontend's RGB565, like the game's (`MAKE_COLOR`; they were the SNES's
BGR555), and `sgb_border_gen` counts every render.  The adapter adds
`gbcore_sgb_border` (the picture, while running as a Super Game Boy with a
border sent) and `gbcore_sgb_border_save`/`_load` (the border's map, tiles
and colours as data, which the frontend keeps for the game's next start --
the state format has never held them), and redraws the border after a state
load, whose palettes it uses.  Test: `vita/hostcore/app_tests.py sgb`
(a cartridge that sends one, `testrom/sgbborder.py`).

### The MBC7 accelerometer (2026-10-23)

`mbc.c`: Kirby Tilt 'n' Tumble's sensor is latched as Pan Docs (MBC7)
describes -- `$55` to `Ax0x` erases the sample to `$8000`, then `$AA` to
`Ax1x` takes one through `gbe_getSensor`; without the erase nothing is taken
-- and `Ax2x`-`Ax5x` read the latched sample's full 16 bits (the original
read a live value and masked the high bytes to 4 bits, so a game saw `$07FF`
instead of the `$81D0` a level cartridge gives).  The adapter's
`gbe_getSensor` returns what the frontend published with `gbcore_set_tilt`
(level `$81D0` before that).  Test: `gbcore/tests/mbc7.c`.

### The Game Boy Camera (2026-11-25)

New `tgbdual/gbcam.h`, included by `mbc.c` only (no new object): cartridge
type `$FC` banks like an MBC3 (64 ROM banks, bank 0 allowed; 16 RAM banks;
RAM bank number with bit 4 set puts the camera registers at `$A000`, mirrored
every `$80`).  `$A000` bit 0 starts a capture and reads 1 while it runs (32446
+ 512 if the N bit is clear + 16 x exposure machine cycles, counted in
scanlines by `mbc_cam_tick`, called first in `gb_run`); on its last line the
M64282FP model (`gbcam_render`: exposure x gain, invert, 2-D / horizontal /
vertical edge enhancement, the 1-D filter, the 4x4 dither matrix) writes the
14x16-tile picture to cartridge RAM bank 0 `$0100`.  The source picture is
`gbe_getCameraImage()` (adapter, `gbcore_set_camera_image`), else a test
pattern.  State: 64 bytes of the reserved block at offset 192 (`CAM1`, busy
count, 54 registers), written and read only for a `$FC` cartridge, so every
other cartridge's state is byte for byte what it was; `gb.c` gets one call in
each of the four save / load sites and one in `gb_run`.  Not modelled: the
sensor's analogue side, double-speed timing (the same number of scanlines).
The gain table is an own smooth curve (steps of the fourth root of two, x0.5 to
x6.7), not the datasheet's decibels.  Cartridge RAM is closed while a capture
runs: `cpu.c` reads 0 and drops writes at `$A000..$BFFF` through
`mbc_cam_ram_blocked()` (always 0 for any other cartridge), and `mbc_cam_starts()`
counts started captures for the frontend (`gbcore_camera_requests`).  Test:
`gbcore/tests/gbcam.c`.

## Two machines in one program (GB link sessions)

A link session runs both players' Game Boys on each console
(`../gbcore_dual.h`). The core stays a set of globals: the build links the
adapter and core as one partial link through `../gbcore_instance.ld`, keeps
only the `gbcore_*` API global, and links a second copy with those names
renamed `gbcoreb_*` (`psp/Makefile`, `tools/run_gb_tests.py`). Changes for
that:

| File | Change |
|---|---|
| `cpu.c` | Serial completion (`seri_occer` expiring in `cpu_exec`) asks the adapter for the byte that lands in SB (`tgb_port_serial_complete`). With an in-memory cable the byte is exchanged THEN, with the peer machine's SB (`cpu_seri_send`), which is what upstream TGB Dual did between its two machines; otherwise the adapter returns `seri_rx`, the byte chosen at the SC write, exactly as before. |
| `gb.c` | `gb_fill_vframe` returns when there is no frame buffer: a headless machine (the partner's Game Boy) has none. It is reached from `gb_reset` and from SGB `MASK_EN` packets whatever `gbSkip` says. |
| `_mrand_table.h` | The noise tables are `const` and named `tgbshared_mrand7/15`, defined once by the new `tgb_shared.c` outside the per-instance link: 64 KiB the second machine does not duplicate. |
| `tgb_port.h` | Prototypes for `tgb_port_serial_complete` and `cpu_seri_send`. |

Headless (`gbcore_set_headless`) suppresses scanline and sample production,
but it still advances the CPU-side APU sequencer. `apu_timing.h` gives CPU
and delayed audio rendering independent timing state and the same transition
rules; audio generation can never mutate the register state read by the game.
Frame-end discards a headless queue and copies current CPU APU state into the
synthesizer, so later audible use does not replay old register writes.
`gbcore/tests/accuracy.c` checks frame hashes and exact audio state replay;
`gbcore/tests/dual.c` checks independent and headless instances.

APU reuse now clears registers, timing and oscillator history in `snd_reset`.
`gb_reset` still does not promise a complete process-image reset of every
other core global. A link session starts each machine with `gbcore_power_on`,
which restores the instance's whole `.data` and clears its `.bss`
(`../gbcore_instance.ld` marks them).

## Reliability review corrections (2026-10-13)

- `gb_fill_vframe` converts the supplied BGR555 colour to the configured
  RGB565 layout, fills the guard area and tolerates a headless NULL buffer.
  Solid SGB masks are reapplied after state load and buffer reallocation.
- NR52 reflects the four channel-active flags regardless of volume. CPU-side
  length, envelope and sweep timers run without sound I/O or audio output,
  at the same normalized rate on CGB double speed. DIV reset, DAC gating,
  master power and length reload are shared by both APU consumers.
- The old 256 reserved core-state bytes preserve sequencer/oscillator state.
  The CGB restore path now consumes the palette-format marker before them.
  The adapter's reserved extension tail preserves sample cadence and input
  edge state. State version/length remain unchanged; old zero extensions load.
- State bank pointers, APU indices/ranges and SGB parser/palette indices are
  validated before memory is changed. Failed model-switch restore puts the
  previous model back. This is not comprehensive validation of every scalar.
- SGB packet count zero is rejected before it can overrun the seven-packet
  buffer. ATTR_CHR counts characters instead of four-character groups.

Reproduction, compatibility limits and remaining review findings are in
`../../docs/audit/gb-core.md`.

## Volume

`TGB_SND_GAIN` is 2. A synthetic ROM playing channel 1 (duty 50%, envelope
15, NR50 0x77) peaks at +/-8203; gpSP renders the GBA's identical PSG at
full volume as +/-8192 (`sound.c`: `8 * 2^28 >> 18`), so GB and GBA games
play at the same level. The SameBoy core this replaced peaked at +/-4540
for the same tone.

## Build flags

`-fgnu89-inline` (non-static `inline` definitions whose addresses the
dispatch tables take), `-fno-strict-aliasing` (VRAM/ROM bytes read through
`word`/`dword` pointers), `-O3` as MasterBoy built `gbcore/`
(`Makefile.psp`; about 1-2% faster than -O2 in PPSSPP), and
`-Wno-unused-parameter`: the I/O and opcode dispatch tables give every
handler one signature and 157 handlers ignore their argument. Every other
warning class stays on and the files build clean with `-Wall -Wextra`
(host gcc and psp-gcc 15.2).

On the PSP the core and its adapter are linked into one relocatable object
(`psp-ld -r`) whose only global symbols are `gbcore_*` (`psp-objcopy
--keep-global-symbol`): the core exports ~780 generic names (`vram`, `ram`,
`oam`, `halt`, `info`...) and `vram` collided with the GBA core's
`mips_stub.o` on the first link.

## Checked against libretro/tgbdual-libretro (2026-11-06)

libretro's tgbdual is the C++ TGB Dual this core descends from; its history
since 2014 was read (HEAD 0392c9c, 2026-08-23). Taken: `cpu_io.h`, a write to
VBK (0xFF4F) is no longer discarded while an HDMA transfer runs
(619eaa4). Already fixed here in the same way: the `spare_oam` 0xFEA0 index,
ROM/RAM size table bounds, 32-bit `dword`, the wave channel length. Not taken:
the dual-machine APU state, RTC and frame-pacing changes (this port has its
own, see above), `cheat_map` and `vframe` shrinking (cheats are compiled out
and `vframe` is already 160x144), and the apu_stat copy removal (this core
renders sound from its own queue). tgbdual_kai and MasterBoy have no newer
code.

## MBC7 EEPROM "write all" (WRAL)

The vendored loop stored both bytes of the word at the even address
(`get_sram()+i*2` twice): every word read back as its low byte twice.  The
second store is now `+i*2+1`, as the single-word write (WRITE) already did.

## Game Boy colours (2.0)

`_cgb_dmg_pal.h` (new) is the Game Boy Color's per-game palette data (51
combinations, title-sum table, 12 button palettes) and the 32 Super Game Boy
system palettes; its sources and licences are in `notices/THIRD-PARTY.md`.
`tgb_shared.c` holds the tables and the lookups (`tgbshared_cgbpal_auto`,
`_manual`, `_colors`, `tgbshared_sgbpal_colors`). `tgb_port.h` declares
`tgb_dmg_is_cgb`. `lcd.c`: a DMG game's palette entries go through the GBC LCD
model (`cgb_lcd_color`) when that flag is set (a GBC palette is chosen), so
they look as a Game Boy Color showed them; otherwise unchanged.

## State loads that are not trusted (20 Nov 2026, 2.0)

The GB / GBC loader (`gb_state_valid`, `state_validate.h`) already validated the state before applying it. `gpsp/gbcore/tests/statefuzz.cpp`
(ASan + UBSan, random mutation of a real state, MBC and colour variants, frame watchdog) found nothing to change in the core: about 175 000
damaged states (cartridge types MBC1, MBC3 with timer, MBC5 with rumble, MBC7 and an unknown type, made by changing the header byte of the test
cartridges, plus the GBC test cartridges), 0 crashes, 0 hangs, 0 reports; and the load proof (frames 1, 97, 300, 655, then 60 frames) is equal on
every test cartridge. The cartridges are small test programs, so a real game's ROM and RAM sizes were not exercised.

## Accuracy against the public test ROMs (24 Nov 2026, 2.0)

Measured with `gbcore/tests/run_suites.py` (blargg, dmg-acid2, cgb-acid2; the
table is in `docs/COMPATIBILITY.md`, "Game Boy test suites"). In order of what
a game can notice:

| File | Change |
|---|---|
| `op_normal.h` | PUSH AF stored `F | 0x0E`: the low four bits of F are always 0 (the "unused bits are 1" comment came from a game hack). This alone failed nine of the eleven cpu_instrs tests, since the tests checksum AF through the stack. |
| `op_normal.h` | DAA rewritten as the plain rule (several flag and value combinations were wrong). ADD SP,n and LD HL,SP+n set no flags; now H and C come from the low byte, Z and N are cleared. |
| `lcd.c` | `lcd_sprite_select`: a line shows the first ten sprites (OAM order) whose rows cover it, whatever their X; on the DMG the lower X is drawn on top (ties: lower OAM index), on the Game Boy Color the lower OAM index. Before, all forty were drawn, so an eleventh sprite showed and a later sprite at a lower X was covered. The picture differs for games that put more than ten sprites on a line (as hardware does). dmg-acid2 and cgb-acid2 now match. The compiled-out ColorIt copy is unchanged. |
| `cpu.c`, `op_normal.h` | HALT with IME off and an interrupt already requested does not halt, and the next byte is read twice. HALT runs that next instruction itself, in one piece, so nothing is left pending for a state, a rewind step or a Quick response frame to lose (`gbcore/tests/timer.c` loads a state taken right after such a HALT). |
| `cpu.c` | Dispatching an interrupt takes 20 clocks (it took none). `gbcore/tests/timer.c` expects the 20 in its HALT test. blargg interrupt_time passes. |
| `apu.c` | `apu_read` returns unused and write-only bits as 1 (NR10 0x80, NR11/NR21 0x3F, NR14/NR24/NR34/NR44 0xBF, NR30 0x7F, NR32 0x9F, unused 0xFF; NR12.. none). Reads only; the state is unchanged. |
| `cpu_io.h` | A read of IF returns the top three bits as 1. |

None of these touch the state layout; old states load. Not changed:
mem_timing (the core advances the clock after each whole instruction),
oam_bug.

## Sound accuracy (2.0, blargg dmg_sound and cgb_sound 13/13)

| File | Change |
|---|---|
| `apu_timing.h` | The sweep's frequency update also writes `sq1_init_freq`, the copy a later NR13/NR14 write builds on (on hardware the sweep rewrites NR13/NR14 themselves; a retrigger after a sweep used the old low byte). Fixes 05-sweep details (subtract mode, overflow on retrigger). |
| `apu_timing.h`, `apu.c` | CPU-side channel 3 clock: `wave_cd` (clocks to the next fetch), `wave_pos`, `wave_since`, advanced in T-clocks by `apu_advance` on the CPU timeline only (the audio generator keeps its own sample-rate position and is untouched). A trigger loads `period + 6` (`WAVE_TRIG`) and restarts at sample 0. |
| `apu.c` | Wave RAM access while channel 3 plays: a read returns the byte the channel is on (CGB: always; DMG: only within `WAVE_WINDOW` = 2 clocks after the fetch, else 0xFF); a write lands on that byte (DMG: same window, else dropped). The redirected address is what is queued for the audio generator. A DMG retrigger when the next fetch is within `WAVE_CORRUPT` = 2 clocks corrupts wave RAM as Pan Docs describes (byte 0-3: only byte 0; else the first four bytes become the aligned group); the corrupting writes are queued too. The three constants were fitted to the CRCs of 09/10/12 (which come from hardware). |
| `cpu.c`, `cpu_io.h`, `gb.h` | The core advances the clock after a whole instruction, so an APU register access used to land on the instruction's first clock. For FF10-FF3F and DIV the APU is now brought to the access's M-cycle (`cpu_access_off` = instruction clocks - 4, from `tmp_clocks`; 0 outside an instruction). The queue stamps use the same time. No other component's timing changes. |
| `apu_timing.h` | State: `WAV1` tag, three words, at byte 96 of the 256 reserved bytes (the APU block fills 0-79, the timer 80-87, the camera 192+). States without the tag load with the channel stopped at sample 0, as before this change. |
| `apu_timing.h`, `gbcore_tgbdual.c` | The three words are part of `gbcore_sync_hash` (`apu_save_wave_timing`): they decide what a wave RAM read returns, so two linked machines that differ only there are told apart. |
