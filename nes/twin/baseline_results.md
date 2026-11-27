# NES conformance baseline
Run of `conformance.py` over the public test ROMs of https://github.com/christopherpow/nes-test-roms (commit
95d8f621ae55cee0d09b91519a8989ae0e64753b, 2 Mar 2022; cloned outside the repository, not vendored) through the Linux twin
(`nes_twin ROM --frames 3000 --status --text`, rerun with 12000 frames when a test was still running). 12 Nov 2026,
FCEUmm commit e0f3086a plus the local patches of `core/PROVENANCE.md`. Command:

    NES_TESTROMS=<clone> python3 -I nes/twin/conformance.py --md out.md [--expect twin/conformance_expect.txt]

The result is read the way blargg's tests report it: `$6001-$6003` = DE B0 61 marks the protocol, `$6000` = 00 is a pass,
80 is still running, 81 asks for the reset button (the twin cannot press it: counted separately), anything else is a fail.
ROMs without the protocol show their result on the screen (**no-protocol**); for the important ones the nametable text was
read with `nes_twin --nt-text` (section "Screen-only tests"). `conformance_expect.txt` holds the 75 passing ROMs;
`--expect` exits 1 and prints REGRESSION lines if one stops passing.

## 1. Before and after (this work)

| | before | after |
|---|---|---|
| pass | 55 | **75** |
| fail | 52 | 32 |
| needs reset (apu_reset, cpu_reset) | 6 | 6 |
| no protocol (screen only) | 176 | 176 |
| load error (`LINUSMUS.NES`, not a game) | 1 | 1 |
| ROMs | 290 | 290 |

No ROM went from pass to fail. Over 300 frames with scripted input, 289 of 290 ROMs have identical picture, audio and RAM
hashes before and after the final left-clip fix, and of the other patches only 27 ROMs changed (all explained by the
opcode, dummy-read and MMC3 fixes); the rest are bit-identical, so the table cache and UB fixes are behaviour neutral.

Fixes (each in `core/PROVENANCE.md`, marked `NESP`):

| Fix | ROMs fixed | Matters for commercial games? |
|---|---|---|
| ATX ($AB) magic constant $EE to $FF | instr_test-v3/v5 `02/03-immediate` and the combined ROMs | Almost never; unofficial opcode use is rare (a few unlicensed/late titles). From memory, unverified. |
| SYA/SXA ($9C/$9E) page-cross address and value | `06/07-abs_xy`, `all_instrs` (v3, v5, nes_instr_test) | Almost never; same reasoning. |
| LAS ($BB) read not RMW, cross cycle | `instr_timing` (2 files), `nes_instr_test` | Almost never. |
| Dummy read on page cross at `target - $100` | `instr_misc/03-dummy_reads`, `cpu_dummy_reads` (screen) | Matters only when a read has side effects ($2002/$2007/$4015/$4016 across a page); rare, a few homebrew and trick games. |
| MMC3 IRQ counter clocked by A12 rises made via $2006/$2007 | `mmc3_test` 1,2,3,5; `mmc3_test_2` 1,2,3,5; `mmc3_irq_tests` | Rare: some games and demos move A12 by hand with rendering off (from memory, unverified). |
| `ppu.c` sprite-0 hit clipped in the left 8 px | `sprite_hit_tests` 05 (screen) | Games with a masked left edge and sprite-0 splits; uncommon. |
| VRC2 `SFORMAT` terminator (found by ASan) | not a test: every state save of a VRC2 game read past an array | Yes for any VRC2 game's save state. |
| Shift UB in `ppu.c`, `mmc5.c` | none (UBSan clean) | No visible effect on x86/ARM gcc. |
| `FCEU_BuildBgPairLUT` cache | none (pure speedup, identical output) | Frame cost 13.9 M to 3.2 M instructions on `$2002` polling code. |

Newly passing ROMs: instr_misc x2, instr_test-v3 x3, instr_test-v5 x3, instr_timing x2, nes_instr_test x2, mmc3_test x4,
mmc3_test_2 x4 (20).

## 2. Remaining failures (32) and why they are not fixed

All need cycle-exact CPU/PPU/APU interplay. FCEUmm runs the CPU one instruction at a time and the PPU one scanline at a
time, so these cannot be fixed by a small, safe patch without a core rewrite (excluded by the task).

| Group | Files | Reason | Commercial-game relevance (from memory, unverified) |
|---|---|---|---|
| ppu_vbl_nmi | 10 | VBL flag/NMI timing to the PPU dot | A few games are timing sensitive (e.g. ones polling $2002 at the exact VBL edge); most work. |
| apu_test | 4-jitter, 5-len_timing, 6-irq_flag_timing, combined | frame counter timing | Few games. |
| apu_reset | 4017_timing, 4017_written | power-on $4017 state | Practically none. |
| cpu_interrupts_v2 | 2,3,4,5 | NMI/IRQ/BRK interaction, DMA | Few; Bugs in IRQ-split games are the usual symptom. |
| sprdma_and_dmc_dma | 2 | DMC DMA steals cycles from OAM DMA | Few (some DPCM-heavy games). |
| mmc3_test, mmc3_test_2 | 4-scanline_timing; 6-MMC6 / 6-MMC3_alt | IRQ cycle timing; MMC6 and old-MMC3 revisions are mutually exclusive with the one FCEUmm picks | Few games depend on revision (the core already special-cases Kick Master, Shougi Meikan, Star Wars PAL). |
| oam_read, oam_stress, cpu_dummy_writes_oam, cpu_exec_space_ppuio, ppu_open_bus, ppu_read_buffer | 1 each | open bus, OAM decay, exec from I/O | Very few. |

## 3. x86_64 vs armhf (qemu-arm user mode, `-cpu max`)

| Check | Result |
|---|---|
| `nes_twin` armhf static (-O2 cortex-a9 neon) vs x86_64, 290 ROMs, 300 frames, scripted pad: per-frame picture hash, WAV md5, RAM md5, status, text | 290/290 identical (old core, then final core) |
| `nescore.o` armhf vs x86_64 through `tests/obj_hash.c` (per-frame picture, audio, RAM, SRAM hash, state size), 240 frames | 51/51 identical (8 synthetic + 43 public); `arm_check.sh` 13/13 |
| armhf link test with the real `nescore.o` | runs; `make check`: 57 globals, all `nes_host_*` for host, armhf and Vita |
| x86 ASan + UBSan + LeakSanitizer, 292 ROM files x 600 frames, scripted pad, plus state size / mem-load / save+load state / FDS eject | clean after the fixes above (state sizes unchanged: 14766, 14812, 14845, 113714) |

Endianness/alignment: no problem found. qemu user mode does not trap unaligned accesses, so alignment is covered only by
the x86 UBSan alignment check (clean). `sprlinebuf` in `ppu.c` is not explicitly aligned (no report).

Cost (instructions per frame; x86 = callgrind Ir, ARM = `qemu-arm -one-insn-per-tb -d exec`, delta of 12 minus 2 frames):

| ROM | x86 Ir/frame | ARM instr/frame | ARM / x86 | est. ms at 444 MHz, IPC 0.7 / 1.0 / 1.3 |
|---|---|---|---|---|
| synthetic NROM | 2.71 M (2.84 M with startup) | 2.81 M | 1.04 | 9.0 / 6.3 / 4.9 |
| ny2011 | 1.92 M (measured pair; 3.24 M incl. startup) | 2.17 M | 1.13 | 7.0 / 4.9 / 3.8 |
| DROPOFF7, TEST.NES, BLOCKS, SPRITE (heaviest) | 3.1-3.4 M | ratio 1.10 assumed: 3.4-3.7 M | | 11-12 / 7.7-8.4 / 5.9-6.5 |

Assumptions: single thread, IPC on a Cortex-A9 not measured (no hardware), memory stalls and cache misses folded into
the IPC range, no GPU upload, audio thread or app cost, 48 kHz audio. The 60 fps budget is 16.6 ms. Verdict: fits with
margin at IPC 0.7 and above, but this is an estimate.

## 4. Ours vs stock libretro-fceumm (same commit e0f3086, our original core without patches)

Stock `fceumm_libretro.so` driven by the headless frontend `tools/lrhead.c`, 3000 frames, no input, all 290 ROMs, against our
core before any local patch:

| Compared | Result |
|---|---|
| verdict ($6000 protocol) | 0 differences |
| system RAM md5 at the end | 289/289 identical (LINUSMUS load error in both) |
| bytes at $6000-3 on 7 no-protocol ROMs (fceuxd, midscanline, nestopia, nintendulator, oam3, pulsar, test001) | differ; harness artifact (open bus vs the frontend's WRAM descriptor), not emulation |
| picture, audio | not comparable bit for bit (stock outputs XRGB8888 with another palette) |
| instructions per frame | stock 5.5 M (NROM) and 15.8 M (TEST.NES) vs ours 2.84 M and 3.15 M: the per-call LUT rebuild is an upstream performance bug |

The original NESP edits (`fds.c`, `fds.h`, `ines.c`) are not exercised by this comparison. No difference between ours and
stock was found other than the deliberate patches.

## 5. Nestopia (libretro/nestopia f1e02d1, cloned in the scratch area, not vendored)

Same ROMs through `lrhead` (2000 frames; PRG-RAM mapped by patching the header to NES 2.0 with 8 KB; result from `$6000`,
else from the nametable text).

| | FCEUmm (ours, final) | Nestopia |
|---|---|---|
| protocol ROMs passing (of 107 that report a result) | 75 | 105 (2 more ask for reset) |
| of FCEUmm's 52 original failures | 20 now fixed | 50 pass, 2 need reset |
| ROMs where Nestopia fails and FCEUmm passes | | none among protocol ROMs |
| screen-only: pal_apu_tests 04-08,10,11 | not graded | shown FAILED (PAL ROMs on NTSC run; not a fair test) |
| instructions per frame (callgrind) | 2.7-3.4 M | 7.5-11.2 M (NROM 7.49, MMC3 8.34, TEST 9.42, SPRITE 11.19, ny2011 8.97): 2.5-3.5x |
| binary text | 1.55 MB (.so) | 3.0 MB (.so), needs libstdc++ (C++98, 646 board files); app side is C only |
| licence | GPL-2.0 | GPL-2.0 |

Estimated Vita cost of Nestopia at 444 MHz: about 8-12 M ARM instructions per frame, i.e. 19-28 ms at IPC 1.0, over the
16.6 ms budget. Recommendation: stay with FCEUmm as the only default (it fits and is stock-identical plus fixes). Do not
offer both: Nestopia's extra accuracy is in cycle-timing tests that few games need, while its cost breaks 60 fps on the
Vita. Revisit only if a specific commercial game fails on FCEUmm and passes on Nestopia.

## 6. Screen-only tests (nametable text via `--nt-text`, 2500 frames)

Before the patches, among the no-protocol ROMs these reported FAILED: sprite_hit_tests 05.left_clip (#3) and
09.timing_basics (#2); sprite_overflow_tests 3.Timing and 4.Obscure; the 7 `vbl_nmi_timing` ROMs; pal_apu_tests 04-08,10,11;
mmc3_irq_tests 1-6; cpu_dummy_reads; dmc_dma_during_read4/dma_4016_read; scanline/scanline.nes x2; blargg_apu_2005 04-09, 11.
After: sprite_hit_tests 01-08, 10, 11 pass (05.left_clip fixed here); 09.timing_basics still FAILED #2 (sprite-0 hit dot
timing). A full before/after rerun of the other screen-only ROMs on the final build was not completed; the mmc3_irq_tests and
cpu_dummy_reads improvements are expected from the MMC3 and dummy-read fixes but are not confirmed.

## 7. Sprite limit removal

FCEUmm: `maxsprites` 8 or 64 in `ppu.c`; the overflow flag is `if (ns > 8) PPU_status |= 0x20` either way, and sprite 0 is
always the first of the first 8 evaluated, so game-visible state does not depend on the option. Verified by RAM + saved
state + audio + SRAM identity, option on vs off, with scripted input, over nrom/mmc1/mmc3/gun and 265 public ROMs
(selftest section 11). Only 3 of 265 public ROMs show any picture difference (other/read2004, other/snow, ppu_read_buffer),
so the public set exercises it weakly; the synthetic 12-sprites-per-row ROM is the meaningful case. Games relying on the
8-sprite limit (nesdev "Sprite overflow games" page, fetched): Castlevania II (swamp), Felix the Cat (bag), Gimmick!,
Gremlins 2, Majou Densetsu II, Ninja Gaiden 1-3, The Legend of Zelda (dungeon screen edges), Lizard. Bee 52 and City Trouble use
the overflow flag for splits. Built-in exception table: only the two Zelda PRG CRCs (0x3fe272fb, 0xeaf7ed72), the only ones in
the core's own database; add others with `nes_host_sprite_exception_add(crc)` once the ROMs are available.

## Not verified

Nothing was run on Vita or any ARM silicon (qemu only; IPC assumed). No commercial game or Duck Hunt tried. Stock picture and
audio not bit-compared. Game-relevance statements above are from memory. nesdev.org/wiki/MMC3 and
nesdev.org/wiki/CPU_unofficial_opcodes could not be fetched (denied), so ATX/SHY/SHX/MMC3 behaviour follows the blargg
test ROMs' own expectations. FDS only covered by synthetic ROMs.

## 8. Per-ROM results (3000 frames, before the left-clip fix, which only affects a screen-only ROM)

| ROM | result | message | fps |
|---|---|---|---|
| 240pee/240pee-bnrom.nes | no-protocol |  | 3428 |
| 240pee/240pee.nes | no-protocol |  | 3988 |
| MMC1_A12/mmc1_a12.nes | no-protocol |  | 3952 |
| PaddleTest3/PaddleTest.nes | no-protocol |  | 3543 |
| apu_mixer/dmc.nes | pass | dmc channel mixing test 1. Should play short tone. 2. Should be nearly silent. 3. Should play short  | 3155 |
| apu_mixer/noise.nes | pass | noise channel mixing test 1. Should play short tone. 2. Should fade noise in, and out, without any t | 3778 |
| apu_mixer/square.nes | pass | square channel mixing test 1. Should play short tone. 2. Should be nearly silent. 3. Should play sho | 4020 |
| apu_mixer/triangle.nes | pass | triangle channel mixing test 1. Should play short tone. 2. Should be nearly silent. 3. Should play s | 3832 |
| apu_reset/4015_cleared.nes | needs-reset | Press RESET | 3632 |
| apu_reset/4017_timing.nes | fail ($6000=03) | Delay after effective $4017 write: 0 Frame IRQ flag should be set sooner after power/reset 4017_timi | 3978 |
| apu_reset/4017_written.nes | fail ($6000=02) | At power, $4017 should be written with $00 4017_written Failed #2 | 4166 |
| apu_reset/irq_flag_cleared.nes | needs-reset | Press RESET | 3595 |
| apu_reset/len_ctrs_enabled.nes | needs-reset | Press RESET | 3995 |
| apu_reset/works_immediately.nes | needs-reset | Press RESET | 3724 |
| apu_test/apu_test.nes | fail ($6000=01) | Frame irq is set too soon 4-jitter Failed #2 While running test 4 of 8 | 3867 |
| apu_test/rom_singles/1-len_ctr.nes | pass | 1-len_ctr Passed | 4216 |
| apu_test/rom_singles/2-len_table.nes | pass | 2-len_table Passed | 4146 |
| apu_test/rom_singles/3-irq_flag.nes | pass | 3-irq_flag Passed | 3785 |
| apu_test/rom_singles/4-jitter.nes | fail ($6000=02) | Frame irq is set too soon 4-jitter Failed #2 | 4277 |
| apu_test/rom_singles/5-len_timing.nes | fail ($6000=02) | Channel: 0 First length of mode 0 is too soon 5-len_timing Failed #2 | 3828 |
| apu_test/rom_singles/6-irq_flag_timing.nes | fail ($6000=02) | Flag first set too soon 6-irq_flag_timing Failed #2 | 4116 |
| apu_test/rom_singles/7-dmc_basics.nes | pass | 7-dmc_basics Passed | 4176 |
| apu_test/rom_singles/8-dmc_rates.nes | pass | 8-dmc_rates Passed | 4000 |
| blargg_apu_2005.07.30/01.len_ctr.nes | no-protocol |  | 4115 |
| blargg_apu_2005.07.30/02.len_table.nes | no-protocol |  | 3624 |
| blargg_apu_2005.07.30/03.irq_flag.nes | no-protocol |  | 3880 |
| blargg_apu_2005.07.30/04.clock_jitter.nes | no-protocol |  | 3773 |
| blargg_apu_2005.07.30/05.len_timing_mode0.nes | no-protocol |  | 3982 |
| blargg_apu_2005.07.30/06.len_timing_mode1.nes | no-protocol |  | 4052 |
| blargg_apu_2005.07.30/07.irq_flag_timing.nes | no-protocol |  | 3693 |
| blargg_apu_2005.07.30/08.irq_timing.nes | no-protocol |  | 3558 |
| blargg_apu_2005.07.30/09.reset_timing.nes | no-protocol |  | 3771 |
| blargg_apu_2005.07.30/10.len_halt_timing.nes | no-protocol |  | 3806 |
| blargg_apu_2005.07.30/11.len_reload_timing.nes | no-protocol |  | 4086 |
| blargg_litewall/blargg_litewall-10c.nes | no-protocol |  | 3972 |
| blargg_litewall/blargg_litewall-9.nes | no-protocol |  | 3832 |
| blargg_litewall/litewall2.nes | no-protocol |  | 3670 |
| blargg_litewall/litewall3.nes | no-protocol |  | 3660 |
| blargg_litewall/litewall5.nes | no-protocol |  | 3854 |
| blargg_nes_cpu_test5/cpu.nes | no-protocol |  | 3392 |
| blargg_nes_cpu_test5/official.nes | no-protocol |  | 3586 |
| blargg_ppu_tests_2005.09.15b/palette_ram.nes | no-protocol |  | 3826 |
| blargg_ppu_tests_2005.09.15b/power_up_palette.nes | no-protocol |  | 3555 |
| blargg_ppu_tests_2005.09.15b/sprite_ram.nes | no-protocol |  | 3316 |
| blargg_ppu_tests_2005.09.15b/vbl_clear_time.nes | no-protocol |  | 3359 |
| blargg_ppu_tests_2005.09.15b/vram_access.nes | no-protocol |  | 4087 |
| branch_timing_tests/1.Branch_Basics.nes | no-protocol |  | 3912 |
| branch_timing_tests/2.Backward_Branch.nes | no-protocol |  | 3595 |
| branch_timing_tests/3.Forward_Branch.nes | no-protocol |  | 3650 |
| cpu_dummy_reads/cpu_dummy_reads.nes | no-protocol |  | 3270 |
| cpu_dummy_writes/cpu_dummy_writes_oam.nes | fail ($6000=02) | [0;37mTEST: cpu_dummy_writes_oam [0;33mThis program verifies that the CPU does 2x writes properly. | 4228 |
| cpu_dummy_writes/cpu_dummy_writes_ppumem.nes | pass | [0;37mTEST: cpu_dummy_writes_ppumem [0;33mThis program verifies that the CPU does 2x writes proper | 4203 |
| cpu_exec_space/test_cpu_exec_space_apu.nes | pass | [0;37mTEST: test_cpu_exec_space_apu [0;33mThis program verifies that the CPU can execute code from | 4453 |
| cpu_exec_space/test_cpu_exec_space_ppuio.nes | fail ($6000=05) | [0;37mTEST:test_cpu_exec_space_ppuio [0;33mThis program verifies that the CPU can execute code fro | 4191 |
| cpu_interrupts_v2/cpu_interrupts.nes | no-protocol |  | 14125 |
| cpu_interrupts_v2/rom_singles/1-cli_latency.nes | pass | 1-cli_latency Passed | 4264 |
| cpu_interrupts_v2/rom_singles/2-nmi_and_brk.nes | fail ($6000=01) | NMI BRK 00 27 36 00 27 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 | 4050 |
| cpu_interrupts_v2/rom_singles/3-nmi_and_irq.nes | fail ($6000=01) | NMI BRK 23 00 23 00 21 00 21 00 25 21 25 21 25 21 25 21 25 21 25 21 25 21 25 21 800DDD77 3-nmi_and_i | 4151 |
| cpu_interrupts_v2/rom_singles/4-irq_and_dma.nes | fail ($6000=01) | 0 +0 0 +1 0 +2 1 +3 1 +4 2 +5 2 +6 4 +7 4 +8 7 +9 7 +10 7 +11 7 +12 8 +13 ... 8 +524 8 +525 8 +526 9 | 3537 |
| cpu_interrupts_v2/rom_singles/5-branch_delays_irq.nes | fail ($6000=01) | test_jmp T+ CK PC 00 01 03 01 00 03 02 01 04 03 00 04 04 02 07 05 01 07 06 00 07 07 01 08 08 00 08 0 | 3755 |
| cpu_reset/ram_after_reset.nes | needs-reset | Press reset AFTER this message disappears | 4318 |
| cpu_reset/registers.nes | needs-reset | A X Y P S 00 00 00 34 FD Press reset AFTER this message disappears | 4969 |
| cpu_timing_test6/cpu_timing_test.nes | no-protocol |  | 3679 |
| dmc_dma_during_read4/dma_2007_read.nes | no-protocol |  | 3578 |
| dmc_dma_during_read4/dma_2007_write.nes | no-protocol |  | 3526 |
| dmc_dma_during_read4/dma_4016_read.nes | no-protocol |  | 3205 |
| dmc_dma_during_read4/double_2007_read.nes | no-protocol |  | 2911 |
| dmc_dma_during_read4/read_write_2007.nes | no-protocol |  | 3437 |
| dmc_tests/buffer_retained.nes | no-protocol |  | 4762 |
| dmc_tests/latency.nes | no-protocol |  | 4722 |
| dmc_tests/status.nes | no-protocol |  | 5010 |
| dmc_tests/status_irq.nes | no-protocol |  | 5063 |
| dpcmletterbox/dpcmletterbox.nes | no-protocol |  | 3438 |
| exram/mmc5exram.nes | no-protocol |  | 3496 |
| full_palette/flowing_palette.nes | no-protocol |  | 4463 |
| full_palette/full_palette.nes | no-protocol |  | 4493 |
| full_palette/full_palette_smooth.nes | no-protocol |  | 3746 |
| instr_misc/instr_misc.nes | pass | 04-dummy_reads_apu Passed All 4 tests passed | 3468 |
| instr_misc/rom_singles/01-abs_x_wrap.nes | pass | 01-abs_x_wrap Passed | 3782 |
| instr_misc/rom_singles/02-branch_wrap.nes | pass | 02-branch_wrap Passed | 3354 |
| instr_misc/rom_singles/03-dummy_reads.nes | pass | 03-dummy_reads Passed | 4105 |
| instr_misc/rom_singles/04-dummy_reads_apu.nes | pass | 04-dummy_reads_apu Passed | 4088 |
| instr_test-v3/all_instrs.nes | pass | All 15 tests passed | 4468 |
| instr_test-v3/official_only.nes | pass | All 15 tests passed | 3984 |
| instr_test-v3/rom_singles/01-implied.nes | pass | 01-implied Passed | 4454 |
| instr_test-v3/rom_singles/02-immediate.nes | pass | 02-immediate Passed | 4444 |
| instr_test-v3/rom_singles/03-zero_page.nes | pass | 03-zero_page Passed | 4149 |
| instr_test-v3/rom_singles/04-zp_xy.nes | pass | 04-zp_xy Passed | 4286 |
| instr_test-v3/rom_singles/05-absolute.nes | pass | 05-absolute Passed | 4362 |
| instr_test-v3/rom_singles/06-abs_xy.nes | pass | 06-abs_xy Passed | 4216 |
| instr_test-v3/rom_singles/07-ind_x.nes | pass | 07-ind_x Passed | 4769 |
| instr_test-v3/rom_singles/08-ind_y.nes | pass | 08-ind_y Passed | 4323 |
| instr_test-v3/rom_singles/09-branches.nes | pass | 09-branches Passed | 3698 |
| instr_test-v3/rom_singles/10-stack.nes | pass | 10-stack Passed | 4303 |
| instr_test-v3/rom_singles/11-jmp_jsr.nes | pass | 11-jmp_jsr Passed | 4113 |
| instr_test-v3/rom_singles/12-rts.nes | pass | 12-rts Passed | 4059 |
| instr_test-v3/rom_singles/13-rti.nes | pass | 13-rti Passed | 4366 |
| instr_test-v3/rom_singles/14-brk.nes | pass | 14-brk Passed | 4460 |
| instr_test-v3/rom_singles/15-special.nes | pass | 15-special Passed | 4381 |
| instr_test-v5/all_instrs.nes | pass | All 16 tests passed | 4494 |
| instr_test-v5/official_only.nes | pass | All 16 tests passed | 4230 |
| instr_test-v5/rom_singles/01-basics.nes | pass | 01-basics Passed | 4624 |
| instr_test-v5/rom_singles/02-implied.nes | pass | 02-implied Passed | 4574 |
| instr_test-v5/rom_singles/03-immediate.nes | pass | 03-immediate Passed | 3968 |
| instr_test-v5/rom_singles/04-zero_page.nes | pass | 04-zero_page Passed | 3911 |
| instr_test-v5/rom_singles/05-zp_xy.nes | pass | 05-zp_xy Passed | 4144 |
| instr_test-v5/rom_singles/06-absolute.nes | pass | 06-absolute Passed | 4395 |
| instr_test-v5/rom_singles/07-abs_xy.nes | pass | 07-abs_xy Passed | 3804 |
| instr_test-v5/rom_singles/08-ind_x.nes | pass | 08-ind_x Passed | 3863 |
| instr_test-v5/rom_singles/09-ind_y.nes | pass | 09-ind_y Passed | 4799 |
| instr_test-v5/rom_singles/10-branches.nes | pass | 10-branches Passed | 4433 |
| instr_test-v5/rom_singles/11-stack.nes | pass | 11-stack Passed | 3962 |
| instr_test-v5/rom_singles/12-jmp_jsr.nes | pass | 12-jmp_jsr Passed | 3526 |
| instr_test-v5/rom_singles/13-rts.nes | pass | 13-rts Passed | 3950 |
| instr_test-v5/rom_singles/14-rti.nes | pass | 14-rti Passed | 3505 |
| instr_test-v5/rom_singles/15-brk.nes | pass | 15-brk Passed | 3589 |
| instr_test-v5/rom_singles/16-special.nes | pass | 16-special Passed | 3969 |
| instr_timing/instr_timing.nes | pass | 2-branch_timing Passed All 2 tests passed | 4287 |
| instr_timing/rom_singles/1-instr_timing.nes | pass | Instruction timing test Takes about 25 seconds. Doesn't time the 8 branches and 12 illegal instructi | 3946 |
| instr_timing/rom_singles/2-branch_timing.nes | pass | 2-branch_timing Passed | 4192 |
| m22chrbankingtest/0-127.nes | no-protocol |  | 4399 |
| mmc3_irq_tests/1.Clocking.nes | no-protocol |  | 3619 |
| mmc3_irq_tests/2.Details.nes | no-protocol |  | 3510 |
| mmc3_irq_tests/3.A12_clocking.nes | no-protocol |  | 3488 |
| mmc3_irq_tests/4.Scanline_timing.nes | no-protocol |  | 3196 |
| mmc3_irq_tests/5.MMC3_rev_A.nes | no-protocol |  | 3599 |
| mmc3_irq_tests/6.MMC3_rev_B.nes | no-protocol |  | 3687 |
| mmc3_test/1-clocking.nes | pass | 1-clocking Passed | 3732 |
| mmc3_test/2-details.nes | pass | 2-details Passed | 3974 |
| mmc3_test/3-A12_clocking.nes | pass | 3-A12_clocking Passed | 3717 |
| mmc3_test/4-scanline_timing.nes | fail ($6000=02) | Scanline 0 IRQ should occur later when $2000=$08 4-scanline_timing Failed #2 | 3736 |
| mmc3_test/5-MMC3.nes | pass | 5-MMC3 Passed | 3609 |
| mmc3_test/6-MMC6.nes | fail ($6000=03) | IRQ shouldn't occur when reloading after counter normally reaches 0 6-MMC6 Failed #3 | 4268 |
| mmc3_test_2/rom_singles/1-clocking.nes | pass | 1-clocking Passed | 4663 |
| mmc3_test_2/rom_singles/2-details.nes | pass | 2-details Passed | 4016 |
| mmc3_test_2/rom_singles/3-A12_clocking.nes | pass | 3-A12_clocking Passed | 4342 |
| mmc3_test_2/rom_singles/4-scanline_timing.nes | fail ($6000=02) | Scanline 0 IRQ should occur later when $2000=$08 4-scanline_timing Failed #2 | 4344 |
| mmc3_test_2/rom_singles/5-MMC3.nes | pass | 5-MMC3 Passed | 3971 |
| mmc3_test_2/rom_singles/6-MMC3_alt.nes | fail ($6000=02) | IRQ shouldn't be set when reloading to 0 due to counter naturally reaching 0 previously 6-MMC3_alt F | 4608 |
| mmc5test/mmc5test.nes | no-protocol |  | 3920 |
| mmc5test_v2/mmc5test.nes | no-protocol |  | 3217 |
| nes15-1.0.0/nes15-NTSC.nes | no-protocol |  | 4218 |
| nes15-1.0.0/nes15-PAL.nes | no-protocol |  | 3979 |
| nes_instr_test/rom_singles/01-implied.nes | pass | 01-implied Passed | 4449 |
| nes_instr_test/rom_singles/02-immediate.nes | pass | 02-immediate Passed | 4804 |
| nes_instr_test/rom_singles/03-zero_page.nes | pass | 03-zero_page Passed | 4564 |
| nes_instr_test/rom_singles/04-zp_xy.nes | pass | 04-zp_xy Passed | 4363 |
| nes_instr_test/rom_singles/05-absolute.nes | pass | 05-absolute Passed | 4330 |
| nes_instr_test/rom_singles/06-abs_xy.nes | pass | 06-abs_xy Passed | 3786 |
| nes_instr_test/rom_singles/07-ind_x.nes | pass | 07-ind_x Passed | 4710 |
| nes_instr_test/rom_singles/08-ind_y.nes | pass | 08-ind_y Passed | 4234 |
| nes_instr_test/rom_singles/09-branches.nes | pass | 09-branches Passed | 4527 |
| nes_instr_test/rom_singles/10-stack.nes | pass | 10-stack Passed | 4769 |
| nes_instr_test/rom_singles/11-special.nes | pass | 11-special Passed | 4290 |
| nmi_sync/demo_ntsc.nes | no-protocol |  | 4010 |
| nmi_sync/demo_pal.nes | no-protocol |  | 4055 |
| nrom368/fail368.nes | no-protocol |  | 3726 |
| nrom368/test1.nes | no-protocol |  | 6832 |
| ny2011/ny2011.nes | no-protocol |  | 2709 |
| oam_read/oam_read.nes | fail ($6000=01) | **************** **************** **************** **************** **************** *************** | 3599 |
| oam_stress/oam_stress.nes | fail ($6000=01) | **************** **************** **************** **************** **************** *************** | 3764 |
| other/2003-test.nes | no-protocol |  | 3556 |
| other/8bitpeoples_-_deadline_console_invitro.nes | no-protocol |  | 2940 |
| other/BLOCKS.NES | no-protocol |  | 3704 |
| other/BladeBuster.nes | no-protocol |  | 4200 |
| other/CMC80s.NES | no-protocol |  | 3854 |
| other/DROPOFF7.NES | no-protocol |  | 3264 |
| other/Duelito.nes | no-protocol |  | 3935 |
| other/FLAME.NES | no-protocol |  | 3759 |
| other/GENIE.NES | no-protocol |  | 4026 |
| other/GREYS.NES | no-protocol |  | 3472 |
| other/LINUSMUS.NES | load-error | load failed: An error occurred while loading the file. | 0 |
| other/MOTION.NES | no-protocol |  | 3753 |
| other/PCM.demo.wgraphics.nes | no-protocol |  | 3753 |
| other/RasterChromaLuma.NES | no-protocol |  | 4118 |
| other/RasterDemo.NES | no-protocol |  | 2861 |
| other/RasterTest1.NES | no-protocol |  | 3771 |
| other/RasterTest2.NES | no-protocol |  | 4123 |
| other/RasterTest3.NES | no-protocol |  | 3690 |
| other/RasterTest3a.NES | no-protocol |  | 3338 |
| other/RasterTest3b.NES | no-protocol |  | 3000 |
| other/RasterTest3c.NES | no-protocol |  | 3421 |
| other/RasterTest3d.NES | no-protocol |  | 3428 |
| other/RasterTest3e.NES | no-protocol |  | 3124 |
| other/Retrocoders - Years behind.NES | no-protocol |  | 3938 |
| other/S0.NES | no-protocol |  | 4196 |
| other/SPRITE.NES | no-protocol |  | 2882 |
| other/Sayoonara!.NES | no-protocol |  | 3578 |
| other/SimpleParallaxDemo.nes | no-protocol |  | 3649 |
| other/Streemerz_bundle.nes | no-protocol |  | 3873 |
| other/TANESPOT.NES | no-protocol |  | 3163 |
| other/TEST.NES | no-protocol |  | 3597 |
| other/apocalypse.nes | no-protocol |  | 3359 |
| other/blargg_litewall-2.nes | no-protocol |  | 3788 |
| other/blargg_litewall-9.nes | no-protocol |  | 4331 |
| other/demo jitter.nes | no-protocol |  | 3645 |
| other/demo.nes | no-protocol |  | 4109 |
| other/fceuxd.nes | no-protocol |  | 4448 |
| other/firefly.nes | no-protocol |  | 3535 |
| other/high-hopes.nes | no-protocol |  | 3595 |
| other/logo (E).nes | no-protocol |  | 3822 |
| other/manhole.nes | no-protocol |  | 3628 |
| other/max-300.nes | no-protocol |  | 3467 |
| other/midscanline.nes | no-protocol |  | 4094 |
| other/minipack.nes | no-protocol |  | 3826 |
| other/nescafe.nes | no-protocol |  | 3780 |
| other/nestest.nes | no-protocol |  | 4141 |
| other/nestopia.nes | no-protocol |  | 4364 |
| other/new-game.nes | no-protocol |  | 4047 |
| other/nintendulator.nes | no-protocol |  | 3971 |
| other/oam3.nes | no-protocol |  | 3651 |
| other/oc.nes | no-protocol |  | 4043 |
| other/physics.0.1.nes | no-protocol |  | 3651 |
| other/pulsar.nes | no-protocol |  | 3463 |
| other/quantum_disco_brothers_by_wAMMA.nes | no-protocol |  | 2749 |
| other/rastesam4.nes | no-protocol |  | 3482 |
| other/read2004.nes | no-protocol |  | 3782 |
| other/snow.nes | no-protocol |  | 2928 |
| other/test001.nes | no-protocol |  | 3850 |
| other/test28.nes | no-protocol |  | 3772 |
| other/window2_ntsc.nes | no-protocol |  | 3564 |
| other/window2_pal.nes | no-protocol |  | 3783 |
| other/window_old_ntsc.nes | no-protocol |  | 4350 |
| other/window_old_pal.nes | no-protocol |  | 3520 |
| pal_apu_tests/01.len_ctr.nes | no-protocol |  | 3756 |
| pal_apu_tests/02.len_table.nes | no-protocol |  | 3796 |
| pal_apu_tests/03.irq_flag.nes | no-protocol |  | 3725 |
| pal_apu_tests/04.clock_jitter.nes | no-protocol |  | 3560 |
| pal_apu_tests/05.len_timing_mode0.nes | no-protocol |  | 3575 |
| pal_apu_tests/06.len_timing_mode1.nes | no-protocol |  | 3861 |
| pal_apu_tests/07.irq_flag_timing.nes | no-protocol |  | 3677 |
| pal_apu_tests/08.irq_timing.nes | no-protocol |  | 3632 |
| pal_apu_tests/10.len_halt_timing.nes | no-protocol |  | 3866 |
| pal_apu_tests/11.len_reload_timing.nes | no-protocol |  | 3950 |
| ppu_open_bus/ppu_open_bus.nes | fail ($6000=03) | Decay value should become zero by one second ppu_open_bus Failed #3 | 4304 |
| ppu_read_buffer/test_ppu_read_buffer.nes | fail ($6000=43) | [0;37mTEST:test_ppu_read_buffer ----------------------------- Testing basic PPU memory I/O. [0;37m | 3762 |
| ppu_vbl_nmi/ppu_vbl_nmi.nes | fail ($6000=01) | VBL period is too short with BG off 01-vbl_basics Failed #7 While running test 1 of 10 | 3606 |
| ppu_vbl_nmi/rom_singles/01-vbl_basics.nes | fail ($6000=07) | VBL period is too short with BG off 01-vbl_basics Failed #7 | 4272 |
| ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes | fail ($6000=01) | T+ 1 2 00 - V 01 - V 02 V - 03 V - 04 V - 05 V - 06 V - 07 V - 08 V - C2633058 02-vbl_set_time Faile | 3816 |
| ppu_vbl_nmi/rom_singles/03-vbl_clear_time.nes | fail ($6000=01) | 00 V 01 V 02 V 03 - 04 - 05 - 06 - 07 - 08 - A937B9D5 03-vbl_clear_time Failed | 4659 |
| ppu_vbl_nmi/rom_singles/04-nmi_control.nes | pass | 04-nmi_control Passed | 4301 |
| ppu_vbl_nmi/rom_singles/05-nmi_timing.nes | fail ($6000=01) | 00 3 01 3 02 3 03 2 04 2 05 2 06 2 07 1 08 1 09 1 1A388616 05-nmi_timing Failed | 3660 |
| ppu_vbl_nmi/rom_singles/06-suppression.nes | fail ($6000=01) | 00 - N 01 - N 02 V N 03 V N 04 V N 05 V N 06 V N 07 V N 08 V N 09 V N 636EA6C0 06-suppression Failed | 4033 |
| ppu_vbl_nmi/rom_singles/07-nmi_on_timing.nes | fail ($6000=01) | 00 N 01 N 02 N 03 - 04 - 05 - 06 - 07 - 08 - 3D994949 07-nmi_on_timing Failed | 3898 |
| ppu_vbl_nmi/rom_singles/08-nmi_off_timing.nes | fail ($6000=01) | 03 - 04 - 05 - 06 - 07 - 08 - 09 - 0A N 0B N 0C N BA340B0D 08-nmi_off_timing Failed | 3974 |
| ppu_vbl_nmi/rom_singles/09-even_odd_frames.nes | fail ($6000=02) | 04 Pattern ----- should not skip any clocks 09-even_odd_frames Failed #2 | 3938 |
| ppu_vbl_nmi/rom_singles/10-even_odd_timing.nes | fail ($6000=02) | 02 Clock is skipped too soon, relative to enabling BG 10-even_odd_timing Failed #2 | 4046 |
| read_joy3/count_errors.nes | no-protocol |  | 3117 |
| read_joy3/count_errors_fast.nes | no-protocol |  | 3530 |
| read_joy3/test_buttons.nes | no-protocol |  | 3579 |
| read_joy3/thorough_test.nes | no-protocol |  | 3472 |
| scanline/scanline.nes | no-protocol |  | 3978 |
| scanline-a1/scanline.nes | no-protocol |  | 4123 |
| scrolltest/scroll.nes | no-protocol |  | 4011 |
| soundtest/SNDTEST.NES | no-protocol |  | 3951 |
| sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes | fail ($6000=01) | T+ Clocks (decimal) 00 528 01 527 02 527 03 527 04 527 05 528 06 527 07 527 08 527 09 527 0A 527 0B  | 4300 |
| sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes | fail ($6000=01) | T+ Clocks (decimal) 00 528 01 527 02 527 03 527 04 527 05 528 06 527 07 527 08 527 09 527 0A 527 0B  | 4299 |
| sprite_hit_tests_2005.10.05/01.basics.nes | no-protocol |  | 3565 |
| sprite_hit_tests_2005.10.05/02.alignment.nes | no-protocol |  | 3693 |
| sprite_hit_tests_2005.10.05/03.corners.nes | no-protocol |  | 3898 |
| sprite_hit_tests_2005.10.05/04.flip.nes | no-protocol |  | 3719 |
| sprite_hit_tests_2005.10.05/05.left_clip.nes | no-protocol |  | 3808 |
| sprite_hit_tests_2005.10.05/06.right_edge.nes | no-protocol |  | 3791 |
| sprite_hit_tests_2005.10.05/07.screen_bottom.nes | no-protocol |  | 3868 |
| sprite_hit_tests_2005.10.05/08.double_height.nes | no-protocol |  | 4010 |
| sprite_hit_tests_2005.10.05/09.timing_basics.nes | no-protocol |  | 3800 |
| sprite_hit_tests_2005.10.05/10.timing_order.nes | no-protocol |  | 3467 |
| sprite_hit_tests_2005.10.05/11.edge_timing.nes | no-protocol |  | 3762 |
| sprite_overflow_tests/1.Basics.nes | no-protocol |  | 4123 |
| sprite_overflow_tests/2.Details.nes | no-protocol |  | 3878 |
| sprite_overflow_tests/3.Timing.nes | no-protocol |  | 3842 |
| sprite_overflow_tests/4.Obscure.nes | no-protocol |  | 3735 |
| sprite_overflow_tests/5.Emulator.nes | no-protocol |  | 3411 |
| spritecans-2011/spritecans.nes | no-protocol |  | 2880 |
| stars_se/StarsSE.NES | no-protocol |  | 3590 |
| stomper/smwstomp.nes | no-protocol |  | 3644 |
| stress/NEStress.NES | no-protocol |  | 4229 |
| tutor/tutor.nes | no-protocol |  | 3716 |
| tvpassfail/tv.nes | no-protocol |  | 4218 |
| vaus-test/vaus-test.nes | no-protocol |  | 4721 |
| vbl_nmi_timing/1.frame_basics.nes | no-protocol |  | 4027 |
| vbl_nmi_timing/2.vbl_timing.nes | no-protocol |  | 4076 |
| vbl_nmi_timing/3.even_odd_frames.nes | no-protocol |  | 4056 |
| vbl_nmi_timing/4.vbl_clear_timing.nes | no-protocol |  | 4066 |
| vbl_nmi_timing/5.nmi_suppression.nes | no-protocol |  | 4247 |
| vbl_nmi_timing/6.nmi_disable.nes | no-protocol |  | 4136 |
| vbl_nmi_timing/7.nmi_timing.nes | no-protocol |  | 3947 |
| volume_tests/volumes.nes | no-protocol |  | 5063 |
| window5/colorwin_ntsc.nes | no-protocol |  | 4042 |
| window5/colorwin_pal.nes | no-protocol |  | 3643 |

pass 75, fail 32, needs reset 6, still running 0, no protocol 176, load error 1, of 290 ROMs
