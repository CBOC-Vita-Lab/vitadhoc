// Small shared helpers: errors, JSON responses, base64url, randomness, version compare, address keys.
// (Trimmed from the Galaxy service's util.js: only what this service uses.)

export class ApiError extends Error {
  /**
   * @param {number} status HTTP status
   * @param {string} code stable machine-readable code (PROTOCOL.md)
   * @param {string} [message] human text, never contains secrets or addresses
   * @param {object} [extra] merged into the error object (e.g. retryAfterSec)
   * @param {Record<string,string>} [headers]
   */
  constructor(status, code, message, extra, headers) {
    super(message || code);
    this.status = status;
    this.code = code;
    this.extra = extra || {};
    this.headers = headers || {};
  }
}

export function json(status, body, headers) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'content-type': 'application/json; charset=utf-8',
      'cache-control': 'no-store',
      'x-content-type-options': 'nosniff',
      ...(headers || {}),
    },
  });
}

/** The error shape of PROTOCOL.md; a retryAfterSec in the extra is also the Retry-After header. */
export function errorResponse(e) {
  const headers = { ...e.headers };
  if (e.extra && Number.isFinite(e.extra.retryAfterSec) && !headers['retry-after']) headers['retry-after'] = String(e.extra.retryAfterSec);
  return json(e.status, { error: { code: e.code, message: e.message, ...e.extra } }, headers);
}

export function b64url(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x2000) s += String.fromCharCode(...bytes.subarray(i, i + 0x2000));
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

export function hex(bytes) {
  let s = '';
  for (const b of bytes) s += b.toString(16).padStart(2, '0');
  return s;
}

export function randomBytes(n) {
  const a = new Uint8Array(n);
  crypto.getRandomValues(a);
  return a;
}

/** Uniform integer in [0, n) without modulo bias. */
export function randomInt(n) {
  const limit = Math.floor(0x100000000 / n) * n;
  const a = new Uint32Array(1);
  for (;;) {
    crypto.getRandomValues(a);
    if (a[0] < limit) return a[0] % n;
  }
}

export function isInt(v, min, max) {
  return Number.isInteger(v) && v >= min && v <= max;
}

/** Parse a numeric env var with a default and clamps. */
export function envInt(env, name, def, min = 0, max = Number.MAX_SAFE_INTEGER) {
  const raw = env && env[name];
  if (raw === undefined || raw === null || raw === '') return def;
  const n = Number(raw);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, Math.trunc(n))) : def;
}

/** Same as envInt but keeps fractions (RATE_LIMIT_SCALE may be 0.5). */
export function envNum(env, name, def, min = 0, max = Number.MAX_SAFE_INTEGER) {
  const raw = env && env[name];
  if (raw === undefined || raw === null || raw === '') return def;
  const n = Number(raw);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : def;
}

/** The app version of every request (`?v=`): dotted numbers only, at most four parts. */
export const VERSION_RE = /^\d{1,4}(\.\d{1,4}){0,3}$/;

/** Compare dotted versions numerically. */
export function cmpVersion(a, b) {
  const pa = String(a).split('.').map(Number);
  const pb = String(b).split('.').map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d < 0 ? -1 : 1;
  }
  return 0;
}

/**
 * Rate-limit key for a client address. IPv4 stays as is; IPv6 is reduced to its /64 (one subscriber normally owns a whole /64, so
 * per-address keys would let one home connection mint millions of buckets); IPv4-mapped IPv6 is the IPv4 address. Only ever used
 * as an in-memory key (and, for the room's creator, one column that is wiped when the room closes).
 */
export function ipKey(ip) {
  const s = String(ip || 'unknown').trim().toLowerCase().split('%')[0];
  if (!s.includes(':')) return s.slice(0, 64);
  const mapped = /^(?:0{0,4}:){2,5}(?:ffff:)(\d{1,3}(?:\.\d{1,3}){3})$/.exec(s) || /^::ffff:(\d{1,3}(?:\.\d{1,3}){3})$/.exec(s);
  if (mapped) return mapped[1];
  let head = s;
  let tail = '';
  const dc = s.indexOf('::');
  if (dc >= 0) { head = s.slice(0, dc); tail = s.slice(dc + 2); }
  let h = head ? head.split(':') : [];
  let t = tail ? tail.split(':') : [];
  const dotted = (arr) => {
    if (arr.length && arr[arr.length - 1].includes('.')) { arr.pop(); arr.push('0', '0'); }
    return arr;
  };
  h = dotted(h);
  t = dotted(t);
  const missing = dc >= 0 ? 8 - h.length - t.length : 0;
  const groups = [...h, ...Array(Math.max(0, missing)).fill('0'), ...t];
  if (groups.length !== 8 || !groups.every((g) => /^[0-9a-f]{1,4}$/.test(g))) return `v6:${s.slice(0, 39)}`;
  return `v6:${groups.slice(0, 4).map((g) => g.padStart(4, '0')).join(':')}`;
}

/**
 * Reads a request body as text, never holding more than `max` bytes: the stream is cancelled the moment it goes past the cap
 * (Content-Length can be missing or wrong). Returns null when the cap was exceeded.
 * @param {Request} request @param {number} max
 */
export async function readBodyCapped(request, max) {
  if (!request.body) return '';
  const reader = request.body.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > max) {
      try { await reader.cancel(); } catch { /* the stream is gone anyway */ }
      return null;
    }
    parts.push(value);
  }
  const all = new Uint8Array(total);
  let at = 0;
  for (const p of parts) { all.set(p, at); at += p.byteLength; }
  return new TextDecoder().decode(all);
}
