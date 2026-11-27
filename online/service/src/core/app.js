// The application: the eight routes of PROTOCOL.md, behind one function. Runs inside the Durable Object (src/do.js), under node for the
// tests and the dev server (src/sqlite-node.js), and is the same code in both. Holds NO room between requests: rooms are rows, the
// counters that matter are saved (budget.js, turn.js), the per-address limits are memory-only by design (limiter.js).
//
// Order of every request: edge checks (route, User-Agent, v, size) -> count it (budget.touch) -> per-address daily ceiling -> /v1/info is
// answered here -> kill switch -> body -> per-route limits -> the day's budget -> the route. No console output anywhere: an address or a
// token must never reach a log line.

import { ApiError, errorResponse, json, ipKey, readBodyCapped } from './util.js';
import { loadConfig } from './config.js';
import { edgeCheck, MAX_BODY } from './edge.js';
import { createBudget } from './budget.js';
import { createLimiter } from './limiter.js';
import { createProvider, createTurnGuards, udpOnly } from './turn.js';
import {
  SCHEMA, MIGRATIONS, TOKEN_RE, parseDesc, strictKeys, checkAddr, loadRoom, sweep, liveRooms, liveRoomsOf, createRoom, roomOfCreate, card, joinRoom, memberOf, peerFor,
  punchMsFor, hostPollMs, setRelayed, reopen, closeRow, deleteRoom,
} from './rooms.js';

/** A member's last poll is written at most this often (ms). */
const POLL_MARK_MS = 5000;

const bad = (msg, field) => new ApiError(400, 'bad_request', msg, field ? { field } : {});

/**
 * @param {{sql: {exec: Function}, tx: (fn: () => any) => any, env?: Record<string, any>, now?: () => number, turnFetch?: typeof fetch}} deps
 */
export function createApp({ sql, tx, env = {}, now = Date.now, turnFetch = fetch }) {
  const cfg = loadConfig(env);
  const db = {
    all: (q, ...b) => sql.exec(q, ...b).toArray(),
    get: (q, ...b) => sql.exec(q, ...b).toArray()[0] ?? null,
    run: (q, ...b) => { sql.exec(q, ...b).toArray(); },
  };
  for (const stmt of SCHEMA) db.run(stmt);
  { // a database made by an earlier build lacks the newer columns of `rooms`
    const have = new Set(db.all(`SELECT name FROM pragma_table_info('rooms')`).map((x) => x.name));
    for (const [col, type] of MIGRATIONS) if (!have.has(col)) db.run(`ALTER TABLE rooms ADD COLUMN ${col} ${type}`);
  }
  const budget = createBudget(db, cfg, cfg.scale);
  const limiter = createLimiter(cfg, db);
  const guards = createTurnGuards(db, cfg, turnFetch);
  const provider = createProvider(cfg.turn, turnFetch);
  let lastSweep = -Infinity;
  let requests = 0;

  const relayOn = () => !!provider && !cfg.kill.has('all') && !cfg.kill.has('relay');
  const paced = (ms) => Math.round(ms * budget.pace());

  async function readJson(request) {
    const text = await readBodyCapped(request, MAX_BODY);
    if (text === null) throw new ApiError(413, 'too_large', 'request body too large');
    if (!text.trim()) throw bad('a JSON body is required');
    if (!/^application\/json/i.test(request.headers.get('content-type') || '')) throw bad('Content-Type must be application/json');
    let body;
    try { body = JSON.parse(text); } catch { throw bad('the body is not valid JSON'); }
    if (typeof body !== 'object' || body === null || Array.isArray(body)) throw bad('a JSON object is expected');
    return body;
  }

  /** `{token}` plus the keys a route allows; returns the token. */
  function tokenOf(body, allowed = []) {
    strictKeys(body, ['token', ...allowed]);
    if (typeof body.token !== 'string' || !TOKEN_RE.test(body.token)) throw bad('token must be the 22 characters the service gave', 'token');
    return body.token;
  }

  /** Room + member of a token-carrying route. 404 for an unknown / closed-and-gone code, 401 for a token that is not a member. */
  function roomAndRole(c, code, token) {
    const r = loadRoom(c, code);
    if (!r) throw new ApiError(404, 'not_found', 'no such room');
    const role = memberOf(r, token);
    if (!role) throw new ApiError(401, 'bad_token', 'not a member of this room');
    return { r, role };
  }

  /** One transaction. An ApiError thrown inside does NOT roll back: what a request wrote before refusing is the lazy expiry of the row it
   *  looked at (rooms past their time become closed and lose their addresses), and no route writes anything before it refuses. Any other
   *  error rolls the transaction back. */
  function atomic(fn) {
    let refused = null;
    const out = tx(() => {
      try { return fn(); } catch (e) { if (e instanceof ApiError) { refused = e; return null; } throw e; }
    });
    if (refused) throw refused;
    return out;
  }

  // ------------------------------------------------------------ the routes (each runs inside one transaction unless it awaits)

  function info(t) {
    return json(200, {
      service: 'vitadhoc', min: cfg.minApp, latest: cfg.latestApp, relay: relayOn(),
      busy: cfg.kill.has('all') || cfg.kill.has('rooms') || budget.busy(t), roomTtlSec: cfg.roomTtlSec,
    });
  }

  function create(c, body, ipk) {
    const d = parseDesc(body, true, cfg);
    // a retry of a create that already got through (the answer was lost): the original answer, not a second room
    const again = roomOfCreate(c, d.rid);
    if (again) return json(201, { code: again.code, token: again.host_token, punchKey: again.punch_key, ttlSec: cfg.roomTtlSec, pollMs: paced(2000) });
    if (liveRoomsOf(c, ipk) >= limiter.roomsPerAddrCap()) throw limiter.tooManyRooms();
    if (liveRooms(c) >= cfg.maxRooms) throw new ApiError(503, 'rooms_full', 'the service has no free room right now', { retryAfterSec: 60 });
    const { code, token, punchKey } = createRoom(c, d, ipk);
    limiter.createDayCount(ipk, c.now);
    return json(201, { code, token, punchKey, ttlSec: cfg.roomTtlSec, pollMs: paced(2000) });
  }

  function lookup(c, params) {
    const r = loadRoom(c, params.code);
    if (!r || r.state === 'closed') throw new ApiError(404, 'not_found', 'no such room');
    return json(200, card(c, r));
  }

  function join(c, params, body) {
    const d = parseDesc(body, false, cfg);
    const r = loadRoom(c, params.code);
    if (!r || r.state === 'closed') throw new ApiError(404, 'not_found', 'no such room');
    // a retry of a join that already got through: the same guest, the original token (not 409 room_full against ourselves)
    const token = d.rid && r.guest_req === d.rid && r.guest_token ? r.guest_token : joinRoom(c, r, d).token;
    const nr = loadRoom(c, r.code);
    return json(200, { token, punchKey: nr.punch_key, mode: nr.mode, peer: peerFor(nr, 'guest'), punchMs: punchMsFor(nr, 'guest'), pollMs: paced(1500) });
  }

  function poll(c, params, body) {
    const token = tokenOf(body, ['pub']);
    let pub;
    if (body.pub !== undefined) pub = checkAddr(body.pub, 'public', 'pub', cfg);
    const { r, role } = roomAndRole(c, params.code, token);
    if (r.state === 'closed') {
      return json(200, { state: 'closed', mode: '', peer: null, relayPeer: '', reason: r.reason || '', punchMs: 4000, pollMs: paced(2000) });
    }
    // a host whose mapping changed while the room is open: the only write a poll ever makes
    if (pub !== undefined && role === 'host' && r.state === 'open' && !r.host_relay_only && pub !== r.host_pub) {
      db.run(`UPDATE rooms SET host_pub = ? WHERE code = ?`, pub, r.code);
    }
    // the relay credential gate (H2) needs to know both members are alive: after the join, a poll leaves its time (at most one write per
    // POLL_MARK_MS per member; an open room's polls write nothing)
    if (r.state !== 'open') {
      const col = role === 'host' ? 'host_poll_ms' : 'guest_poll_ms';
      if (!r[col] || c.now - r[col] >= POLL_MARK_MS) db.run(`UPDATE rooms SET ${col} = ? WHERE code = ?`, c.now, r.code);
    }
    const ageSec = Math.max(0, Math.floor((c.now - r.created_ms) / 1000));
    return json(200, {
      state: r.state,
      mode: r.state === 'open' ? '' : r.state === 'relay' ? 'relay' : r.mode,
      peer: peerFor(r, role),
      relayPeer: (role === 'host' ? r.guest_relayed : r.host_relayed) || '',
      reason: '',
      punchMs: punchMsFor(r, role),
      pollMs: paced(r.state === 'open' ? hostPollMs(ageSec) : 1500),
    });
  }

  /** Synchronous authentication and room-state check for route 6, before any async usage read. */
  function relayPrecheck(rc, code, body) {
    const token = tokenOf(body);
    const { r, role } = roomAndRole(rc, code, token);
    if (r.state === 'closed') throw new ApiError(404, 'not_found', 'no such room');
    if (r.state === 'open') throw new ApiError(403, 'relay_not_allowed', 'relay credentials are for rooms with two members');
    if (!provider) throw new ApiError(503, 'relay_unavailable', 'the relay is not configured');
    return { r, role, token };
  }

  /** Route 6, step 1 (synchronous): who may ask, every guard, and the reservation. Returns what step 3 needs to give it back. */
  function relayReserve(c, params, body, ipk) {
    const { r, role, token } = relayPrecheck(c, params.code, body);
    const col = role === 'host' ? 'host_mints' : 'guest_mints';
    // both members must be alive: each polled (the guest's join counts as its first poll) within RELAY_POLL_FRESH_SEC and after the join.
    // One person can still be both members from one address, so this only raises the price of an abuse; the caps below stand it.
    if (cfg.relayFreshSec > 0) {
      const fresh = (t) => t && t >= (r.joined_ms || 0) && c.now - t <= cfg.relayFreshSec * 1000;
      if (!fresh(r.host_poll_ms) || !fresh(r.guest_poll_ms)) {
        throw new ApiError(403, 'relay_not_allowed', 'both members must be polling the room before a relay credential is given');
      }
    }
    const addrWait = limiter.mintDayCheck(ipk, c.now);
    if (addrWait) throw addrWait;
    if ((r[col] || 0) >= limiter.mintMemberCap() || (r.host_mints || 0) + (r.guest_mints || 0) + (r.old_mints || 0) >= limiter.mintRoomCap()) throw limiter.relayLimit();
    guards.reserve(c.now); // 503 relay_budget (monthly, then daily)
    db.run(`UPDATE rooms SET ${col} = ${col} + 1 WHERE code = ?`, r.code);
    limiter.mintDayCount(ipk, c.now);
    return { code: r.code, col, tokenCol: role === 'host' ? 'host_token' : 'guest_token', token, now: c.now };
  }

  async function relay(c, params, body, ipk) {
    atomic(() => relayPrecheck({ db, cfg, now: now() }, params.code, body));
    if (provider) await guards.usageCheck(now()); // optional: the real egress of the month (a no-op unless TURN_USAGE_* are set)
    const rsv = atomic(() => relayReserve({ db, cfg, now: now() }, params, body, ipk));
    let minted;
    try {
      minted = await provider.mint(cfg.turn.ttlSec, rsv.now);
    } catch (e) {
      tx(() => { // the reservation goes back
        guards.release(rsv.now);
        db.run(`UPDATE rooms SET ${rsv.col} = MAX(0, ${rsv.col} - 1) WHERE code = ? AND ${rsv.tokenCol} = ?`, rsv.code, rsv.token);
        limiter.mintDayCount(ipk, rsv.now, -1);
      });
      if (e instanceof ApiError) throw e;
      throw new ApiError(502, 'turn_upstream', 'the relay service is unavailable');
    }
    return json(200, { ttlSec: cfg.turn.ttlSec, servers: udpOnly(minted.servers) });
  }

  function endpoint(c, params, body) {
    const token = tokenOf(body, ['relay']);
    const relayed = checkAddr(body.relay, 'public', 'relay', cfg);
    if (!relayed) throw bad('relay must be "a.b.c.d:port"', 'relay');
    const { r, role } = roomAndRole(c, params.code, token);
    if (r.state === 'closed') throw new ApiError(404, 'not_found', 'no such room');
    if (r.state === 'open') throw new ApiError(403, 'relay_not_allowed', 'relay addresses are for rooms with two members');
    if (setRelayed(c, r, role, relayed)) guards.used(c.now);
    return json(200, { relayPeer: (role === 'host' ? r.guest_relayed : r.host_relayed) || '' });
  }

  function close(c, params, body) {
    const token = tokenOf(body, ['reason']);
    if (!['linked', 'cancel', 'failed'].includes(body.reason)) throw bad('reason must be linked, cancel or failed', 'reason');
    const { r, role } = roomAndRole(c, params.code, token);
    if (r.state === 'closed') return json(200, {});
    if (role === 'host') {
      if (body.reason === 'linked') deleteRoom(c, r.code); // addresses do not outlive the link
      else closeRow(c, r.code, body.reason, c.now); // the guest's next poll reads closed + reason
    } else if (body.reason !== 'linked') {
      reopen(c, r); // the guest gave up: the same code works for another try
    } // a guest's `linked` changes nothing: the host's own close deletes the row
    return json(200, {});
  }

  // ------------------------------------------------------------ the pipeline

  async function handle(request) {
    requests++;
    const t = now();
    const checked = edgeCheck(request, cfg);
    if (checked.error) return errorResponse(checked.error);
    const route = checked.route;
    const ipk = ipKey(request.headers.get('cf-connecting-ip'));
    const c = { db, cfg, now: t };
    try {
      return await pipeline(request, route, ipk, c, t);
    } catch (e) {
      if (e instanceof ApiError) {
        // a code or token that does not exist counts against the address (it cannot enumerate codes by polling or joining either)
        if ((e.code === 'not_found' || e.code === 'bad_token') && route.name !== 'create') limiter.miss(ipk, t);
        return errorResponse(e);
      }
      return errorResponse(new ApiError(500, 'internal', 'internal error')); // never the message: it could hold a value
    } finally {
      try { budget.settle(now()); } catch { /* the count is best effort once the answer is made */ }
    }
  }

  async function pipeline(request, route, ipk, c, t) {
    if (t - lastSweep >= cfg.sweepEveryMs || t < lastSweep) { lastSweep = t; atomic(() => sweep(c)); }
    budget.touch(t);
    const gate = budget.gateAddress(ipk, t);
    if (gate) throw gate;
    if (route.isInfo) return info(t);
    // KILL_SWITCH: all / rooms refuse every room route except the close of a room (it only frees one); relay refuses route 6
    const killed = (!route.isLeave && (cfg.kill.has('all') || cfg.kill.has('rooms'))) || (cfg.kill.has('relay') && route.name === 'relay');
    if (killed) throw new ApiError(503, 'switched_off', 'online play is switched off', cfg.message ? { message: cfg.message } : {});
    const body = request.method === 'POST' ? await readJson(request) : null;
    // limits that need no row, before the budget: a refused request is abuse, not demand
    const name = route.name;
    if (name === 'lookup' || name === 'join' || name === 'poll' || name === 'relay' || name === 'endpoint' || name === 'close') {
      const g = limiter.guessCheck(ipk, t);
      if (g) throw g;
    }
    const lim = name === 'create' ? limiter.bucket('create', ipk, t) || limiter.createDayCheck(ipk, t)
      : name === 'lookup' ? limiter.bucket('lookup', ipk, t)
        : name === 'join' ? limiter.bucket('join', ipk, t)
          : name === 'poll' && body && typeof body.token === 'string' ? limiter.pollToken(body.token, t) : null;
    if (lim) throw lim;
    const refused = budget.admit(t, !!route.isNew, { leave: !!route.isLeave });
    if (refused) throw refused;
    switch (name) {
      case 'create': return atomic(() => create(c, body, ipk));
      case 'lookup': return atomic(() => lookup(c, route.params));
      case 'join': return atomic(() => join(c, route.params, body));
      case 'poll': return atomic(() => poll(c, route.params, body));
      case 'relay': return await relay(c, route.params, body, ipk);
      case 'endpoint': return atomic(() => endpoint(c, route.params, body));
      case 'close': return atomic(() => close(c, route.params, body));
      default: throw new ApiError(404, 'not_found', 'no such endpoint');
    }
  }

  return {
    handle,
    rooms: () => liveRooms({ db, cfg, now: now() }),
    status: () => ({ requests, rooms: liveRooms({ db, cfg, now: now() }), budget: budget.status(now()), turn: guards.status(now()), limiter: limiter.size() }),
  };
}
