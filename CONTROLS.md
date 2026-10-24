# Controls

These are the defaults. Settings > Controls changes game buttons, shortcuts and
turbo buttons, and the menus always stay reachable, whatever you map.

## Home Screen

| Button | Does |
| --- | --- |
| Left / right | Move along your games |
| Cross | Play, or **Continue** where you left off |
| Up | **Start over** from the battery save |
| Down | Show the game's save states; pick one and press Cross |
| Square | Star or unstar the game |
| Select | All games, Starred, Recent |
| Triangle | Switch between GBA, GB and GBC |
| Circle | Find a game by name |
| Start | Settings |
| Touch | Tap games, buttons and hints; drag or flick lists |

## In A Game

| Button | Does |
| --- | --- |
| D-pad / left stick | D-pad |
| Cross / Circle | A / B |
| L / R, Start, Select | The same buttons |
| Hold Start + Select, or tap the game | Open the menu |
| Square | Fast forward (hold or toggle, in Settings) |
| Right stick right | Fast forward while held |
| Right stick left, or Select + Square | Rewind while held |
| Select + L / Select + R | Quick save / quick load |
| Triangle | Next picture preset |
| Select + Triangle | Pause screen |
| L + R + Select | Screenshot (saved in `ux0:data/GBAdhoc/screenshots/`) |
| Select + Down | Mystery Gift: listen for a phone (GBA) |
| Right stick down | Cheat button, for button-activated codes |

## Motion Games

WarioWare: Twisted!, Yoshi Topsy-Turvy, Koro Koro Puzzle and Kirby Tilt 'n'
Tumble feel the Vita turn and tilt. Settings > This game switches them to the
right stick, changes the strength or direction, and sets level: hold the Vita
the way it should count as flat and press Cross on Level. Boktai's sunlight is
set there too.

## Turbo Buttons

Turbo A, Turbo A (2), Turbo B and Turbo B (2) start unset. Give each a button,
choose hold or toggle, and 10, 15 or 30 presses a second. They rest during link
play and stop when the menu opens or a state loads.

## Two Players On One Vita

Flick the right stick sideways, or tap the other picture, to switch which
Game Boy you control. Tap the one you control for the menu. A second controller
can give each player their own pad; see [Play Together](PLAY-TOGETHER.md).

Start and Select reach the game a tenth of a second late, so a shortcut such
as Select + L doesn't also press Select in the game. `chord_guard = 0` in
`config.ini` turns that off.
