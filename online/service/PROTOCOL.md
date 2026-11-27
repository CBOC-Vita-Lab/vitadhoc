# VitAdhoc online service: protocol reference

The wire protocol of `https://vitadhoc.psvita.dev` between the VitAdhoc app and the room service in this folder. Sections 1.1 to 1.6 below are the
design's reference text (what the client and the service agree on); "As built" at the end lists the places where the implementation made a
decision the text left open. Sections 1.7 and 1.8 of the design (UDP on the link socket: STUN, punch, TURN) are client-side and are not repeated
here; the service only hands out the punch key, the peer's addresses and the relay credentials.

### 1.1 Service: general rules

Base `https://vitadhoc.psvita.dev`. Twin and tests: `http://127.0.0.1:<port>` (vita.ini `online_url`).
Only GET and POST. No custom request headers are needed (kept so on purpose: the twin's plain client and
the Vita's TLS client then send byte-identical requests):

- App version: query `?v=2.0.0` on EVERY request (`/^\d{1,4}(\.\d{1,4}){0,3}$/`). Missing or malformed -> 400
  `bad_version`. Below `MIN_APP_VERSION` -> 426 `update_required` with `min`, `latest`. `/v1/info` takes `v` but
  never refuses it.
- Member secret: `token` inside the JSON body of POSTs (never in a URL). 22 characters base64url (16 random
  bytes).
- Bodies: `Content-Type: application/json`, at most 1024 bytes (413 `too_large`), an object, unknown keys ->
  400 `bad_request`.
- User-Agent must be non-empty (the Vita sends `VitAdhoc/...`). Empty -> 400 `bad_request`.
- Answers: JSON, `cache-control: no-store`. Error shape `{"error":{"code":"...","message":"...", ...extra}}`;
  when `retryAfterSec` is in the extra, the same number is the `Retry-After` header.
- STRINGS THE SERVICE STORES OR RETURNS never contain `"`, `\` or bytes outside 0x20..0x7E: `title` and `nick`
  are sanitised on input (other bytes -> `?`, trimmed, cut to length). So the client's JSON reader need not
  handle escapes (it rejects a body that has a backslash).
- Text fields: `nick` 1..15, `title` 0..23, `game` 1..8 of `[A-Z0-9]`, `console` one of `gba gb gbc nes snes ws md sms sg scd` (not `gg`: the Game Gear has no two-player mode),
  `compat` 8 lower-case hex digits, `kind` one of 0 (GBA Wireless Adapter), 1 (GBA Link Cable), 2 (GB/GBC
  cable), 4 (share game data), 5 (Two players: one shared NES, WonderSwan or Super NES game).
- Addresses: `"a.b.c.d:port"` or `""`. `pub` must be a public IPv4 address (not 0/8, 10/8, 100.64/10, 127/8,
  169.254/16, 172.16/12, 192.168/16, 224/3) unless env `ALLOW_PRIVATE_ADDR=1` (dev server, tests). `lan` must be
  private (10/8, 172.16/12, 192.168/16, 169.254/16) unless that env. Port 1..65535.
- Room code: 5 characters of `ABCDEFGHJKMNPQRSTUVWXYZ23456789` (31 symbols). The service upper-cases input;
  a code with any other character -> 404 `not_found` (same answer as an unknown code).

Member description `desc` (the body of create, join and every poll; about 250 bytes):

```json
{"kind":1,"console":"gba","game":"BPEE","rom":"9f3a01c2","title":"Pokemon Emerald","nick":"Alice",
 "ver":"2.0.0","compat":"1a2b3c4d","pub":"203.0.113.7:19019","lan":"192.168.1.23:19019","sym":0,"relayOnly":0}
```

`ver` = the app version (the same text as `v`). `rom` = the game FILE's identity, 8 hex digits or `""` when
unknown: CRC-32 over the file size (4 bytes, little-endian) followed by 64 slices of 4096 bytes at offsets
`i * ((size - 4096) / 63)`, i = 0..63 (a file under 256 KiB: the whole file), taken from the image the core
runs (after a patch of `rom_patch.c`; `""` if only the unpatched file can be read while a patch is in use).

`sym` 1 = STUN saw two different mappings (symmetric NAT). `relayOnly` 1 = the player set "Always use the
relay"; then `pub` and `lan` are sent as `""`. A joiner's `kind`, `console`, `title`, `rom` are ignored.

`rid` (optional, create and join only): the client's request id, 16 lower-case hex digits (64 random bits), chosen
once per attempt and sent unchanged on every retry of it. A repeated create with a `rid` that made a live room
answers 201 with the ORIGINAL `code`, `token`, `punchKey` (no second room, no use of the per-address room limits); a
repeated join with the `rid` the room's guest joined with answers 200 with the ORIGINAL `token` (not 409
`room_full`). The id is forgotten with the room, and when the guest leaves (`close` cancel / failed). Anything but
16 lower-case hex digits -> 400 `bad_request`. A client that is told to stop while a create / join is in flight
repeats it with the same `rid` only to learn the token and close the room.

### 1.2 Routes

| # | method, path | body | 2xx answer |
|---|---|---|---|
| 1 | GET `/v1/info` | - | `{"service":"vitadhoc","min":"2.0.0","latest":"2.0.0","relay":true,"busy":false,"roomTtlSec":600}` |
| 2 | POST `/v1/rooms` | `desc` | 201 `{"code":"K7M2Q","token":"...","punchKey":"0123456789abcdef","ttlSec":600,"pollMs":2000}` |
| 3 | GET `/v1/rooms/{code}` | - | 200 card `{"code","kind","console","game","rom","title","nick","ver","compat","state":"open"\|"full","ageSec":42}` |
| 4 | POST `/v1/rooms/{code}/join` | `desc` | 200 `{"token","punchKey","mode":"punch"\|"relay","peer":{"nick","pub","lan","sym"},"punchMs":6000,"pollMs":1500}` |
| 5 | POST `/v1/rooms/{code}/poll` | `{"token"}`, optionally `"pub":"ip:port"` (a host whose mapping changed while the room is open) | 200 `{"state":"open"\|"joined"\|"relay"\|"closed","mode","peer":{...}\|null,"relayPeer":"ip:port"\|"","reason":"","punchMs":4000,"pollMs":2000}` |
| 6 | POST `/v1/rooms/{code}/relay` | `{"token"}` | 200 `{"ttlSec":3600,"servers":[{"urls":["turn:turn.cloudflare.com:3478?transport=udp", ...],"username":"...","credential":"..."}]}` |
| 7 | POST `/v1/rooms/{code}/endpoint` | `{"token","relay":"ip:port"}` | 200 `{"relayPeer":"ip:port"\|""}` |
| 8 | POST `/v1/rooms/{code}/close` | `{"token","reason":"linked"\|"cancel"\|"failed"}` | 200 `{}` |

Any other path or method: 404 `not_found` answered by the Worker without waking the object.

Errors (status, code, when; client meaning in 1.6):

| status | code | when |
|---|---|---|
| 400 | `bad_request` | malformed JSON / field (message names the field; `error.field` names it too), empty User-Agent |
| 400 | `bad_version` | `v` missing or malformed |
| 401 | `bad_token` | routes 5-8: token is not a member of that room |
| 403 | `relay_not_allowed` | route 6/7 while the room has no second member; route 6 also while either member has not polled within `RELAY_POLL_FRESH_SEC` (30 s) since the join (the guest's join counts as its poll) |
| 404 | `not_found` | unknown, expired or malformed code; unknown path |
| 409 | `room_full` | join: a second member is already in |
| 409 | `version_mismatch` | join: `compat` differs from the room's (extra `hostVer`: the host's `ver`) |
| 413 | `too_large` | body over 1024 bytes |
| 426 | `update_required` | `v` below MIN_APP_VERSION (extra `min`, `latest`) |
| 429 | `rate_limited` | a limit below (extra `retryAfterSec`, `reason`: `slow_down`, `daily_cap`, `guessing`, `too_many_rooms`, `relay_limit`) |
| 502 | `turn_upstream` | Cloudflare TURN refused or failed |
| 503 | `over_budget` | the day's request budget (extra `retryAfterSec` 60..900, `reason":"busy"`) |
| 503 | `rooms_full` | MAX_ROOMS live rooms |
| 503 | `switched_off` | `KILL_SWITCH` has `all`, `rooms` or (route 6) `relay` (extra `message` = MAINTENANCE_MESSAGE) |
| 503 | `relay_unavailable` | TURN not configured |
| 503 | `relay_budget` | TURN daily or monthly guard, or the optional real-usage guard (extra `retryAfterSec`) |

Rate limits (all in memory; every number an env var with this default; `RATE_LIMIT_SCALE` multiplies them for
tests):

| where | key | limit |
|---|---|---|
| zone | IP | the shared rule `psvita-shared-ratelimit` (100 requests / 10 s on `/v1/*`): NOT changed by VitAdhoc |
| Worker | address (`ipKey`, IPv6 /64) | token bucket burst 40, 1/s (`EDGE_IP_BURST`, `EDGE_IP_PER_SEC`), 429 `slow_down` |
| object | address | 1500 requests per UTC day (`ADDR_DAILY`), 429 `daily_cap` until midnight; the Worker remembers the verdict |
| object, route 2 | address | burst 3, one per 120 s; at most 4 live rooms per address (`too_many_rooms`); 40 a day |
| object, route 3 | address | burst 8, one per 5 s; 20 misses (404) per hour, then 429 `guessing` for the rest of the hour; 60 misses per UTC day, then until midnight (codes cannot be enumerated: 28.6 million codes against at most a few hundred rooms) |
| object, route 4 | address | burst 4, one per 10 s |
| object, route 5 | token | at most one per 800 ms (429 `slow_down`, `retryAfterSec` 1) |
| object, route 6 | member / room / address / all | 2 per member, 4 per room (its whole life), 16 per address per day (stored as a salted fingerprint), `TURN_MAX_CREDS_PER_DAY` 100 (stored); every session fetches a pair while it punches, see 1.5 |
| object | all | `DAILY_REQUEST_BUDGET` 8000: route 2 refused from 80 % (`LIVE_RESERVE_PCT` 20), everything else except route 8 from 100 %; route 8 is never refused |

### 1.3 Room lifecycle, timeouts, cadence

Each room is ONE ROW in the Durable Object's SQLite (reviewer: an idle object may leave memory after about
10 s, and the Free plan's objects are SQLite-backed anyway). The object keeps no room in memory between
requests, sets no alarm and no timer (Durable Object duration is shared with Galaxy and Strikers).

```sql
CREATE TABLE IF NOT EXISTS rooms (
  code TEXT PRIMARY KEY, state TEXT NOT NULL,            -- open | joined | relay | closed
  kind INTEGER, console TEXT, game TEXT, rom TEXT, title TEXT, ver TEXT, compat TEXT,
  host_nick TEXT, host_pub TEXT, host_lan TEXT, host_sym INTEGER, host_relay_only INTEGER,
  host_token TEXT, host_relayed TEXT, host_mints INTEGER, host_req TEXT,      -- *_req: the create / join `rid`
  guest_nick TEXT, guest_pub TEXT, guest_lan TEXT, guest_sym INTEGER, guest_relay_only INTEGER,
  guest_token TEXT, guest_relayed TEXT, guest_mints INTEGER, guest_req TEXT,
  punch_key TEXT, mode TEXT, reason TEXT, addr_key TEXT,   -- addr_key: the creator's ipKey, for "live rooms per address"
  created_ms INTEGER, joined_ms INTEGER, expires_ms INTEGER,
  host_poll_ms INTEGER, guest_poll_ms INTEGER,             -- last poll after the join (written at most every 5 s); the guest's join sets its own
  relay_uses INTEGER DEFAULT 0,                            -- relayed addresses counted as used credentials (information: `turn_used`)
  old_mints INTEGER DEFAULT 0);                            -- guest credentials of earlier guests (a reopen resets guest_mints to 0)
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS misses (fp TEXT PRIMARY KEY, hour INTEGER, h INTEGER, d INTEGER);  -- wrong codes per address today (salted fingerprint)
CREATE TABLE IF NOT EXISTS mints (fp TEXT PRIMARY KEY, n INTEGER);                           -- relay credentials per address today (salted fingerprint)
```

```
            POST rooms                join                    close(linked) by the host
 (none) ---------------> open --------------------> joined ---------------------------> row deleted
                          |  ^                        |  a member posted its relayed address (route 7)
                          |  | guest close            v
                          |  +--(cancel/failed)--- relay
                          |  host close(cancel), or expires_ms passed: closed (kept 30 s), then deleted
```

- `open`: `expires_ms = created_ms + 600 s`. Then `closed`, reason `expired`.
- `joined` / `relay`: `expires_ms = joined_ms + 120 s` (the connect deadline). A guest `close` with
  `cancel`/`failed` puts the room back to `open` (guest columns cleared incl. `guest_mints`, `expires_ms` back to
  `created_ms + 600 s`): the same code works for another try.
- `closed`: kept 30 s so the other member's poll reads `{"state":"closed","reason":...}` instead of 404;
  lookup and join on it answer 404. `close("linked")` deletes the row at once (addresses do not outlive
  the link).
- Expiry is lazy: every request first runs `DELETE FROM rooms WHERE expires_ms < now - 30000` and turns
  rows past `expires_ms` into `closed` (at most once per 5 s per object lifetime, and always on the first
  request after a start). No row outlives its expiry by more than the next request to the service; the
  privacy text says "until the link forms or 10 minutes pass, deleted at the next request after that".
- Row writes per session: create 1, join 1, each relayed address 1, state changes 0..2, delete 1: about
  4..8. Polls write nothing (except a host's changed `pub`).
- `mode` is `relay` when either member has `relayOnly`, or either `pub` is `""`, or EITHER has `sym` 1
  (reviewer: a mapped port that differs between two STUN queries is a hard NAT: skip the punch); else
  `punch`. In relay mode `peer.pub` and `peer.lan` are always `""`. In punch mode `peer.lan` is given only
  when both `pub` hold the same IP (same router); else `""`.
- The punch race is 4 s from the moment both sides know each other. `punchMs` for the host: 4000 (it
  starts when its poll shows the guest). For the joiner: the host's current poll interval + 4000 (6000,
  9000 or 14000), because the host may only learn of it at its next poll; a joiner's PUNCH that reaches
  the host opens it at once (1.8), which is the common case.
- Poll cadence (the service says it in `pollMs`; the client clamps to 1000..15000 and uses the same table
  when the field is missing): `open` and age < 30 s: 2000; < 120 s: 5000; else 10000; `joined`/`relay`: 1500.
  Multiplied by the budget's pace (1.0 up to 1.4 from half of the new-session budget, `budget.pace()`).
- Request count of a typical session (friend joins after 60 s, punch works): host 1 create + 15 + 6 polls
  + 1 poll that shows the guest + 1 relay (credentials in parallel) + 1 close = 25; joiner 1 lookup + 1 join
  + 1 relay + 0..2 polls = 3..5. Total 28..30. A relayed link adds 2 endpoint posts + about 4 polls.
  A full 10 minute wait alone: 82. Zero once linked.

### 1.4 Budget and counters

`settings` holds three keys: `budget_day` (`<dayIndex>:<n>:<ra>:<rb>:<ob>`, Galaxy's format and Galaxy's
`budget.js`), `turn_day` (`<yyyy-mm-dd>:<mints>`), `turn_month` (`<yyyy-mm>:<credentials minted>`).

- `budget_day`: Galaxy's mechanism unchanged, with ONE parameter made explicit: the save step
  (`BUDGET_SAVE_EVERY`, Galaxy's constant 200) defaults to 20 here, and the first request after the object
  starts saves at once. Reason (decision D4): this object is idle most of the day and loses its memory
  between sessions; at 200 it would lose most of the day's count, and the 8000 guard would never trip.
- TURN guards (security review H2: every credential counts at the mint): `turn_day >= TURN_MAX_CREDS_PER_DAY` (100) -> 503 `relay_budget`
  (retry 3600); `turn_month * TURN_EST_MB_PER_USE (50) >= TURN_MONTHLY_BUDGET_MB (500000)` -> 503 `relay_budget` (retry 86400). Both are
  incremented in the same step, BEFORE the upstream call, and given back if it fails. A relayed address posted with route 7 no longer
  changes the month (an abuser never posts); `turn_used` (`<yyyy-mm>:<n>`) still counts the first post of each member as information.
  Optional `turn_usage` (`<yyyy-mm>:<egress bytes>:<read at ms>`): the real egress read from Cloudflare's GraphQL analytics when
  `TURN_USAGE_ACCOUNT_ID` and `TURN_USAGE_API_TOKEN` are set; at `TURN_USAGE_BUDGET_GB` (700) route 6 answers 503 `relay_budget` (3600).
  No usage-report route (decision D7). Arithmetic: README, "Quota arithmetic: Cloudflare Realtime TURN".
- Per-address counters: memory (`budget.js` maps and the route buckets), never logged. They reset when the object leaves memory, with
  two exceptions kept as a salted fingerprint of the address key (never the address; a salt in `settings` new every UTC day, the old rows
  deleted with it): the wrong-code counts (`misses`) and the relay credentials minted per address (`mints`).

### 1.5 Relay credential flow

1. AS SOON AS a member knows it has a peer (the joiner: its join answer; the host: the poll that shows the
   guest, or an incoming PUNCH), it POSTs route 6 while the punch runs, so a fallback costs no wait. The
   service checks the member, that BOTH members have polled within `RELAY_POLL_FRESH_SEC` (30 s) after the join (the joiner's first try
   usually meets a host that has not polled yet: 403 `relay_not_allowed`, not fatal; the client asks again at the fallback), and the guards, mints with `cloudflareProvider.mint(TURN_TTL_SEC = 3600)`,
   filters with `udpOnly()`, answers. Route 6 does not change the room's state. A failed route 6 is not
   fatal while the punch can still win.
2. From the FIRST server entry that has a `username` the client collects the `turn:<host>:<port>?transport=udp`
   urls and orders the ports 3478, 443, then any other listed (53). `<host>` is resolved once
   (`online_http_resolve`; an IPv4 literal needs no DNS). The resolve also runs during the punch.
3. On fallback (punch timed out, `mode` relay, or the poll says `relay`): Allocate on the link socket
   (1.8) at port 3478; no answer within 3 s -> the next port; all ports failed -> "relay unavailable"
   (`ONLINE_FAIL_UNREACHABLE`). Then POST the relayed address with route 7 (the service sets
   `state = "relay"`, stores it, counts it in `turn_used`: information only) and poll until `relayPeer` is non-empty (route
   7's own answer may already carry it).
4. BOTH sides post their RELAYED address and each makes its permission and channel for the other's
   relayed address (Cloudflare refuses permissions for private or loopback peers, and this is what hides
   both public addresses). Data flows relay to relay.
5. Credentials last 1 hour and are not renewed (zero requests once linked). Whether Cloudflare lets an
   allocation made in time be refreshed past that hour is not known: risk R6 and the follow-up package RN.
6. On teardown the client releases the allocation (Refresh, lifetime 0, once, best effort).

### 1.6 Version / compat check, and what the player reads

- App too old: any route answers 426 -> "This version of VitAdhoc is too old for online play." No `/v1/info`
  call is made in the normal flow (it exists for the smoke test, the docs and the deploy check).
- App version and build compatibility are two checks, and neither is the game check. The room carries
  `ver` (the app version) and `compat` = `S.compat[kind]` of link_vita.c (FNV of version, link ABI, CPU
  backend, BIOS for the GBA cable; version only for the others) as 8 hex digits. The joiner compares both
  on the room card (before joining, no request spent); the service refuses a join whose `compat` differs
  (409 `version_mismatch` with `hostVer`). What the joiner reads says WHO acts (`cmpVersion(card.ver, mine)`):
  the host's is newer -> `ONLINE_VER_OLD_HERE`; the host's is older -> `ONLINE_VER_OLD_THERE`; the versions
  are equal but `compat` differs -> `ONLINE_SETUP` (GBA cable only: BIOS file or CPU setting).
- Game identity, checked separately and only after the two above pass: the card's `console` + `game` (GBA
  4-character header code; GB/GBC `room_code_gb` title code; Two players: 8 hex digits of the game file's CRC-32)
  and `rom` (1.1). The UI decides (package U, "library match"); the session's own SHA-1 exchange (fe_gblink
  HELLO/NEED) stays the final word for cable modes.

Client sentences (one line each, in `ui_link_fail_text`; `UI_DEV` is "Vita" / "PS TV" as today):

| `UI_FAIL_*` (new) | from | line 1 / line 2 |
|---|---|---|
| `ONLINE_CODE` | 404 after the retries; guest sees `closed`/`expired` | "That code is wrong or has expired." / "Codes last 10 minutes. Ask your friend for a new one." |
| `ONLINE_UPDATE` | 426 | "This version of VitAdhoc is too old for online play." / "Install the latest version on this " UI_DEV ", then try again." |
| `ONLINE_VER_OLD_HERE` | card: host's `ver` is newer; 409 with a newer `hostVer` | "Your friend has a newer VitAdhoc (%s)." / "Update this " UI_DEV " to the same version, then join again." |
| `ONLINE_VER_OLD_THERE` | card: host's `ver` is older | "Your friend has an older VitAdhoc (%s)." / "Ask them to update to %s, then host again." |
| `ONLINE_SETUP` | same `ver`, different `compat` | "Both run the same VitAdhoc, but the Game Boy Advance setup differs." / "Use the same BIOS file and CPU setting on both, then try again." |
| `ONLINE_CLOCK` | the TLS client's clock floor (`NP_HTTPS_E_CLOCK`) | "This " UI_DEV "'s date is wrong." / "Set the date and time in Settings, then try again." |
| `ONLINE_GAME` | card: no game in the library can enter that room | "Your friend is playing a different game." / "Add %s to this " UI_DEV " to join." (the card's title) |
| `ONLINE_GAME_FILE` | card: same game code, no file with the host's `rom`; the player may still pick one (U 5) | "Your copy of %s is a different file from your friend's." / "The link checks it when it starts." |
| `ONLINE_FULL` | card state `full`, 409 `room_full` | "Somebody already joined that room." / "Ask your friend to host again." |
| `ONLINE_UNREACHABLE` | punch and relay both failed, 502, `relay_unavailable`, `relay_budget` | "Couldn't reach your friend." / "Try again, or both use Same Wi-Fi." |
| `ONLINE_BUSY` | 503 `over_budget`, `rooms_full`, `switched_off`, 429 `daily_cap`, any other 429 that asks for more than 120 s (or gives no wait) | "Online play is busy for today." / "Try again tomorrow, or both use Same Wi-Fi." |
| `ONLINE_SLOW` | 429 (`slow_down`, `too_many_rooms`, ...) whose `retryAfterSec` is 1 to 120: the host's create and the joiner's lookup or join | "Too many tries from this network." / "Wait a minute and try again." |
| `ONLINE_SERVICE` | no answer three times, a certificate refusal (the log says which), 5xx, 400 | "The online service could not be reached." / "Check this " UI_DEV "'s internet connection and try again." |
| `ONLINE_CLOSED` | poll `closed` with `cancel` | "Your friend closed the room." / "Ask them to host again." |

Not failures: "Connecting through the relay." (LINKING screen line 2 / waiting page while stage is a relay
stage); the quality toast "Connected to %s. Connection: Great|Good|Slow" (Great < 80 ms, Good < 120 ms round trip, Slow above); with Slow one more toast line
"Slow connection: the game may feel delayed."; once linked, the session chip reads "Link Cable  P2  Great (relay)" or "Wireless Adapter  Great" (no room code; "(relay)" only when relayed). Connection lost mid-game: today's `UI_FAIL_LINK_LOST`.


---------------------------------------------------------------------------------------------------------------

## As built (decisions where the text above is silent or ambiguous)

- **Info.** `GET /v1/info` is never refused by the budget, the kill switch or the version gate (it is how the app, the smoke test and the
  deploy script see whether the service is busy). It is counted as a request but not against the admitted budget. `busy` is true from 80 % of the
  day's budget, and while `KILL_SWITCH` has `all` or `rooms`; `relay` is true only when TURN is configured and not switched off. The answer has
  exactly the six keys shown.
- **Close is never refused.** Route 8 is exempt from the budget (as written) and from the `all` / `rooms` kill switch (it only frees a room).
  A guest's `close` with `linked` changes nothing (the host's own `linked` deletes the row); a guest's `cancel` / `failed` re-opens the room.
- **Kill switch.** `all` and `rooms` answer 503 `switched_off` on routes 2 to 7; `relay` on route 6. `message` (the maintenance text, sanitised)
  replaces the error's `message`.
- **Required and optional fields.** Create needs `kind`, `console`, `game`, `nick`, `ver`, `compat`; `rom`, `title`, `pub`, `lan`, `sym`,
  `relayOnly` default to `""`, `""`, `""`, `""`, 0, 0. A join needs `nick`, `ver`, `compat`; the room's own fields in a join body are ignored (not
  validated). A token that is not 22 base64url characters is 400 `bad_request`; a well-formed token that is not a member is 401 `bad_token`.
  Posting a body needs `Content-Type: application/json` (else 400). `relayOnly` 1 empties `pub` and `lan` before anything is stored.
- **Addresses.** `pub` must be public by the listed ranges only (the documentation ranges 192.0.2/24, 198.51.100/24 and 203.0.113/24 are
  accepted; the smoke test uses 198.18.0.x). The relayed address of route 7 follows the `pub` rule.
- **Guessing.** The "misses" of the lookup limit also count an unknown code or a wrong token on routes 4 to 8 (a poll or a join cannot be used to
  enumerate codes); while an address is blocked as `guessing`, even a right code is refused, so the answer never tells a hit from a miss. A hit
  never counts.
  The two miss counts are the one per-address limit that is also written to the object's storage (table `misses`: a salted fingerprint
  of the address key, the hour, the counts; `settings.miss_day` holds the day and its salt): an idle object leaves memory after about
  ten seconds, and a guesser who waits for that would otherwise start again from zero. The rows of a day go at the first miss of the next.
- **Tombstones.** A closed room keeps for 30 s only its code, state, reason and the two member tokens (so the other member's poll can read
  `closed`); every address, nickname, the creator's address key and the punch key are wiped at the moment it closes. `mode` is `""` in a poll
  while the room is open or closed. Once a member has posted a relayed address the poll says `state: "relay"`, `mode: "relay"`.
- **Relay credentials.** Route 6 limits (2 per member, 4 per room) are counted in the room's row (`host_mints`, `guest_mints`) and survive a
  guest giving up and another joining; the per-address count and the daily / monthly counters are described in 1.4.
- **Pace.** `pollMs` in the answers of routes 2, 4 and 5 is multiplied by the budget's pace (1.0 up to half of the new-room budget, rising to 1.4
  at the limit).
- **Order of checks.** route and `v` and size (the Worker does these first) -> counted -> the address's daily cap -> info -> kill switch -> body
  -> guess block and per-route buckets -> the day's budget -> the route. A request refused by a per-address or per-token limit therefore does not
  use the budget up.
- **Retry-After.** Every `retryAfterSec` is also the `Retry-After` header.
- **Scaling.** `RATE_LIMIT_SCALE` multiplies every per-address, per-token, per-member and per-room limit (and the edge bucket); the daily budget,
  `MAX_ROOMS` and the TURN daily and monthly guards are not scaled.
