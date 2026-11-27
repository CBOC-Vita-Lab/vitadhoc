# VitAdhoc online service

The room service behind VitAdhoc's "Online" connection: one Cloudflare Worker and one Durable Object on `https://vitadhoc.psvita.dev`. It lets two
consoles find each other (a five-character room code), swap the addresses they need for a direct link (the punch handshake) and, when a direct link
cannot form, hands each a short-lived relay credential (Cloudflare Realtime TURN). It never carries game data: the game's packets go console to
console (or console to relay to console) on UDP. The wire protocol is in `PROTOCOL.md`; the owner-side deploy steps are in `DEPLOY.txt`.

Nothing in this folder is ever deployed by a test or by an agent. The tests run on plain `node` (22.5 or newer, for `node:sqlite`) with no packages.

## Layout

```
wrangler.toml            the Worker "vitadhoc-online", custom domain vitadhoc.psvita.dev, workers_dev off, the Durable Object class RoomServer
src/worker.js            the edge: route table, User-Agent, v gate, body size, per-address bucket, remembered "over budget" / "daily cap" verdicts
src/do.js                the Durable Object: builds the application over the object's SQLite; no alarm, no timer, no room kept in memory
src/sqlite-node.js       the same SQL handle over node:sqlite (tests, dev server)
src/core/app.js          createApp({sql, tx, env, now, turnFetch}) -> { handle(request), rooms(), status() }: the pipeline and the eight routes
src/core/rooms.js        the rows: validation, codes, lifecycle, lazy expiry, address checks
src/core/budget.js       the day's request budget, the per-address daily cap, the poll pace
src/core/limiter.js      the in-memory per-address / per-token limits (memory only; the guess counters and the credentials minted per address are also kept as salted fingerprints until the UTC day ends)
src/core/turn.js         the TURN provider (Cloudflare, stub), the UDP filter, the daily / monthly credential guards (counted at the mint), the optional usage read
src/core/edge.js         the route table shared by the Worker and the application, and the checks that need only the request
src/core/{config,util,edge_bucket}.js
dev/server.mjs           the dev server: the same application over node:sqlite behind plain HTTP, for the Vita twins and the client tests
scripts/smoke.mjs        node scripts/smoke.mjs <base url> [--relay]: a whole session with two fake members
scripts/deploy-vitadhoc-online.sh   deploy / verify (never run by an agent)
scripts/setup-vitadhoc-zone.sh      the one WAF rule of the zone (never run by an agent)
test/*.test.mjs          node:test + node:assert only
```

## Tests

```
cd online/service
npm test                 # node --test "test/*.test.mjs"  (about 30 s, no install)
node dev/server.mjs --port 0      # prints READY <port>, then one "REQ <method> <path> <status> <code>" line per request
node scripts/smoke.mjs http://127.0.0.1:<port>
bash scripts/deploy-vitadhoc-online.sh --account-id <32 hex> --print-plan     # contacts nothing
bash scripts/deploy-vitadhoc-online.sh --self-check
bash scripts/setup-vitadhoc-zone.sh --account-id <32 hex> --token-file /nonexistent --print-plan
bash scripts/setup-vitadhoc-zone.sh --self-check
```

(`node --test test/` does not work on Node 22: a directory is not expanded; use the glob above.) The deploy and zone scripts are also run, against stub
`npx` and `curl` and a small mock of the zone API, by `test/scripts.test.mjs`.

The dev server's defaults are for tests: `ALLOW_PRIVATE_ADDR=1` (loopback and LAN addresses accepted), `TURN_PROVIDER=stub` with
`TURN_STUB_URLS=turn:198.51.100.9:3478?transport=udp` and `TURN_STUB_SECRET=turn-test-secret` (credentials follow the TURN REST scheme, so the test TURN
server can check them), and `RATE_LIMIT_SCALE=1000` (twins that host again and again from 127.0.0.1 are not limited; pass `--env RATE_LIMIT_SCALE=1`
for the real limits). `--env K=V` is repeatable, `--clock-skew-ms N` moves the server clock, the client address is the header `x-test-ip` when present.
Test controls (never part of the protocol): `POST /_test/evict` (drop the application object and build a new one over the same database, what Cloudflare
does when an idle object leaves memory), `POST /_test/advance {"ms":N}` (move the clock, to expire a room), `GET /_test/stats`.

## Configuration (wrangler `[vars]`; every number is a variable)

| variable | default | meaning |
|---|---|---|
| `MIN_APP_VERSION`, `LATEST_APP_VERSION` | 2.0.0 | the `v` gate: older apps get 426 `update_required` (at the Worker, no object request) |
| `KILL_SWITCH` | empty | `all`, `rooms` (refuse routes 2 to 7), `relay` (refuse route 6); route 8 and `/v1/info` stay up |
| `MAINTENANCE_MESSAGE` | empty | the text of 503 `switched_off` |
| `DAILY_REQUEST_BUDGET` | 8000 | requests the object admits per UTC day (0 = off) |
| `LIVE_RESERVE_PCT` | 20 | a NEW room (route 2) is refused from (100 - this) % of the budget; everything else but route 8 and info from 100 % |
| `BUDGET_SAVE_EVERY` | 20 | the day's count is saved every this many requests, and at the first request after the object starts |
| `POLL_PACE_MAX_PCT` | 140 | the clients are asked to stretch their polls up to this much, from half of the new-room budget |
| `MAX_ROOMS`, `ROOM_TTL_SEC`, `CONNECT_SEC` | 300, 600, 120 | live rooms; open-room life; deadline once a guest joined |
| `EDGE_IP_BURST`, `EDGE_IP_PER_SEC` | 40, 1 | the Worker's bucket per address (per isolate) |
| `ADDR_DAILY` | 1500 | requests per address per UTC day, then 429 `daily_cap` until midnight |
| `CREATE_BURST`, `CREATE_EVERY_SEC`, `ROOMS_PER_ADDR`, `CREATES_PER_ADDR_DAY` | 3, 120, 4, 40 | route 2 per address |
| `LOOKUP_BURST`, `LOOKUP_EVERY_SEC`, `MISSES_PER_HOUR`, `MISSES_PER_DAY` | 8, 5, 20, 60 | route 3 per address; the miss caps give 429 `guessing` |
| `JOIN_BURST`, `JOIN_EVERY_SEC` | 4, 10 | route 4 per address |
| `POLL_MIN_MS` | 800 | route 5: one poll per token per this many ms |
| `MINTS_PER_MEMBER`, `MINTS_PER_ROOM`, `MINTS_PER_ADDR_DAY` | 2, 4, 16 | route 6: per member, per room (for its whole life: guests that left count), per address a day (stored, see Privacy) |
| `RELAY_POLL_FRESH_SEC` | 30 | route 6 needs both members to have polled the room within this many seconds (the host after the join; the guest's join counts); 0 = off |
| `TURN_MAX_CREDS_PER_DAY`, `TURN_TTL_SEC` | 100, 3600 | credentials minted per UTC day (reserved before the call; stored); lifetime asked of Cloudflare |
| `TURN_EST_MB_PER_USE`, `TURN_MONTHLY_BUDGET_MB` | 50, 500000 | the monthly guard: EVERY credential minted x 50 MB (counted at the mint, stored) against 500,000 MB |
| `TURN_USAGE_ACCOUNT_ID`, `TURN_USAGE_API_TOKEN` (secret), `TURN_USAGE_BUDGET_GB`, `TURN_USAGE_EVERY_SEC` | unset, unset, 700, 300 | optional: read the month's real TURN egress from Cloudflare's GraphQL analytics and answer 503 `relay_budget` at the budget; off unless both ids are set |
| `TURN_PROVIDER` | cloudflare | `cloudflare` (secrets `TURN_KEY_ID`, `TURN_KEY_API_TOKEN`) or `stub` (dev only) |
| `ALLOW_PRIVATE_ADDR` | unset | 1 accepts private / loopback addresses in `pub` and public ones in `lan` (dev server, tests); never set in production |
| `RATE_LIMIT_SCALE` | 1 | multiplies every per-address / per-token / per-member / per-room limit (tests set it high to switch them off) |

## Quota arithmetic (Cloudflare free plan)

Every request that reaches the Worker counts against the account's 100,000 Worker requests a day, whatever we answer, and the account is shared with
the other services on the same zone; the Worker can only keep the Durable Object (and its own bill) clear of the junk. This service admits at most
**8000 requests a day** (`DAILY_REQUEST_BUDGET`). A session costs about 28 to 30 requests (host: 1 create, about 21 polls while a friend takes a
minute to type the code, 1 poll that shows the guest, 1 relay, 1 close; joiner: 1 lookup, 1 join, 1 relay, 0 to 2 polls); a link that has formed costs
zero more. So 8000 requests are roughly 270 sessions a day; **new rooms stop at 80 %** (6400 requests, about 210 sessions) and the last 20 % keeps
rooms that already exist working (their polls, joins, relays and closes) up to 8000; a close and `/v1/info` are never refused. Once the budget is spent the Worker answers by
itself (503 `over_budget`, `Retry-After` 60 to 900 s) and wakes the object about once every 10 minutes to notice a raised budget. Polls stretch up to
1.4 times from half of the new-room budget. A full 10 minute wait alone costs 82 requests.

The count is saved to the object's SQLite every 20 requests and at the first request after the object starts (decision D4: this object is idle most of
the day and loses its memory between sessions; saving every 200, as the other services do, would forget most of the day's count). A restart therefore
forgets fewer than 20 requests. Rows written per session: create 1, join 1, each relayed address 1, state changes 0 to 2, delete 1: about 4 to 8, plus
one budget write per 20 requests and per relay credential three small writes (day, month and address counters) and one for each poll mark (at most one per member per 5 s once a guest has joined): a few thousand a day against the free plan's 100,000. Rows read are
bounded by `MAX_ROOMS` (300) per request. The object sets no alarm and no timer, and polls of an open room write nothing, so its duration is the sum of its requests.

### Quota arithmetic: Cloudflare Realtime TURN (security review H2)

Cloudflare Realtime TURN is free up to **1,000 GB of egress a month** (shared with Realtime SFU; only traffic from the relay to a client is billed)
and $0.05 a GB after that. The Worker cannot see bytes, so it counts credentials, and a credential is counted **when it is minted**, whether or not
its owner ever uses it or posts a relayed address (an abuser never posts, so counting posts is no guard; the post is kept only as the information
counter `turn_used`). Everything below is stored in the object's SQLite and survives the object leaving memory.

| limit | value | what it bounds |
|---|---|---|
| gate | both members polled within 30 s | a credential needs two live members (the host polled after the join; the guest's join is its first poll). One person can still be both, so this only raises the price |
| per member / per room | 2 / 4 for the room's whole life | `host_mints`, `guest_mints`, `old_mints` are columns of the room's row |
| per address a day | 16 | `mints` table: a salted fingerprint of the address (never the address), new salt and empty table each UTC day |
| all addresses a day | `TURN_MAX_CREDS_PER_DAY` = 100 | `settings.turn_day` |
| all addresses a month | 500,000 MB / 50 MB = 10,000 credentials | `settings.turn_month` (never reached by the daily cap: 100 x 31 = 3,100) |

The arithmetic. Free allowance 1,000 GB / 30 days = 33.3 GB a day, 33,300 MB. Every session mints two credentials (one per member, while the punch
runs), so 100 a day is **50 sessions a day**; the request budget of this page allows about 270. A one-hour credential on a game link carries about
50 MB at most (an estimate, host: a link cable or wireless adapter session is tens of kilobits a second; nothing has run on a Vita), so the
day is **100 x 50 MB = 5 GB** at the estimate, 150 GB a month: 15 % of the free tier. Each credential could carry **333 MB** (33,300 MB / 100, that is
0.74 Mbit/s for the hour, 6.6 times the estimate) before one day's 100 credentials alone used the whole day's share (a thirtieth) of the allowance. A credential
used flat out by a modified client is a different thing: Cloudflare drops a single client above about 50 to 100 Mbit/s, which is 22 to 45 GB in one
hour, so **no credential count can bound the bytes of a modified client**; the counts bound how often it can try (at most 16 credentials from one
address, 100 in all, a day) and the real guard is the optional usage read below plus a billing notification (DEPLOY.txt). To raise the daily cap
as the player base grows keep `TURN_MAX_CREDS_PER_DAY x TURN_EST_MB_PER_USE x 6` under 33,300 MB.

A session that punches directly never uses its credentials (the client fetches them beside the punch as a fallback; the guest's first try is
usually refused by the gate because the host has not polled yet, and it asks again at the fallback), so the real relayed traffic is a fraction of
the estimate. The estimate is not lowered by what a client posts: that would trust the very caller being limited.

Optional real usage (`TURN_USAGE_ACCOUNT_ID` + secret `TURN_USAGE_API_TOKEN`): the Worker reads `callsTurnUsageAdaptiveGroups { sum { egressBytes } }`
for the month from `https://api.cloudflare.com/client/v4/graphql` (documented at developers.cloudflare.com/realtime/turn/analytics/; the token needs
the "Account Analytics" permission; this query was written from the documentation and is tested here only against a stub: check it once against the live
account), at most every `TURN_USAGE_EVERY_SEC` (300 s), kept in `settings.turn_usage`. At `TURN_USAGE_BUDGET_GB` (700) route 6 answers 503 `relay_budget`
(retry 3600). A failed read keeps the last known figure for the month and, when there is none, fails open (the estimate guards still stand).
Cloudflare's GraphQL analytics are not a billing meter and can lag by minutes: keep the budget well under 1,000.

## Privacy

The service stores, for a room that is alive, what the two consoles need to find each other: the nickname, the public address and the LAN address
each console reported (the relayed address when a relay is used), the game, the title and the file identity, and the creator's address as a key for
"at most 4 live rooms per address". A room lives until the link forms (the host's close deletes the row at once) or 10 minutes pass (2 minutes
after a guest joined); an expired room loses every address, nickname and key at the next request after that (rows nobody asks about: within 5 s of any
request), and a stub of it (code, state, reason, the two tokens) is kept for 30 more seconds so the other console's poll can read "closed", then
deleted. Nothing is kept for statistics: no account, no history, no address on disk outside a live room's row. The rate limits are memory only and are
lost when the object leaves memory, with two exceptions (2.0 security review). The second: an address that minted a relay credential gets a row in `mints` (a fingerprint made the same way, salted with `settings.mint_day`, new every UTC day, and the count), so `MINTS_PER_ADDR_DAY` survives the object leaving memory. The first: an address that sent a wrong code or token gets a row in `misses`
with a salted fingerprint of its key (two 32-bit hashes, never the address) and its two counts, so the `guessing` limit survives the object leaving
memory; the salt (`settings.miss_day`) is new every UTC day and the first miss of a day deletes the rows of the day before. Nothing is logged (`observability` is off and the code never writes a log line, which a test checks). Cloudflare itself sees
the requests as any host does; that is outside this service.
