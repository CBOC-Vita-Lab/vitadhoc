// The Worker: what it answers without waking the object (every request the object sees is a Durable Object request on the bill).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeWorker, hostDesc, guestDesc, UA } from './helpers.mjs';

test('unknown paths and wrong methods are 404 not_found at the edge, with no object request', async () => {
  const w = await makeWorker();
  for (const [m, p] of [['GET', '/'], ['GET', '/v1'], ['GET', '/v1/rooms'], ['POST', '/v1/info'], ['DELETE', '/v1/rooms/ABCDE'], ['GET', '/v1/rooms/ABCDE/join'],
    ['POST', '/v1/rooms/ABCDE/unknown'], ['GET', '/v1/admin/stats'], ['GET', '/times'], ['PUT', '/v1/rooms'], ['GET', '/v1/rooms/ABCDEFGHIJKLMNOPQ']]) {
    const r = await w.call(m, p);
    assert.equal(r.status, 404, `${m} ${p}`);
    assert.equal(r.data.error.code, 'not_found');
  }
  assert.equal(w.stats.doRequests, 0);
});

test('the v gate: missing / malformed v is 400 bad_version, an old one 426 with min and latest, all at the edge', async () => {
  const w = await makeWorker();
  for (const q of ['', '?v=', '?v=abc', '?v=2.0.0-dev', '?v=1.2.3.4.5', '?v=2..0', '?v=12345.0', '?w=2.0.0']) {
    const r = await w.call('GET', `/v1/rooms/ABCDE${q}`, { noV: true });
    assert.equal(r.status, 400, q);
    assert.equal(r.data.error.code, 'bad_version');
  }
  const old = await w.call('GET', '/v1/rooms/ABCDE?v=1.9.9', { noV: true });
  assert.equal(old.status, 426);
  assert.equal(old.data.error.code, 'update_required');
  assert.equal(old.data.error.min, '2.0.0');
  assert.equal(old.data.error.latest, '2.0.0');
  assert.equal(w.stats.doRequests, 0);
  // equal and newer pass the gate (and reach the object: unknown code -> 404 from the object)
  for (const v of ['2.0.0', '2.0', '2.0.1', '10.0.0']) assert.equal((await w.call('GET', `/v1/rooms/ABCDE?v=${v}`, { noV: true })).status, 404, v);
  assert.equal(w.stats.doRequests, 4);
});

test('/v1/info takes v but never refuses it', async () => {
  const w = await makeWorker();
  for (const q of ['', '?v=1.0.0', '?v=junk', '?v=2.0.0']) {
    const r = await w.call('GET', `/v1/info${q}`, { noV: true });
    assert.equal(r.status, 200, q);
    assert.deepEqual(Object.keys(r.data).sort(), ['busy', 'latest', 'min', 'relay', 'roomTtlSec', 'service']);
  }
});

test('an empty User-Agent is 400 bad_request at the edge', async () => {
  const w = await makeWorker();
  const r = await w.call('GET', '/v1/rooms/ABCDE', { headers: { 'user-agent': '' } });
  assert.equal(r.status, 400);
  assert.equal(r.data.error.code, 'bad_request');
  assert.equal(w.stats.doRequests, 0);
});

test('a body announced over 1024 bytes is 413 too_large at the edge; a lying length is caught by the object', async () => {
  const w = await makeWorker();
  const big = await w.call('POST', '/v1/rooms', { body: hostDesc({ title: 'x'.repeat(2000) }) });
  assert.equal(big.status, 413);
  assert.equal(big.data.error.code, 'too_large');
  assert.equal(w.stats.doRequests, 0);
  // no Content-Length announced: the object reads at most 1024 bytes
  const resp = await w.worker.fetch(new Request('https://test.invalid/v1/rooms?v=2.0.0', {
    method: 'POST', headers: { 'cf-connecting-ip': '198.51.100.77', 'user-agent': UA, 'content-type': 'application/json' }, body: JSON.stringify(hostDesc({ title: 'x'.repeat(3000) })),
  }), w.env);
  assert.equal(resp.status, 413);
  assert.equal(w.stats.doRequests, 1);
});

test('the Worker bucket: burst 40, then 429 slow_down with Retry-After, per address; other addresses are not affected', async () => {
  const w = await makeWorker({ ALLOW_PRIVATE_ADDR: '0' });
  let first429 = 0;
  for (let i = 1; i <= 45; i++) {
    const r = await w.call('GET', '/v1/info', { ip: '198.51.100.50' });
    if (r.status === 429 && !first429) first429 = i;
    if (r.status === 429) {
      assert.equal(r.data.error.code, 'rate_limited');
      assert.equal(r.data.error.reason, 'slow_down');
      assert.ok(r.data.error.retryAfterSec >= 1);
      assert.equal(r.headers.get('retry-after'), String(r.data.error.retryAfterSec));
    }
  }
  assert.ok(first429 >= 41 && first429 <= 42, `first 429 at ${first429}`);
  assert.ok(w.stats.doRequests <= 42, 'refused requests never reached the object');
  assert.equal((await w.call('GET', '/v1/info', { ip: '198.51.100.51' })).status, 200);
});

test('EDGE_IP_BURST / EDGE_IP_PER_SEC and RATE_LIMIT_SCALE change the bucket', async () => {
  const w = await makeWorker({ EDGE_IP_BURST: '3', EDGE_IP_PER_SEC: '1' });
  const codes = [];
  for (let i = 0; i < 5; i++) codes.push((await w.call('GET', '/v1/info')).status);
  assert.deepEqual(codes, [200, 200, 200, 429, 429]);
  const off = await makeWorker({ EDGE_IP_BURST: '3', RATE_LIMIT_SCALE: '1000' });
  for (let i = 0; i < 30; i++) assert.equal((await off.call('GET', '/v1/info')).status, 200);
});

test('the Worker remembers "over budget": new rooms (new-over) and everything but close and info (over) are answered without the object', async () => {
  const w = await makeWorker({ DAILY_REQUEST_BUDGET: '10', LIVE_RESERVE_PCT: '20', RATE_LIMIT_SCALE: '1000' });
  const ip = (n) => `198.51.100.${n}`;
  // fill to the 80 % line (8 admitted): create a room and poll it
  const c = await w.call('POST', '/v1/rooms', { body: hostDesc(), ip: ip(1) });
  assert.equal(c.status, 201);
  const { code, token } = c.data;
  for (let i = 0; i < 7; i++) assert.equal((await w.call('GET', `/v1/rooms/${code}`, { ip: ip(2 + i) })).status, 200);
  const doBefore = w.stats.doRequests;
  const refused = await w.call('POST', '/v1/rooms', { body: hostDesc(), ip: ip(20) });
  assert.equal(refused.status, 503);
  assert.equal(refused.data.error.code, 'over_budget');
  assert.equal(refused.data.error.reason, 'busy');
  assert.ok(refused.data.error.retryAfterSec >= 60 && refused.data.error.retryAfterSec <= 900);
  assert.equal(refused.headers.get('retry-after'), String(refused.data.error.retryAfterSec));
  assert.equal(w.stats.doRequests, doBefore + 1);
  // the next new rooms are answered by the Worker
  for (let i = 0; i < 5; i++) assert.equal((await w.call('POST', '/v1/rooms', { body: hostDesc(), ip: ip(30 + i) })).status, 503);
  assert.equal(w.stats.doRequests, doBefore + 1, 'the Worker answered the repeats itself');
  // other routes still go through up to 100 %
  assert.equal((await w.call('GET', `/v1/rooms/${code}`, { ip: ip(60) })).status, 200);
  assert.equal((await w.call('GET', `/v1/rooms/${code}`, { ip: ip(61) })).status, 200);
  const over = await w.call('GET', `/v1/rooms/${code}`, { ip: ip(62) });
  assert.equal(over.status, 503);
  const doOver = w.stats.doRequests;
  for (let i = 0; i < 4; i++) assert.equal((await w.call('GET', `/v1/rooms/${code}`, { ip: ip(70 + i) })).status, 503);
  assert.equal(w.stats.doRequests, doOver, 'everything is answered by the Worker once the whole budget is spent');
  // close and info are never refused
  assert.equal((await w.call('POST', `/v1/rooms/${code}/close`, { body: { token, reason: 'cancel' }, ip: ip(80) })).status, 200);
  const info = await w.call('GET', '/v1/info', { ip: ip(81) });
  assert.equal(info.status, 200);
  assert.equal(info.data.busy, true);
});

test('the Worker remembers an address over its daily cap (x-vad-block)', async () => {
  const w = await makeWorker({ ADDR_DAILY: '5', RATE_LIMIT_SCALE: '1' });
  const ip = '198.51.100.99';
  const codes = [];
  for (let i = 0; i < 5; i++) codes.push((await w.call('GET', '/v1/info', { ip })).status);
  assert.deepEqual(codes, [200, 200, 200, 200, 200]);
  const doBefore = w.stats.doRequests;
  const r = await w.call('GET', '/v1/info', { ip });
  assert.equal(r.status, 429);
  assert.equal(r.data.error.reason, 'daily_cap');
  assert.equal(w.stats.doRequests, doBefore + 1);
  for (let i = 0; i < 6; i++) assert.equal((await w.call('GET', '/v1/rooms/ABCDE', { ip })).status, 429);
  assert.equal(w.stats.doRequests, doBefore + 1, 'the repeats cost no object request');
  assert.equal((await w.call('GET', '/v1/info', { ip: '198.51.100.100' })).status, 200, 'another address is unaffected');
});

test('IPv6 addresses share a /64 bucket', async () => {
  const w = await makeWorker({ EDGE_IP_BURST: '2' });
  assert.equal((await w.call('GET', '/v1/info', { ip: '2001:db8:1:2:aaaa::1' })).status, 200);
  assert.equal((await w.call('GET', '/v1/info', { ip: '2001:db8:1:2:bbbb::2' })).status, 200);
  assert.equal((await w.call('GET', '/v1/info', { ip: '2001:db8:1:2:cccc::3' })).status, 429);
  assert.equal((await w.call('GET', '/v1/info', { ip: '2001:db8:1:3::1' })).status, 200);
});
