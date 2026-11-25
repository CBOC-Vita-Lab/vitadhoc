# Game Compatibility

What we know about the games people have reported problems with. Read the
caveats first: this is **not** a Vita test.

## How it was checked

Each game was started in a PC build of VitAdhoc's own engine (the same
program, with the pad, screen and sound replaced by files), once as 1.2 and
once as 1.3. Both the PC (x86) engine and the ARM engine the Vita uses were
run (the ARM one under an emulator, so slowly). A script pressed Start, A and
Right every few seconds for 45 to 480 seconds of game time, took a picture
every 2 seconds, and recorded the sound. The built-in BIOS was used, not a
real `gba_bios.bin`. Games were played from a fresh save.

What a script can see: the game starts, a "game was reset" event, a frozen or
all-black or all-white picture, and pictures that alternate every frame. What
it cannot see: wrong sprites, wrong colours in small places, flicker you only
notice while playing, how the music sounds, or how it feels. Anything in the
"not checked" column needs eyes and ears on a Vita.

## Results in 1.3 (no new per-game testing in 1.4)

| Game | Result | Notes |
|---|---|---|
| Mortal Kombat: Deadly Alliance | **Fixed in 1.3** | In 1.2 the Midway logo stayed for over 40 seconds and the title and name screens were covered in green speckles (a decompression call in the built-in BIOS read a wrong size from the game's data). Now the logo lasts a few seconds and the menus look right. We have not tried it with a real BIOS file. |
| Need for Speed: Underground | **Fixed in 1.3** | In 1.2 it froze at its language screen with no sound. Starts and plays in 1.3. |
| Big Mutha Truckers | Starts and runs | No reset in 8 minutes. The "Emulation fault - game was reset" report could not be repeated, in 1.2 or 1.3. |
| Smashing Drive | Starts and runs | Same: no reset in 8 minutes. Its sound is loud and clips a lot. |
| Street Racing Syndicate | Starts and runs | Same: no reset in 8 minutes. |
| Mario Golf: Advance Tour | Starts and runs | No black screen seen, in 1.2 or 1.3. Menus and a name entry reached. |
| Mario Tennis: Power Tour | Starts and runs | No white screen seen apart from the health and safety notice at the start (white by design, a few seconds). Menus and name entry reached. |
| Need for Speed: Underground 2 | Starts and runs | Reached a race. No alternating pictures seen. |
| Need for Speed: Porsche Unleashed | Starts and runs | Reached the menus. |
| SSX Tricky | Starts and runs | Reached a run. The sound clips a lot (loud mix), the same in 1.2. |
| Golden Sun, Golden Sun: The Lost Age | Start and run | Intro and first rooms played. |
| F-Zero GP Legend | Starts and runs | Reached the pilot screen. |
| The Legend of Zelda: The Minish Cap | Starts and runs | Intro and file select. |
| Castlevania: Aria of Sorrow, Driver 2 Advance, Mario Kart: Super Circuit, Pokemon FireRed, Pokemon Emerald | Start and run | Only tried on the PC engine. |

1.2 and 1.3 gave identical pictures and sound in every game above except the
two fixed ones (and small timing differences in a game's random choices).

## NES, WonderSwan and Super NES in 1.4

- **NES and WonderSwan** (new in 1.4) have been run only with synthetic test
  cartridges (NES) and test cartridges plus one game (WonderSwan), on the PC
  build. No commercial NES game has been tried, so there is no list of games
  that work yet. Famicom Disk System games need your own `disksys.rom`.
  Pocket Challenge V2, WonderWitch and WonderSwan games with a clock are
  untested. Neither console has run on a real Vita.
- **Super NES** was checked on 15 commercial games the tester owns, three
  thousand frames each, for wide view (never a wrong tile) and for Super FX
  speed (identical pictures and sound before and after). European (PAL) games
  now run at their true 50 Hz speed. Not a Vita test.
- **GBA** games that rewrite their own code (a self-modifying game) flush only
  the part that changed in 1.4. Software-3D games (bitmap modes) start on
  sharp bilinear so they are not asked to redraw a whole smoothed picture each
  frame. Sprite priority is exact in 1.4; the OAM-order check is in "Sprite
  order" below.

## Game Boy test suites

The Game Boy and Game Boy Color core (TGB Dual) was run headless, on the PC
build, against public test ROMs: blargg's suites (serial text, screen text or
the cartridge-RAM report), dmg-acid2 and cgb-acid2 (the screen against the
reference picture). Pass counts, **host (PC x86)**, not a Vita test. Runner:
`gpsp/gbcore/tests/run_suites.py`; the results the gate holds the core to are
in `gpsp/gbcore/tests/suites-expected.txt`.

| Suite | TGB Dual before | TGB Dual after | Gambatte (Accurate) |
|---|---|---|---|
| blargg cpu_instrs, 11 single ROMs | 2 / 11 | 11 / 11 | 11 / 11 |
| blargg cpu_instrs, combined ROM | 0 / 1 | 1 / 1 | 1 / 1 |
| blargg instr_timing | 1 / 1 | 1 / 1 | 1 / 1 |
| blargg interrupt_time | 0 / 1 | 1 / 1 | 1 / 1 |
| blargg halt_bug | 0 / 1 | 1 / 1 | 0 / 1 |
| blargg dmg_sound (12 singles + combined) | 7 / 13 | 13 / 13 | 13 / 13 |
| blargg cgb_sound (12 singles + combined) | 8 / 13 | 13 / 13 | 13 / 13 |
| blargg mem_timing and mem_timing-2 (3 singles + combined each) | 0 / 8 | 0 / 8 | 8 / 8 |
| blargg oam_bug (8 singles + combined) | 2 / 9 | 2 / 9 | 3 / 9 |
| dmg-acid2 | fail | pass | pass |
| cgb-acid2 | fail | pass | pass |
| Total | 20 / 60 | 45 / 60 | 53 / 60 |

The Gambatte column is the Accurate core (`gbcore/gambatte`), run the same way (`run_suites.py --core gambatte`;
results the gate holds it to: `gbcore/tests/suites-expected-gambatte.txt`; gate steps `gb-suites-gambatte` and
`gb-gambatte-core`). Its blargg halt_bug fails when the ROM runs as a Game Boy Color (the ROM's header asks for
colour) and passes when forced to a Game Boy; the oam_bug tests that need the sprite-memory corruption are not
emulated. Gambatte's costs are in Known Issues (host instruction counts, not a Vita).

Mooneye's acceptance tests were not run: they need the WLA-DX assembler,
which could not be built here. The runner and the gate step support them
(`GATE_GB_MOONEYE`).

Still failing: mem_timing (the core runs a whole instruction and then
advances the clock, so a read or write lands on the wrong cycle inside it),
oam_bug (the corruption a 16-bit increment does to sprite memory is not
emulated; its timing tests also depend on cycle-exact reads). Games that
depend on those are rare.

The sound tests for sweep details and for wave RAM read, trigger and write
while the channel plays now pass too (2.0): the sweep rewrites the channel's
frequency registers, and channel 3 has its own clock, so a wave RAM read or
write while it plays finds the byte it is on (a Game Boy Color), or only
right after the channel fetched it (a Game Boy), and a retrigger on a Game
Boy at that moment corrupts the start of wave RAM. The sound itself
changes only for those rare cases, for a sweep followed by a frequency
write, and for register writes landing a few clocks later (well under one
sample).

## Sprite order (OAM order)

Golden Sun 1 and 2, Mario Golf and The Minish Cap were reported with sprites
that peek through a menu or overlap in the wrong order. The cause is a rule of
the real console: among sprites, a later sprite takes a pixel over only with a
strictly better priority, and a transparent pixel of such a sprite still hands
its priority to the sprite already there. gpSP drew by priority only. 1.4 has
the rule (docs/TECHNICAL.md, OBJ_EXACT); 2.0 rechecked it on the PC build:

- Golden Sun 1 (opening, dialogue boxes over the party) and Mario Golf (title,
  menus, the first walk) were played by script and one frame in three was
  compared, line by line, with the sprite and background renderer of mGBA
  (reference only, run on the same video memory and registers). Golden Sun:
  1198 frames, 5 differed with the old drawing (1 to 27 pixels each, around
  a Psynergy ring) and none with the exact one; Mario Golf: 998 frames, none
  differed either way. Frames where the game changes video memory in the
  middle of the picture (about 6 percent) cannot be replayed that way and were
  left out; the colour blend rounds one step differently from mGBA and was
  allowed one step.
- A test cartridge of our own with 64 random sprites (every priority, holes,
  affine) over three backgrounds is checked pixel by pixel against the rules
  (`app_tests.py --only objrand`): 16 pixels are wrong with the old drawing,
  none with the exact one.
- Not covered: sprites of the "object window" mode (they do not take part in
  the rule), the menus of The Minish Cap and the Golden Sun battle screens
  (not reached), and Golden Sun 2 (not available). Not a Vita test.

## Not fixed, and not checked

- **Flicker** (Need for Speed games, SSX Tricky): no picture that alternates
  every frame was found in the races we reached. Flicker that is part of how
  the game draws would not show up this way.
- **Sound** (Golden Sun and other Camelot games, F-Zero GP Legend missing
  instruments): the sound has the same measurements in 1.2 and 1.3. We cannot
  tell by script whether an instrument is missing.
- **Resets and black or white screens on a real Vita:** none of them could be
  repeated on the PC engines. If you still see one, please open an issue with
  the game, its region, whether you use your own BIOS, and what you did just
  before.
