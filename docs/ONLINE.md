# Online Play

Online play links two Vitas that are far apart with a five-character room
code. No router settings, no port forwarding, no accounts. It is new in
VitAdhoc 2.0 and has not yet been tried on two real consoles: see
[Known Issues](../KNOWN-ISSUES.md).

This page has two halves: [for players](#for-players) (what to press, what the
messages mean, what is sent where) and [for developers](#for-developers) (how
it is built, the limits, how to test and run the service).

## For Players

### Host a game

1. Open the game you want to play, then the in-game menu > Play together.
2. Connection: **Online**.
3. Choose **Host a game**. The screen shows a code of five characters, the
   game's name and "Give this code to your friend". The game keeps running
   under the code, as it does when you host on Wi-Fi.
4. Tell your friend the code (a message, a call, anything). When they join,
   their nickname appears, the link starts by itself and a short message says
   how good the line is ("Connected to Alice. Connection: Great").
5. **Cancel** closes the room. A code nobody uses stops working after 10
   minutes.

### Join a game

What you need on your Vita depends on the kind of link:

- **GBA Link Cable and Game Boy / Game Boy Color Link Cable:** your own game.
  It need not be your friend's. Ruby on one Vita and Sapphire on the other
  works, and so does a `.gb` game with a `.gbc` game. A game the other Vita
  does not have is sent across the first time.
- **GBA Wireless Adapter:** the same game as your friend's.
- **Two players (NES, WonderSwan, Super NES):** the very same game file as
  your friend's.

- From a game: in-game menu > Play together > Connection: Online > **Join a
  game**.
- From the game list (no game open): Settings > **Join online**. VitAdhoc
  finds your copy of the game and starts it for you.

Type the code on the pad. D-pad up and down change the letter in a box, left
and right move between boxes, Square deletes. Or touch the 31 keys on the
screen. Triangle, Start or the Join key sends it. A card shows the game, the
kind of link and the host's nickname; X joins.

Codes use the letters and numbers `A B C D E F G H J K M N P Q R S T U V W X
Y Z 2 3 4 5 6 7 8 9`. There is no I, L, O, 0 or 1, so nothing looks alike.

What can be played: everything Play together does over Wi-Fi: GBA Link
Cable, GBA Wireless Adapter, Game Boy and Game Boy Color Link Cable, Two
players for NES, WonderSwan and Super NES games, and sharing a save or state.
Both Vitas need the same VitAdhoc version and, for the GBA cable, the same
BIOS file and CPU setting. The two of you may be on different networks,
countries or phone hotspots.

### Two players over Online

NES, WonderSwan and Super NES games are played by two Vitas as one shared
game. The host is player 1 and the joiner is player 2. For both you need the
very same game file: a patched copy does not match an unpatched one.

- **NES and WonderSwan:** open the game, then the menu > Play together >
  Connection: Online. The host chooses **Host a game** and shows the code; the
  joiner chooses **Join a game** (or, from the game list, Settings > **Join
  online**) and types it.
- **Super NES:** in the game list, highlight the game and press Start for
  Settings, then choose **Two players** (the last row of Link setup). The host
  chooses **Host a game** > **Online** and shows the code. The joiner chooses
  **Join a game** > **Online** and types it. The game starts by itself once the
  link is up. (The Super NES game is played by a separate player program, so
  its in-game menu has no Play together page: this is the way in.)

Save states, cheats, rewind and fast forward are off while linked. On NES and
WonderSwan the joiner plays the host's game, and the joiner's own game comes
back as it was when the link ends.

### How the two Vitas connect

First VitAdhoc tries a **direct** link: each Vita learns the address the
internet sees it as (a free Cloudflare lookup), the two exchange those
through the room, and both send to each other at the same moment so each
home router lets the other in. It takes a few seconds and needs nothing from
you. If that does not work (some routers, most phone hotspots and mobile
networks), the screen says "Connecting through the relay." and the two Vitas
talk through Cloudflare's relay instead. It is a little slower. Nothing to
set.

Settings in Play together > Connection:

| Setting | What it does |
| --- | --- |
| **Always use the relay** (off) | Skip the direct link; each player's address stays hidden from the other. Costs some speed. |
| **Link delay** (Auto) | Auto picks how many frames the link waits, from the measured round trip: up to 12 frames on Online, up to 8 on other connections. You can set a fixed number from 1 to 8. The host's choice counts. |

While you play, a small label shows the link, your player number and the
line, for example "Link Cable  P2  Great" or "Wireless Adapter  Great", with
"(relay)" at the end when the link goes through the relay.

The message when a link forms says **Great** (round trip under about 80 ms),
**Good** (under about 120 ms) or **Slow** (the game still links, and says it
may feel delayed). These limits are first guesses: they will be tuned after
the first two-Vita sittings.

### When it does not work

| The screen says | What to do |
| --- | --- |
| That code is wrong or has expired. | Check the letters. Codes last 10 minutes; ask for a new one. |
| Your friend has a newer / an older VitAdhoc (x.y.z). | The sentence says who updates: the one with the older version installs the newer. |
| Both run the same VitAdhoc, but the Game Boy Advance setup differs. | Use the same BIOS file and the same CPU setting on both. |
| This Vita's date is wrong. | Set the date and time in the system settings. A Vita whose clock was reset thinks it is 2011 and cannot check the service's certificate. |
| Your friend is playing a different game. / Add X to this Vita to join. | You need a copy of the host's game. |
| Your copy of X is a different file from your friend's. | You can still go on. On a Link Cable your own game is used and your friend's is sent across; the link checks both games when it starts. |
| Somebody already joined that room. | A room holds two players. Ask your friend to host again. |
| Couldn't reach your friend. | Try again, or both use Same Wi-Fi. A strict network may block the relay too. |
| Online play is busy for today. | The service limits its day on purpose (below). Try tomorrow, or use Same Wi-Fi. |
| Too many tries from this network. Wait a minute and try again. | Wait a minute, then try again. This is a short limit on how fast one network may ask for rooms. |
| The online service could not be reached. | This Vita's internet connection, or the service is down for a while. `ux0:data/GBAdhoc/log/gbadhoc.log` has `online_` lines that say which step. |
| Your friend closed the room. | Ask them to host again. |

If the connection is lost in the middle of a game, you get the same message as
on Wi-Fi: both games save and carry on alone.

### What is sent where (privacy)

- **No accounts, no PSN name, no email.** The only personal text VitAdhoc sends
  is the nickname you set in Settings.
- **The room service** (`vitadhoc.psvita.dev`, one Cloudflare Worker) keeps, for
  a room that is alive: the code, the game's name and a short fingerprint of
  the game file, the kind of link, both nicknames and each Vita's internet and
  local address (the relay's address when a relay is used). It keeps them until
  the link forms or 10 minutes pass (2 minutes after a friend joins without
  linking). They are then deleted, at the next request the service receives
  after that. It keeps nothing for statistics and has no log of its own. One
  thing outlives a room, until the end of the day (UTC): a network that sent
  a wrong code or key is counted, under a scrambled fingerprint and never its
  address, so that nobody can try codes all day.
  Cloudflare, like any web host, sees the requests.
- **Your friend sees your internet address** on a direct link (that is what a
  direct link is). **Always use the relay** hides it from them: then each Vita
  only sees Cloudflare's relay address.
- **Game data never goes to the service.** It goes from Vita to Vita, or
  through Cloudflare's relay when that is needed. As on Wi-Fi, VitAdhoc does
  not encrypt the link's packets.
- The app remembers nothing between visits: not the code, not your friend.
  The log file is the exception (`ux0:data/GBAdhoc/log/gbadhoc.log`, the last
  few sessions): its `online_` lines hold the room code, your friend's
  nickname and both internet addresses. Read it before you share it.
- **Play online with people you know.** The host's console sends its game
  state (and, for a Game Boy cable link with one cartridge, the game) to the
  guest's console, which loads it, as it would load a save state someone gave
  you. VitAdhoc checks sizes and checksums, but the emulator cores were not
  written to withstand a state that was damaged on purpose.

### Limits

The service runs on Cloudflare's free plan, shared with other services, so it
limits itself: 300 rooms at once, one room per code, a day's request budget
(new rooms stop being offered when about 80 % is used, which is about 200
sessions a day), and a monthly relay allowance. A session costs about 30
requests; once linked it costs none. Limits per person keep one Vita from
using the day up. If you hit the day's limit, the message says "busy for
today". If you are only asking too fast, it says to wait a minute. Same Wi-Fi
never needs the service.

## For Developers

### Pieces

| Piece | Where | What |
| --- | --- | --- |
| Room service | `online/service/` | One Cloudflare Worker and one Durable Object (SQLite, one row per room). Routes under `/v1/`. `PROTOCOL.md` has the wire protocol, `README.md` the layout and the quota arithmetic, `DEPLOY.txt` the owner's steps. |
| Room client | `gpsp/vita/online_http.{h,cpp}` | The app's side of the eight routes, a non-blocking request slot polled once a frame. |
| HTTPS client | `gpsp/vita/nethttps/` | The project's own TLS client (Mbed TLS 3.6.8, TLS 1.2 and 1.3, certificate checks always on, eight compiled-in roots, a clock floor). Not `sceHttp`: Cloudflare's free edge certificates are ECDSA and `sceSsl` gives no control. `ra_http.c` stays RetroAchievements'. |
| Connection | `gpsp/vita/online_vita.{h,c}` | The stage machine: STUN, create or join, the punch race with relay credentials fetched in parallel, relay fallback, the quality measure. Every stage logs an `EVT online_*` line. |
| NAT layer | `gpsp/netdrv/transport_lan.{h,c}` | STUN, punch, keep-alive and the TURN client on the link's own UDP socket and I/O thread. |
| Library match | `gpsp/vita/online_lib.{h,c}` | The joiner's step from a room card to a file: code first, then the file fingerprint; a cable game is never opened on its code alone. |
| Menus | `gpsp/vita/ui_vita_online.h`, `ui_vita_play.h`, `ui_vita_snes.h` | Code pad, host page, room card, the sentences; the game list's Two players page for Super NES games. |
| Twin tests | `gpsp/vita/hostcore/online_tools.py`, `gpsp/netdrv/tests/natsim/` | The service under node, the NAT and TURN simulator as a UDP service on the loopback. |

### Flow

```
host                           service                         joiner
 STUN -> own public address
 POST /v1/rooms  ------------>  one row, code, token
 poll (2 s, then 5 s, 10 s)                                      type the code
                                <------------ GET /v1/rooms/CODE  (card)
                                <------------ POST .../join
 poll shows the friend --------------------------------------->  join answer has the host
 POST .../relay (credentials, in parallel with the punch)        POST .../relay
 ==== PUNCH (4 s race on the link's UDP socket) =================================
 linked: host closes the room (row deleted)   or, if no answer in time:
 Allocate on the relay, POST .../endpoint, both talk relay to relay
```

- A room is one SQLite row; an open room lives 10 minutes, a joined one has
  120 seconds to link. Expiry is lazy: every request first deletes what has
  run out. The object sets no alarm and keeps nothing in memory between requests.
- The relay is chosen at once if either player set Always use the relay, STUN
  failed, or either NAT looks symmetric (the mapped port changed between two
  STUN queries).
- Relay credentials last 1 hour and are not renewed; a punched link never
  needs them. If Cloudflare turns out to refuse refreshing an allocation past
  that hour, a relayed session would end as "link lost" at about an hour: this
  is open risk R6, only the live relay can say.
- Every request carries `?v=<app version>`; the secret token travels in POST
  bodies, never in a URL; the app's own User-Agent is sent. There are no
  custom headers, so the twin's plain-HTTP client and the Vita's TLS client
  send the same bytes.
- The Vita build needs an Mbed TLS prefix (`MBEDTLS=`): see
  [Building](BUILDING.md). The twin works without it over `http://` only.

### Settings and switches

| Where | Key | Meaning |
| --- | --- | --- |
| menu | Connection: Online, Always use the relay, Link delay | as above (`config.ini`: `net_conn`, `online_relay`, `link_delay`) |
| `vita.ini` | `link_direct = 1` | Shows the old typed-address connection ("Direct address", needs UDP 19019 forwarded). A 1.4 setting of "Internet" becomes Online unless this is set. |
| `vita.ini` | `online_url` | Another service address (the twin and tests point it at `http://127.0.0.1:<port>`). |

### Operating the service

The owner's steps are in `online/service/DEPLOY.txt` and, for Codex, in
`online/CODEX-REQUEST-vitadhoc-online.md`. In short: one Worker
`vitadhoc-online` on the custom domain `vitadhoc.psvita.dev`, two secrets for
the Cloudflare Realtime TURN key, one host-scoped WAF rule in the zone. The
zone's shared rate-limiting rule is not touched. Switch it off without a
deploy by setting the Worker variable `KILL_SWITCH` (`all`, `rooms` or
`relay`). Raise `MIN_APP_VERSION` when an app version must stop being allowed.

### Tests

[Testing](TESTING.md) lists every online suite and how to run it. Nothing in
them leaves the loopback.
