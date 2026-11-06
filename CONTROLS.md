# Controls

These are the defaults. Settings > Controls changes game buttons, shortcuts and
turbo buttons, and the menus always stay reachable, whatever you map.

**Cross or Circle?** The names below are for a Vita whose Enter button is Cross
(most of the world). VitAdhoc follows your Vita's own setting (Settings >
System > Enter Button Assignment): with Circle as Enter, Circle plays, picks
and is A, and Cross is back and B. Settings > Controls > A and B buttons
(Circle is A / Cross is A) changes it for VitAdhoc alone.

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
| Cross / Circle | A / B (swapped with Circle is A) |
| L / R, Start, Select | The same buttons |
| Hold Start + Select, or tap the game | Open the menu |
| Square | Fast forward (hold or toggle, in Settings) |
| Right stick right | Fast forward while held |
| Select + Square | Rewind while held (the right stick left too, if you turn on Rewind with right stick) |
| Select + L / Select + R | Quick save / quick load |
| Triangle | Next picture preset |
| Select + Triangle | Pause screen |
| PS + Start | The Vita's own screenshot |
| Select + Down | Mystery Gift: listen for a phone (GBA) |
| Right stick down | Cheat button, for button-activated codes |

VitAdhoc's own screenshot (L + R + Select, saved in
`ux0:data/GBAdhoc/screenshots/`) is off. Settings > Controls > Screenshot
button combo turns it on; Button mapping can give Screenshot any other chord.

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
can give each player their own pad, and either player opens the menu with
Start + Select on their own pad. With no second controller, Player 2 on the
touch screen puts a pad on the front screen; see
[Play Together](PLAY-TOGETHER.md).

Start and Select reach the game a tenth of a second late, so a shortcut such
as Select + L doesn't also press Select in the game. `chord_guard = 0` in
`config.ini` turns that off.
