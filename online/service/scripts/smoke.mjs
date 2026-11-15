#!/usr/bin/env node
// Smoke test of a running service: node scripts/smoke.mjs <base url> [--relay]
//
//   info, create, lookup, poll, join, poll, close, lookup (gone) with two fake members, then exits 0; exits 1 on any surprise.
//   --relay also asks route 6 for relay credentials once (it MINTS one real credential on the live service: use it once after the TURN
//   secrets are set) and checks the answer holds a UDP turn: url with a username.
// The fake members use 198.18.0.1 and 198.18.0.2 (the benchmark range: not private, not in the service's refused list, so the real
// service's address check accepts them). Nothing is stored beyond the room, which the last step closes.

const base = (process.argv[2] || '').replace(/\/$/, '');
const wantRelay = process.argv.includes('--relay');
if (!/^https?:\/\/[^/]+$/.test(base)) { console.error('usage: node scripts/smoke.mjs <base url> [--relay]'); process.exit(2); }

const V = 'v=2.0.0';
let step = 0;
const fail = (msg) => { console.error(`smoke FAILED at step ${step}: ${msg}`); process.exit(1); };
async function call(method, path, body) {
  step++;
  const init = { method, headers: { 'user-agent': 'VitAdhoc-smoke/2.0' } };
  if (body !== undefined) { init.body = JSON.stringify(body); init.headers['content-type'] = 'application/json'; }
  let resp;
  try { resp = await fetch(`${base}${path}${path.includes('?') ? '&' : '?'}${V}`, init); } catch (e) { fail(`${method} ${path}: no answer (${e.message})`); }
  const text = await resp.text();
  let data = null;
  try { data = JSON.parse(text); } catch { /* checked below */ }
  return { status: resp.status, data, text };
}
const expect = (cond, what, r) => { if (!cond) fail(`${what} (status ${r.status}, body ${r.text.slice(0, 200)})`); console.log(`ok ${step} ${what}`); };

const host = { kind: 1, console: 'gba', game: 'SMOK', rom: '', title: 'Smoke test', nick: 'SmokeHost', ver: '2.0.0', compat: '00000000', pub: '198.18.0.1:40001', lan: '192.168.1.2:40001', sym: 0, relayOnly: 0 };
const guest = { nick: 'SmokeGuest', ver: '2.0.0', compat: '00000000', pub: '198.18.0.2:40002', lan: '192.168.1.3:40002', sym: 0, relayOnly: 0 };

let r = await call('GET', '/v1/info');
expect(r.status === 200 && r.data && r.data.service === 'vitadhoc' && typeof r.data.min === 'string', 'info', r);
const relayConfigured = r.data.relay === true;

r = await call('POST', '/v1/rooms', host);
expect(r.status === 201 && /^[A-Z2-9]{5}$/.test(r.data.code) && r.data.token && r.data.punchKey, 'create', r);
const { code, token: ht } = r.data;

r = await call('GET', `/v1/rooms/${code}`);
expect(r.status === 200 && r.data.state === 'open' && r.data.game === 'SMOK', 'lookup', r);

r = await call('POST', `/v1/rooms/${code}/poll`, { token: ht });
expect(r.status === 200 && r.data.state === 'open' && r.data.peer === null, 'host poll (open)', r);

r = await call('POST', `/v1/rooms/${code}/join`, guest);
expect(r.status === 200 && r.data.token && r.data.mode === 'punch' && r.data.peer && r.data.peer.nick === 'SmokeHost', 'join', r);
const gt = r.data.token;

await new Promise((res) => setTimeout(res, 900)); // the service lets one poll through per token per 800 ms
r = await call('POST', `/v1/rooms/${code}/poll`, { token: ht });
expect(r.status === 200 && r.data.state === 'joined' && r.data.peer && r.data.peer.nick === 'SmokeGuest', 'host poll (joined)', r);

if (wantRelay) {
  r = await call('POST', `/v1/rooms/${code}/relay`, { token: gt });
  const ok = r.status === 200 && Array.isArray(r.data.servers) && r.data.servers.some((s) => s.username && s.urls.some((u) => /^turn:.*transport=udp/.test(u)));
  expect(ok, 'relay credentials (a UDP turn: url with a username)', r);
} else if (!relayConfigured) {
  console.log('note: relay is not configured on this service (info.relay is false); pass --relay after setting the TURN secrets');
}

r = await call('POST', `/v1/rooms/${code}/close`, { token: ht, reason: 'linked' });
expect(r.status === 200, 'close (linked)', r);

r = await call('GET', `/v1/rooms/${code}`);
expect(r.status === 404 && r.data.error.code === 'not_found', 'lookup after close is gone', r);

console.log('smoke OK');
