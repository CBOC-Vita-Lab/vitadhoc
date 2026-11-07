# Play Together

Open the in-game menu > Play together. VitAdhoc does GBA Link Cable, GBA
Wireless Adapter and Game Boy / Game Boy Color Link Cable games. Back up an
important save before linking: two-Vita play is still being confirmed on
hardware for version 1.0 (see [Known Issues](KNOWN-ISSUES.md)).

## Two Vitas

Use the same VitAdhoc version on both consoles. A patched game (a ROM hack or
translation) needs the same patch on both. Vita-to-PSP linking is not
supported. Both players must have permission to use the cartridges involved.

| Connection | Setup |
| --- | --- |
| Wi-Fi | Join the same router or hotspot. One player chooses Host a game; the other chooses Join a game and selects the discovered room. |
| Internet | Join the host address shown by the app. The host needs reachable UDP port 19019; automatic UPnP may open it, otherwise router configuration is needed. Do not expose unrelated services or disable a firewall. |
| Ad hoc | Choose Ad hoc and use the Vita's system connection screen. No router is needed. |

GBA cable sessions can link two different games: Ruby with Sapphire, FireRed
with LeafGreen, Emerald with FireRed, the way the real cable allows. Each of
you opens your own game and one hosts; the other joins, and the room list
shows both games ("FireRed <-> Emerald"). The first time, each Vita sends the
other its game over the link (16 MB takes seconds on one Wi-Fi network and
longer over the internet, where the screen shows a time estimate and you can
cancel). It is checked, then kept in the app's own cache folder
(`ux0:data/GBAdhoc/cache/link/`, a hashed file name, up to 4 games or 96 MB,
oldest removed first), never in your game folders and never listed as a game,
so every later session starts at once. Delete the files there any time. Your
own save stays on your own Vita; the other player's save travels to your
Vita for the session only. Both players must own their games: do not use this
to share a game you are not allowed to share. Playing the same game on both
Vitas needs no transfer. The two games still need to be able to link to each
other (a Pokemon Ruby will not trade with Mario Kart): VitAdhoc checks only
that both are Game Boy Advance games, as a cable would. To trade between
versions on one Vita, use Two players on one Vita (below).
GBA Wireless Adapter
sessions need a game that supports the adapter. GB/GBC cable mode can use
different cartridges, and a Game Boy (`.gb`) cartridge can play with a Game
Boy Color (`.gbc`) one: Red with Blue, Yellow, Gold, Silver or Crystal, for
example. Each Vita runs both games and the implementation exchanges cartridge
bytes in memory for the session, not to storage. Do not use it to distribute a
cartridge you are not allowed to share.

Fast link, in Settings > Link setup, batches two frames of GBA cable input.
It is off by default and the host's setting controls the session. It does not
make a weak internet connection reliable. Cheats suspend while linked and
return afterwards; achievements pause and start counting again afterwards. Rewind, turbo and preview recording are not normal linked
features. Three- and four-player links and single-cartridge multiboot are absent.

## Two Players On One Vita (split screen)

Play together > Two players on this Vita runs two emulated handhelds in one
VitAdhoc, joined by a virtual link cable, and shows them side by side: two
equal pictures, the player 1 picture on the left, player 2 on the right. This
is the split-screen mode. There are three ways to give player 2 a way to play.

**What you need (pick one)**

1. **A PS TV (or a Vita on a TV) with two controllers.** Pair two DualShock 3
   or DualShock 4 pads in the system settings. The first pad is player 1; the
   second is player 2.
2. **A handheld Vita with an external pad through a plugin.** The Vita's own
   buttons stay player 1's. A plugin that pairs a DualShock (ds4vita, ds3vita,
   ds34vita, MiniVitaTV and similar) makes the pad show up as a second
   controller. VitAdhoc takes the second paired pad as player 2. If your plugin
   also copies the pad onto the Vita's own buttons, both players' game gets that
   pad's presses; turn that option off in the plugin. No plugin is included or
   installed by the app.
3. **One Vita, no controller: Player 2 on the touch screen.** In the two-player
   page choose Player 2 on the touch screen: on. Player 2's half of the front
   screen shows a translucent D-pad, A, B, L, R, Start and Select. Player 1
   plays with the buttons. Both can press at the same moment (the touch panel
   reads several fingers). It is off until you turn it on, and the Vita
   remembers the choice. If a second controller is connected, the controller
   is used instead.

**While you play**

- Each player's game gets only their own buttons. The right stick sideways
  changes whose sound you hear (only one game makes sound at a time).
- Either player can open the menu: hold Start + Select on your own pad. The
  menu stops both games; when it closes, nothing held reaches either game
  until it is let go. Either pad can move around the menu.
- With two pads or the touch pad the layout is always side by side. With one
  pad only (Layout setting) you can choose large + small, and the right stick
  or a tap switches which game the pad drives.
- A GBA game is shown at 2x (480 x 320 each, as large as two fit side by
  side). A Game Boy or Game Boy Color game is shown at 3x (480 x 432 each).
- A second controller that is plugged in or switched off during play is picked
  up within a moment.
- The second handheld starts from a copy of your save, from its own
  `<game>.p2.sav`, or runs a different cartridge with that game's own save.

**Which games this is for**

Split screen is for games that link two players with a cable, with each player
on their own screen, as if two handhelds were cabled together:

- GBA games that use the Link Cable for two players: trading and battling
  (Pokemon Ruby, Sapphire, FireRed, LeafGreen and Emerald are linked this way
  in VitAdhoc's game table), and two-player modes of other games that need a
  cartridge each (racing, versus and battle modes of the kind Mario Kart
  Super Circuit has).
- Game Boy and Game Boy Color games that use the Game Boy Link Cable for two
  players: trading and battling, and versus modes in puzzle and action games.
  Game Boy (`.gb`) and Game Boy Color (`.gbc`) games are one family, as on
  real hardware where one cable joins a Game Boy and a Game Boy Color. Pick
  any of them as the partner's game, whatever the file ends in, and each
  handheld runs as its own kind: a `.gb` game as a Game Boy, a `.gbc` game as
  a Game Boy Color. That is what trading between different versions needs:
  Red, Blue and Yellow with each other, those with Gold, Silver and Crystal
  (the Time Capsule), Gold, Silver and Crystal with each other, and a Pokemon
  Trading Card Game duel. Pokemon games are the target; they are not tested
  one by one with real cartridges, only with test cartridges of the
  repository (a `.gb` with a `.gbc`, in both directions, and a Color in
  double speed with a Game Boy).
- GBA games with each other: Ruby, Sapphire, Emerald, FireRed and LeafGreen
  can be the two games (the partner's game is any `.gba`, not only the same
  one).
- The Link Cable is the GBA's multi-player mode and the 8-bit and 32-bit
  normal modes; these are what the test cartridge in the repository checks.
  The repository's automated tests use test cartridges of its own; they do not
  show that any particular commercial game works, so a game may still not
  work. Please report the ones that do and don't.

**What is not supported**

- Games where one cartridge starts the other player without a second one
  (single-cartridge "multiboot" play).
- Three- and four-player links. Only two machines run here.
- Wireless Adapter games (use two Vitas for those).
- Games that don't have a link-cable mode. A game with several players on one
  screen ("hot seat") plays on one handheld, not in this mode.

**Tested and not tested**

The behaviour is tested on the Linux stand-in of the app with scripted pads
and touches (two pads, the touch pad, either player's menu, a pad plugged in
and out). Which sceCtrl port each pad gets on a PS TV, and with each plugin,
and the controller types (DualShock 3, DualShock 4) are written from the
VitaSDK headers and other homebrew and have not been confirmed on a real
console. Real-hardware reports are welcome.

Bluetooth between two Vitas is not a link transport; use Wi-Fi or ad hoc.

## Share A Save Or State

From the home screen, open Settings > Link setup > Share game data. On the other
Vita, choose Join a share and select the offered file/target state slot.
Both Vitas need VitAdhoc. This works over local Wi-Fi only, not
internet or ad hoc. A replaced battery save is retained as `<game>.sav.bak`.
Check the received file before removing your own backup.

## Mystery Gift

The GBA implementation supports the protocol used by
[Mystery Gift Station](https://github.com/ShoshinFauteux/MysteryGiftStation)
for FireRed, LeafGreen and Emerald. Use only gift content you may lawfully use.

1. Start the station on the phone and choose its gift/network.
2. Join its Mystery Gift Wi-Fi network using the Vita's system settings.
3. Open the game, then press Select + Down or choose Mystery Gift in Play together.
4. Use the original game's MYSTERY GIFT menu to receive the card.

Select + Down stops listening. It cannot run alongside a link session.
A test gift from a computer has been received on a real Vita; the full
phone-to-in-game/save flow has not been verified. This is not a claim that
every event, region or game revision works.
