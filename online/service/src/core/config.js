// Configuration: wrangler vars / secrets (env) with the defaults of PROTOCOL.md 1.2 and 1.4. Every number is an env var;
// RATE_LIMIT_SCALE multiplies the per-address, per-token, per-member and per-room limits (tests set it high to switch them off,
// or 1 to test them). The daily request budget, MAX_ROOMS and the TURN daily / monthly guards are NOT scaled.

import { envInt, envNum } from './util.js';

export const KILL_NAMES = ['all', 'rooms', 'relay'];

/** @param {Record<string, any>} env */
export function loadConfig(env) {
  env = env || {};
  const kill = new Set(String(env.KILL_SWITCH || '').split(',').map((s) => s.trim().toLowerCase()).filter(Boolean));
  return {
    minApp: String(env.MIN_APP_VERSION || '2.0.0'),
    latestApp: String(env.LATEST_APP_VERSION || '2.0.0'),
    kill,
    message: String(env.MAINTENANCE_MESSAGE || '').replace(/[^\x20-\x7e]/g, '?').replace(/["\\]/g, "'").slice(0, 120),
    allowPrivate: env.ALLOW_PRIVATE_ADDR === '1' || env.ALLOW_PRIVATE_ADDR === 1,
    scale: envNum(env, 'RATE_LIMIT_SCALE', 1, 0.001, 1e6),
    maxRooms: envInt(env, 'MAX_ROOMS', 300, 1, 100000),
    roomTtlSec: envInt(env, 'ROOM_TTL_SEC', 600, 10, 86400),
    connectSec: envInt(env, 'CONNECT_SEC', 120, 5, 3600),
    tombstoneMs: 30000,
    sweepEveryMs: 5000,
    // route limits (1.2)
    addrDaily: envInt(env, 'ADDR_DAILY', 1500, 0),
    createBurst: envInt(env, 'CREATE_BURST', 3, 1),
    createEverySec: envInt(env, 'CREATE_EVERY_SEC', 120, 1),
    roomsPerAddr: envInt(env, 'ROOMS_PER_ADDR', 4, 1),
    createsPerDay: envInt(env, 'CREATES_PER_ADDR_DAY', 40, 1),
    lookupBurst: envInt(env, 'LOOKUP_BURST', 8, 1),
    lookupEverySec: envInt(env, 'LOOKUP_EVERY_SEC', 5, 1),
    missesPerHour: envInt(env, 'MISSES_PER_HOUR', 20, 1),
    missesPerDay: envInt(env, 'MISSES_PER_DAY', 60, 1),
    joinBurst: envInt(env, 'JOIN_BURST', 4, 1),
    joinEverySec: envInt(env, 'JOIN_EVERY_SEC', 10, 1),
    pollMinMs: envInt(env, 'POLL_MIN_MS', 800, 1),
    mintsPerMember: envInt(env, 'MINTS_PER_MEMBER', 2, 1),
    mintsPerRoom: envInt(env, 'MINTS_PER_ROOM', 4, 1),
    mintsPerAddrDay: envInt(env, 'MINTS_PER_ADDR_DAY', 16, 1),
    relayFreshSec: envInt(env, 'RELAY_POLL_FRESH_SEC', 30, 0, 600),
    // budget (1.4)
    dailyBudget: envInt(env, 'DAILY_REQUEST_BUDGET', 8000, 0),
    liveReservePct: envInt(env, 'LIVE_RESERVE_PCT', 20, 0, 100),
    paceMax: envInt(env, 'POLL_PACE_MAX_PCT', 140, 100, 300) / 100,
    saveEvery: envInt(env, 'BUDGET_SAVE_EVERY', 20, 1),
    turn: {
      provider: String(env.TURN_PROVIDER || ''),
      keyId: String(env.TURN_KEY_ID || ''),
      apiToken: String(env.TURN_KEY_API_TOKEN || ''),
      stubUrls: String(env.TURN_STUB_URLS || ''),
      stubSecret: String(env.TURN_STUB_SECRET || ''),
      ttlSec: envInt(env, 'TURN_TTL_SEC', 3600, 60, 86400),
      maxPerDay: envInt(env, 'TURN_MAX_CREDS_PER_DAY', 100, 0),
      estMbPerUse: envInt(env, 'TURN_EST_MB_PER_USE', 50, 0),
      monthlyBudgetMb: envInt(env, 'TURN_MONTHLY_BUDGET_MB', 500000, 0),
      // optional: read the real egress from Cloudflare's GraphQL analytics (needs an "Account Analytics" token; off unless both are set)
      usageAccountId: String(env.TURN_USAGE_ACCOUNT_ID || ''),
      usageToken: String(env.TURN_USAGE_API_TOKEN || ''),
      usageBudgetGb: envInt(env, 'TURN_USAGE_BUDGET_GB', 700, 0),
      usageEverySec: envInt(env, 'TURN_USAGE_EVERY_SEC', 300, 30, 86400),
    },
  };
}
