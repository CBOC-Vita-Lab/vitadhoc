// In-memory per-address / per-token limits of the object (PROTOCOL.md 1.2). Memory only: never stored, never logged. They reset when the
// object leaves memory (README: the quota arithmetic still holds because the zone rule, the Worker's bucket and the stored daily budget
// stand behind them). Every limit is scaled by RATE_LIMIT_SCALE.

import { createEdgeBucket } from './edge_bucket.js';
import { ApiError } from './util.js';

const MAX_KEYS = 20000;
const dayIdx = (now) => Math.floor(now / 86400000);
const secondsTo = (end, now) => Math.max(1, Math.ceil((end - now) / 1000));

function boundedSet(map, key, value) {
  if (!map.has(key) && map.size >= MAX_KEYS) map.delete(map.keys().next().value);
  map.set(key, value);
}

/** @param {ReturnType<import('./config.js').loadConfig>} cfg */
export function createLimiter(cfg) {
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
    /** Route 6: MINTS_PER_ADDR_DAY credentials a day per address. */
    mintDayCheck(ipk, now) {
      const e = mintsToday.get(ipk);
      if (e && e.day === dayIdx(now) && e.n >= scaled(cfg.mintsPerAddrDay)) {
        return tooMany(secondsTo((dayIdx(now) + 1) * 86400000, now), 'relay_limit', 'too many relay credentials from this address today');
      }
      return null;
    },
    mintDayCount(ipk, now, by = 1) {
      const d = dayIdx(now);
      const e = mintsToday.get(ipk);
      const n = Math.max(0, (e && e.day === d ? e.n : 0) + by);
      boundedSet(mintsToday, ipk, { day: d, n });
    },
    mintMemberCap: () => scaled(cfg.mintsPerMember),
    mintRoomCap: () => scaled(cfg.mintsPerRoom),
    relayLimit: (wait = 3600) => tooMany(wait, 'relay_limit', 'too many relay credentials for this room'),

    /** Lookup / join / poll / relay / endpoint / close of a code or token that does not exist: after MISSES_PER_HOUR in a UTC hour, or
     *  MISSES_PER_DAY in a UTC day, the address is answered 429 `guessing` until the hour (or day) ends. Hits do not count. */
    guessCheck(ipk, now) {
      const e = misses.get(ipk);
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
      const e = misses.get(ipk) || { hour, h: 0, day: d, d: 0 };
      if (e.hour !== hour) { e.hour = hour; e.h = 0; }
      if (e.day !== d) { e.day = d; e.d = 0; }
      e.h++; e.d++;
      boundedSet(misses, ipk, e);
    },
    size: () => Object.values(buckets).reduce((a, b) => a + b.size(), 0) + createsToday.size + mintsToday.size + misses.size,
  };
}
