# Provenance of nes/core

- **Upstream:** libretro-fceumm, <https://github.com/libretro/libretro-fceumm>
  (FCE Ultra / FCEUX derived NES core), `src/` directory.
- **Commit:** `e0f3086a8104f8fbbe49a2615b10401504a9533f`
  ("libretro: send path checks through the frontend VFS (#660)", 2026-11-02).
- **Vendored:** 2026-11-12 into this directory.
- **Licence:** GPL-2.0-or-later (FCEUmm; `LICENSE` is upstream's `Copying`, shipped as
  `licenses/FCEUmm-GPL-2.0.txt`; it is not byte-identical to `licenses/GPL-2.0.txt`, which
  carries the newer FSF address). `AUTHORS` is upstream's `Authors` file. Because `nes.bin` links this
  core, the program `nes.bin` as a whole is distributed under the GPL; it shares no code with `eboot.bin`.

## What is here

- `*.c`, `*.h` of `src/`, `boards/` (with the `.inc` files), `input/`, `palettes/`, and the
  `fir/*.h` filter tables: byte-identical to upstream, except the files named below.
- `libretro-common/` : `include/{libretro.h,boolean.h,retro_common_api.h,retro_inline.h,
  retro_miscellaneous.h,streams/memory_stream.h}` and `streams/memory_stream.c`, taken unchanged from
  `src/drivers/libretro/libretro-common` of the same commit (MIT, `licenses/libretro-common-MIT.txt`).
- `sources.mk`: the list of translation units we build (ours, MIT).
- `compat/`: four small header stand-ins for the libretro-common pieces the core includes but we do
  not build (`compat/strl.h`, `string/stdstring.h`, `file/file_path.h`, `streams/file_stream.h`: strlcpy,
  string helpers, path join, read-only stdio file streams). Ours, MIT.
- `fourscore_db.h`: the `fourscore_db_list` and `famicom_4p_db_list` CRC tables, copied unchanged from
  `src/drivers/libretro/libretro.c` (lines 3575-3835) so the host can pick the Four Score the way the
  libretro core does. GPL, as its source file.

## Not taken

`src/drivers/` (the libretro driver and the other front ends), `src/hdpack/`, `src/ntsc/`, and the filter
generator sources in `src/fir/` (`*.coef`, `*.scm`, `Makefile`). The frontend that replaces the driver is
`nes/frontend/host.cpp`.

## Local changes to upstream files (marked `NESP` in the source)

- `fds.c`: new function `FCEU_FDSInfo(int *side, int *sides, int *inserted)` before `FCEU_FDSEject`
  (reads the drive state for a menu), and `FCEU_FDSSetBIOS(const uint8_t *bios8192)` before `FDSLoad`: when set,
  `FDSLoad` copies the 8192 byte Disk System BIOS from that memory instead of opening `disksys.rom` (three small
  changes inside `FDSLoad`: the open is skipped, the read becomes a `memcpy`, `FCEU_fclose` is guarded for the unused
  file). `fds.h`: the two declarations. - `ines.c` (in `iNESLoad`, the NES 2.0 CHR-RAM size): the allocation is raised to at least 8 KiB. A header that declares
  less (the public test ROM `other/oam3.nes` declares 1 KiB with a mapper that has 8 KiB of CHR-RAM) let PPU writes run
  past the buffer (found with valgrind: invalid write in `B2007`), which corrupted the heap.

Further local patches of 12 Nov 2026 (accuracy work; each marked `NESP` in the source, each with a test in the twin's
conformance run, see `nes/twin/baseline_results.md`):

- `ops.h`, `ATX #imm` ($AB): the "magic" constant is `$FF` (A = X = imm on the 2A03, what blargg's `instr_test` ROMs and the
  nesdev notes give), was `$EE`.
- `ops.h`, `SYA abs,X` ($9C) and `SXA abs,Y` ($9E) (SHY/SHX): value = register & (high byte of the base address + 1);
  when the index crosses a page the high byte of the target address becomes that value (the old code wrote the value to
  the normal target).
- `ops.h`, `LAS abs,Y` ($BB): is a plain read with the page-cross cycle (it was coded as a read-modify-write that wrote
  the byte back to memory and took no cross cycle; `instr_timing` reported it).
- `x6502.c`, `GetABIRD` and `GetIYRD`: the dummy read on a page cross is at `target - 0x100` (un-carried address); it was
  `target ^ 0x100`, wrong whenever the base page number was odd (`instr_misc/03-dummy_reads`).
- `ppu.c`, `ppu.h`, `fceu.c`, `boards/mmc3.c`: new hook `PPU_A12Hook`, called for a rising A12 made by the CPU through
  `$2006` / `$2007` while rendering is off or in vertical blank. The MMC3 board uses it to clock its IRQ counter like the
  real chip (the scanline hook still does the rendering-time clocks; the two never count the same edge). Installed only
  when the board's IRQ source is the standard handler (not for the Kick Master / Shougi Meikan / PAL Star Wars hacks).
  No new saved state, no change in state size. Fixes `mmc3_test` and `mmc3_test_2` files 1, 2, 3 and 5.

- `ppu.c`, `FCEU_BuildBgPairLUT`: the background pen-pair table is rebuilt only when `PALRAM[0..15]` changed (it is a pure
  function of them; `RefreshLine` rebuilt it 256 entries at a time on every `$2002` read and PPU register write, i.e.
  thousands of times a frame in a vblank polling loop). Output unchanged (identical picture and RAM hashes over all 290
  public test ROMs); instructions per frame (callgrind, same ROM and input): 3.72 M to 2.84 M on the synthetic NROM game, 13.9 M to 3.2 M on `DROPOFF7`, `TEST.NES`, `BLOCKS.NES` and the other demos that poll `$2002`.
- Undefined behaviour found by AddressSanitizer / UBSan over the public test ROMs: `boards/asic_vrc2and4.c`
  `VRC2_stateRegs` had no `{ 0 }` terminator, so every state save of a VRC2 game (mapper 22, 23, 25 ...) ran past the end of
  the array (`m22chrbankingtest`; the same check over all 226 `SFORMAT` tables found no other unterminated one);
  `ppu.c` (`ppulut3` build) and `boards/mmc5.c` (`<< 24` of a byte promoted to a signed int) now shift unsigned values.

Check: `diff -r <clone>/src nes/core` lists only the files named in this section as different, plus the additions above and
the omissions above.

The program side (`nes/frontend/host.c`, plain C) supplies what the libretro driver supplied; `nes/frontend/Makefile`
links core + host into the single relocatable object `nescore.o` whose only global symbols are `nes_host_*`.
