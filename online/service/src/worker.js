// Edge entry point. Everything real happens in the single Durable Object; the Worker rejects junk before waking it (unknown paths and
// methods, empty User-Agent, missing / old app version, oversized bodies: core/edge.js), limits each address with a small bucket, and
// answers by itself while the object has said "over budget" or "this address is over its daily cap". Every request the object sees costs
// a Durable Object request on the free plan; a request answered here does not (it still counts as a Worker request: Cloudflare counted
// it before we saw it).
//
// Verdicts of the object, as headers on its refusals:
//   x-vad-budget: over      everything except route 8 (close) and /v1/info is refused until the time named in Retry-After
//   x-vad-budget: new-over  only a new room (route 2) is refused
//   x-vad-block: addr:<s>   this address is over its daily cap for <s> more seconds
// Memory only (this isolate). One request every 10 minutes still goes through to notice a raised budget.

export { RoomServer } from './do.js';
import { edgeCheck } from './core/edge.js';
import { loadConfig } from './core/config.js';
import { createEdgeBucket } from './core/edge_bucket.js';
import { ApiError, errorResponse, ipKey, envInt, envNum } from './core/util.js';

let bucket = null;
let bucketCfg = '';
function edgeLimit(request, env) {
  const burst = envInt(env, 'EDGE_IP_BURST', 40, 0);
  const perSec = envNum(env, 'EDGE_IP_PER_SEC', 1, 0.001);
  const key = `${burst}:${perSec}`;
  if (!bucket || bucketCfg !== key) { bucket = createEdgeBucket({ burst, perSec }); bucketCfg = key; }
  const scale = envNum(env, 'RATE_LIMIT_SCALE', 1, 0.001, 1e6);
  const wait = bucket.take(request.headers.get('cf-connecting-ip') || 'unknown', Date.now(), scale);
  return wait > 0 ? errorResponse(new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait, reason: 'slow_down' })) : null;
}

/** address key -> ms until which the object's daily-cap verdict stands */
const blocks = new Map();
const MAX_BLOCKS = 6000;

const over = { hard: { until: 0, probeAt: 0, payload: null, body: null }, soft: { until: 0, probeAt: 0, payload: null, body: null } };
function overAnswer(st, now) {
  if (!st.until) return null;
  if (now >= st.until) { st.until = 0; return null; }
  if (now >= st.probeAt) { st.probeAt = now + 600_000; return null; }
  const wait = Math.max(60, Math.ceil((st.until - now) / 1000));
  const body = st.payload
    ? JSON.stringify({ ...st.payload, error: { ...st.payload.error, retryAfterSec: wait } })
    : st.body;
  return new Response(body, { status: 503, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store', 'retry-after': String(wait), 'x-vad-budget': 'over' } });
}

/**
 * The object refused the call (it threw instead of answering). Before, the throw left this Worker uncaught and Cloudflare answered its
 * HTML error page 1101, which the client cannot read. Same rule as the Galaxy service (its worker.js forward(), quota32 / fix33 S2): only an
 * error that NAMES a limit or quota (the free plan's daily Durable Object limit) is answered 503 over_budget (reason busy, as the
 * object's own budget refusal: the client shows ONLINE_BUSY); every other throw (a deploy resetting the object, overload, a transient
 * internal error) is a plain 503 `unavailable` (a 5xx to the client: ONLINE_SERVICE, try again). The text match is a heuristic.
 */
function objectRefused(e) {
  const msg = String((e && e.message) || e || '');
  const quota = !(e && (e.overloaded || e.retryable)) && /exceeded|daily|quota|free tier/i.test(msg) && /limit|quota|allowed|free tier|volume/i.test(msg) && !/reset because|code was updated|was reset|memory|cpu|subrequest/i.test(msg);
  console.error(`[object-throw] ${quota ? 'quota' : 'other'}: ${msg.slice(0, 200)}`); // keeps a bug in the object visible in the logs
  if (quota) return errorResponse(new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: 120, reason: 'busy' }, { 'retry-after': '120' }));
  return errorResponse(new ApiError(503, 'unavailable', 'online play is busy, please try again in a moment', { retryAfterSec: 3, reason: 'object_error' }, { 'retry-after': '3' }));
}

async function forward(request, env, ipk, route) {
  let resp;
  try {
    resp = await env.ROOMS.get(env.ROOMS.idFromName('main')).fetch(request);
  } catch (e) {
    return objectRefused(e);
  }
  const flag = resp.headers.get('x-vad-budget');
  if (flag === 'over' || flag === 'new-over') {
    const wait = Number(resp.headers.get('retry-after')) || 600;
    const now = Date.now();
    const st = flag === 'over' ? over.hard : over.soft;
    st.until = now + wait * 1000;
    st.probeAt = now + 600_000;
    st.body = await resp.clone().text();
    try {
      const parsed = JSON.parse(st.body);
      st.payload = parsed && typeof parsed === 'object' && parsed.error && typeof parsed.error === 'object' ? parsed : null;
    } catch {
      st.payload = null;
    }
  } else if (resp.status < 500 && !route.isInfo && !route.isLeave) {
    over.hard.until = 0;
    if (route.isNew) over.soft.until = 0;
  }
  const m = /^addr:(\d{1,6})$/.exec(resp.headers.get('x-vad-block') || '');
  if (m) {
    if (blocks.size >= MAX_BLOCKS) {
      const now = Date.now();
      for (const [k, t] of blocks) if (t <= now) blocks.delete(k);
      if (blocks.size >= MAX_BLOCKS) blocks.delete(blocks.keys().next().value);
    }
    blocks.set(ipk, Date.now() + Number(m[1]) * 1000);
  }
  return resp;
}

export default {
  /** @param {Request} request @param {{ROOMS: DurableObjectNamespace}} env */
  async fetch(request, env) {
    const cfg = loadConfig(env);
    const checked = edgeCheck(request, cfg);
    if (checked.error) return errorResponse(checked.error);
    const route = checked.route;
    const now = Date.now();
    const ipk = ipKey(request.headers.get('cf-connecting-ip') || 'unknown');
    const until = blocks.get(ipk);
    if (until) {
      if (until > now) {
        const wait = Math.max(1, Math.ceil((until - now) / 1000));
        return errorResponse(new ApiError(429, 'rate_limited', 'too many requests from this address today', { retryAfterSec: wait, reason: 'daily_cap' }));
      }
      blocks.delete(ipk);
    }
    if (!route.isInfo && !route.isLeave) {
      const hard = over.hard.until && overAnswer(over.hard, now);
      if (hard) return hard;
      const soft = route.isNew && over.soft.until && overAnswer(over.soft, now);
      if (soft) return soft;
    }
    const slow = edgeLimit(request, env); // before anything that wakes the object
    if (slow) return slow;
    return forward(request, env, ipk, route);
  },
};
