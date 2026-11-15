// A cheap per-address token bucket the Worker applies BEFORE it wakes the Durable Object (every request that reaches the object costs a
// Durable Object request on the free plan; the object's own limits come too late for that budget). Also the shape the object uses for
// its per-address / per-token route limits (core/limiter.js builds on it).
//
// BEST EFFORT: the table lives in this isolate's memory. Cloudflare may run many isolates and recycles them, so a flood spread over many
// isolates gets through in part, and the object's own limits and the zone's shared rate-limit rule still apply behind this one.

import { ipKey } from './util.js';

const MAX_KEYS = 4000;

/** @param {{burst: number, perSec: number}} opts bucket size and refill; burst 0 = off */
export function createEdgeBucket({ burst, perSec } = { burst: 40, perSec: 1 }) {
  /** @type {Map<string, {tokens: number, at: number}>} */
  const table = new Map();
  return {
    /** Seconds to wait (>= 1) when the key is out of tokens, else 0. `key` is used as given (callers reduce addresses with ipKey). */
    takeKey(key, now, scale = 1) {
      if (!(burst > 0)) return 0;
      const cap = Math.max(1, burst * scale);
      const rate = Math.max(0.0001, perSec * scale);
      let b = table.get(key);
      if (!b) {
        if (table.size >= MAX_KEYS) table.delete(table.keys().next().value); // oldest first (Map keeps insertion order)
        b = { tokens: cap, at: now };
      } else {
        table.delete(key); // re-insert: most recently used last
        b.tokens = Math.min(cap, b.tokens + (Math.max(0, now - b.at) / 1000) * rate);
        b.at = now;
      }
      table.set(key, b);
      if (b.tokens >= 1) {
        b.tokens -= 1;
        return 0;
      }
      return Math.max(1, Math.ceil((1 - b.tokens) / rate));
    },
    take(rawIp, now, scale = 1) { return this.takeKey(ipKey(rawIp), now, scale); },
    size: () => table.size,
    clear: () => table.clear(),
  };
}
