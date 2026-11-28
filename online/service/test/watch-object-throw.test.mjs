// F3 (online watch 28 Nov 2026): when the object throws (the free plan's daily Durable Object limit, a reset, overload), the Worker answers
// JSON 503 the client can read, not Cloudflare's HTML error 1101.
import test from 'node:test';
import assert from 'node:assert/strict';
import { UA, V } from './helpers.mjs';

async function workerWith(err) {
  const mod = await import(`../src/worker.js?fresh=${Math.random()}`);
  const env = { RATE_LIMIT_SCALE: '1000', ROOMS: { idFromName: (n) => n, get: () => ({ fetch: async () => { throw err; } }) } };
  return async (path) => {
    const r = await mod.default.fetch(new Request(`https://test.invalid${path}?${V}`, { method: 'GET', headers: { 'cf-connecting-ip': '198.51.100.130', 'user-agent': UA } }), env);
    return { status: r.status, ct: r.headers.get('content-type') || '', retry: r.headers.get('retry-after'), data: JSON.parse(await r.text()) };
  };
}

test('the daily Durable Object limit is answered 503 over_budget JSON (reason busy) with a long wait', async () => {
  const go = await workerWith(new Error('Exceeded allowed volume of requests in Durable Objects free tier.'));
  const r = await go('/v1/info');
  assert.equal(r.status, 503);
  assert.match(r.ct, /application\/json/);
  assert.equal(r.data.error.code, 'over_budget');
  assert.equal(r.data.error.reason, 'busy');
  assert.equal(r.retry, '120');
});

test('any other throw is a plain JSON 503 with a short retry', async () => {
  for (const e of [new Error('Durable Object reset because its code was updated.'), Object.assign(new Error('Durable Object is overloaded.'), { overloaded: true }), new Error('internal error'), new Error('Durable Object exceeded its memory limit and was reset.'), new Error('Durable Object exceeded its CPU time limit and was reset.'), new Error('Subrequest depth limit exceeded'), Object.assign(new Error('Exceeded allowed volume of requests in Durable Objects free tier.'), { retryable: true }), 'a string throw']) {
    const go = await workerWith(e);
    const r = await go('/v1/info');
    assert.equal(r.status, 503);
    assert.equal(r.data.error.code, 'unavailable');
    assert.match(r.ct, /application\/json/);
    assert.equal(r.retry, '3');
    assert.equal(r.data.error.retryAfterSec, 3);
  }
});
