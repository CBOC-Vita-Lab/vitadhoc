// Test harness: the real application over node:sqlite with an injectable clock, and the real Worker in front of a counting stand-in
// for the Durable Object binding.
import { register } from 'node:module';
import { createApp } from '../src/core/app.js';
import { openNodeSql } from '../src/sqlite-node.js';

register('./stubs/cf-loader.mjs', import.meta.url); // "cloudflare:workers" -> the stub, for src/worker.js

export const T0 = Date.UTC(2026, 10, 15, 3, 0, 0); // 2026-11-15 03:00 UTC
export const V = 'v=2.0.0';
export const UA = 'VitAdhoc/2.0.0 (PlayStation Vita)';

export const hostDesc = (o = {}) => ({
  kind: 1, console: 'gba', game: 'BPEE', rom: '9f3a01c2', title: 'Pokemon Emerald', nick: 'Alice', ver: '2.0.0', compat: '1a2b3c4d',
  pub: '203.0.113.7:19019', lan: '192.168.1.23:19019', sym: 0, relayOnly: 0, ...o,
});
export const guestDesc = (o = {}) => ({
  nick: 'Bob', ver: '2.0.0', compat: '1a2b3c4d', pub: '198.51.100.20:20020', lan: '192.168.7.5:20020', sym: 0, relayOnly: 0, ...o,
});

/** Builds a server over `store` (default: a fresh in-memory database). `rebuild()` drops the app object and makes a new one over the same
 *  database, which is what Cloudflare does when an idle object leaves memory. */
export function makeServer(envOverrides = {}, opts = {}) {
  const store = opts.store || openNodeSql();
  const clock = { now: opts.t0 ?? T0 };
  const env = { RATE_LIMIT_SCALE: '1000', ALLOW_PRIVATE_ADDR: '0', TURN_PROVIDER: 'stub', TURN_STUB_URLS: 'turn:198.51.100.9:3478?transport=udp', TURN_STUB_SECRET: 'turn-test-secret', ...envOverrides };
  const make = () => createApp({ sql: store.sql, tx: store.tx, env, now: () => clock.now, ...(opts.deps || {}) });
  const s = { app: make(), store, clock, env };
  let ipCounter = 0;
  s.rebuild = () => { s.app = make(); return s.app; };
  s.advance = (ms) => { clock.now += ms; };
  /** call(method, path, body?, {ip, headers, raw, noV}) -> {status, data, headers} */
  s.call = async (method, path, body, o = {}) => {
    const h = { 'cf-connecting-ip': o.ip || `198.51.100.${(ipCounter++ % 250) + 1}`, 'user-agent': UA, ...(o.headers || {}) };
    const init = { method, headers: h };
    if (body !== undefined || o.raw !== undefined) {
      init.body = o.raw !== undefined ? o.raw : JSON.stringify(body);
      if (!o.noType) h['content-type'] = 'application/json';
      h['content-length'] = String(Buffer.byteLength(init.body));
    }
    const sep = path.includes('?') ? '&' : '?';
    const resp = await s.app.handle(new Request(`https://test.invalid${path}${o.noV ? '' : `${sep}${V}`}`, init));
    const text = await resp.text();
    let data = null;
    try { data = JSON.parse(text); } catch { data = text; }
    return { status: resp.status, data, headers: resp.headers, text };
  };
  s.create = (desc = hostDesc(), o = {}) => s.call('POST', '/v1/rooms', desc, o);
  s.join = (code, desc = guestDesc(), o = {}) => s.call('POST', `/v1/rooms/${code}/join`, desc, o);
  s.poll = (code, token, extra = {}, o = {}) => s.call('POST', `/v1/rooms/${code}/poll`, { token, ...extra }, o);
  s.relay = (code, token, o = {}) => s.call('POST', `/v1/rooms/${code}/relay`, { token }, o);
  s.endpoint = (code, token, relay, o = {}) => s.call('POST', `/v1/rooms/${code}/endpoint`, { token, relay }, o);
  s.close = (code, token, reason = 'linked', o = {}) => s.call('POST', `/v1/rooms/${code}/close`, { token, reason }, o);
  s.rows = (q = 'SELECT * FROM rooms') => store.sql.exec(q).toArray();
  s.setting = (key) => (store.sql.exec('SELECT value FROM settings WHERE key = ?', key).toArray()[0] || {}).value;
  return s;
}

/** A room with a host and a guest joined; returns the codes and tokens. */
export async function joined(srv, hostOver = {}, guestOver = {}, o = {}) {
  const c = await srv.create(hostDesc(hostOver), o.host);
  if (c.status !== 201) throw new Error(`create failed ${c.status} ${c.text}`);
  const j = await srv.join(c.data.code, guestDesc(guestOver), o.guest);
  if (j.status !== 200) throw new Error(`join failed ${j.status} ${j.text}`);
  return { code: c.data.code, ht: c.data.token, gt: j.data.token, created: c.data, joined: j.data };
}

/** The Worker (src/worker.js) in front of the application, with a counting stand-in for the Durable Object binding. */
export async function makeWorker(envOverrides = {}, opts = {}) {
  const mod = await import(`../src/worker.js?fresh=${Math.random()}`); // a fresh isolate: module state starts empty
  const store = openNodeSql();
  const clock = { now: opts.t0 ?? Date.now() };
  const env = { ALLOW_PRIVATE_ADDR: '0', TURN_PROVIDER: 'stub', ...envOverrides };
  const app = createApp({ sql: store.sql, tx: store.tx, env, now: () => Date.now() });
  const stats = { doRequests: 0 };
  env.ROOMS = { idFromName: (n) => n, get: () => ({ fetch: async (req) => { stats.doRequests++; return app.handle(req); } }) };
  const call = async (method, path, { body, ip = '198.51.100.9', headers = {}, noV = false } = {}) => {
    const h = { 'cf-connecting-ip': ip, 'user-agent': UA, ...headers };
    const init = { method, headers: h };
    if (body !== undefined) { init.body = JSON.stringify(body); h['content-type'] = 'application/json'; h['content-length'] = String(Buffer.byteLength(init.body)); }
    const sep = path.includes('?') ? '&' : '?';
    const resp = await mod.default.fetch(new Request(`https://test.invalid${path}${noV ? '' : `${sep}${V}`}`, init), env);
    const text = await resp.text();
    let data = null;
    try { data = JSON.parse(text); } catch { data = text; }
    return { status: resp.status, data, headers: resp.headers };
  };
  return { worker: mod.default, env, app, stats, call, store, clock };
}
