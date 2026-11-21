// In-memory per-address / per-token limits of the object (PROTOCOL.md 1.2). Memory only, never logged, with TWO exceptions: the guess
// counters (wrong codes and tokens: createMissStore) and the relay credentials minted per address (createMintStore) are also kept on disk as
// a salted fingerprint until the UTC day ends, because they are what stands between a room code and somebody trying them all, and between
// the TURN allowance and somebody minting all day. The others reset when the object leaves memory (README: the quota
// arithmetic still holds because the zone rule, the Worker's bucket and the stored daily budget stand behind them). Every limit is
// scaled by RATE_LIMIT_SCALE.

import { createEdgeBucket } from './edge_bucket.js';
import { ApiError } from './util.js';

const MAX_KEYS = 20000;
const dayIdx = (now) => Math.floor(now / 86400000);
const secondsTo = (end, now) => Math.max(1, Math.ceil((end - now) / 1000));

function boundedSet(map, key, value) {
  if (!map.has(key) && map.size >= MAX_KEYS) map.delete(map.keys().next().value);
  map.set(key, value);
}

/** FNV-1a (32 bits) of a string, from `seed`. */
function fnv(seed, text) {
  let h = seed >>> 0;
  for (let i = 0; i < text.length; i++) { h ^= text.charCodeAt(i) & 0xff; h = Math.imul(h, 0x01000193) >>> 0; }
  return h;
}

/**
 * The guess counters on disk (security review, 2.0). Everything else here is memory only, and an idle object leaves memory after about
 * ten seconds: without this an address could try a handful of codes, wait for the object to forget, and go on all day. A wrong code or
 * token writes one row: a salted fingerprint of the address key (never the address), the UTC hour and day and the two counts. The salt is
 * new every UTC day and the old rows go with it, so nothing here outlives the day.
 * @param {{get: Function, run: Function, all?: Function}|undefined} db
 */
function createMissStore(db) {
  if (!db) return null;
  let salt = '';
  let saltDay = -1;
  let noneDay = -1; // the day we looked and found no salt (nothing counted yet): do not look again until we make one
  function roll(day, make) {
    if (saltDay === day) return true;
    if (noneDay !== day) {
      const row = db.get(`SELECT value FROM settings WHERE key = 'miss_day'`);
      const [d, s] = String((row && row.value) || '').split(':');
      if (Number(d) === day && s) { salt = s; saltDay = day; return true; }
      noneDay = day;
    }
    if (!make) return false; // nothing was counted today: nothing to read
    const a = new Uint32Array(2);
    crypto.getRandomValues(a);
    salt = a[0].toString(16).padStart(8, '0') + a[1].toString(16).padStart(8, '0');
    saltDay = day;
    db.run(`DELETE FROM misses`);
    db.run(`INSERT INTO settings (key, value) VALUES ('miss_day', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `${day}:${salt}`);
    return true;
  }
  const fp = (ipk) => fnv(0x811c9dc5, `${salt}|${ipk}`).toString(16).padStart(8, '0') + fnv(0x1b873593, `${ipk}|${salt}`).toString(16).padStart(8, '0');
  return {
    /** The stored counts of an address for `day`, or null. */
    load(ipk, day) {
      if (!roll(day, false)) return null;
      const r = db.get(`SELECT hour, h, d FROM misses WHERE fp = ?`, fp(ipk));
      return r ? { hour: Number(r.hour), h: Number(r.h), day, d: Number(r.d) } : null;
    },
    save(ipk, e) {
      roll(e.day, true);
      db.run(`INSERT INTO misses (fp, hour, h, d) VALUES (?, ?, ?, ?) ON CONFLICT(fp) DO UPDATE SET hour = excluded.hour, h = excluded.h, d = excluded.d`,
        fp(ipk), e.hour, e.h, e.d);
    },
  };
}

/**
 * Relay credentials minted per address today, on disk (security review H2; the same way as the guess counters above): a salted fingerprint
 * of the address key, never the address, a new salt every UTC day, the rows of the day before deleted with it. Without it an address could
 * mint its share, wait for the object to leave memory and mint it again. One row write per credential (and one back when the upstream
 * call fails), bounded by TURN_MAX_CREDS_PER_DAY.
 */
function createMintStore(db) {
  if (!db) return null;
  let salt = '';
  let saltDay = -1;
  let noneDay = -1;
  function roll(day, make) {
    if (saltDay === day) return true;
    if (noneDay !== day) {
      const row = db.get(`SELECT value FROM settings WHERE key = 'mint_day'`);
      const [d, s] = String((row && row.value) || '').split(':');
      if (Number(d) === day && s) { salt = s; saltDay = day; return true; }
      noneDay = day;
    }
    if (!make) return false;
    const a = new Uint32Array(2);
    crypto.getRandomValues(a);
    salt = a[0].toString(16).padStart(8, '0') + a[1].toString(16).padStart(8, '0');
    saltDay = day;
    db.run(`DELETE FROM mints`);
    db.run(`INSERT INTO settings (key, value) VALUES ('mint_day', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `${day}:${salt}`);
    return true;
  }
  const fp = (ipk) => fnv(0x811c9dc5, `m|${salt}|${ipk}`).toString(16).padStart(8, '0') + fnv(0x1b873593, `${ipk}|${salt}|m`).toString(16).padStart(8, '0');
  return {
    load(ipk, day) {
      if (!roll(day, false)) return 0;
      const r = db.get(`SELECT n FROM mints WHERE fp = ?`, fp(ipk));
      return r ? Number(r.n) || 0 : 0;
    },
    save(ipk, day, n) {
      roll(day, true);
      db.run(`INSERT INTO mints (fp, n) VALUES (?, ?) ON CONFLICT(fp) DO UPDATE SET n = excluded.n`, fp(ipk), Math.max(0, n));
    },
  };
}

/**
 * @param {ReturnType<import('./config.js').loadConfig>} cfg
 * @param {{get: Function, run: Function}} [db] the object's storage: the guess counters are kept there too (see createMissStore)
 */
export function createLimiter(cfg, db) {
  const missStore = createMissStore(db);
  const mintStore = createMintStore(db);
  const scale = cfg.scale;
  const scaled = (v) => Math.max(1, Math.ceil(v * scale));
  const mk = (burst, everySec) => createEdgeBucket({ burst, perSec: 1 / everySec });
  const buckets = {
    create: mk(cfg.createBurst, cfg.createEverySec),
    lookup: mk(cfg.lookupBurst, cfg.lookupEverySec),
    join: mk(cfg.joinBurst, cfg.joinEverySec),
    poll: createEdgeBucket({ burst: 1, perSec: 1000 / cfg.pollMinMs }),
  };
  /** @type {Map<string, {day: number, n: number}>} */
  const createsToday = new Map();
  /** @type {Map<string, {day: number, n: number}>} */
  const mintsToday = new Map();
  /** @type {Map<string, {hour: number, h: number, day: number, d: number}>} */
  const misses = new Map();

  const tooMany = (wait, reason, msg = 'slow down') => new ApiError(429, 'rate_limited', msg, { retryAfterSec: wait, reason });

  return {
    /** Token bucket of a route per address (create, lookup, join). Returns an ApiError or null. */
    bucket(name, ipk, now) {
      const wait = buckets[name].takeKey(ipk, now, scale);
      return wait > 0 ? tooMany(wait, 'slow_down') : null;
    },
    /** Route 5: one poll per POLL_MIN_MS per token. */
    pollToken(token, now) {
      const wait = buckets.poll.takeKey(token, now, scale);
      return wait > 0 ? tooMany(1, 'slow_down') : null;
    },
    /** Route 2: CREATES_PER_ADDR_DAY a day (counted when a room is made). */
    createDayCheck(ipk, now) {
      const e = createsToday.get(ipk);
      if (e && e.day === dayIdx(now) && e.n >= scaled(cfg.createsPerDay)) {
        return tooMany(secondsTo((dayIdx(now) + 1) * 86400000, now), 'daily_cap', 'too many rooms from this address today');
      }
      return null;
    },
    createDayCount(ipk, now) {
      const d = dayIdx(now);
      const e = createsToday.get(ipk);
      boundedSet(createsToday, ipk, e && e.day === d ? { day: d, n: e.n + 1 } : { day: d, n: 1 });
    },
    roomsPerAddrCap: () => scaled(cfg.roomsPerAddr),
    tooManyRooms: () => tooMany(60, 'too_many_rooms', 'too many open rooms from this address'),
    /** Route 6: MINTS_PER_ADDR_DAY credentials a day per address (kept on disk too: a restart does not reset it). */
    mintDayCheck(ipk, now) {
      const d = dayIdx(now);
      let e = mintsToday.get(ipk);
      if (!e && mintStore) { e = { day: d, n: mintStore.load(ipk, d) }; boundedSet(mintsToday, ipk, e); }
      if (e && e.day === d && e.n >= scaled(cfg.mintsPerAddrDay)) {
        return tooMany(secondsTo((d + 1) * 86400000, now), 'relay_limit', 'too many relay credentials from this address today');
      }
      return null;
    },
    mintDayCount(ipk, now, by = 1) {
      const d = dayIdx(now);
      let e = mintsToday.get(ipk);
      const base = e && e.day === d ? e.n : (mintStore ? mintStore.load(ipk, d) : 0);
      const n = Math.max(0, base + by);
      boundedSet(mintsToday, ipk, { day: d, n });
      if (mintStore) mintStore.save(ipk, d, n);
    },
    mintMemberCap: () => scaled(cfg.mintsPerMember),
    mintRoomCap: () => scaled(cfg.mintsPerRoom),
    relayLimit: (wait = 3600) => tooMany(wait, 'relay_limit', 'too many relay credentials for this room'),

    /** Lookup / join / poll / relay / endpoint / close of a code or token that does not exist: after MISSES_PER_HOUR in a UTC hour, or
     *  MISSES_PER_DAY in a UTC day, the address is answered 429 `guessing` until the hour (or day) ends. Hits do not count. */
    guessCheck(ipk, now) {
      let e = misses.get(ipk);
      if (!e && missStore) {
        // not in memory (the object came back): one read, remembered either way so the next request of this address reads nothing
        e = missStore.load(ipk, dayIdx(now)) || { hour: Math.floor(now / 3600000), h: 0, day: dayIdx(now), d: 0 };
        boundedSet(misses, ipk, e);
      }
      if (!e) return null;
      const hour = Math.floor(now / 3600000);
      if (e.day === dayIdx(now) && e.d >= scaled(cfg.missesPerDay)) {
        return tooMany(secondsTo((dayIdx(now) + 1) * 86400000, now), 'guessing', 'too many wrong codes today');
      }
      if (e.hour === hour && e.h >= scaled(cfg.missesPerHour)) {
        return tooMany(secondsTo((hour + 1) * 3600000, now), 'guessing', 'too many wrong codes, try again later');
      }
      return null;
    },
    miss(ipk, now) {
      const hour = Math.floor(now / 3600000);
      const d = dayIdx(now);
      const e = misses.get(ipk) || (missStore && missStore.load(ipk, d)) || { hour, h: 0, day: d, d: 0 };
      if (e.hour !== hour) { e.hour = hour; e.h = 0; }
      if (e.day !== d) { e.day = d; e.d = 0; }
      e.h++; e.d++;
      boundedSet(misses, ipk, e);
      if (missStore) { try { missStore.save(ipk, e); } catch { /* the count in memory still stands */ } }
    },
    size: () => Object.values(buckets).reduce((a, b) => a + b.size(), 0) + createsToday.size + mintsToday.size + misses.size,
  };
}
