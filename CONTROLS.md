# Controls

These are the build-21 defaults. Settings > Controls can change game buttons,
shortcuts and turbo bindings. Menu navigation stays available even after a
bad game mapping.

## Game Browser And Menus

| Input | Action |
| --- | --- |
| Left / right | Move through the game shelf |
| Cross | Start the selected game or confirm |
| Circle | Back |
| Triangle | Switch GBA, GB and GBC lists |
| Square | Favourite or unfavourite the selected game |
| Select | Show favourites |
| Down on the shelf | Open that game's save-state cards |
| Start | Settings |
| Front touch | Select/tap rows, cards and footer hints; drag/flick scrollable lists |

## During A Game

| Input | Action |
| --- | --- |
| D-pad / left stick | Emulated D-pad |
| Cross / Circle | A / B |
| L / R | GBA L / R |
| Start / Select | Game Start / Select |
| Hold Start + Select | Open the app menu |
| Tap the game picture | Open the app menu |
| Square | Fast forward, according to its Hold/Toggle setting |
| Right stick right | Fast forward while held |
| Select + Square / right stick left | Rewind while held |
| Right stick down | Cheat activation button for supported button-activated codes |
| Triangle | Cycle video presets |
| Select + L / Select + R | Quick save / quick load |
| Select + Triangle | Pause/wake overlay |
| L + R + Select | Save screenshot to `ux0:data/GBAdhoc/screenshots/` |
| Select + Down | Start/stop the GBA Mystery Gift listener |

The Start + Select escape chord cannot be rebound into a shortcut. The PS
button belongs to the Vita system, not the PSP HOME-button option in legacy
code. Start/Select reach the game after a short guard interval so a shortcut
does not also press the game button. `chord_guard = 0` in `config.ini` disables
that guard.

## Turbo And Remapping

Four turbo actions start unbound: Turbo A, Turbo A (2), Turbo B and Turbo B (2).
Choose a button, Hold or Toggle, and 10, 15 or 30 presses per emulated second.
Fast forward changes their real-time rate. Turbo rests during link/local-pair
play and resets on menu entry, state load or leaving the game. Conflict prompts
show when a new mapping takes an existing input; Reset restores defaults.

## Touch And Two Players

Front touch operates menus; it is not an on-screen GBA controller. A tap
activates on release, while a drag scrolls or changes selection without
accidentally confirming. `touch = 0` in `config.ini` or `touch_menu = 0` in
`vita.ini` disables the in-game picture tap. The rear panel and motion sensors
have no assigned emulator controls.

With two emulated handhelds on one Vita, a sideways right-stick flick or a
tap on the other picture changes the controlled player. Tap the active picture
for the menu. A separate second controller can give each player a pad, but
controller-port behaviour has not been accepted on physical hardware. PS TV
has no touch panel; use the buttons and Start + Select. See
[Play Together](PLAY-TOGETHER.md) for controller requirements and limits.
