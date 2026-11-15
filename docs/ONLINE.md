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

Either way, you need the same game on your Vita as your friend has.

- From a game: in-game menu > Play together > Connection: Online > **Join a
  game**.
- From the game list (no game open): Settings > **Join online**. VitAdhoc
  finds your copy of the game by its cartridge code and starts it for you.

Type the code on the pad. D-pad up and down change the letter in a box, left
and right move between boxes, Square deletes. Or touch the 31 keys on the
screen. Triangle, Start or the Join key sends it. A card shows the game, the
kind of link and the host's nickname; X joins.

Codes use the letters and numbers `A B C D E F G H J K M N P Q R S T U V W X
Y Z 2 3 4 5 6 7 8 9`. There is no I, L, O, 0 or 1, so nothing looks alike.

What can be played: everything Play together does over Wi-Fi: GBA Link
Cable, GBA Wireless Adapter, Game Boy and Game Boy Color Link Cable, and
sharing a save or state. Both Vitas need the same VitAdhoc version and, for
the GBA cable, the same BIOS file and CPU setting. A patched game needs the
same patch on both. The two of you may be on different networks, countries or
phone hotspots.

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
| Your copy of X is a different file from your friend's. | The link checks the two games when it starts. |
| Somebody already joined that room. | A room holds two players. Ask your friend to host again. |
| Couldn't reach your friend. | Try again, or both use Same Wi-Fi. A strict network may block the relay too. |
| Online play is busy for today. | The service limits its day on purpose (below). Try tomorrow, or use Same Wi-Fi. |
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
  after that. It keeps nothing for statistics and has no log of its own.
  Cloudflare, like any web host, sees the requests.
- **Your friend sees your internet address** on a direct link (that is what a
  direct link is). **Always use the relay** hides it from them: then each Vita
  only sees Cloudflare's relay address.
- **Game data never goes to the service.** It goes from Vita to Vita, or
  through Cloudflare's relay when that is needed. As on Wi-Fi, VitAdhoc does
  not encrypt the link's packets.
- The app remembers nothing between visits: not the code, not your friend.

### Limits

The service runs on Cloudflare's free plan, shared with other services, so it
limits itself: 300 rooms at once, one room per code, a day's request budget
(new rooms stop being offered when about 80 % is used, which is about 200
sessions a day), and a monthly relay allowance. A session costs about 30
requests; once linked it costs none. Limits per person keep one Vita from
using the day up. If you hit one, the message says "busy". Same Wi-Fi never
needs the service.

## For Developers

### Pieces

| Piece | Where | What |
| --- | --- | --- |
| Room service | `online/service/` | One Cloudflare Worker and one Durable Object (SQLite, one row per room). Routes under `/v1/`. `PROTOCOL.md` has the wire protocol, `README.md` the layout and the quota arithmetic, `DEPLOY.txt` the owner's steps. |
| Room client | `gpsp/vita/online_http.{h,cpp}` | The app's side of the eight routes, a non-blocking request slot polled once a frame. |
| HTTPS client | `gpsp/vita/nethttps/` | The project's own TLS client (Mbed TLS 3.6.7, TLS 1.2 and 1.3, certificate checks always on, eight compiled-in roots, a clock floor). Not `sceHttp`: Cloudflare's free edge certificates are ECDSA and `sceSsl` gives no control. `ra_http.c` stays RetroAchievements'. |
| Connection | `gpsp/vita/online_vita.{h,c}` | The stage machine: STUN, create or join, the punch race with relay credentials fetched in parallel, relay fallback, the quality measure. Every stage logs an `EVT online_*` line. |
| NAT layer | `gpsp/netdrv/transport_lan.{h,c}` | STUN, punch, keep-alive and the TURN client on the link's own UDP socket and I/O thread. |
| Library match | `gpsp/vita/online_lib.{h,c}` | The joiner's step from a room card to a file: code first, then the file fingerprint; a cable game is never opened on its code alone. |
| Menus | `gpsp/vita/ui_vita_online.h`, `ui_vita_play.h` | Code pad, host page, room card, the sentences. |
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
| menu | Connection: Online, Always use the relay, Link delay | as above (`vita.ini`: `net_conn`, `online_relay`, `link_delay`) |
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
