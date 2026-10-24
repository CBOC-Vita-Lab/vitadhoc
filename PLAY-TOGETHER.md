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

GBA cable sessions need the same ROM file on both Vitas. GBA Wireless Adapter
sessions need the same supported game. GB/GBC cable mode can use different
cartridges, such as compatible paired versions: the implementation exchanges
cartridge bytes in memory for the session, not to storage. Do not use it to
distribute a cartridge you are not allowed to share.

Fast link, in Settings > Link setup, batches two frames of GBA cable input.
It is off by default and the host's setting controls the session. It does not
make a weak internet connection reliable. Cheats suspend while linked and
return afterwards; achievements pause and start counting again afterwards. Rewind, turbo and preview recording are not normal linked
features. Three- and four-player links and single-cartridge multiboot are absent.

## Two Players On One Vita

Choose Two players on this Vita. The second emulated handheld can use a copy
of the first save, a separate `<game>.p2.sav`, or a different compatible
cartridge and its own save. A right-stick flick or a tap on the other screen
changes which one you control.

PS TV can use a paired second DualShock. A handheld needs a controller setup
that exposes a distinct second pad, not mirrored Vita buttons; the original
guide specifies ds34vita with reVita's Emulate DS4 mode, or two controllers.
Those plugin and controller combinations haven't been confirmed yet. No plugins
are included or installed by the app. Bluetooth between two Vitas is not a
link transport; use Wi-Fi or ad hoc instead.

## Share A Save Or State

From the home screen, open Settings > Link setup > Share game data. On the other
Vita, choose Join a share and select the offered file/target state slot.
Both sides need build 20 or later. This works over local Wi-Fi only, not
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
Build 20 accepted a test parcel from a computer on physical Vita; the full
phone-to-in-game/save flow has not been verified. This is not a claim that
every event, region or game revision works.
