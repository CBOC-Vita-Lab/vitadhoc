// The route table and the checks that need nothing but the request itself, shared by the Worker (so junk never wakes the Durable Object,
// where the free plan's request and row budgets are spent) and by the application (defence in depth, and the dev server).

import { ApiError, VERSION_RE, cmpVersion } from './util.js';

export const MAX_BODY = 1024;

/**
 * Every route: method, pattern, name, isNew (a NEW session: refused from 80 % of the daily budget), isLeave (frees a room: the budget never
 * refuses it), isInfo (never refused by the budget or the kill switch, never version-gated: it is how a client, the smoke test and the deploy
 * script see whether the service is busy). Order matters only where two patterns could match one path (none do).
 */
export const ROUTES = [
  { method: 'GET', pattern: '/v1/info', name: 'info', isInfo: true },
  { method: 'POST', pattern: '/v1/rooms', name: 'create', isNew: true },
  { method: 'GET', pattern: '/v1/rooms/:code', name: 'lookup' },
  { method: 'POST', pattern: '/v1/rooms/:code/join', name: 'join' },
  { method: 'POST', pattern: '/v1/rooms/:code/poll', name: 'poll' },
  { method: 'POST', pattern: '/v1/rooms/:code/relay', name: 'relay' },
  { method: 'POST', pattern: '/v1/rooms/:code/endpoint', name: 'endpoint' },
  { method: 'POST', pattern: '/v1/rooms/:code/close', name: 'close', isLeave: true },
];

/** pattern -> RegExp with named groups; the code segment is any 1..16 characters (the room code check is the object's: a bad code is 404). */
export const compilePattern = (pattern) => new RegExp(`^${pattern.replace(/:(\w+)/g, '(?<$1>[^/]{1,16})')}$`);
const COMPILED = ROUTES.map((r) => ({ ...r, re: compilePattern(r.pattern) }));

/** The route a request matches (with `params`), or null: unknown path OR method (both are 404 not_found, PROTOCOL.md 1.2). */
export function matchRoute(method, pathname) {
  for (const r of COMPILED) {
    if (r.method !== method) continue;
    const m = r.re.exec(pathname);
    if (m) return { ...r, params: m.groups || {} };
  }
  return null;
}

/** "<METHOD> <pattern>" is not needed; the Worker and the object only use the route's name. */

/**
 * What the Worker may decide without the object: unknown path or method (404), empty User-Agent (400 bad_request), a missing or malformed
 * `v` (400 bad_version), a version below MIN_APP_VERSION (426), a body announced over MAX_BODY (413). Returns { route } or { error }.
 * `/v1/info` takes `v` but never refuses it.
 * @param {Request} request @param {{minApp: string, latestApp: string}} cfg
 */
export function edgeCheck(request, cfg) {
  const url = new URL(request.url);
  const route = matchRoute(request.method, url.pathname);
  if (!route) return { error: new ApiError(404, 'not_found', 'no such endpoint') };
  if (!String(request.headers.get('user-agent') || '').trim()) {
    return { error: new ApiError(400, 'bad_request', 'a User-Agent header is required') };
  }
  if (!route.isInfo) {
    const v = url.searchParams.get('v');
    if (v === null || !VERSION_RE.test(v)) return { error: new ApiError(400, 'bad_version', 'v must be the app version, such as 2.0.0') };
    if (cmpVersion(v, cfg.minApp) < 0) {
      return { error: new ApiError(426, 'update_required', 'this version is too old for online play', { min: cfg.minApp, latest: cfg.latestApp }) };
    }
  }
  if (request.method === 'POST') {
    const cl = request.headers.get('content-length');
    if (cl !== null && (!/^\d{1,9}$/.test(cl) || Number(cl) > MAX_BODY)) return { error: new ApiError(413, 'too_large', 'request body too large') };
  }
  return { route };
}
