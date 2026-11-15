// Relay credentials (route 6, PROTOCOL.md 1.4 and 1.5): the provider, the udp filter, every guard.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { makeServer, joined, T0 } from './helpers.mjs';
import { udpOnly, stubProvider } from '../src/core/turn.js';

const sec = (n) => n * 1000;
const err = (r, status, code) => { assert.equal(r.status, status, r.text); assert.equal(r.data.error.code, code, r.text); };
const day = (ms) => new Date(ms).toISOString().slice(0, 10);

test('stub credentials follow the TURN REST scheme: username <expiry>:<id>, credential = base64(HMAC-SHA1(secret, username)), valid for 3600 s', async () => {
  const s = makeServer();
  const r = await joined(s);
  const a = await s.relay(r.code, r.gt);
  assert.equal(a.status, 200);
  assert.equal(a.data.ttlSec, 3600);
  const turn = a.data.servers.find((x) => x.username);
  assert.ok(turn);
  assert.deepEqual(turn.urls, ['turn:198.51.100.9:3478?transport=udp']);
  const [exp, id] = turn.username.split(':');
  assert.equal(Number(exp), Math.floor(T0 / 1000) + 3600);
  assert.match(id, /^stub-[A-Za-z0-9_-]+$/);
  assert.equal(turn.credential, createHmac('sha1', 'turn-test-secret').update(turn.username).digest('base64'));
  const b = await s.relay(r.code, r.ht);
  assert.notEqual(b.data.servers.find((x) => x.username).username, turn.username, 'each credential is its own');
});

test('udpOnly keeps the UDP turn: urls on 3478, 443 and 53 and the plain stun: url, drops tcp and turns:', () => {
  const out = udpOnly([
    { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
    { urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:443?transport=udp', 'turn:turn.cloudflare.com:53?transport=udp', 'turn:turn.cloudflare.com:3478?transport=tcp',
      'turn:turn.cloudflare.com:80?transport=tcp', 'turns:turn.cloudflare.com:5349?transport=tcp', 'turns:turn.cloudflare.com:443?transport=tcp'], username: 'u', credential: 'c' },
    { urls: ['turns:only.example:443?transport=tcp'], username: 'u', credential: 'c' },
  ]);
  assert.deepEqual(out, [
    { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
    { urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:443?transport=udp', 'turn:turn.cloudflare.com:53?transport=udp'], username: 'u', credential: 'c' },
  ]);
  assert.deepEqual(udpOnly([{ urls: 'turn:x:3478?transport=udp', username: 'u', credential: 'c' }]), [{ urls: ['turn:x:3478?transport=udp'], username: 'u', credential: 'c' }]);
});

test('the Cloudflare provider asks for ttl 3600 with the key id and token, and the answer is filtered to UDP', async () => {
  const calls = [];
  const turnFetch = async (url, init) => {
    calls.push({ url: String(url), init });
    return new Response(JSON.stringify({ iceServers: [
      { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
      { urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:53?transport=udp', 'turn:turn.cloudflare.com:3478?transport=tcp', 'turns:turn.cloudflare.com:5349?transport=tcp'], username: 'cfuser', credential: 'cfpass' },
    ] }), { status: 200, headers: { 'content-type': 'application/json' } });
  };
  const s = makeServer({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid-123456', TURN_KEY_API_TOKEN: 'secret-api-token-xyz' }, { deps: { turnFetch } });
  const r = await joined(s);
  const a = await s.relay(r.code, r.gt);
  assert.equal(a.status, 200);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'https://rtc.live.cloudflare.com/v1/turn/keys/keyid-123456/credentials/generate-ice-servers');
  assert.equal(calls[0].init.method, 'POST');
  assert.deepEqual(JSON.parse(calls[0].init.body), { ttl: 3600 });
  assert.equal(calls[0].init.headers.authorization, 'Bearer secret-api-token-xyz');
  assert.deepEqual(a.data, { ttlSec: 3600, servers: [
    { urls: ['stun:stun.cloudflare.com:3478', 'stun:stun.cloudflare.com:53'] },
    { urls: ['turn:turn.cloudflare.com:3478?transport=udp', 'turn:turn.cloudflare.com:53?transport=udp'], username: 'cfuser', credential: 'cfpass' },
  ] });
  assert.ok(!a.text.includes('secret-api-token-xyz'), 'the API token never leaves the Worker');
});

test('TURN_TTL_SEC changes the ttl asked upstream and the ttlSec answered', async () => {
  let body;
  const turnFetch = async (u, init) => { body = JSON.parse(init.body); return new Response(JSON.stringify({ iceServers: [] }), { status: 200 }); };
  const s = makeServer({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid-123456', TURN_KEY_API_TOKEN: 'tok-tok-tok-tok-tok', TURN_TTL_SEC: '1800' }, { deps: { turnFetch } });
  const r = await joined(s);
  assert.equal((await s.relay(r.code, r.gt)).data.ttlSec, 1800);
  assert.deepEqual(body, { ttl: 1800 });
});

test('upstream failure: 502 turn_upstream and the reservation (day counter, member count, address count) is given back', async () => {
  let fail = true;
  const turnFetch = async () => (fail ? new Response('nope', { status: 500 }) : new Response(JSON.stringify({ iceServers: [{ urls: ['turn:t:3478?transport=udp'], username: 'u', credential: 'c' }] }), { status: 200 }));
  const s = makeServer({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid-123456', TURN_KEY_API_TOKEN: 'tok-tok-tok-tok-tok', RATE_LIMIT_SCALE: '1', MINTS_PER_MEMBER: '1', MINTS_PER_ADDR_DAY: '1', TURN_MAX_CREDS_PER_DAY: '1' }, { deps: { turnFetch } });
  const r = await joined(s);
  err(await s.relay(r.code, r.gt), 502, 'turn_upstream');
  assert.equal(s.setting('turn_day'), `${day(T0)}:0`);
  assert.equal(s.rows()[0].guest_mints, 0);
  fail = false;
  assert.equal((await s.relay(r.code, r.gt)).status, 200, 'the failure used up nothing: the single credential of everything is still there');
  assert.equal(s.setting('turn_day'), `${day(T0)}:1`);
  // a thrown network error is a 502 too
  const s2 = makeServer({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid-123456', TURN_KEY_API_TOKEN: 'tok-tok-tok-tok-tok' }, { deps: { turnFetch: async () => { throw new Error('boom'); } } });
  const r2 = await joined(s2);
  err(await s2.relay(r2.code, r2.gt), 502, 'turn_upstream');
});

test('the daily guard: TURN_MAX_CREDS_PER_DAY credentials, counted at each mint (reserved before the upstream call); 503 relay_budget, retry 3600; a new day resets it', async () => {
  const s = makeServer({ TURN_MAX_CREDS_PER_DAY: '3', MINTS_PER_MEMBER: '100', MINTS_PER_ROOM: '100', RATE_LIMIT_SCALE: '1000' });
  const r = await joined(s);
  for (let i = 0; i < 3; i++) assert.equal((await s.relay(r.code, i % 2 ? r.ht : r.gt)).status, 200);
  assert.equal(s.setting('turn_day'), `${day(T0)}:3`);
  const refused = await s.relay(r.code, r.gt);
  err(refused, 503, 'relay_budget');
  assert.equal(refused.data.error.retryAfterSec, 3600);
  assert.equal(refused.headers.get('retry-after'), '3600');
  s.advance(sec(21 * 3600));
  const r2 = await joined(s);
  assert.equal((await s.relay(r2.code, r2.gt)).status, 200);
  assert.equal(s.setting('turn_day'), `${day(T0 + 21 * 3600 * 1000)}:1`);
});

test('the daily guard reserves BEFORE the upstream call: two requests in flight at the limit cannot both pass', async () => {
  let release;
  const gate = new Promise((res) => { release = res; });
  const turnFetch = async () => { await gate; return new Response(JSON.stringify({ iceServers: [{ urls: ['turn:t:3478?transport=udp'], username: 'u', credential: 'c' }] }), { status: 200 }); };
  const s = makeServer({ TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid-123456', TURN_KEY_API_TOKEN: 'tok-tok-tok-tok-tok', TURN_MAX_CREDS_PER_DAY: '1', RATE_LIMIT_SCALE: '1000' }, { deps: { turnFetch } });
  const r = await joined(s);
  const a = s.relay(r.code, r.gt);
  const b = s.relay(r.code, r.ht);
  await new Promise((res) => setTimeout(res, 20));
  release();
  const [x, y] = await Promise.all([a, b]);
  assert.deepEqual([x.status, y.status].sort(), [200, 503]);
});

test('the monthly guard counts credentials USED: the first relayed address of each member (route 7), not the mint; at TURN_MONTHLY_BUDGET_MB route 6 answers 503 relay_budget (86400)', async () => {
  const s = makeServer({ TURN_EST_MB_PER_USE: '50', TURN_MONTHLY_BUDGET_MB: '100', RATE_LIMIT_SCALE: '1000' });
  const r = await joined(s);
  for (const t of [r.gt, r.ht, r.gt, r.ht]) assert.equal((await s.relay(r.code, t)).status, 200);
  assert.equal(s.setting('turn_month'), undefined, 'four mints, none used: the month is still empty');
  await s.endpoint(r.code, r.gt, '198.18.0.2:4000');
  assert.equal(s.setting('turn_month'), `${day(T0).slice(0, 7)}:1`);
  await s.endpoint(r.code, r.gt, '198.18.0.2:4001');
  assert.equal(s.setting('turn_month'), `${day(T0).slice(0, 7)}:1`, 'the same member posting again is not a second use');
  assert.equal((await s.relay(r.code, r.ht)).status, 200, '1 used x 50 MB < 100');
  await s.endpoint(r.code, r.ht, '198.18.0.1:3000');
  assert.equal(s.setting('turn_month'), `${day(T0).slice(0, 7)}:2`);
  const r2 = await joined(s);
  const refused = await s.relay(r2.code, r2.gt);
  err(refused, 503, 'relay_budget');
  assert.equal(refused.data.error.retryAfterSec, 86400);
  s.advance(sec(25 * 24 * 3600)); // into December
  const r3 = await joined(s);
  assert.equal((await s.relay(r3.code, r3.gt)).status, 200);
});

test('relay_unavailable when TURN is not configured; info says relay false', async () => {
  for (const env of [{ TURN_PROVIDER: '' }, { TURN_PROVIDER: 'cloudflare' }, { TURN_PROVIDER: 'cloudflare', TURN_KEY_ID: 'keyid-123456' }]) {
    const s = makeServer(env);
    const r = await joined(s);
    err(await s.relay(r.code, r.gt), 503, 'relay_unavailable');
    assert.equal((await s.call('GET', '/v1/info')).data.relay, false);
  }
});

test('route 6 needs a member (401), a second member (403), and does not change the room', async () => {
  const s = makeServer();
  const c = await s.create();
  err(await s.relay(c.data.code, c.data.token), 403, 'relay_not_allowed');
  err(await s.relay(c.data.code, 'A'.repeat(22)), 401, 'bad_token');
  const j = await s.join(c.data.code);
  const before = JSON.stringify(s.rows().map(({ host_mints, guest_mints, ...rest }) => rest));
  assert.equal((await s.relay(c.data.code, j.data.token)).status, 200);
  assert.equal(JSON.stringify(s.rows().map(({ host_mints, guest_mints, ...rest }) => rest)), before, 'only the credential counter moved');
  assert.equal(s.rows()[0].guest_mints, 1);
});

test('stubProvider without URLs gives random meaningless credentials', async () => {
  const out = await stubProvider({}).mint(3600, 0);
  const t = out.servers.find((x) => x.username);
  assert.match(t.urls[0], /^turn:turn\.example\.invalid/);
  assert.equal(t.username.split(':')[0], '3600');
});

test('a relayed address posted without a credential is not a use: join, post, cancel in a loop cannot spend the month', async () => {
  const s = makeServer({ TURN_EST_MB_PER_USE: '50', TURN_MONTHLY_BUDGET_MB: '100', RATE_LIMIT_SCALE: '1000' });
  const c = await s.create();
  for (let i = 0; i < 6; i++) {
    const j = await s.join(c.data.code);
    assert.equal(j.status, 200);
    assert.equal((await s.endpoint(c.data.code, j.data.token, `198.18.0.2:${4000 + i}`)).status, 200, 'the post itself is served');
    assert.equal((await s.close(c.data.code, j.data.token, 'cancel')).status, 200);
  }
  assert.equal(s.setting('turn_month'), undefined, 'no credential was minted: nothing was used');
  // one credential minted in the room: one use, however many guests post after it
  let j = await s.join(c.data.code);
  assert.equal((await s.relay(c.data.code, j.data.token)).status, 200);
  await s.endpoint(c.data.code, j.data.token, '198.18.0.2:5000');
  assert.equal(s.setting('turn_month'), `${day(T0).slice(0, 7)}:1`);
  for (let i = 0; i < 4; i++) {
    await s.close(c.data.code, j.data.token, 'cancel');
    j = await s.join(c.data.code);
    await s.endpoint(c.data.code, j.data.token, `198.18.0.2:${5001 + i}`);
  }
  assert.equal(s.setting('turn_month'), `${day(T0).slice(0, 7)}:1`, 'uses never pass the mints of the room');
  assert.equal((await s.relay(c.data.code, j.data.token)).status, 200, 'the guard did not trip');
});
