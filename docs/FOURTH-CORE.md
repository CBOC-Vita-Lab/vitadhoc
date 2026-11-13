# The fourth CPU core

The PS Vita has four CPU cores. Apps normally get three. The fourth is reserved for the system, and an app can use it only when the console runs a kernel plugin that unlocks it (CapUnlocker is the one we have tested with in mind). VitAdhoc can use that core, but only when it is sure the core really works, and it never needs it.

## What it is

Players and the main app run several threads: emulation, sound, picture work, saving. On three cores some of them share. With the extra core the sound thread of the SNES player moves to it, so the sound-chip and the picture threads each keep a core to themselves; the main app's picture-scaling helpers and render thread get the same kind of room (see `playerkit/INTEGRATION.md`, section 9, for exactly which threads).

## Safe without the plugin

- At start the app runs a 10 ms test on that core. If the core works (about all of the time went to the test) it is used. If the system refuses the request, or the test got almost no CPU time, the app runs exactly as before: same threads, same cores, same priorities. A bad-luck result is tested again, up to 6 times 40 ms apart, so one noisy reading does not cost you the core for the session.
- If the system takes the core back while a game is running, the app notices within about a fifth of a second, moves the thread back to its old core, and goes on. At worst you hear one short glitch. It tries again later, at most twice a session.
- Nothing changes in saves, states, settings or controls.

## What changes with it

Only where threads run. The game runs at the same speed; what you may see is more headroom (fewer slow frames and sound gaps in demanding scenes, such as heavy filters or the wide view). It does not raise the clock speed; that is up to your clock plugin.

## The setting and where to see it

- SNES player: `ux0:data/GBAdhoc/snes/threads.ini`, line `core4=auto` (default) or `core4=off`.
- Main app: `vita.ini`, line `core4 = auto` (default) or `core4 = off` (once the main-app change in `playerkit/INTEGRATION.md` 9.4 is made).
- The log (`log.txt`) shows the test (`cores: probe 1 of 6 ...`) and one summary line: how many attempts, what share the core gave, the decision, whether the 4-core layout is in use and whether the watchdog ever stepped in. About shows one line: `Extra CPU core: in use` or `Extra CPU core: not available (needs the CapUnlocker plugin)`.

## Honest status

Everything here is tested on a PC with a simulated kernel. It has not run on a console yet; the owner's test procedure is in `playerkit/INTEGRATION.md` 9.5.
