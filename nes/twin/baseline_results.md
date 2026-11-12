# NES conformance baseline
Run of `conformance.py` over the public test ROMs of https://github.com/christopherpow/nes-test-roms (commit
95d8f621ae55cee0d09b91519a8989ae0e64753b, 2 Mar 2022; cloned outside the repository, not vendored) through the Linux twin
(`nes_twin ROM --frames 3000 --status --text`, rerun with 12000 frames when a test was still running). 12 Nov 2026,
FCEUmm commit e0f3086a, host glue as in this tree. Command:

    NES_TESTROMS=<clone> python3 -I nes/twin/conformance.py --md out.md

The result is read the way blargg's tests report it: `$6001-$6003` = DE B0 61 marks the protocol, `$6000` = 00 is a pass,
80 is still running, 81 asks for the reset button (the twin cannot press it: counted separately), anything else is a fail.
ROMs that do not use the protocol only show their result on the screen (**no-protocol**): they were run without crashing
but are neither passes nor failures here.

## Summary

| | count |
|---|---|
| pass | 55 |
| fail | 52 |
| needs reset (apu_reset, cpu_reset) | 6 |
| no protocol (screen-only demos and tests) | 176 |
| load error | 1 |
| ROMs | 290 |

Twin speed on these ROMs (thread CPU time, x86_64, a machine shared with other jobs): mean 2994 frames/s, slowest 902, fastest 13125
(60 frames/s is real time).

## What the failures are

I did not run the same ROMs in stock libretro-fceumm to compare, so this is a statement about this build, not a diff
against upstream. The core files are upstream's, apart from the three small edits in `core/PROVENANCE.md` (FDS BIOS and
drive info, and the CHR-RAM size below). The failing groups test cycle-exact behaviour or unofficial opcodes: `ppu_vbl_nmi` (10 of 11 fail: VBL/NMI timing against the PPU
clock), `mmc3_test` and `mmc3_test_2` (all 12 files: A12 clocking and IRQ timing details), `apu_test` (the frame IRQ and
length counter *timing* files 4, 5, 6 and the combined ROM fail), `cpu_interrupts_v2` (the cli-latency file passes; NMI/BRK,
NMI/IRQ, IRQ/DMA and branch-delay files fail), `sprdma_and_dmc_dma`, `instr_timing` (the instruction timing file; branch timing
passes), `instr_misc` (the dummy-reads file), `oam_read`, `oam_stress`, `ppu_open_bus`, `ppu_read_buffer`,
`cpu_dummy_writes_oam`, `cpu_exec_space_ppuio`, and in `instr_test-v3`, `instr_test-v5` and `nes_instr_test` the single-file
tests for the unofficial opcodes ATX #n ($AB) and SYA/SXA ($9C/$9E) (and the combined `all_instrs` ROMs that contain them).
What passes: both `official_only.nes` ROMs (v3 "All 15 tests passed", v5 "All 16 tests passed"), 14 of 17 files of `instr_test-v3`
and 15 of 18 of `instr_test-v5`, `apu_mixer` (4 of 4), length counter and table, IRQ flag, DMC basics and DMC rates of
`apu_test`, `cli_latency`, `cpu_dummy_writes_ppumem`, `cpu_exec_space_apu`. Which games, if any, are affected was not checked: no commercial games were available here.

## Found by this run

`other/oam3.nes` crashed the first full run: a NES 2.0 header declared 1 KiB of CHR-RAM, the PPU wrote past that buffer (valgrind:
invalid write in `B2007`, heap corruption, segfault at exit). Fixed in `core/ines.c` (the allocation is at least 8 KiB; marked
`NESP`). After the fix the ROM runs 3000 frames under valgrind without an error (it is a screen-only test: no protocol). The summary
above counts it as no-protocol. `other/LINUSMUS.NES` is refused by the core's loader ("An error occurred while loading the file"; it is
a music file, not a game) and is left as the one load error.

The synthetic ROMs of `selftest.sh` also run 80 frames clean under `valgrind -q` (NROM, MMC1, MMC3, UNIF, header variants, FDS
from memory with eject/side/insert, cheats, state save), and a state save with `--leak-check=full` reports nothing.

## Per ROM

| ROM | result | message | frames/s |
|---|---|---|---|
| 240pee/240pee-bnrom.nes | no-protocol |  | 3160 |
| 240pee/240pee.nes | no-protocol |  | 3137 |
| MMC1_A12/mmc1_a12.nes | no-protocol |  | 2943 |
| PaddleTest3/PaddleTest.nes | no-protocol |  | 3081 |
| apu_mixer/dmc.nes | pass | dmc channel mixing test 1. Should play short tone. 2. Should be nearly silent. 3. Should play short  | 4153 |
| apu_mixer/noise.nes | pass | noise channel mixing test 1. Should play short tone. 2. Should fade noise in, and out, without any t | 3965 |
| apu_mixer/square.nes | pass | square channel mixing test 1. Should play short tone. 2. Should be nearly silent. 3. Should play sho | 3844 |
| apu_mixer/triangle.nes | pass | triangle channel mixing test 1. Should play short tone. 2. Should be nearly silent. 3. Should play s | 3547 |
| apu_reset/4015_cleared.nes | needs-reset | Press RESET | 3580 |
| apu_reset/4017_timing.nes | fail ($6000=03) | Delay after effective $4017 write: 0 Frame IRQ flag should be set sooner after power/reset 4017_timi | 3426 |
| apu_reset/4017_written.nes | fail ($6000=02) | At power, $4017 should be written with $00 4017_written Failed #2 | 2836 |
| apu_reset/irq_flag_cleared.nes | needs-reset | Press RESET | 3099 |
| apu_reset/len_ctrs_enabled.nes | needs-reset | Press RESET | 3626 |
| apu_reset/works_immediately.nes | needs-reset | Press RESET | 3824 |
| apu_test/apu_test.nes | fail ($6000=01) | Frame irq is set too soon 4-jitter Failed #2 While running test 4 of 8 | 3499 |
| apu_test/rom_singles/1-len_ctr.nes | pass | 1-len_ctr Passed | 3828 |
| apu_test/rom_singles/2-len_table.nes | pass | 2-len_table Passed | 3235 |
| apu_test/rom_singles/3-irq_flag.nes | pass | 3-irq_flag Passed | 3585 |
| apu_test/rom_singles/4-jitter.nes | fail ($6000=02) | Frame irq is set too soon 4-jitter Failed #2 | 3668 |
| apu_test/rom_singles/5-len_timing.nes | fail ($6000=02) | Channel: 0 First length of mode 0 is too soon 5-len_timing Failed #2 | 3396 |
| apu_test/rom_singles/6-irq_flag_timing.nes | fail ($6000=02) | Flag first set too soon 6-irq_flag_timing Failed #2 | 3664 |
| apu_test/rom_singles/7-dmc_basics.nes | pass | 7-dmc_basics Passed | 3193 |
| apu_test/rom_singles/8-dmc_rates.nes | pass | 8-dmc_rates Passed | 3452 |
| blargg_apu_2005.07.30/01.len_ctr.nes | no-protocol |  | 3237 |
| blargg_apu_2005.07.30/02.len_table.nes | no-protocol |  | 3155 |
| blargg_apu_2005.07.30/03.irq_flag.nes | no-protocol |  | 2976 |
| blargg_apu_2005.07.30/04.clock_jitter.nes | no-protocol |  | 3035 |
| blargg_apu_2005.07.30/05.len_timing_mode0.nes | no-protocol |  | 3188 |
| blargg_apu_2005.07.30/06.len_timing_mode1.nes | no-protocol |  | 2977 |
| blargg_apu_2005.07.30/07.irq_flag_timing.nes | no-protocol |  | 2571 |
| blargg_apu_2005.07.30/08.irq_timing.nes | no-protocol |  | 3334 |
| blargg_apu_2005.07.30/09.reset_timing.nes | no-protocol |  | 2911 |
| blargg_apu_2005.07.30/10.len_halt_timing.nes | no-protocol |  | 2790 |
| blargg_apu_2005.07.30/11.len_reload_timing.nes | no-protocol |  | 3071 |
| blargg_litewall/blargg_litewall-10c.nes | no-protocol |  | 1455 |
| blargg_litewall/blargg_litewall-9.nes | no-protocol |  | 1472 |
| blargg_litewall/litewall2.nes | no-protocol |  | 3066 |
| blargg_litewall/litewall3.nes | no-protocol |  | 3238 |
| blargg_litewall/litewall5.nes | no-protocol |  | 2926 |
| blargg_nes_cpu_test5/cpu.nes | no-protocol |  | 3030 |
| blargg_nes_cpu_test5/official.nes | no-protocol |  | 3132 |
| blargg_ppu_tests_2005.09.15b/palette_ram.nes | no-protocol |  | 3101 |
| blargg_ppu_tests_2005.09.15b/power_up_palette.nes | no-protocol |  | 3309 |
| blargg_ppu_tests_2005.09.15b/sprite_ram.nes | no-protocol |  | 2968 |
| blargg_ppu_tests_2005.09.15b/vbl_clear_time.nes | no-protocol |  | 2704 |
| blargg_ppu_tests_2005.09.15b/vram_access.nes | no-protocol |  | 3499 |
| branch_timing_tests/1.Branch_Basics.nes | no-protocol |  | 2703 |
| branch_timing_tests/2.Backward_Branch.nes | no-protocol |  | 3042 |
| branch_timing_tests/3.Forward_Branch.nes | no-protocol |  | 3327 |
| cpu_dummy_reads/cpu_dummy_reads.nes | no-protocol |  | 1273 |
| cpu_dummy_writes/cpu_dummy_writes_oam.nes | fail ($6000=02) | [0;37mTEST: cpu_dummy_writes_oam [0;33mThis program verifies that the CPU does 2x writes properly. | 2980 |
| cpu_dummy_writes/cpu_dummy_writes_ppumem.nes | pass | [0;37mTEST: cpu_dummy_writes_ppumem [0;33mThis program verifies that the CPU does 2x writes proper | 2249 |
| cpu_exec_space/test_cpu_exec_space_apu.nes | pass | [0;37mTEST: test_cpu_exec_space_apu [0;33mThis program verifies that the CPU can execute code from | 2601 |
| cpu_exec_space/test_cpu_exec_space_ppuio.nes | fail ($6000=05) | [0;37mTEST:test_cpu_exec_space_ppuio [0;33mThis program verifies that the CPU can execute code fro | 2912 |
| cpu_interrupts_v2/cpu_interrupts.nes | no-protocol |  | 13125 |
| cpu_interrupts_v2/rom_singles/1-cli_latency.nes | pass | 1-cli_latency Passed | 3249 |
| cpu_interrupts_v2/rom_singles/2-nmi_and_brk.nes | fail ($6000=01) | NMI BRK 00 27 36 00 27 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 26 36 00 | 3499 |
| cpu_interrupts_v2/rom_singles/3-nmi_and_irq.nes | fail ($6000=01) | NMI BRK 23 00 23 00 21 00 21 00 25 21 25 21 25 21 25 21 25 21 25 21 25 21 25 21 800DDD77 3-nmi_and_i | 3524 |
| cpu_interrupts_v2/rom_singles/4-irq_and_dma.nes | fail ($6000=01) | 0 +0 0 +1 0 +2 1 +3 1 +4 2 +5 2 +6 4 +7 4 +8 7 +9 7 +10 7 +11 7 +12 8 +13 ... 8 +524 8 +525 8 +526 9 | 2652 |
| cpu_interrupts_v2/rom_singles/5-branch_delays_irq.nes | fail ($6000=01) | test_jmp T+ CK PC 00 01 03 01 00 03 02 01 04 03 00 04 04 02 07 05 01 07 06 00 07 07 01 08 08 00 08 0 | 2894 |
| cpu_reset/ram_after_reset.nes | needs-reset | Press reset AFTER this message disappears | 4321 |
| cpu_reset/registers.nes | needs-reset | A X Y P S 00 00 00 34 FD Press reset AFTER this message disappears | 5049 |
| cpu_timing_test6/cpu_timing_test.nes | no-protocol |  | 2258 |
| dmc_dma_during_read4/dma_2007_read.nes | no-protocol |  | 1105 |
| dmc_dma_during_read4/dma_2007_write.nes | no-protocol |  | 1112 |
| dmc_dma_during_read4/dma_4016_read.nes | no-protocol |  | 1125 |
| dmc_dma_during_read4/double_2007_read.nes | no-protocol |  | 1228 |
| dmc_dma_during_read4/read_write_2007.nes | no-protocol |  | 1238 |
| dmc_tests/buffer_retained.nes | no-protocol |  | 4355 |
| dmc_tests/latency.nes | no-protocol |  | 4745 |
| dmc_tests/status.nes | no-protocol |  | 6290 |
| dmc_tests/status_irq.nes | no-protocol |  | 5413 |
| dpcmletterbox/dpcmletterbox.nes | no-protocol |  | 2954 |
| exram/mmc5exram.nes | no-protocol |  | 2313 |
| full_palette/flowing_palette.nes | no-protocol |  | 5171 |
| full_palette/full_palette.nes | no-protocol |  | 4698 |
| full_palette/full_palette_smooth.nes | no-protocol |  | 5462 |
| instr_misc/instr_misc.nes | fail ($6000=01) | LDA abs,x 03-dummy_reads Failed #3 While running test 3 of 4 | 3164 |
| instr_misc/rom_singles/01-abs_x_wrap.nes | pass | 01-abs_x_wrap Passed | 3116 |
| instr_misc/rom_singles/02-branch_wrap.nes | pass | 02-branch_wrap Passed | 2930 |
| instr_misc/rom_singles/03-dummy_reads.nes | fail ($6000=03) | LDA abs,x 03-dummy_reads Failed #3 | 3423 |
| instr_misc/rom_singles/04-dummy_reads_apu.nes | pass | 04-dummy_reads_apu Passed | 3481 |
| instr_test-v3/all_instrs.nes | fail ($6000=01) | AB ATX #n 02-immediate Failed While running test 2 of 15 | 3411 |
| instr_test-v3/official_only.nes | pass | All 15 tests passed | 3222 |
| instr_test-v3/rom_singles/01-implied.nes | pass | 01-implied Passed | 3764 |
| instr_test-v3/rom_singles/02-immediate.nes | fail ($6000=01) | AB ATX #n 02-immediate Failed | 3021 |
| instr_test-v3/rom_singles/03-zero_page.nes | pass | 03-zero_page Passed | 2493 |
| instr_test-v3/rom_singles/04-zp_xy.nes | pass | 04-zp_xy Passed | 3252 |
| instr_test-v3/rom_singles/05-absolute.nes | pass | 05-absolute Passed | 3177 |
| instr_test-v3/rom_singles/06-abs_xy.nes | fail ($6000=01) | 9C SYA abs,X 9E SXA abs,Y 06-abs_xy Failed | 3419 |
| instr_test-v3/rom_singles/07-ind_x.nes | pass | 07-ind_x Passed | 2610 |
| instr_test-v3/rom_singles/08-ind_y.nes | pass | 08-ind_y Passed | 3043 |
| instr_test-v3/rom_singles/09-branches.nes | pass | 09-branches Passed | 3345 |
| instr_test-v3/rom_singles/10-stack.nes | pass | 10-stack Passed | 3595 |
| instr_test-v3/rom_singles/11-jmp_jsr.nes | pass | 11-jmp_jsr Passed | 3223 |
| instr_test-v3/rom_singles/12-rts.nes | pass | 12-rts Passed | 3618 |
| instr_test-v3/rom_singles/13-rti.nes | pass | 13-rti Passed | 3059 |
| instr_test-v3/rom_singles/14-brk.nes | pass | 14-brk Passed | 3544 |
| instr_test-v3/rom_singles/15-special.nes | pass | 15-special Passed | 3000 |
| instr_test-v5/all_instrs.nes | fail ($6000=01) | AB ATX #n 03-immediate Failed While running test 3 of 16 | 3362 |
| instr_test-v5/official_only.nes | pass | All 16 tests passed | 3167 |
| instr_test-v5/rom_singles/01-basics.nes | pass | 01-basics Passed | 3330 |
| instr_test-v5/rom_singles/02-implied.nes | pass | 02-implied Passed | 3263 |
| instr_test-v5/rom_singles/03-immediate.nes | fail ($6000=01) | AB ATX #n 03-immediate Failed | 3814 |
| instr_test-v5/rom_singles/04-zero_page.nes | pass | 04-zero_page Passed | 3110 |
| instr_test-v5/rom_singles/05-zp_xy.nes | pass | 05-zp_xy Passed | 3301 |
| instr_test-v5/rom_singles/06-absolute.nes | pass | 06-absolute Passed | 3332 |
| instr_test-v5/rom_singles/07-abs_xy.nes | fail ($6000=01) | 9C SYA abs,X 9E SXA abs,Y 07-abs_xy Failed | 3129 |
| instr_test-v5/rom_singles/08-ind_x.nes | pass | 08-ind_x Passed | 3530 |
| instr_test-v5/rom_singles/09-ind_y.nes | pass | 09-ind_y Passed | 3509 |
| instr_test-v5/rom_singles/10-branches.nes | pass | 10-branches Passed | 2872 |
| instr_test-v5/rom_singles/11-stack.nes | pass | 11-stack Passed | 3288 |
| instr_test-v5/rom_singles/12-jmp_jsr.nes | pass | 12-jmp_jsr Passed | 3580 |
| instr_test-v5/rom_singles/13-rts.nes | pass | 13-rts Passed | 2659 |
| instr_test-v5/rom_singles/14-rti.nes | pass | 14-rti Passed | 3192 |
| instr_test-v5/rom_singles/15-brk.nes | pass | 15-brk Passed | 3197 |
| instr_test-v5/rom_singles/16-special.nes | pass | 16-special Passed | 3261 |
| instr_timing/instr_timing.nes | fail ($6000=01) | Instruction timing test Takes about 25 seconds. Doesn't time the 8 branches and 12 illegal instructi | 3075 |
| instr_timing/rom_singles/1-instr_timing.nes | fail ($6000=04) | Instruction timing test Takes about 25 seconds. Doesn't time the 8 branches and 12 illegal instructi | 3049 |
| instr_timing/rom_singles/2-branch_timing.nes | pass | 2-branch_timing Passed | 2808 |
| m22chrbankingtest/0-127.nes | no-protocol |  | 3461 |
| mmc3_irq_tests/1.Clocking.nes | no-protocol |  | 3157 |
| mmc3_irq_tests/2.Details.nes | no-protocol |  | 2918 |
| mmc3_irq_tests/3.A12_clocking.nes | no-protocol |  | 3266 |
| mmc3_irq_tests/4.Scanline_timing.nes | no-protocol |  | 2715 |
| mmc3_irq_tests/5.MMC3_rev_A.nes | no-protocol |  | 2947 |
| mmc3_irq_tests/6.MMC3_rev_B.nes | no-protocol |  | 3258 |
| mmc3_test/1-clocking.nes | fail ($6000=03) | Should decrement when A12 is toggled via PPUADDR 1-clocking Failed #3 | 2949 |
| mmc3_test/2-details.nes | fail ($6000=02) | Counter isn't working when reloaded with 255 2-details Failed #2 | 2750 |
| mmc3_test/3-A12_clocking.nes | fail ($6000=04) | Should be clocked when A12 changes to 1 via PPUADDR write 3-A12_clocking Failed #4 | 2571 |
| mmc3_test/4-scanline_timing.nes | fail ($6000=02) | Scanline 0 IRQ should occur later when $2000=$08 4-scanline_timing Failed #2 | 2677 |
| mmc3_test/5-MMC3.nes | fail ($6000=02) | Should reload and set IRQ every clock when reload is 0 5-MMC3 Failed #2 | 2912 |
| mmc3_test/6-MMC6.nes | fail ($6000=02) | IRQ should be set when reloading to 0 after clear 6-MMC6 Failed #2 | 2248 |
| mmc3_test_2/rom_singles/1-clocking.nes | fail ($6000=03) | Should decrement when A12 is toggled via PPUADDR 1-clocking Failed #3 | 2339 |
| mmc3_test_2/rom_singles/2-details.nes | fail ($6000=02) | Counter isn't working when reloaded with 255 2-details Failed #2 | 2796 |
| mmc3_test_2/rom_singles/3-A12_clocking.nes | fail ($6000=04) | Should be clocked when A12 changes to 1 via PPUADDR write 3-A12_clocking Failed #4 | 3282 |
| mmc3_test_2/rom_singles/4-scanline_timing.nes | fail ($6000=02) | Scanline 0 IRQ should occur later when $2000=$08 4-scanline_timing Failed #2 | 3200 |
| mmc3_test_2/rom_singles/5-MMC3.nes | fail ($6000=02) | Should reload and set IRQ every clock when reload is 0 5-MMC3 Failed #2 | 2660 |
| mmc3_test_2/rom_singles/6-MMC3_alt.nes | fail ($6000=02) | IRQ shouldn't be set when reloading to 0 due to counter naturally reaching 0 previously 6-MMC3_alt F | 2881 |
| mmc5test/mmc5test.nes | no-protocol |  | 2494 |
| mmc5test_v2/mmc5test.nes | no-protocol |  | 2900 |
| nes15-1.0.0/nes15-NTSC.nes | no-protocol |  | 2632 |
| nes15-1.0.0/nes15-PAL.nes | no-protocol |  | 2933 |
| nes_instr_test/rom_singles/01-implied.nes | pass | 01-implied Passed | 3408 |
| nes_instr_test/rom_singles/02-immediate.nes | fail ($6000=01) | AB ATX #n 02-immediate Failed | 3126 |
| nes_instr_test/rom_singles/03-zero_page.nes | pass | 03-zero_page Passed | 3237 |
| nes_instr_test/rom_singles/04-zp_xy.nes | pass | 04-zp_xy Passed | 3035 |
| nes_instr_test/rom_singles/05-absolute.nes | pass | 05-absolute Passed | 3658 |
| nes_instr_test/rom_singles/06-abs_xy.nes | fail ($6000=01) | 9C SYA abs,X 9E SXA abs,Y 06-abs_xy Failed | 3364 |
| nes_instr_test/rom_singles/07-ind_x.nes | pass | 07-ind_x Passed | 3541 |
| nes_instr_test/rom_singles/08-ind_y.nes | pass | 08-ind_y Passed | 3860 |
| nes_instr_test/rom_singles/09-branches.nes | pass | 09-branches Passed | 3081 |
| nes_instr_test/rom_singles/10-stack.nes | pass | 10-stack Passed | 3031 |
| nes_instr_test/rom_singles/11-special.nes | pass | 11-special Passed | 2887 |
| nmi_sync/demo_ntsc.nes | no-protocol |  | 2832 |
| nmi_sync/demo_pal.nes | no-protocol |  | 2604 |
| nrom368/fail368.nes | no-protocol |  | 3105 |
| nrom368/test1.nes | no-protocol |  | 6340 |
| ny2011/ny2011.nes | no-protocol |  | 1357 |
| oam_read/oam_read.nes | fail ($6000=01) | **************** **************** **************** **************** **************** *************** | 3120 |
| oam_stress/oam_stress.nes | fail ($6000=01) | **************** **************** **************** **************** **************** *************** | 2686 |
| other/2003-test.nes | no-protocol |  | 3187 |
| other/8bitpeoples_-_deadline_console_invitro.nes | no-protocol |  | 2544 |
| other/BLOCKS.NES | no-protocol |  | 1012 |
| other/BladeBuster.nes | no-protocol |  | 3110 |
| other/CMC80s.NES | no-protocol |  | 2132 |
| other/DROPOFF7.NES | no-protocol |  | 902 |
| other/Duelito.nes | no-protocol |  | 2569 |
| other/FLAME.NES | no-protocol |  | 3220 |
| other/GENIE.NES | no-protocol |  | 2742 |
| other/GREYS.NES | no-protocol |  | 3261 |
| other/LINUSMUS.NES | load-error | load failed: An error occurred while loading the file. | 0 |
| other/MOTION.NES | no-protocol |  | 2674 |
| other/PCM.demo.wgraphics.nes | no-protocol |  | 3096 |
| other/RasterChromaLuma.NES | no-protocol |  | 2870 |
| other/RasterDemo.NES | no-protocol |  | 2418 |
| other/RasterTest1.NES | no-protocol |  | 3358 |
| other/RasterTest2.NES | no-protocol |  | 3021 |
| other/RasterTest3.NES | no-protocol |  | 2295 |
| other/RasterTest3a.NES | no-protocol |  | 2335 |
| other/RasterTest3b.NES | no-protocol |  | 2663 |
| other/RasterTest3c.NES | no-protocol |  | 2734 |
| other/RasterTest3d.NES | no-protocol |  | 2448 |
| other/RasterTest3e.NES | no-protocol |  | 2839 |
| other/Retrocoders - Years behind.NES | no-protocol |  | 3655 |
| other/S0.NES | no-protocol |  | 1511 |
| other/SPRITE.NES | no-protocol |  | 1262 |
| other/Sayoonara!.NES | no-protocol |  | 1894 |
| other/SimpleParallaxDemo.nes | no-protocol |  | 991 |
| other/Streemerz_bundle.nes | no-protocol |  | 1837 |
| other/TANESPOT.NES | no-protocol |  | 2643 |
| other/TEST.NES | no-protocol |  | 987 |
| other/apocalypse.nes | no-protocol |  | 3256 |
| other/blargg_litewall-2.nes | no-protocol |  | 1497 |
| other/blargg_litewall-9.nes | no-protocol |  | 1551 |
| other/demo jitter.nes | no-protocol |  | 935 |
| other/demo.nes | no-protocol |  | 1017 |
| other/fceuxd.nes | no-protocol |  | 3452 |
| other/firefly.nes | no-protocol |  | 2914 |
| other/high-hopes.nes | no-protocol |  | 1881 |
| other/logo (E).nes | no-protocol |  | 2607 |
| other/manhole.nes | no-protocol |  | 1846 |
| other/max-300.nes | no-protocol |  | 1592 |
| other/midscanline.nes | no-protocol |  | 2366 |
| other/minipack.nes | no-protocol |  | 3128 |
| other/nescafe.nes | no-protocol |  | 3314 |
| other/nestest.nes | no-protocol |  | 2987 |
| other/nestopia.nes | no-protocol |  | 3275 |
| other/new-game.nes | no-protocol |  | 3474 |
| other/nintendulator.nes | no-protocol |  | 3322 |
| other/oam3.nes | no-protocol | (first run: crash; fixed, see below) | 3301 |
| other/oc.nes | no-protocol |  | 1994 |
| other/physics.0.1.nes | no-protocol |  | 3544 |
| other/pulsar.nes | no-protocol |  | 2599 |
| other/quantum_disco_brothers_by_wAMMA.nes | no-protocol |  | 3007 |
| other/rastesam4.nes | no-protocol |  | 1668 |
| other/read2004.nes | no-protocol |  | 3568 |
| other/snow.nes | no-protocol |  | 2466 |
| other/test001.nes | no-protocol |  | 2702 |
| other/test28.nes | no-protocol |  | 2586 |
| other/window2_ntsc.nes | no-protocol |  | 3123 |
| other/window2_pal.nes | no-protocol |  | 3018 |
| other/window_old_ntsc.nes | no-protocol |  | 1310 |
| other/window_old_pal.nes | no-protocol |  | 1460 |
| pal_apu_tests/01.len_ctr.nes | no-protocol |  | 2662 |
| pal_apu_tests/02.len_table.nes | no-protocol |  | 2818 |
| pal_apu_tests/03.irq_flag.nes | no-protocol |  | 2790 |
| pal_apu_tests/04.clock_jitter.nes | no-protocol |  | 3188 |
| pal_apu_tests/05.len_timing_mode0.nes | no-protocol |  | 3329 |
| pal_apu_tests/06.len_timing_mode1.nes | no-protocol |  | 2795 |
| pal_apu_tests/07.irq_flag_timing.nes | no-protocol |  | 2651 |
| pal_apu_tests/08.irq_timing.nes | no-protocol |  | 2772 |
| pal_apu_tests/10.len_halt_timing.nes | no-protocol |  | 3550 |
| pal_apu_tests/11.len_reload_timing.nes | no-protocol |  | 3324 |
| ppu_open_bus/ppu_open_bus.nes | fail ($6000=03) | Decay value should become zero by one second ppu_open_bus Failed #3 | 3431 |
| ppu_read_buffer/test_ppu_read_buffer.nes | fail ($6000=43) | [0;37mTEST:test_ppu_read_buffer ----------------------------- Testing basic PPU memory I/O. [0;37m | 2361 |
| ppu_vbl_nmi/ppu_vbl_nmi.nes | fail ($6000=01) | VBL period is too short with BG off 01-vbl_basics Failed #7 While running test 1 of 10 | 3176 |
| ppu_vbl_nmi/rom_singles/01-vbl_basics.nes | fail ($6000=07) | VBL period is too short with BG off 01-vbl_basics Failed #7 | 3381 |
| ppu_vbl_nmi/rom_singles/02-vbl_set_time.nes | fail ($6000=01) | T+ 1 2 00 - V 01 - V 02 V - 03 V - 04 V - 05 V - 06 V - 07 V - 08 V - C2633058 02-vbl_set_time Faile | 3504 |
| ppu_vbl_nmi/rom_singles/03-vbl_clear_time.nes | fail ($6000=01) | 00 V 01 V 02 V 03 - 04 - 05 - 06 - 07 - 08 - A937B9D5 03-vbl_clear_time Failed | 3246 |
| ppu_vbl_nmi/rom_singles/04-nmi_control.nes | pass | 04-nmi_control Passed | 3293 |
| ppu_vbl_nmi/rom_singles/05-nmi_timing.nes | fail ($6000=01) | 00 3 01 3 02 3 03 2 04 2 05 2 06 2 07 1 08 1 09 1 1A388616 05-nmi_timing Failed | 3288 |
| ppu_vbl_nmi/rom_singles/06-suppression.nes | fail ($6000=01) | 00 - N 01 - N 02 V N 03 V N 04 V N 05 V N 06 V N 07 V N 08 V N 09 V N 636EA6C0 06-suppression Failed | 3296 |
| ppu_vbl_nmi/rom_singles/07-nmi_on_timing.nes | fail ($6000=01) | 00 N 01 N 02 N 03 - 04 - 05 - 06 - 07 - 08 - 3D994949 07-nmi_on_timing Failed | 2941 |
| ppu_vbl_nmi/rom_singles/08-nmi_off_timing.nes | fail ($6000=01) | 03 - 04 - 05 - 06 - 07 - 08 - 09 - 0A N 0B N 0C N BA340B0D 08-nmi_off_timing Failed | 3768 |
| ppu_vbl_nmi/rom_singles/09-even_odd_frames.nes | fail ($6000=02) | 04 Pattern ----- should not skip any clocks 09-even_odd_frames Failed #2 | 3481 |
| ppu_vbl_nmi/rom_singles/10-even_odd_timing.nes | fail ($6000=02) | 02 Clock is skipped too soon, relative to enabling BG 10-even_odd_timing Failed #2 | 3653 |
| read_joy3/count_errors.nes | no-protocol |  | 1431 |
| read_joy3/count_errors_fast.nes | no-protocol |  | 1409 |
| read_joy3/test_buttons.nes | no-protocol |  | 2862 |
| read_joy3/thorough_test.nes | no-protocol |  | 1241 |
| scanline/scanline.nes | no-protocol |  | 2633 |
| scanline-a1/scanline.nes | no-protocol |  | 2734 |
| scrolltest/scroll.nes | no-protocol |  | 1715 |
| soundtest/SNDTEST.NES | no-protocol |  | 1007 |
| sprdma_and_dmc_dma/sprdma_and_dmc_dma.nes | fail ($6000=01) | T+ Clocks (decimal) 00 528 01 527 02 527 03 527 04 527 05 528 06 527 07 527 08 527 09 527 0A 527 0B  | 3810 |
| sprdma_and_dmc_dma/sprdma_and_dmc_dma_512.nes | fail ($6000=01) | T+ Clocks (decimal) 00 528 01 527 02 527 03 527 04 527 05 528 06 527 07 527 08 527 09 527 0A 527 0B  | 3318 |
| sprite_hit_tests_2005.10.05/01.basics.nes | no-protocol |  | 3246 |
| sprite_hit_tests_2005.10.05/02.alignment.nes | no-protocol |  | 3216 |
| sprite_hit_tests_2005.10.05/03.corners.nes | no-protocol |  | 3149 |
| sprite_hit_tests_2005.10.05/04.flip.nes | no-protocol |  | 2917 |
| sprite_hit_tests_2005.10.05/05.left_clip.nes | no-protocol |  | 2932 |
| sprite_hit_tests_2005.10.05/06.right_edge.nes | no-protocol |  | 2745 |
| sprite_hit_tests_2005.10.05/07.screen_bottom.nes | no-protocol |  | 3216 |
| sprite_hit_tests_2005.10.05/08.double_height.nes | no-protocol |  | 3070 |
| sprite_hit_tests_2005.10.05/09.timing_basics.nes | no-protocol |  | 2825 |
| sprite_hit_tests_2005.10.05/10.timing_order.nes | no-protocol |  | 3213 |
| sprite_hit_tests_2005.10.05/11.edge_timing.nes | no-protocol |  | 3248 |
| sprite_overflow_tests/1.Basics.nes | no-protocol |  | 3561 |
| sprite_overflow_tests/2.Details.nes | no-protocol |  | 3136 |
| sprite_overflow_tests/3.Timing.nes | no-protocol |  | 3304 |
| sprite_overflow_tests/4.Obscure.nes | no-protocol |  | 3047 |
| sprite_overflow_tests/5.Emulator.nes | no-protocol |  | 3443 |
| spritecans-2011/spritecans.nes | no-protocol |  | 2614 |
| stars_se/StarsSE.NES | no-protocol |  | 1472 |
| stomper/smwstomp.nes | no-protocol |  | 3224 |
| stress/NEStress.NES | no-protocol |  | 2228 |
| tutor/tutor.nes | no-protocol |  | 3022 |
| tvpassfail/tv.nes | no-protocol |  | 3452 |
| vaus-test/vaus-test.nes | no-protocol |  | 2892 |
| vbl_nmi_timing/1.frame_basics.nes | no-protocol |  | 2652 |
| vbl_nmi_timing/2.vbl_timing.nes | no-protocol |  | 2872 |
| vbl_nmi_timing/3.even_odd_frames.nes | no-protocol |  | 3589 |
| vbl_nmi_timing/4.vbl_clear_timing.nes | no-protocol |  | 2685 |
| vbl_nmi_timing/5.nmi_suppression.nes | no-protocol |  | 2462 |
| vbl_nmi_timing/6.nmi_disable.nes | no-protocol |  | 3160 |
| vbl_nmi_timing/7.nmi_timing.nes | no-protocol |  | 2569 |
| volume_tests/volumes.nes | no-protocol |  | 4829 |
| window5/colorwin_ntsc.nes | no-protocol |  | 2496 |
| window5/colorwin_pal.nes | no-protocol |  | 2304 |
