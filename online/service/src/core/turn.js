// TURN credential minting (route 6). The provider call sits behind a tiny interface so tests and the dev server use a stub and production
// uses Cloudflare Realtime TURN. The long-lived TURN key API token never leaves the Worker; clients only get short-lived username /
// credential pairs, and only while they are a member of a room that has a second member.

import { ApiError, b64url, randomBytes } from './util.js';

/**
 * @typedef {{urls: string[], username?: string, credential?: string}} IceServer
 * @typedef {{mint(ttlSec: number, nowMs?: number): Promise<{servers: IceServer[]}>}} TurnProvider
 */

/** Cloudflare Realtime TURN: POST .../turn/keys/{id}/credentials/generate-ice-servers with {ttl}. */
export function cloudflareProvider(cfg, fetchImpl = fetch) {
  return {
    async mint(ttlSec) {
      const url = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(cfg.keyId)}/credentials/generate-ice-servers`;
      const resp = await fetchImpl(url, {
        method: 'POST',
        headers: { authorization: `Bearer ${cfg.apiToken}`, 'content-type': 'application/json' },
        body: JSON.stringify({ ttl: ttlSec }),
      });
      if (!resp.ok) throw new ApiError(502, 'turn_upstream', 'the relay service refused the request');
      const data = await resp.json();
      const list = Array.isArray(data.iceServers) ? data.iceServers : data.iceServers ? [data.iceServers] : [];
      return { servers: list };
    },
  };
}

/**
 * Fake credentials for the dev server and the tests. With `TURN_STUB_URLS` (comma separated turn: URLs) and `TURN_STUB_SECRET` they follow
 * the "TURN REST" scheme that coturn's `use-auth-secret` and the test TURN server (turnsim) check: username `<expiry unix s>:<id>`,
 * credential = base64(HMAC-SHA1(secret, username)). Without them: random and meaningless (turn.example.invalid). Never used with the
 * real provider.
 */
export function stubProvider(cfg = {}) {
  const urlList = String(cfg.stubUrls || '').split(',').map((u) => u.trim()).filter(Boolean);
  const secret = String(cfg.stubSecret || '');
  return {
    async mint(ttlSec, nowMs = Date.now()) {
      const exp = Math.floor(nowMs / 1000) + ttlSec;
      if (urlList.length && secret) {
        const username = `${exp}:stub-${b64url(randomBytes(6))}`;
        const enc = new TextEncoder();
        const key = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
        const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode(username)));
        let bin = '';
        for (const b of mac) bin += String.fromCharCode(b);
        return { servers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: urlList, username, credential: btoa(bin) }] };
      }
      return {
        servers: [
          { urls: ['stun:stun.cloudflare.com:3478'] },
          {
            urls: ['turn:turn.example.invalid:3478?transport=udp', 'turn:turn.example.invalid:443?transport=udp', 'turn:turn.example.invalid:53?transport=udp', 'turn:turn.example.invalid:3478?transport=tcp'],
            username: `${exp}:stub-${b64url(randomBytes(6))}`,
            credential: b64url(randomBytes(18)),
          },
        ],
      };
    },
  };
}

export function createProvider(cfg, fetchImpl) {
  if (cfg.provider === 'stub') return stubProvider(cfg);
  if (cfg.provider === 'cloudflare' && cfg.keyId && cfg.apiToken) return cloudflareProvider(cfg, fetchImpl);
  return null;
}

/** The Vita has no TCP and no TLS-to-relay: keep UDP TURN and plain STUN urls only (3478, 443, 53 whichever the provider lists). */
export function udpOnly(servers) {
  const out = [];
  for (const s of servers) {
    const urls = (Array.isArray(s.urls) ? s.urls : [s.urls]).filter(
      (u) => typeof u === 'string' && /^(stun|turn):/.test(u) && !/transport=tcp/i.test(u),
    );
    if (urls.length) out.push({ urls, ...(s.username ? { username: s.username, credential: s.credential } : {}) });
  }
  return out;
}

// --- the guards of 1.4, kept in the `settings` table: turn_day `<yyyy-mm-dd>:<mints>`, turn_month `<yyyy-mm>:<used credentials>` ---

const dayKey = (ms) => new Date(ms).toISOString().slice(0, 10);
const monthKey = (ms) => new Date(ms).toISOString().slice(0, 7);

function readCounter(db, key, period) {
  const row = db.get(`SELECT value FROM settings WHERE key = ?`, key);
  const [p, n] = String((row && row.value) || '').split(':');
  return p === period ? Number(n) || 0 : 0;
}
function writeCounter(db, key, period, n) {
  db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, key, `${period}:${Math.max(0, n)}`);
}

export function createTurnGuards(db, cfg) {
  const t = cfg.turn;
  return {
    status(now) {
      return {
        mintsToday: readCounter(db, 'turn_day', dayKey(now)), maxPerDay: t.maxPerDay,
        usedThisMonth: readCounter(db, 'turn_month', monthKey(now)), estMbThisMonth: readCounter(db, 'turn_month', monthKey(now)) * t.estMbPerUse,
        monthlyBudgetMb: t.monthlyBudgetMb,
      };
    },
    /** Throws 503 relay_budget (monthly first, then daily); on success reserves one credential for today (give back with `release` on failure). */
    reserve(now) {
      if (t.monthlyBudgetMb > 0 && readCounter(db, 'turn_month', monthKey(now)) * t.estMbPerUse >= t.monthlyBudgetMb) {
        throw new ApiError(503, 'relay_budget', 'the relay budget for this month is used up', { retryAfterSec: 86400 });
      }
      const n = readCounter(db, 'turn_day', dayKey(now));
      if (t.maxPerDay > 0 && n >= t.maxPerDay) {
        throw new ApiError(503, 'relay_budget', 'the relay budget for today is used up', { retryAfterSec: 3600 });
      }
      writeCounter(db, 'turn_day', dayKey(now), n + 1);
    },
    release(now) {
      writeCounter(db, 'turn_day', dayKey(now), readCounter(db, 'turn_day', dayKey(now)) - 1);
    },
    /** A member posted its relayed address for the first time: the credential was USED (the monthly guard counts these). */
    used(now) {
      writeCounter(db, 'turn_month', monthKey(now), readCounter(db, 'turn_month', monthKey(now)) + 1);
    },
  };
}
