## VitAdhoc 2.0: Online play

Play Link Cable games with a friend anywhere. One of you hosts and gets a
code of five characters; the other types it. No router settings, no port
forwarding, no accounts.

The online parts are verified on the PC test builds against simulated home
routers and a test relay. They have not yet been tried on two real Vitas or
against the live service, so no speed or reliability is promised: see
[Known Issues](KNOWN-ISSUES.md).

**Online play**

- **Host:** in-game menu > Play together > Connection: Online > Host a game.
  The screen shows the code. Your game keeps running underneath. When your
  friend joins, the link starts by itself and a message says how good the
  connection is (Great, Good or Slow).
- **Join:** type the code on a pad made for it (d-pad or touch), from the
  game menu or from the game list (Settings > Join online). VitAdhoc finds
  your copy of the game and starts it for you, or tells you which game to add.
- **It works for the same games as Wi-Fi:** GBA Link Cable, GBA Wireless
  Adapter, Game Boy and Game Boy Color Link Cable, and sharing a save or state.
- **Direct when possible, through a relay when not.** The two Vitas first try
  to reach each other directly. If a router or a phone hotspot will not allow
  it, the link goes through a Cloudflare relay by itself ("Connecting through
  the relay."). **Always use the relay** (Play together > Connection) skips the
  direct try and hides each player's address from the other.
- **Plain messages that say who acts:** a wrong or expired code; the host has a
  newer or older VitAdhoc (and which of you updates); a different game or file;
  a wrong clock; a busy day; the connection lost.
- **More forgiving delay:** Link delay Auto may use up to 12 frames on Online
  (8 on every other connection) so a long line still plays steadily.
- **Privacy:** no account, no PSN name; only your nickname is sent. A room
  (code, game name, nicknames, addresses) lives until the link forms or 10
  minutes pass, then it is deleted. See [Online Play](docs/ONLINE.md).

**Also**

- The old typed-address "Internet" connection is now **Direct address**,
  hidden until `link_direct = 1` is in `vita.ini`. A setting of Internet from
  an older version becomes Online.
- Both Vitas need VitAdhoc 2.0 or later to play online, and the same version as
  each other (the message says who updates). Same Wi-Fi and Nearby are as before.

**Not in 2.0:** NES, Super NES and WonderSwan two-player over the internet,
three- and four-player links, and single-cartridge multiboot. Online needs the
service to be running: it limits its own day, and "busy" means try again
tomorrow or use Same Wi-Fi.

### Install or update

Install `VitAdhoc.vpk` with VitaShell over the existing bubble (title ID
`GBAD00001`), or use the
[CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases/latest).
Games, saves and settings stay in place. Your friend installs it too.

`VitAdhoc-install.zip` contains the same VPK, full source, guides and
checksums. No commercial games or proprietary BIOS are included.

SHA-256 `VitAdhoc.vpk`:
`0000000000000000000000000000000000000000000000000000000000000000`
