// The room rows of PROTOCOL.md 1.3 and everything that decides about them: field validation, address checks, codes, the lifecycle
// (open -> joined -> relay -> closed / deleted), lazy expiry. Nothing here keeps a room in a variable: every function reads and writes the
// `rooms` table through `c.db`, so an object that leaves memory and comes back loses nothing.
//
// c = { db: {all, get, run}, cfg, now }.

import { ApiError, b64url, hex, randomBytes, randomInt, isInt } from './util.js';

export const CODE_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
export const CODE_LEN = 5;
export const CONSOLES = ['gba', 'gb', 'gbc', 'nes', 'snes', 'ws'];
export const KINDS = [0, 1, 2, 4, 5];

export const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS rooms (
    code TEXT PRIMARY KEY, state TEXT NOT NULL,
    kind INTEGER, console TEXT, game TEXT, rom TEXT, title TEXT, ver TEXT, compat TEXT,
    host_nick TEXT, host_pub TEXT, host_lan TEXT, host_sym INTEGER, host_relay_only INTEGER,
    host_token TEXT, host_relayed TEXT, host_mints INTEGER,
    guest_nick TEXT, guest_pub TEXT, guest_lan TEXT, guest_sym INTEGER, guest_relay_only INTEGER,
    guest_token TEXT, guest_relayed TEXT, guest_mints INTEGER,
    punch_key TEXT, mode TEXT, reason TEXT, addr_key TEXT,
    created_ms INTEGER, joined_ms INTEGER, expires_ms INTEGER, relay_uses INTEGER DEFAULT 0)`,
  `CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)`,
];

const bad = (msg, field) => new ApiError(400, 'bad_request', msg, field ? { field } : {});

// ---------------------------------------------------------------- strings and addresses

/** Strings the service stores or returns never hold `"`, `\` or bytes outside 0x20..0x7E: other bytes become `?`, then trim, then cut. */
export function cleanText(s, max) {
  return String(s).replace(/[^\x20-\x7e]|["\\]/g, '?').trim().slice(0, max).trim();
}

/** Strict "a.b.c.d:port" -> {ip: [a,b,c,d], port} or null. No leading zeros, port 1..65535. */
export function parseEndpoint(s) {
  if (typeof s !== 'string' || s.length > 21) return null;
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/.exec(s);
  if (!m) return null;
  const ip = [m[1], m[2], m[3], m[4]].map((x) => (x.length > 1 && x[0] === '0' ? NaN : Number(x)));
  const port = Number(m[5]);
  if (ip.some((x) => !(x >= 0 && x <= 255)) || !(port >= 1 && port <= 65535) || (m[5].length > 1 && m[5][0] === '0')) return null;
  return { ip, port };
}

/** Not a public address: 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.168/16, 224/3. */
export function isNonPublic([a, b]) {
  return a === 0 || a === 10 || (a === 100 && b >= 64 && b <= 127) || a === 127 || (a === 169 && b === 254)
    || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || a >= 224;
}
/** Private LAN address: 10/8, 172.16/12, 192.168/16, 169.254/16. */
export function isLan([a, b]) {
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 169 && b === 254);
}

/** An address field: "" or "a.b.c.d:port"; `pub` must be public, `lan` private, unless ALLOW_PRIVATE_ADDR=1. Returns the text. */
export function checkAddr(value, kind, field, cfg) {
  if (value === undefined || value === '') return '';
  const e = parseEndpoint(value);
  if (!e) throw bad(`${field} must be "a.b.c.d:port" or ""`, field);
  if (!cfg.allowPrivate) {
    if (kind === 'public' && isNonPublic(e.ip)) throw bad(`${field} must be a public address`, field);
    if (kind === 'lan' && !isLan(e.ip)) throw bad(`${field} must be a private LAN address`, field);
  }
  return value;
}

// ---------------------------------------------------------------- member descriptions

const DESC_KEYS = ['kind', 'console', 'game', 'rom', 'title', 'nick', 'ver', 'compat', 'pub', 'lan', 'sym', 'relayOnly'];

export function strictKeys(body, allowed) {
  if (typeof body !== 'object' || body === null || Array.isArray(body)) throw bad('a JSON object is expected');
  for (const k of Object.keys(body)) if (!allowed.includes(k)) throw bad('unexpected field', k.slice(0, 32));
  return body;
}

/** @param {boolean} host true for create (the room's own fields are required and kept), false for join (they are ignored) */
export function parseDesc(body, host, cfg) {
  strictKeys(body, DESC_KEYS);
  const need = (k) => { if (body[k] === undefined) throw bad(`${k} is required`, k); return body[k]; };
  const d = {};
  if (typeof need('nick') !== 'string') throw bad('nick must be text', 'nick');
  d.nick = cleanText(body.nick, 15);
  if (!d.nick) throw bad('nick must have 1 to 15 characters', 'nick');
  if (typeof need('ver') !== 'string' || !/^\d{1,4}(\.\d{1,4}){0,3}$/.test(body.ver)) throw bad('ver must look like 2.0.0', 'ver');
  d.ver = body.ver;
  if (typeof need('compat') !== 'string' || !/^[0-9a-f]{8}$/.test(body.compat)) throw bad('compat must be 8 lower-case hex digits', 'compat');
  d.compat = body.compat;
  d.sym = body.sym === undefined ? 0 : body.sym;
  d.relayOnly = body.relayOnly === undefined ? 0 : body.relayOnly;
  if (d.sym !== 0 && d.sym !== 1) throw bad('sym must be 0 or 1', 'sym');
  if (d.relayOnly !== 0 && d.relayOnly !== 1) throw bad('relayOnly must be 0 or 1', 'relayOnly');
  d.pub = checkAddr(body.pub, 'public', 'pub', cfg);
  d.lan = checkAddr(body.lan, 'lan', 'lan', cfg);
  if (d.relayOnly === 1) { d.pub = ''; d.lan = ''; } // "Always use the relay": no address is stored at all
  if (host) {
    if (!isInt(need('kind'), 0, 5) || !KINDS.includes(body.kind)) throw bad('kind must be 0, 1, 2, 4 or 5', 'kind');
    d.kind = body.kind;
    if (!CONSOLES.includes(need('console'))) throw bad('console must be one of gba gb gbc nes snes ws', 'console');
    d.console = body.console;
    if (typeof need('game') !== 'string' || !/^[A-Z0-9]{1,8}$/.test(body.game)) throw bad('game must be 1 to 8 of A-Z 0-9', 'game');
    d.game = body.game;
    if (body.rom !== undefined && (typeof body.rom !== 'string' || !/^([0-9a-f]{8})?$/.test(body.rom))) throw bad('rom must be 8 hex digits or ""', 'rom');
    d.rom = body.rom || '';
    if (body.title !== undefined && typeof body.title !== 'string') throw bad('title must be text', 'title');
    d.title = cleanText(body.title || '', 23);
  }
  return d;
}

// ---------------------------------------------------------------- codes, tokens, tokens' shape

export const TOKEN_RE = /^[A-Za-z0-9_-]{22}$/;

/** Upper-cases the input; a code with any other character is null (answered 404, like an unknown code). */
export function normalizeCode(raw) {
  const s = String(raw || '').toUpperCase();
  if (s.length !== CODE_LEN) return null;
  for (const ch of s) if (!CODE_ALPHABET.includes(ch)) return null;
  return s;
}
export function makeCode() {
  let s = '';
  for (let i = 0; i < CODE_LEN; i++) s += CODE_ALPHABET[randomInt(CODE_ALPHABET.length)];
  return s;
}
const makeToken = () => b64url(randomBytes(16));

// ---------------------------------------------------------------- the rows

/** Columns wiped when a room closes: addresses, relayed addresses, nicknames, the creator's address key and the punch key. The row
 *  stays only as a tombstone (code, state, reason, the two tokens) so the other member's poll reads `closed`. */
const WIPE = `host_nick = NULL, guest_nick = NULL, host_pub = '', host_lan = '', guest_pub = '', guest_lan = '', host_relayed = '', guest_relayed = '', addr_key = '', punch_key = ''`;

export function closeRow(c, code, reason, expiresMs) {
  c.db.run(`UPDATE rooms SET state = 'closed', reason = ?, expires_ms = ?, ${WIPE} WHERE code = ?`, reason, expiresMs, code);
}

/** Expiry is lazy. Runs at the first request after a start and then at most every sweepEveryMs: rooms past their time become `closed`
 *  (reason expired, kept 30 s), rooms 30 s past their time are deleted. Single room reads (loadRoom) also check their own row. */
export function sweep(c) {
  c.db.run(`UPDATE rooms SET state = 'closed', reason = 'expired', ${WIPE} WHERE state != 'closed' AND expires_ms <= ?`, c.now);
  c.db.run(`DELETE FROM rooms WHERE expires_ms < ?`, c.now - c.cfg.tombstoneMs);
}

/** The row of a code (or null: malformed, unknown, or a tombstone already past its 30 s). A row past its time turns `closed` here. */
export function loadRoom(c, rawCode) {
  const code = normalizeCode(rawCode);
  if (!code) return null;
  let r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
  if (!r) return null;
  if (r.expires_ms < c.now - c.cfg.tombstoneMs) { c.db.run(`DELETE FROM rooms WHERE code = ?`, code); return null; }
  if (r.state !== 'closed' && r.expires_ms <= c.now) {
    closeRow(c, code, 'expired', r.expires_ms);
    r = c.db.get(`SELECT * FROM rooms WHERE code = ?`, code);
  }
  return r;
}

export const liveRooms = (c) => c.db.get(`SELECT COUNT(*) AS n FROM rooms WHERE state != 'closed'`).n;
export const liveRoomsOf = (c, addrKey) => c.db.get(`SELECT COUNT(*) AS n FROM rooms WHERE state != 'closed' AND addr_key = ?`, addrKey).n;

/** Poll cadence while a room is open (1.3): age < 30 s 2000, < 120 s 5000, else 10000; joined / relay 1500. */
export function hostPollMs(ageSec) { return ageSec < 30 ? 2000 : ageSec < 120 ? 5000 : 10000; }

export function createRoom(c, d, ipk) {
  let code = '';
  for (let i = 0; i < 40; i++) {
    const t = makeCode();
    if (!c.db.get(`SELECT 1 AS x FROM rooms WHERE code = ?`, t)) { code = t; break; }
  }
  if (!code) throw new ApiError(503, 'rooms_full', 'no free room code, try again');
  const token = makeToken();
  const punchKey = hex(randomBytes(8));
  c.db.run(
    `INSERT INTO rooms (code, state, kind, console, game, rom, title, ver, compat,
       host_nick, host_pub, host_lan, host_sym, host_relay_only, host_token, host_relayed, host_mints,
       guest_mints, punch_key, mode, reason, addr_key, created_ms, expires_ms)
     VALUES (?, 'open', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '', 0, 0, ?, '', '', ?, ?, ?)`,
    code, d.kind, d.console, d.game, d.rom, d.title, d.ver, d.compat,
    d.nick, d.pub, d.lan, d.sym, d.relayOnly, token, punchKey, ipk, c.now, c.now + c.cfg.roomTtlSec * 1000,
  );
  return { code, token, punchKey };
}

export function card(c, r) {
  return {
    code: r.code, kind: r.kind, console: r.console, game: r.game, rom: r.rom, title: r.title, nick: r.host_nick, ver: r.ver, compat: r.compat,
    state: r.state === 'open' ? 'open' : 'full', ageSec: Math.max(0, Math.floor((c.now - r.created_ms) / 1000)),
  };
}

/** relay when either member asked for it, either has no public address, or EITHER saw two different mappings (symmetric NAT); else punch. */
export function modeOf(host, guest) {
  return host.relayOnly || guest.relayOnly || !host.pub || !guest.pub || host.sym || guest.sym ? 'relay' : 'punch';
}

export function joinRoom(c, r, d) {
  if (r.state !== 'open') throw new ApiError(409, 'room_full', 'somebody already joined this room');
  if (d.compat !== r.compat) throw new ApiError(409, 'version_mismatch', 'the two apps are not compatible', { hostVer: r.ver });
  const token = makeToken();
  const mode = modeOf({ relayOnly: r.host_relay_only, pub: r.host_pub, sym: r.host_sym }, d);
  c.db.run(
    `UPDATE rooms SET state = 'joined', guest_nick = ?, guest_pub = ?, guest_lan = ?, guest_sym = ?, guest_relay_only = ?, guest_token = ?, guest_relayed = '',
       mode = ?, joined_ms = ?, expires_ms = ? WHERE code = ?`,
    d.nick, d.pub, d.lan, d.sym, d.relayOnly, token, mode, c.now, c.now + c.cfg.connectSec * 1000, r.code,
  );
  return { token, mode };
}

/** The member a token belongs to ('host' | 'guest') or null. Plain comparison: the tokens are 128 random bits and the answer for a wrong one
 *  is the same whatever the prefix. */
export function memberOf(r, token) {
  if (r.host_token && token === r.host_token) return 'host';
  if (r.guest_token && token === r.guest_token) return 'guest';
  return null;
}

const ipOf = (a) => String(a || '').split(':')[0];

/** What `role` is told about the OTHER member. In relay mode no address is given; in punch mode `lan` only when both public addresses
 *  carry the same IP (one router). */
export function peerFor(r, role) {
  const guestSide = role === 'host'; // the host is told about the guest
  if (guestSide && !r.guest_token) return null;
  const p = guestSide
    ? { nick: r.guest_nick, pub: r.guest_pub, lan: r.guest_lan, sym: r.guest_sym }
    : { nick: r.host_nick, pub: r.host_pub, lan: r.host_lan, sym: r.host_sym };
  if (r.mode === 'relay' || r.state === 'relay') return { nick: p.nick, pub: '', lan: '', sym: p.sym };
  const same = r.host_pub && r.guest_pub && ipOf(r.host_pub) === ipOf(r.guest_pub);
  return { nick: p.nick, pub: p.pub, lan: same ? p.lan : '', sym: p.sym };
}

/** The punch race window (1.3): host 4000; guest = the host's poll interval at the time it joined + 4000 (6000, 9000 or 14000). */
export function punchMsFor(r, role) {
  if (role === 'host') return 4000;
  const joinedAge = Math.floor(((r.joined_ms || r.created_ms) - r.created_ms) / 1000);
  return hostPollMs(joinedAge) + 4000;
}

/** A member's relayed address (route 7). First post of a member: the credential counts as USED (returns true), but a room never counts
 *  more uses than the credentials it was given (`relay_uses` <= host_mints + guest_mints, which the per-room cap holds at 4). Without that
 *  bound the monthly relay guard could be spent by anybody for nothing: route 7 takes any public address, and a guest that joins, posts
 *  one and cancels (the room reopens with its relayed columns cleared) would add a "use" each round without a single credential minted. */
export function setRelayed(c, r, role, relayed) {
  const col = role === 'host' ? 'host_relayed' : 'guest_relayed';
  const uses = r.relay_uses || 0;
  const counted = !r[col] && uses < (r.host_mints || 0) + (r.guest_mints || 0);
  c.db.run(`UPDATE rooms SET ${col} = ?, state = 'relay', relay_uses = ? WHERE code = ?`, relayed, uses + (counted ? 1 : 0), r.code);
  return counted;
}

/** A guest that cancelled or failed: the same code works for another try (guest columns cleared, back to open). */
export function reopen(c, r) {
  c.db.run(
    `UPDATE rooms SET state = 'open', guest_nick = NULL, guest_pub = '', guest_lan = '', guest_sym = 0, guest_relay_only = 0, guest_token = NULL, guest_relayed = '',
       host_relayed = '', mode = '', joined_ms = NULL, expires_ms = ? WHERE code = ?`,
    r.created_ms + c.cfg.roomTtlSec * 1000, r.code,
  );
}

export function deleteRoom(c, code) { c.db.run(`DELETE FROM rooms WHERE code = ?`, code); }
