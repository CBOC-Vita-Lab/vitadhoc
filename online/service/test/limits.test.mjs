// Every limit of PROTOCOL.md 1.2 (RATE_LIMIT_SCALE=1 so they are real), with an injected clock.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, guestDesc, joined, T0, V, UA } from './helpers.mjs';

const sec = (n) => n * 1000;
const rl = (r, status, reason) => { assert.equal(r.status, status, r.text); if (reason) { assert.equal(r.data.error.code, 'rate_limited', r.text); assert.equal(r.data.error.reason, reason, r.text); } };
const real = (o = {}) => makeServer({ RATE_LIMIT_SCALE: '1', ...o });

test('route 2, per address: burst 3, then one per 120 s (429 slow_down)', async () => {
  const s = real({ ROOMS_PER_ADDR: '100' });
  const ip = '198.51.100.10';
  for (let i = 0; i < 3; i++) assert.equal((await s.create(hostDesc(), { ip })).status, 201);
  const r = await s.create(hostDesc(), { ip });
  rl(r, 429, 'slow_down');
  assert.ok(r.data.error.retryAfterSec >= 100 && r.data.error.retryAfterSec <= 120, String(r.data.error.retryAfterSec));
  s.advance(sec(119));
  rl(await s.create(hostDesc(), { ip }), 429, 'slow_down');
  s.advance(sec(2));
  assert.equal((await s.create(hostDesc(), { ip })).status, 201);
  assert.equal((await s.create(hostDesc(), { ip: '198.51.100.11' })).status, 201, 'another address');
});

test('route 2: at most 4 live rooms per address (429 too_many_rooms); closing one frees a place', async () => {
  const s = real({ CREATE_BURST: '100' });
  const ip = '198.51.100.12';
  const made = [];
  for (let i = 0; i < 4; i++) { const c = await s.create(hostDesc(), { ip }); assert.equal(c.status, 201); made.push(c.data); }
  rl(await s.create(hostDesc(), { ip }), 429, 'too_many_rooms');
  await s.close(made[0].code, made[0].token, 'linked', { ip });
  assert.equal((await s.create(hostDesc(), { ip })).status, 201);
  rl(await s.create(hostDesc(), { ip }), 429, 'too_many_rooms');
  s.advance(sec(601));
  assert.equal((await s.create(hostDesc(), { ip })).status, 201, 'expired rooms do not count');
});

test('route 2: 40 rooms a day per address, then 429 daily_cap until midnight', async () => {
  const s = real({ CREATE_BURST: '1000', ROOMS_PER_ADDR: '1000', MAX_ROOMS: '1000', ADDR_DAILY: '0', DAILY_REQUEST_BUDGET: '0' });
  const ip = '198.51.100.13';
  for (let i = 0; i < 40; i++) assert.equal((await s.create(hostDesc(), { ip })).status, 201, `room ${i}`);
  const r = await s.create(hostDesc(), { ip });
  rl(r, 429, 'daily_cap');
  assert.equal(r.data.error.retryAfterSec, 21 * 3600, 'until 00:00 UTC (it is 03:00)');
  s.advance(sec(21 * 3600));
  assert.equal((await s.create(hostDesc(), { ip })).status, 201);
});

test('route 3, per address: burst 8, then one per 5 s', async () => {
  const s = real();
  const c = await s.create();
  const ip = '198.51.100.14';
  for (let i = 0; i < 8; i++) assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip })).status, 200);
  const r = await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip });
  rl(r, 429, 'slow_down');
  assert.ok(r.data.error.retryAfterSec >= 1 && r.data.error.retryAfterSec <= 5);
  s.advance(sec(5));
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip })).status, 200);
});

test('route 4, per address: burst 4, then one per 10 s', async () => {
  const s = real({ CREATE_BURST: '100', ROOMS_PER_ADDR: '100' });
  const ip = '198.51.100.15';
  const codes = [];
  for (let i = 0; i < 6; i++) codes.push((await s.create(hostDesc(), { ip: `198.51.100.${100 + i}` })).data.code);
  for (let i = 0; i < 4; i++) assert.equal((await s.join(codes[i], guestDesc(), { ip })).status, 200);
  rl(await s.join(codes[4], guestDesc(), { ip }), 429, 'slow_down');
  s.advance(sec(10));
  assert.equal((await s.join(codes[4], guestDesc(), { ip })).status, 200);
});

test('route 5, per token: one poll per 800 ms (429 slow_down, retryAfterSec 1)', async () => {
  const s = real();
  const c = await s.create();
  assert.equal((await s.poll(c.data.code, c.data.token)).status, 200);
  const r = await s.poll(c.data.code, c.data.token);
  rl(r, 429, 'slow_down');
  assert.equal(r.data.error.retryAfterSec, 1);
  s.advance(799);
  rl(await s.poll(c.data.code, c.data.token), 429, 'slow_down');
  s.advance(1);
  assert.equal((await s.poll(c.data.code, c.data.token)).status, 200);
  // another token is separate (and so is the same token from another address: the limit is the token's)
  const d = await s.create(hostDesc(), { ip: '198.51.100.31' });
  assert.equal((await s.poll(d.data.code, d.data.token)).status, 200);
});

test('route 6, per member 2, per room 4, per address 40 a day', async () => {
  const s = real({ MINTS_PER_ADDR_DAY: '3', MINTS_PER_ROOM: '100' });
  const gip = '198.51.100.40';
  const r = await joined(s, {}, {}, { guest: { ip: gip }, host: { ip: '198.51.100.41' } });
  assert.equal((await s.relay(r.code, r.gt, { ip: gip })).status, 200);
  assert.equal((await s.relay(r.code, r.gt, { ip: gip })).status, 200);
  rl(await s.relay(r.code, r.gt, { ip: gip }), 429, 'relay_limit');
  assert.equal((await s.relay(r.code, r.ht, { ip: '198.51.100.41' })).status, 200, 'the host has its own two');
  // per room: 4 in all
  const s2 = real({ MINTS_PER_MEMBER: '3', MINTS_PER_ROOM: '4' });
  const r2 = await joined(s2);
  assert.equal((await s2.relay(r2.code, r2.gt)).status, 200);
  assert.equal((await s2.relay(r2.code, r2.gt)).status, 200);
  assert.equal((await s2.relay(r2.code, r2.ht)).status, 200);
  assert.equal((await s2.relay(r2.code, r2.ht)).status, 200);
  rl(await s2.relay(r2.code, r2.ht), 429, 'relay_limit');
  rl(await s2.relay(r2.code, r2.gt), 429, 'relay_limit');
  // per address a day
  const s3 = real({ MINTS_PER_ADDR_DAY: '2', MINTS_PER_MEMBER: '10', MINTS_PER_ROOM: '10' });
  const r3 = await joined(s3, {}, {}, { host: { ip: '198.51.100.50' }, guest: { ip: '198.51.100.50' } });
  assert.equal((await s3.relay(r3.code, r3.gt, { ip: '198.51.100.50' })).status, 200);
  assert.equal((await s3.relay(r3.code, r3.ht, { ip: '198.51.100.50' })).status, 200);
  const third = await s3.relay(r3.code, r3.ht, { ip: '198.51.100.50' });
  rl(third, 429, 'relay_limit');
  assert.equal(third.data.error.retryAfterSec, 21 * 3600, 'until 00:00 UTC');
});

test('ADDR_DAILY: 1500 requests per address per UTC day, then 429 daily_cap until midnight; other addresses unaffected', async () => {
  const s = real({ ADDR_DAILY: '1500', DAILY_REQUEST_BUDGET: '0' });
  const ip = '198.51.100.60';
  for (let i = 0; i < 1500; i++) {
    const r = await s.call('GET', '/v1/info', undefined, { ip });
    assert.equal(r.status, 200, `request ${i}`);
  }
  const r = await s.call('GET', '/v1/info', undefined, { ip });
  rl(r, 429, 'daily_cap');
  assert.equal(r.data.error.retryAfterSec, 21 * 3600);
  assert.equal(r.headers.get('x-vad-block'), `addr:${21 * 3600}`);
  assert.equal((await s.call('GET', '/v1/info', undefined, { ip: '198.51.100.61' })).status, 200);
  s.advance(sec(21 * 3600));
  assert.equal((await s.call('GET', '/v1/info', undefined, { ip })).status, 200);
});

test('per-address lookup limit: lookups from one address are limited to the bucket, whatever the code', async () => {
  const s = real();
  const ip = '198.51.100.70';
  const codes = [];
  for (let i = 0; i < 12; i++) codes.push((await s.call('GET', `/v1/rooms/ABCD${'23456789'[i % 8]}`, undefined, { ip })).status);
  assert.deepEqual(codes.slice(0, 8), Array(8).fill(404));
  assert.deepEqual(codes.slice(8), Array(4).fill(429));
});

test('enumeration: 20 misses in an hour, then 429 guessing for the rest of the hour; hits do not count', async () => {
  const s = real({ ADDR_DAILY: '0', DAILY_REQUEST_BUDGET: '0' });
  const ip = '198.51.100.71';
  const c = await s.create(hostDesc(), { ip: '198.51.100.72' });
  s.clock.now = Date.UTC(2026, 10, 15, 0, 30, 0); // inside one UTC hour
  for (let i = 0; i < 20; i++) {
    assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip })).status, 200, 'a hit does not count');
    s.advance(sec(6)); // the lookup bucket refills one per 5 s
    const r = await s.call('GET', `/v1/rooms/ABCD${'23456789'[i % 8]}`, undefined, { ip });
    assert.equal(r.status, 404, `miss ${i}`);
    s.advance(sec(6));
  }
  const r = await s.call('GET', '/v1/rooms/ABCDF', undefined, { ip });
  rl(r, 429, 'guessing');
  // even a right code is refused while guessing: the answer must not tell a hit from a miss
  rl(await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip }), 429, 'guessing');
  assert.ok(r.data.error.retryAfterSec > 0 && r.data.error.retryAfterSec <= 3600);
  // join and the token routes are guesses too
  rl(await s.join(c.data.code, guestDesc(), { ip }), 429, 'guessing');
  // another address is not affected
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip: '198.51.100.73' })).status, 200);
  // the next hour: free again
  s.advance(r.data.error.retryAfterSec * 1000);
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip })).status, 200);
});

test('enumeration: 60 misses in a UTC day, then 429 guessing until midnight', async () => {
  const s = real({ ADDR_DAILY: '0', DAILY_REQUEST_BUDGET: '0', MISSES_PER_HOUR: '20' });
  const ip = '198.51.100.74';
  s.clock.now = Date.UTC(2026, 10, 15, 0, 5, 0);
  let n = 0;
  for (let h = 0; h < 3; h++) {
    for (let i = 0; i < 20; i++) {
      const r = await s.call('GET', `/v1/rooms/ABC${'23456789'[n % 8]}${'23456789'[(n >> 3) % 8]}`, undefined, { ip });
      assert.equal(r.status, 404, `miss ${n}`);
      n++;
      s.advance(sec(6));
    }
    s.clock.now = Date.UTC(2026, 10, 15, h + 1, 5, 0);
  }
  assert.equal(n, 60);
  const r = await s.call('GET', '/v1/rooms/ABCDF', undefined, { ip });
  rl(r, 429, 'guessing');
  assert.equal(r.data.error.retryAfterSec, 75300, 'until 00:00 UTC (it is 03:05)');
  assert.ok(r.data.error.retryAfterSec > 3600 * 20, 'the day block outlasts the hour');
  s.clock.now = Date.UTC(2026, 10, 15, 6, 0, 0);
  rl(await s.call('GET', '/v1/rooms/ABCDF', undefined, { ip }), 429, 'guessing');
  s.clock.now = Date.UTC(2026, 10, 16, 0, 0, 1);
  assert.equal((await s.call('GET', '/v1/rooms/ABCDF', undefined, { ip })).status, 404);
});

test('misses: a wrong token on poll / relay / endpoint / close and an unknown code on those routes count too', async () => {
  const s = real({ ADDR_DAILY: '0', DAILY_REQUEST_BUDGET: '0', MISSES_PER_HOUR: '5' });
  const ip = '198.51.100.75';
  const r = await joined(s);
  let last;
  for (let i = 0; i < 6; i++) {
    last = await s.call('POST', `/v1/rooms/${r.code}/close`, { token: 'B'.repeat(22), reason: 'cancel' }, { ip });
    s.advance(sec(1));
  }
  assert.equal(last.status, 429);
  assert.equal(last.data.error.reason, 'guessing');
  // the real members, from their own address, are fine
  assert.equal((await s.poll(r.code, r.ht, {}, { ip: '198.51.100.76' })).status, 200);
});

test('every limit scales with RATE_LIMIT_SCALE', async () => {
  const s = makeServer({ RATE_LIMIT_SCALE: '2', ROOMS_PER_ADDR: '100' });
  const ip = '198.51.100.80';
  for (let i = 0; i < 6; i++) assert.equal((await s.create(hostDesc(), { ip })).status, 201, `room ${i}`);
  rl(await s.create(hostDesc(), { ip }), 429, 'slow_down');
});

test('enumeration: the guess counters survive the object leaving memory (a salted fingerprint on disk, never the address)', async () => {
  const s = makeServer({ RATE_LIMIT_SCALE: '1', LOOKUP_BURST: '1000' });
  const c = await s.create();
  const ip = '203.0.113.99';
  for (let i = 0; i < 20; i++) {
    const r = await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip });
    assert.equal(r.status, 404, `miss ${i}`);
    if (i % 5 === 4) s.rebuild(); // the object forgets everything it held in memory
  }
  s.rebuild();
  rl(await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip }), 429, 'guessing');
  rl(await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip }), 429, 'guessing');
  // another address is not touched, and what is stored is not an address
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`, undefined, { ip: '203.0.113.98' })).status, 200);
  const stored = JSON.stringify(s.store.sql.exec('SELECT * FROM misses').toArray()) + String(s.setting('miss_day'));
  assert.ok(!stored.includes('203.0.113'), stored);
  assert.equal(s.store.sql.exec('SELECT COUNT(*) AS n FROM misses').toArray()[0].n, 1);
  // the next hour: the hourly block is over, the day's count goes on
  s.advance(3600 * 1000);
  s.rebuild();
  assert.equal((await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip })).status, 404);
  // the next UTC day: a new salt, yesterday's rows are gone at the first miss
  s.advance(24 * 3600 * 1000);
  s.rebuild();
  assert.equal((await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip: '203.0.113.97' })).status, 404);
  assert.equal(s.store.sql.exec('SELECT COUNT(*) AS n FROM misses').toArray()[0].n, 1);
});

test('UTC-midnight rollover: pre-midnight miss settling after midnight does not wipe day D+1 misses', async () => {
  const s = real({ ADDR_DAILY: '0', DAILY_REQUEST_BUDGET: '0', LOOKUP_BURST: '100', MISSES_PER_HOUR: '2', MISSES_PER_DAY: '2' });
  const day = Math.floor(T0 / 86400000);
  const d0 = Date.UTC(2026, 10, 15, 23, 59, 59, 900);
  const d1 = Date.UTC(2026, 10, 16, 0, 0, 0, 100);
  s.clock.now = d0;

  let releaseBody;
  const gate = new Promise((res) => { releaseBody = res; });
  const stream = new ReadableStream({
    async start(controller) {
      await gate;
      controller.enqueue(new TextEncoder().encode(JSON.stringify({ token: 'A'.repeat(22) })));
      controller.close();
    },
  });
  const pending = s.app.handle(new Request(`https://test.invalid/v1/rooms/ZZZZZ/poll?${V}`, {
    method: 'POST',
    headers: { 'cf-connecting-ip': '203.0.113.10', 'user-agent': UA, 'content-type': 'application/json' },
    body: stream,
    duplex: 'half',
  }));
  await new Promise((r) => setTimeout(r, 15));

  s.clock.now = d1;
  const ipD1 = '203.0.113.11';
  assert.equal((await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip: ipD1 })).status, 404);
  assert.equal((await s.call('GET', '/v1/rooms/ABCDF', undefined, { ip: ipD1 })).status, 404);
  assert.ok(s.setting('miss_day').startsWith(`${day + 1}:`), s.setting('miss_day'));
  assert.equal(s.rows('SELECT * FROM misses').length, 1);

  releaseBody();
  const rOld = await pending;
  assert.equal(rOld.status, 404);
  assert.ok(s.setting('miss_day').startsWith(`${day + 1}:`), `miss_day rolled backwards: ${s.setting('miss_day')}`);
  assert.equal(s.rows('SELECT * FROM misses').length, 1, 'day D+1 misses row must not be deleted');

  s.rebuild();
  rl(await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip: ipD1 }), 429, 'guessing');
});

test('UTC-midnight rollover: pre-midnight relay rollback after midnight does not wipe day D+1 mints or turn_day', async () => {
  let gateRelease;
  const gate = new Promise((res) => { gateRelease = res; });
  let callIdx = 0;
  const turnFetch = async () => {
    const idx = ++callIdx;
    if (idx === 1) {
      await gate;
      return new Response('nope', { status: 500 });
    }
    return new Response(JSON.stringify({ iceServers: [{ urls: ['turn:t:3478?transport=udp'], username: `u${idx}`, credential: 'c' }] }), { status: 200 });
  };
  const s = makeServer({
    TURN_PROVIDER: 'cloudflare',
    TURN_KEY_ID: 'keyid-123456',
    TURN_KEY_API_TOKEN: 'tok-tok-tok-tok-tok',
    RATE_LIMIT_SCALE: '1',
    MINTS_PER_ADDR_DAY: '1',
    MINTS_PER_MEMBER: '10',
    MINTS_PER_ROOM: '10',
    TURN_MAX_CREDS_PER_DAY: '100',
    ADDR_DAILY: '0',
    DAILY_REQUEST_BUDGET: '0',
  }, { deps: { turnFetch } });

  const day = Math.floor(T0 / 86400000);
  const d0 = Date.UTC(2026, 10, 15, 23, 59, 59, 900);
  const d1 = Date.UTC(2026, 10, 16, 0, 0, 0, 100);
  s.clock.now = d0;
  const ip1 = '203.0.113.21';
  const ip2 = '203.0.113.22';
  const r1 = await joined(s);
  const p1 = s.relay(r1.code, r1.gt, { ip: ip1 });
  await new Promise((r) => setTimeout(r, 15));

  s.clock.now = d1;
  const r2 = await joined(s);
  assert.equal((await s.relay(r2.code, r2.gt, { ip: ip2 })).status, 200);
  const r3 = await joined(s);
  assert.equal((await s.relay(r3.code, r3.gt, { ip: ip1 })).status, 200);
  assert.ok(s.setting('mint_day').startsWith(`${day + 1}:`), s.setting('mint_day'));
  assert.equal(s.setting('turn_day'), '2026-11-16:2');
  assert.equal(s.setting('turn_month'), '2026-11:3');

  gateRelease();
  const res1 = await p1;
  assert.equal(res1.status, 502);
  assert.ok(s.setting('mint_day').startsWith(`${day + 1}:`), `mint_day rolled backwards: ${s.setting('mint_day')}`);
  assert.deepEqual(s.rows('SELECT n FROM mints').map((x) => x.n), [1, 1], 'day D+1 mints rows preserved');
  assert.equal(s.setting('turn_day'), '2026-11-16:2', 'day D+1 turn_day preserved');
  assert.equal(s.setting('turn_month'), '2026-11:2', 'month counter released failed pre-midnight reservation');
  rl(await s.relay(r3.code, r3.ht, { ip: ip1 }), 429, 'relay_limit');
});
