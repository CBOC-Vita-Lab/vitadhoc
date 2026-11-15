#!/usr/bin/env node
// The dev server: the service's application code (src/core, the same code the Durable Object runs) behind plain HTTP on 127.0.0.1, over
// node:sqlite. For the Vita twins (vita.ini online_url), the client tests and smoke.mjs.
//
//   node dev/server.mjs [--port N] [--env K=V]... [--clock-skew-ms N]
//
// --port 0 (default) = any free port. Prints "READY <port>", then one line per request: "REQ <method> <path> <status> <code or ->" (no query,
// no address, no token). Defaults for tests: ALLOW_PRIVATE_ADDR=1, TURN_PROVIDER=stub with TURN_STUB_URLS / TURN_STUB_SECRET, and
// RATE_LIMIT_SCALE=1000 (so twins that host again and again from 127.0.0.1 are not limited; pass --env RATE_LIMIT_SCALE=1 for the real limits).
// The client address is the header x-test-ip when present, else the socket's. Test controls (never part of the protocol):
//   POST /_test/evict    drops the application object and builds a new one over the SAME database (an idle object leaving memory)
//   POST /_test/advance  {"ms": N} moves the server clock forward (expire a room without waiting)
//   GET  /_test/stats    {requests, rooms, budget, turn}

import http from 'node:http';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { createApp } from '../src/core/app.js';
import { openNodeSql } from '../src/sqlite-node.js';

export const DEV_ENV = {
  ALLOW_PRIVATE_ADDR: '1',
  TURN_PROVIDER: 'stub',
  TURN_STUB_URLS: 'turn:198.51.100.9:3478?transport=udp',
  TURN_STUB_SECRET: 'turn-test-secret',
  RATE_LIMIT_SCALE: '1000',
};

export async function startDev({ port = 0, env = {}, clockSkewMs = 0, log = (l) => console.log(l) } = {}) {
  const fullEnv = { ...DEV_ENV, ...env };
  const store = openNodeSql(':memory:'); // one database for the process's life
  const clock = { offset: clockSkewMs };
  const now = () => Date.now() + clock.offset;
  let requestsBefore = 0;
  const build = () => createApp({ sql: store.sql, tx: store.tx, env: fullEnv, now });
  let app = build();

  const server = http.createServer(async (req, res) => {
    const chunks = [];
    for await (const ch of req) chunks.push(ch);
    const body = Buffer.concat(chunks);
    const url = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
    const reply = (status, obj) => res.writeHead(status, { 'content-type': 'application/json' }).end(JSON.stringify(obj));
    if (url.pathname.startsWith('/_test/')) {
      if (url.pathname === '/_test/evict' && req.method === 'POST') { requestsBefore += app.status().requests; app = build(); return reply(200, { ok: true }); }
      if (url.pathname === '/_test/advance' && req.method === 'POST') {
        const j = JSON.parse(body.toString('utf8') || '{}');
        if (Number.isFinite(j.ms)) clock.offset += j.ms;
        return reply(200, { nowMs: now() });
      }
      if (url.pathname === '/_test/stats' && req.method === 'GET') { const s = app.status(); return reply(200, { ...s, requests: s.requests + requestsBefore }); }
      return reply(404, { error: { code: 'not_found', message: 'no such test control' } });
    }
    const headers = new Headers();
    for (const [k, v] of Object.entries(req.headers)) if (typeof v === 'string') headers.set(k, v);
    headers.set('cf-connecting-ip', String(req.headers['x-test-ip'] || (req.socket.remoteAddress || '127.0.0.1').replace(/^::ffff:/, '')));
    const hasBody = !['GET', 'HEAD'].includes(req.method);
    const resp = await app.handle(new Request(url, { method: req.method, headers, body: hasBody && body.length ? body : undefined }));
    const text = await resp.text();
    let code = '-';
    try { const j = JSON.parse(text); if (j && j.error) code = j.error.code; } catch { /* not JSON */ }
    log(`REQ ${req.method} ${url.pathname} ${resp.status} ${code}`);
    const out = {};
    resp.headers.forEach((v, k) => { out[k] = v; });
    res.writeHead(resp.status, out).end(text);
  });
  await new Promise((r) => server.listen(port, '127.0.0.1', r));
  return {
    port: server.address().port,
    url: `http://127.0.0.1:${server.address().port}`,
    env: fullEnv,
    close: () => new Promise((r) => { server.closeAllConnections?.(); server.close(() => { store.close(); r(); }); }),
  };
}

function parseArgs(argv) {
  const o = { env: {} };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--port') o.port = Number(argv[++i]);
    else if (a === '--env') {
      const kv = String(argv[++i] || '');
      const at = kv.indexOf('=');
      if (at < 1) { console.error('--env needs K=V'); process.exit(2); }
      o.env[kv.slice(0, at)] = kv.slice(at + 1);
    } else if (a === '--clock-skew-ms') o.clockSkewMs = Number(argv[++i]);
    else { console.error(`unknown option ${a}`); process.exit(2); }
  }
  return o;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const s = await startDev(parseArgs(process.argv.slice(2)));
  console.log(`READY ${s.port}`);
}
