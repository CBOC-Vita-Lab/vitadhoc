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

## Results in 1.3

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

## Not fixed, and not checked

- **OAM order** (Golden Sun 1 and 2, Mario Golf, The Minish Cap): sprites
  that peek through a menu or are in the wrong order. This is a rendering
  detail of the real console that gpSP does not copy. Not fixed in 1.3; a
  script cannot judge it.
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
