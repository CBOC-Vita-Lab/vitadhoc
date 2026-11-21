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

// --- the guards, kept in the `settings` table (survive the object leaving memory):
//   turn_day   `<yyyy-mm-dd>:<credentials minted today>`
//   turn_month `<yyyy-mm>:<credentials minted this month>`   (the monthly guard: counted AT THE MINT, security review H2)
//   turn_used  `<yyyy-mm>:<credentials whose owner posted a relayed address>`   (information only: it can only be LOWER than turn_month,
//              and an abuser never posts, so it must never lower the estimate that guards the allowance)
//   turn_usage `<yyyy-mm>:<egress bytes>:<read at, ms>`   (optional real usage, see createUsageReader)

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

/**
 * Optional: the month's real TURN egress from Cloudflare's GraphQL analytics (`callsTurnUsageAdaptiveGroups`, field `egressBytes`; the only
 * billed direction). Needs an API token with "Account Analytics" permission (not the TURN key's token) and the account id; off unless both
 * are set. One read at most every TURN_USAGE_EVERY_SEC, kept in `turn_usage` so a restart does not read again. A failed read FAILS OPEN
 * (the estimate guards still stand) and is not retried for a minute. Above TURN_USAGE_BUDGET_GB route 6 answers 503 relay_budget.
 */
export function createUsageReader(db, t, fetchImpl = fetch) {
  const on = !!(t.usageAccountId && t.usageToken);
  const query = `query($a: String!, $from: Date!, $to: Date!) { viewer { accounts(filter: { accountTag: $a }) {
    callsTurnUsageAdaptiveGroups(limit: 1, filter: { date_geq: $from, date_leq: $to }) { sum { egressBytes } } } } }`;
  return {
    enabled: on,
    cached(now) {
      const [p, bytes, at] = String((db.get(`SELECT value FROM settings WHERE key = 'turn_usage'`) || {}).value || '').split(':');
      return p === monthKey(now) ? { bytes: Number(bytes) || 0, at: Number(at) || 0 } : null;
    },
    /** Throws 503 relay_budget when the month's egress is at or above the budget; otherwise returns quietly (also when the read fails). */
    async check(now) {
      if (!on || !(t.usageBudgetGb > 0)) return;
      let u = this.cached(now);
      if (!u || now - u.at >= t.usageEverySec * 1000) {
        const month = monthKey(now);
        let bytes = u ? u.bytes : 0;
        try {
          const resp = await fetchImpl('https://api.cloudflare.com/client/v4/graphql', {
            method: 'POST',
            headers: { authorization: `Bearer ${t.usageToken}`, 'content-type': 'application/json' },
            body: JSON.stringify({ query, variables: { a: t.usageAccountId, from: `${month}-01`, to: dayKey(now) } }),
          });
          if (resp.ok) {
            const data = await resp.json();
            const rows = data && data.data && data.data.viewer && data.data.viewer.accounts && data.data.viewer.accounts[0]
              && data.data.viewer.accounts[0].callsTurnUsageAdaptiveGroups;
            if (Array.isArray(rows) && !(data.errors && data.errors.length)) {
              bytes = rows.reduce((a, r) => a + (Number(r && r.sum && r.sum.egressBytes) || 0), 0);
            }
          }
        } catch { /* fail open: the estimate guards still stand */ }
        db.run(`INSERT INTO settings (key, value) VALUES ('turn_usage', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, `${month}:${bytes}:${now}`);
        u = { bytes, at: now };
      }
      if (u.bytes >= t.usageBudgetGb * 1e9) {
        throw new ApiError(503, 'relay_budget', 'the relay budget for this month is used up', { retryAfterSec: 3600 });
      }
    },
  };
}

export function createTurnGuards(db, cfg, fetchImpl) {
  const t = cfg.turn;
  const usage = createUsageReader(db, t, fetchImpl);
  return {
    status(now) {
      const minted = readCounter(db, 'turn_month', monthKey(now));
      const u = usage.enabled ? usage.cached(now) : null;
      return {
        mintsToday: readCounter(db, 'turn_day', dayKey(now)), maxPerDay: t.maxPerDay,
        mintedThisMonth: minted, estMbThisMonth: minted * t.estMbPerUse, monthlyBudgetMb: t.monthlyBudgetMb,
        usedThisMonth: readCounter(db, 'turn_used', monthKey(now)),
        egressGbThisMonth: u ? Math.round(u.bytes / 1e7) / 100 : null, usageBudgetGb: usage.enabled ? t.usageBudgetGb : 0,
      };
    },
    /** Optional real-usage check (async, before `reserve`); 503 relay_budget above TURN_USAGE_BUDGET_GB. */
    usageCheck: (now) => usage.check(now),
    /** Throws 503 relay_budget (monthly first, then daily); on success counts one credential for today AND this month (give back with `release`). */
    reserve(now) {
      const m = readCounter(db, 'turn_month', monthKey(now));
      if (t.monthlyBudgetMb > 0 && m * t.estMbPerUse >= t.monthlyBudgetMb) {
        throw new ApiError(503, 'relay_budget', 'the relay budget for this month is used up', { retryAfterSec: 86400 });
      }
      const n = readCounter(db, 'turn_day', dayKey(now));
      if (t.maxPerDay > 0 && n >= t.maxPerDay) {
        throw new ApiError(503, 'relay_budget', 'the relay budget for today is used up', { retryAfterSec: 3600 });
      }
      writeCounter(db, 'turn_day', dayKey(now), n + 1);
      writeCounter(db, 'turn_month', monthKey(now), m + 1);
    },
    release(now) {
      writeCounter(db, 'turn_day', dayKey(now), readCounter(db, 'turn_day', dayKey(now)) - 1);
      writeCounter(db, 'turn_month', monthKey(now), readCounter(db, 'turn_month', monthKey(now)) - 1);
    },
    /** A member posted its relayed address for the first time: the credential was USED. Information only (status); it does not move a guard. */
    used(now) {
      writeCounter(db, 'turn_used', monthKey(now), readCounter(db, 'turn_used', monthKey(now)) + 1);
    },
  };
}
