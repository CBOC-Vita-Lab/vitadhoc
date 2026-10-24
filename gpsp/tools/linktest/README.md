# linktest: a link-cable test ROM we can ship

`linktest.gba` is a small homebrew GBA program that uses the serial port the
way linked games do, checks every word it receives, and leaves its verdict at
a fixed address. It exists so that a dynarec backend can be shown to carry the
two-machine cable (`docs/GBA-LINK-MIRROR.md`) without a commercial ROM: on the
desktop x86 build, on the ARM32 backend under `qemu-arm`, and with the ARM
translation caches mapped far from the program as on a PS Vita.

It is part of this project (GPL-2.0). Sources: `crt0.s`, `main.c`, `iwram.c`,
`linktest.h`, `linktest.ld`. No Nintendo logo is included, so a real BIOS will
not boot it; the open-source BIOS gpSP embeds does.

    make                 # arm-none-eabi-gcc + python3, no devkitPro, no libc
    make FLAGPROBE=1     # linktest_flagprobe.gba, see "Flag probe"

The built ROMs are committed, so the rigs below need no GBA toolchain.

## What it does

Both machines run the same ROM. Each enters multi-player mode and waits; the
first exchange that carries the parent's and the child's opening words, each in
its own slot, decides the roles from SIOCNT bit 2 (SI: 0 parent, 1 child) once
bit 3 (SD) says both are in multi-player mode. With no partner after 150
frames it runs the cable code alone (role 3). Then it loops through ten phases
for ever:

| # | mode | who starts, how paced | checked |
|--:|---|---|---|
| 1 | multi-player, 115200 | parent, one transfer a frame (V-blank IRQ) | 32 words each way against a sequence both sides compute; own word read back in its SIOMULTI slot |
| 2 | multi-player, 115200 | parent, bursts from timer 2 (one per 16384 cycles) | 96 words each way |
| 3-5 | multi-player, 9600 / 38400 / 57600 | parent, busy bit polled, serial IRQ off on the parent | 4 words each way per speed |
| 6 | multi-player | one a frame | 16 words carrying each side's buttons, each echoed back by the other and compared |
| 7 | normal 8-bit, parent clocks (256 kHz and 2 MHz alternating) | 12 a frame, then 12 from timer 2 | child: data, start cleared, SI equals the SO level the parent set; parent: receives 0 (one-way cable), SI low |
| 8 | normal 32-bit, parent clocks | as 7 | as 7 |
| 9 | normal 32-bit, **child** clocks, parent armed as slave | child, one a frame | child receives the parent's word; parent receives 0 |
| 10 | general purpose | parent drives SC (clock), SD, SO from timer 2: 32 steps; then the child drives SC, SD: 8 steps | child polls RCNT for SC edges and checks SD and SI at each, and counts SI falling-edge IRQs; parent samples SC/SD once a frame |

Every phase starts with a SYNC word pair and ends with a REPORT pair in
multi-player mode: each side tells the other how many checks it passed and
whether any failed, and the receiver checks the count against what the phase
should produce. Phases 7-10 leave multi-player mode in between, the child at
its "safe line" (below), and come back.

While all this runs, the main loop does work meant to be awkward for a
dynarec, every frame:

- ROM (Thumb) code rewrites an instruction of the IWRAM (ARM) checksum mixer
  that the serial interrupt handler calls on every transfer, then checks the
  mixer computes with the new instruction;
- an IWRAM routine rewrites one of its own instructions and runs it;
- Div and Sqrt BIOS calls made from IWRAM (the dynarec replaces Div by a
  helper in its ROM cache; Sqrt is a direct branch from RAM-cache code to the
  BIOS SWI block);
- a Thumb routine in EWRAM, installed by DMA3 or by the CPU, an immediate
  patched in place, with a Div SWI inside.

Interrupts in use: V-blank, V-count (line 6), timer 2 (pacing), timer 3 (a
65536-cycle tick that drifts through the frame), serial. The handler and the
whole protocol are IWRAM code (`iwram.c`).

The screen (mode 3) shows a band (left: red = parent, blue = child, grey = no
partner; right: green until anything fails) and eight rows of hex digits:
loop/role/phase, checks passed, checks failed, checks the other side reported,
checksum of received words, own and received buttons, code checks, transfers.

## The verdict: EWRAM 0x02000000

`struct results` in `linktest.h`; 32-bit words:

| offset | field |
|---|---|
| 00 | magic `LKT1` (0x31544B4C) |
| 04 | role: 1 parent, 2 child, 3 no partner |
| 08 | V-blanks seen |
| 0C | completed loops through all ten phases |
| 10 | phase in progress |
| 14 | **failed checks (must be 0)** |
| 18 | passed checks |
| 1C | running checksum of every word received |
| 20 / 24 | multi-player / normal transfers completed |
| 28 / 2C | passed checks / failed phases the other side reported |
| 30 / 34 | self-modifying-code checks passed / **failed (must be 0)** |
| 38 | **time-outs (must be 0)** |
| 3C | checksum of timer readings taken in the transfer handlers |
| 40-50 | interrupt counts: V-blank, V-count, timer 2, timer 3, serial |
| 54 / 58 / 5C | buttons seen; checksum of key words sent / received |
| 60 / 64 / 68 | first failure: `phase<<24 | code<<16 | index`, got, expected |
| 6C | SI falling-edge interrupts (child) |
| 80.. / C0.. | passed / failed checks by phase (index 1-10; 11 = no partner) |

`check.py` reads `gl_host --peek` output and decides pass or fail.

## Running it

    cd tools/gbamirror
    make                              # gl_host, x86 dynarec
    make -f Makefile.armhf            # gl_host_arm, gl_host_armfar, gl_host_armctl
    ./linktest_rig.sh OUT 20000 -- ./gl_host
    ./linktest_rig.sh OUT 20000 -- qemu-arm -L /usr/arm-linux-gnueabihf ./gl_host_arm
    GL_JIT=far   ./linktest_rig.sh OUT 20000 -- qemu-arm -L /usr/arm-linux-gnueabihf ./gl_host_armfar
    GL_JIT=split ./linktest_rig.sh OUT 20000 -- qemu-arm -L /usr/arm-linux-gnueabihf ./gl_host_armfar

`linktest_rig.sh` runs the pair, a rerun, `--swap`, three lookaheads, a second
input script (the result must change), and the yield test, and writes
`OUT/summary.txt`. One run by hand:

    ./gl_host --rom ../linktest/linktest.gba --bios-dir /nonexistent \
        --frames 20000 --script0 ../linktest/keys0.txt --script1 ../linktest/keys1.txt \
        --peek 0x02000000:28 --peek 0x02000080:16 --peek 0x020000c0:16 \
        | python3 ../linktest/check.py --frames 20000

`--bios-dir` pointing at a directory without `gba_bios.bin` selects the
built-in open-source BIOS.

## Why the hashes can be compared across lookaheads

The pair stepper delivers what the child does on the cable to the parent up to
`lookahead` cycles late (default half a frame). A program whose parent watches
the child's pins continuously therefore runs differently at different
lookaheads; that is the design, not a fault. This ROM is written so that it
does not: the child changes its mode or its pins only in its V-count interrupt
at line 6, and the parent looks at child-driven state only in its V-blank
interrupt at line 160, more than half a frame later. Everything else the two
exchange is either started by the parent (delivered on time) or a reply the
parent waits for. So `--lookahead 2464 / 5000 / 10000` and the default must
give identical hashes, and a difference is a finding.

Two were found this way (2026-10-04):

1. **A halted parent's transfer ended one cycle late when it had to wait for
   the child** (`gbalink_core.c`, `gbalink_halt_yield`): the halt loop tested
   for the missing reply after the iteration that reached the transfer's end,
   so the end moved by a cycle with the lookahead. First visible at linked
   frame 35, in the timer the serial handler reads. The opt-in
   `-DGBALINK_HALT_BLOCK_EXACT` tests before that iteration; with it all four
   lookaheads are identical. Off by default: 3.5.0 behaviour is unchanged.
   `Makefile.armhf` builds with it (`LINK_EXACT=0` to turn it off);
   `make CORE_EXTRA=-DGBALINK_HALT_BLOCK_EXACT` for the x86 tool.
2. **The flags an interrupt handler is entered with depended on where the
   stepper had stopped the core.** Raising an interrupt stores CPSR = 0xD2;
   a core re-entered from C takes its flags from that, a core that continues
   inside the stub kept the interrupted code's flags. Fixed in the ARM stub
   (`arm/arm_stub.S`, `update_gba_flags_from_cpsr`, link builds only). The x86
   stub has the same property and is unchanged; on x86 it shows as isolated
   frames whose register hash differs (a frame that ends in the first
   instructions of a handler) in the yield and lookahead comparisons.

## Flag probe

No real program looks at the flags it is interrupted with, so finding 2 is
invisible to `linktest.gba` except when a frame happens to end at a handler's
first instructions. `linktest_flagprobe.gba` folds those flags into
`results.pad[0]` (0x02000070) on every interrupt, which makes the difference
permanent and immediate. `linktest_rig.sh --flagprobe` runs the yield and
lookahead comparisons on it: identical on the ARM backend with the fix,
different from frame 7 without it, different on x86.

## Input scripts

`keys0.txt` / `keys1.txt` (and `keys1b.txt`, a different slot-1 script) are
`gl_host --script0/--script1` files. Phase 6 sends the buttons across, so a run
with `keys1b.txt` must hash differently from one with `keys1.txt` and still
pass; the rig checks both.
