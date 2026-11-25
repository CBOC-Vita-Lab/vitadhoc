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
| Triangle | Open the console list under the top bar, with the console in use highlighted; each further Triangle or Down moves on, Up goes back; it closes by itself after a moment, or on any other button (or tap the console name, then a row) |
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
| Square | Fast forward (hold or toggle; 2x, 3x, Unlimited or Unlimited Smooth, with or without sound, in Settings > Fast-forward) |
| Right stick right | Fast forward while held (the Super NES and Sega players too) |
| Select + Square | Rewind while held (the right stick left too, if you turn on Rewind with right stick; in the Super NES and Sega players the right stick left is the way) |
| Select + L / Select + R | Quick save / quick load |
| Triangle | Next picture preset |
| Select + Triangle | Pause screen |
| PS + Start | The Vita's own screenshot |
| Select + Down | Mystery Gift: listen for a phone (GBA) |
| Right stick down | Cheat button, for button-activated codes |

SNES and Sega games (the separate players) open their menu the same way: hold
Start + Select (L + R + Start also works there). Settings > Controls > **Tap
screen for menu** turns the tap on the game off everywhere, the players
included; the chords stay. In those players Controls > Left stick can be
D-pad (the default, as in the main app), Off or its own buttons.

VitAdhoc's own screenshot (L + R + Select, saved in
`ux0:data/GBAdhoc/screenshots/`) is off. Settings > Controls > Screenshot
button combo turns it on; Button mapping can give Screenshot any other chord.

## Rear Touch Panel And Button Test

The back of the Vita is a touch panel. **Settings > Controls > Rear touch
panel** (Off by default) makes its four corners (top left, top right, bottom
left, bottom right) inputs like any button: **Button mapping** can give each one
any game button, shortcut or turbo button, alone or in a chord. The panel is split
in the middle both ways, so a finger lights one corner and two fingers two. A
game can have its own choice: **Settings > This game > Rear touch panel**
(Settings, Off or On). The Super NES and Sega players have the same row in their
Controls page. Nothing is on a corner until you put it there; mind that the
grips of the Vita rest against the panel, so give a corner a button you do not mind
pressing by accident. On the WonderSwan, which plays the Vita's own buttons, a
corner bound to a game button presses what that button's key presses.

**Settings > Controls > Button test** shows every button, both sticks, the touch
screen and the four rear corners live, and what each does for the console on
screen, from your current mapping. Use it to check a worn button or a mapping.
Every button is being tested, so none of them leaves the page: hold Start +
Select for half a second. The Super NES and Sega players have it on the
Controls page of their menu (Start + Select, Settings, Controls).

## NES Zapper

Games that use the NES Zapper (Duck Hunt, Hogan's Alley, Wild Gunman, Gumshoe and
the others the game database lists) use the front touch screen as the gun: touch
the picture where you want to aim, and the trigger is pulled while the finger is
down. In those games a tap does not open the menu; hold Start + Select. Nothing
is drawn on the picture. **Settings > This game > Port 2** (NES only) is Auto
(the database decides), Zapper (any game) or Controller (never); it is read when the
game starts.

## Sega Genesis / Mega Drive, Sega CD, Master System, Game Gear, SG-1000

These play in `md.bin` (Sega CD with the Genesis pad: the same buttons). Every button can be remapped per game in its menu,
which opens with Start + Select held (or a tap on the game).

| Vita | Genesis / Mega Drive | Master System, SG-1000 | Game Gear |
| --- | --- | --- | --- |
| D-pad / left stick | D-pad | D-pad | D-pad |
| Square | A | | |
| Cross | B | 1 | 1 |
| Circle | C | 2 | 2 |
| L / Triangle / R | X / Y / Z (six-button games) | | |
| Start | Start | Pause | Start |
| Select | Mode (a tap) | | |

Three-button games use A, B and C only.

On the Genesis the face buttons belong to the game, so fast forward and rewind
are on the right stick (right fast forwards, left rewinds). Start + Select
opens the menu; Select alone, tapped, is Mode. The pad can be remapped per game
in the menu.

## WonderSwan

| Held | Sideways game | Upright game (Vertical play: Upright) | Vita turned on its side |
| --- | --- | --- | --- |
| D-pad | the cross | the cross, turned with the picture | the cross |
| Circle / Cross | A / B | the second cross | the second cross |
| Triangle / Square | Rewind / Fast-forward | the second cross | the second cross |
| L / R | | A / B | A / B |
| Start | Start | Start | Start |

The game turns the picture and the buttons together; the first time, a hint
says so. Rotation and Vertical play are in Settings > This game. Button
mapping does not change WonderSwan buttons.

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
