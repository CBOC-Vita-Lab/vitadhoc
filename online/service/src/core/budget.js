// The service's own request accounting (Galaxy's budget.js, cut to one address class and made explicit about the save step).
//
// A request that reaches the Worker has already been counted by Cloudflare against the account's 100,000 a day, whatever we answer.
// What this module does:
//   1. keeps the day's count of requests ADMITTED apart from the ones REFUSED for abuse;
//   2. a per-address daily ceiling (ADDR_DAILY) with a verdict the Worker remembers (x-vad-block), so the next requests of that
//      address are answered by the Worker without waking the object;
//   3. a reserve: a NEW session (route 2) stops at (100 - LIVE_RESERVE_PCT) % of the budget; everything else goes on to 100 %;
//      a route-8 close is never refused (it frees a room), and /v1/info is never refused (it says whether the service is busy);
//   4. `pace`: 1.0 .. paceMax, rising from half of the new-session budget, sent to the clients in pollMs.
// The day is the UTC day, like Cloudflare's own counter.
//
// SAVE STEP (decision D4): this object is idle most of the day and loses its memory between sessions. Galaxy saves the count every
// 200 requests; here every BUDGET_SAVE_EVERY (default 20), and the first request after the object starts saves at once, so a restart
// never forgets more than that many requests and the 8000 guard can trip.

import { ApiError } from './util.js';

const MAX_KEYS = 20000;
const KEY = 'budget_day';
/** The longest wait the service ever asks of a player for a budget refusal (seconds). */
export const RETRY_CAP_SEC = 900;

function parse(value, day) {
  const [d, n, ra, rb, ob] = String(value || '').split(':').map(Number);
  if (d !== day) return { n: 0, ra: 0, rb: 0, ob: 0 };
  return { n: n || 0, ra: ra || 0, rb: rb || 0, ob: ob || 0 };
}

/**
 * @param {{get: (q: string, ...b: any[]) => any, run: (q: string, ...b: any[]) => void}} db
 * @param {{dailyBudget: number, liveReservePct: number, paceMax: number, saveEvery: number, addrDaily: number}} cfg
 * @param {number} rateScale RATE_LIMIT_SCALE
 */
export function createBudget(db, cfg, rateScale) {
  // n admitted, ra refused for abuse, rb refused over budget, ob = EVERY request that reached the object
  const st = { day: -1, n: 0, ra: 0, rb: 0, ob: 0, stored: 0, force: true };
  let addr = new Map();
  const dayIndex = (now) => Math.floor(now / 86400000);
  const midnight = (now) => Math.max(1, Math.ceil(((dayIndex(now) + 1) * 86400000 - now) / 1000));
  const newLimit = () => Math.max(0, Math.floor((cfg.dailyBudget * (100 - cfg.liveReservePct)) / 100));

  function save(now) {
    st.stored = st.ob;
    st.force = false;
    db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, KEY, `${dayIndex(now)}:${st.n}:${st.ra}:${st.rb}:${st.ob}`);
  }
  /** Loads today's stored counts on the first call after a start and when the day changed. */
  function roll(now) {
    const day = dayIndex(now);
    if (st.day === day) return;
    const row = db.get(`SELECT value FROM settings WHERE key = ?`, KEY);
    const s = parse(row && row.value, day);
    st.day = day; st.n = s.n; st.ra = s.ra; st.rb = s.rb; st.ob = s.ob; st.stored = s.ob;
    st.force = true; // the first request after a start saves at once
    addr = new Map();
  }
  /** Saves when the save step has passed or after a start. Called once at the END of every request (`settle`), so the stored row also
   *  holds that request's own admitted / refused counts. */
  function dirty(now) {
    if (st.force || st.ob - st.stored >= cfg.saveEvery) save(now);
  }
  function bump(map, key) {
    const v = (map.get(key) || 0) + 1;
    if (!map.has(key) && map.size >= MAX_KEYS) {
      for (const [k, c] of map) if (c <= 2) map.delete(k); // one-off visitors go first: the big counts (the offenders) stay
      if (map.size >= MAX_KEYS) { let q = MAX_KEYS >> 2; for (const k of map.keys()) { map.delete(k); if (--q <= 0) break; } }
    }
    map.set(key, v);
    return v;
  }

  const api = {
    status(now) {
      roll(now);
      return {
        day: st.day, used: st.n, limit: cfg.dailyBudget, newSessionLimit: newLimit(), reservePct: cfg.liveReservePct,
        rejectedAddress: st.ra, rejectedBudget: st.rb, objectRequests: st.ob, pace: api.pace(), saveEvery: cfg.saveEvery,
      };
    },
    used: () => st.n,
    /** Every request that reaches the object, admitted or refused, is one Durable Object request on the platform's bill: counted first of all. */
    touch(now) {
      roll(now);
      st.ob++;
    },
    /** End of a request: store the counts when due (every BUDGET_SAVE_EVERY requests; at once after a start or a new day). */
    settle(now) {
      roll(now);
      dirty(now);
    },
    objects: () => st.ob,
    /** True once new sessions are refused (route 2 from 80 %) or everything is (100 %): what /v1/info calls busy. */
    busy(now) {
      roll(now);
      return cfg.dailyBudget > 0 && st.n >= newLimit();
    },

    /** Per-address daily ceiling. Returns an ApiError (429 daily_cap, with `x-vad-block: addr:<seconds>` for the Worker) or null. */
    gateAddress(ipk, now) {
      const cap = cfg.addrDaily > 0 ? Math.max(1, Math.ceil(cfg.addrDaily * rateScale)) : 0;
      if (!cap) return null;
      roll(now);
      if ((addr.get(ipk) || 0) >= cap) {
        st.ra++;
        const wait = midnight(now);
        return new ApiError(429, 'rate_limited', 'too many requests from this address today', { retryAfterSec: wait, reason: 'daily_cap' },
          { 'x-vad-block': `addr:${wait}` });
      }
      bump(addr, ipk);
      return null;
    },

    /**
     * The day's budget. isNew = route 2: served only up to the new-session limit (80 %); everything else up to the whole budget. `leave`
     * (route 8) frees a room and is never refused (still counted). Returns an ApiError (503 over_budget with `x-vad-budget: new-over|over`
     * for the Worker) or null. The wait is capped at RETRY_CAP_SEC.
     */
    admit(now, isNew, { leave = false } = {}) {
      if (!cfg.dailyBudget) return null;
      roll(now);
      if (leave) { st.n++; return null; }
      const limit = isNew ? newLimit() : cfg.dailyBudget;
      if (st.n >= limit) {
        st.rb++;
        const wait = Math.min(RETRY_CAP_SEC, Math.max(60, midnight(now)));
        return new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: wait, reason: 'busy' },
          { 'x-vad-budget': isNew && cfg.liveReservePct > 0 ? 'new-over' : 'over' });
      }
      st.n++;
      return null;
    },

    /** Poll pacing for the clients: 1 up to half of the new-session budget, then rising to paceMax. */
    pace() {
      const max = cfg.paceMax || 1;
      const nl = newLimit() || cfg.dailyBudget;
      if (!cfg.dailyBudget || max <= 1 || !nl) return 1;
      const used = st.n / nl;
      if (used <= 0.5) return 1;
      return Math.round((1 + (max - 1) * Math.min(1, (used - 0.5) / 0.5)) * 100) / 100;
    },
  };
  return api;
}
