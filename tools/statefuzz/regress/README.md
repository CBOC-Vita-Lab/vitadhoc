# State fuzz regression cases

A case that crashed or hung a loader is saved by the harness as `crash-*.st` / `hang-*.st` in its case directory (`OUTDIR/cases-<core>`
of `tools/statefuzz/statefuzz.sh`). Such a file is a whole game state, so it is not committed here. What is committed is the way to keep
one without the game:

    harness ROM DIR --base-out base.st [--base N]        # the machine's own state at the warm point (N: a later base state)
    python3 tools/statefuzz/recipe.py base.st crash-x.st > x.recipe     # only the changed bytes, text, a few lines
    harness ROM DIR --case x.recipe [--base N]           # loads base + recipe and runs frames: loaded or refused, never a crash or hang

A recipe belongs to one public test ROM and the core as it was when it was made (a later change to the core's state content moves the base;
`recipe.py` refuses a case that differs from its base in more than `--max` bytes, 4096 by default). Add a recipe here with the ROM it was
made from (a public or homebrew ROM only, named in the recipe's first comment line) when a new class of damage is found.

Classes found by the 2.0 fuzzing, each fixed in the loader and checked by the fuzz runs and the load proof (details in each core's
PROVENANCE file or LOCAL-PATCHES.md):

- Genesis / Master System: PSG and YM2612 counters and offsets, the audio buffer size, VDP table bases and viewport, Z80 words and bank,
  YM2413 table indexes, a pending VDP DMA on a Master System, the FM clock offset (`fm_cycles_start`).
- Super NES: timing block (refused before anything is applied), DMA mode, PPU table selectors, Super FX pointers, SPC/DSP voice positions.
- NES: a state is walked twice (checked, then applied); scroll and address registers, sound timestamps.
- WonderSwan: section sizes, sound buffer offsets, V30MZ segment prefix and cycle counter.
- GBA: cycle counts and timers (a tiny sound step looped 16 million times), BIOS size, PC in regions without memory, tile fetch bases.
