// The day's request budget (PROTOCOL.md 1.4): 8000 a day, new rooms refused from 80 %, close and info never refused, the count saved every
// 20 requests and at the first request after a start.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, T0, UA, V } from './helpers.mjs';
import { createBudget } from '../src/core/budget.js';
import { loadConfig } from '../src/core/config.js';
import { SCHEMA } from '../src/core/rooms.js';
import { openNodeSql } from '../src/sqlite-node.js';

const open = (o = {}) => makeServer({ ADDR_DAILY: '0', ...o }); // per-address ceilings off: the budget is under test
const day = Math.floor(T0 / 86400000);

test('budget at 80 %: with the default 8000, 6400 requests are admitted, then new rooms are refused while everything else is served up to 8000', async () => {
  const s = open();
  assert.equal((await s.create()).status, 201); // admitted: 1
  const lookup = () => s.call('GET', '/v1/rooms/ABCDE');
  for (let i = 0; i < 6398; i++) assert.equal((await lookup()).status, 404, `request ${i}`); // admitted: 6399
  const last = await s.create(hostDesc(), { ip: '198.51.100.201' }); // admitted: 6400
  assert.equal(last.status, 201);
  const refused = await s.create(hostDesc(), { ip: '198.51.100.202' });
  assert.equal(refused.status, 503);
  assert.equal(refused.data.error.code, 'over_budget');
  assert.equal(refused.data.error.reason, 'busy');
  assert.ok(refused.data.error.retryAfterSec >= 60 && refused.data.error.retryAfterSec <= 900);
  assert.equal(refused.headers.get('x-vad-budget'), 'new-over');
  assert.equal(s.app.status().budget.used, 6400);
  // everything else is still served: lookups, polls, joins
  for (let i = 0; i < 1600; i++) assert.equal((await lookup()).status, 404, `served ${i}`); // 8000
  const over = await lookup();
  assert.equal(over.status, 503);
  assert.equal(over.data.error.code, 'over_budget');
  assert.equal(over.headers.get('x-vad-budget'), 'over');
  assert.equal(s.app.status().budget.used, 8000);
  // at 100 % all refused but close (and info)
  const c = await s.call('POST', `/v1/rooms/${last.data.code}/poll`, { token: last.data.token });
  assert.equal(c.status, 503);
  assert.equal((await s.create(hostDesc(), { ip: '198.51.100.203' })).status, 503);
  assert.equal((await s.close(last.data.code, last.data.token, 'cancel')).status, 200, 'close is never refused');
  const info = await s.call('GET', '/v1/info');
  assert.equal(info.status, 200);
  assert.equal(info.data.busy, true);
  // the next UTC day starts fresh
  s.advance(21 * 3600 * 1000);
  assert.equal((await s.create()).status, 201);
  assert.equal(s.app.status().budget.used, 1);
});

test('budget at 80 % with a small budget: DAILY_REQUEST_BUDGET=20 refuses new rooms from the 17th admitted request', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '20' });
  const codes = [];
  for (let i = 0; i < 16; i++) {
    const r = i % 2 ? await s.call('GET', '/v1/rooms/ABCDE') : await s.create(hostDesc(), { ip: `198.51.100.${i + 1}` });
    if (i % 2 === 0) codes.push(r.data);
    assert.ok([201, 404].includes(r.status), `request ${i}: ${r.status}`);
  }
  assert.equal((await s.create(hostDesc(), { ip: '198.51.100.100' })).status, 503);
  assert.equal((await s.call('GET', '/v1/info')).data.busy, true);
  for (let i = 0; i < 4; i++) assert.equal((await s.poll(codes[0].code, codes[0].token, {}, { ip: `198.51.100.${150 + i}` })).status, 200);
  assert.equal((await s.poll(codes[0].code, codes[0].token)).status, 503);
  assert.equal((await s.close(codes[0].code, codes[0].token, 'cancel')).status, 200);
});

test('info: busy is false below 80 % and true from it; info itself is never refused and is not counted against the budget', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '10' });
  for (let i = 0; i < 20; i++) assert.equal((await s.call('GET', '/v1/info')).data.busy, false);
  assert.equal(s.app.status().budget.used, 0);
  for (let i = 0; i < 8; i++) await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal((await s.call('GET', '/v1/info')).data.busy, true);
});

test('DAILY_REQUEST_BUDGET=0 switches the budget off', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '0' });
  for (let i = 0; i < 50; i++) assert.equal((await s.call('GET', '/v1/rooms/ABCDE')).status, 404);
});

test('LIVE_RESERVE_PCT moves the line', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '10', LIVE_RESERVE_PCT: '50' });
  for (let i = 0; i < 5; i++) await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal((await s.create()).status, 503);
  assert.equal((await s.call('GET', '/v1/rooms/ABCDE')).status, 404);
});

test('the counter is saved every BUDGET_SAVE_EVERY requests (default 20) and at the first request after a start', async () => {
  const s = open();
  const stored = () => (s.setting('budget_day') || '').split(':').map(Number);
  assert.equal(stored()[0] || 0, 0);
  await s.call('GET', '/v1/rooms/ABCDE');
  assert.deepEqual(stored().slice(0, 2), [day, 1], 'saved at once by the first request of the object');
  const last = () => stored()[4];
  const at1 = last();
  for (let i = 0; i < 19; i++) await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal(last(), at1, '19 requests since the save: not yet');
  await s.call('GET', '/v1/rooms/ABCDE');
  assert.ok(last() > at1, 'the 20th since the save writes it');
  assert.equal(last(), 21);
  // after a rebuild the first request saves again, whatever the distance
  s.rebuild();
  await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal(last(), 22);
  for (let i = 0; i < 5; i++) await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal(last(), 22);
});

test('BUDGET_SAVE_EVERY is a variable', async () => {
  const s = open({ BUDGET_SAVE_EVERY: '5' });
  for (let i = 0; i < 6; i++) await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal(Number(s.setting('budget_day').split(':')[4]), 6);
});

test('the guard still trips when the object leaves memory every 25 requests: each rebuild loses less than BUDGET_SAVE_EVERY', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '100', BUDGET_SAVE_EVERY: '20' });
  let served = 0;
  for (let round = 0; round < 20 && served < 200; round++) {
    s.rebuild();
    for (let i = 0; i < 25; i++) {
      const r = await s.call('GET', '/v1/rooms/ABCDE');
      if (r.status === 404) served++;
    }
  }
  assert.ok(served < 140, `a budget of 100 let ${served} requests through across rebuilds`);
  assert.ok(s.app.status().budget.used >= 90);
  assert.ok(served >= 100);
});

test('the stored row: <dayIndex>:<n>:<ra>:<rb>:<ob>, and a new day starts at zero', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '5', BUDGET_SAVE_EVERY: '1' });
  for (let i = 0; i < 6; i++) await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal(s.setting('budget_day'), `${day}:5:0:1:6`);
  s.advance(86400000);
  await s.call('GET', '/v1/rooms/ABCDE');
  assert.equal(s.setting('budget_day'), `${day + 1}:1:0:0:1`);
});

test('poll pace: 1.0 up to half of the new-room budget, then rising to 1.4: pollMs stretches before the cliff', async () => {
  const s = open({ DAILY_REQUEST_BUDGET: '100' });
  const c = await s.create();
  assert.equal(c.data.pollMs, 2000);
  for (let i = 0; i < 38; i++) await s.call('GET', '/v1/rooms/ABCDE'); // 39 admitted: below half of 80
  assert.equal((await s.poll(c.data.code, c.data.token)).data.pollMs, 2000);
  for (let i = 0; i < 20; i++) await s.call('GET', '/v1/rooms/ABCDE'); // 60 admitted
  const mid = (await s.poll(c.data.code, c.data.token, {}, { ip: '198.51.100.222' })).data.pollMs;
  assert.ok(mid > 2000 && mid < 2800, `pollMs ${mid}`);
  for (let i = 0; i < 18; i++) await s.call('GET', '/v1/rooms/ABCDE'); // 78 admitted
  const high = (await s.poll(c.data.code, c.data.token, {}, { ip: '198.51.100.223' })).data.pollMs;
  assert.ok(high > mid && high <= 2800, `pollMs ${high}`);
});

test('KILL_SWITCH: all / rooms refuse every room route but close with 503 switched_off and the maintenance message; relay refuses route 6; info stays up', async () => {
  for (const kill of ['all', 'rooms']) {
    const s = makeServer({ KILL_SWITCH: kill, MAINTENANCE_MESSAGE: 'Back soon "really"' });
    const r = await s.create();
    assert.equal(r.status, 503);
    assert.equal(r.data.error.code, 'switched_off');
    assert.equal(r.data.error.message, "Back soon 'really'", 'the message is the maintenance text, without quotes');
    assert.equal((await s.call('GET', '/v1/rooms/ABCDE')).status, 503);
    assert.equal((await s.call('GET', '/v1/info')).data.busy, true);
    assert.equal((await s.close('ABCDE', 'A'.repeat(22), 'cancel')).status, 404, 'close is not switched off: it reaches the room lookup');
  }
  const none = makeServer({ KILL_SWITCH: 'all' });
  assert.equal((await none.create()).data.error.message, 'online play is switched off', 'no message set: the default text');
  const s = makeServer({ KILL_SWITCH: 'relay', MAINTENANCE_MESSAGE: 'Relay is resting' });
  const c = await s.create();
  assert.equal(c.status, 201);
  const j = await s.join(c.data.code);
  const r = await s.relay(c.data.code, j.data.token);
  assert.equal(r.status, 503);
  assert.equal(r.data.error.code, 'switched_off');
  assert.equal(r.data.error.message, 'Relay is resting');
  assert.equal((await s.call('GET', '/v1/info')).data.relay, false);
});

test('UTC-midnight rollover: stale pre-await timestamps never roll budget_day backwards or wipe per-address counts', async () => {
  const store = openNodeSql();
  for (const stmt of SCHEMA) store.sql.exec(stmt).toArray();
  const db = {
    get: (q, ...b) => store.sql.exec(q, ...b).toArray()[0] ?? null,
    run: (q, ...b) => { store.sql.exec(q, ...b).toArray(); },
  };
  const cfg = loadConfig({ DAILY_REQUEST_BUDGET: '100', ADDR_DAILY: '1', BUDGET_SAVE_EVERY: '1', RATE_LIMIT_SCALE: '1' });
  const b = createBudget(db, cfg, 1);
  const d0 = Date.UTC(2026, 10, 15, 23, 59, 59, 900);
  const d1 = Date.UTC(2026, 10, 16, 0, 0, 0, 100);
  b.touch(d0);
  assert.equal(b.gateAddress('ip1', d0), null);
  assert.equal(b.admit(d0, false), null);
  b.settle(d0);

  // Midnight crosses and day D+1 runs
  b.touch(d1);
  assert.equal(b.gateAddress('ip2', d1), null);
  assert.equal(b.admit(d1, false), null);
  b.settle(d1);
  const savedD1 = store.sql.exec(`SELECT value FROM settings WHERE key = 'budget_day'`).toArray()[0].value;
  assert.ok(savedD1.startsWith(`${day + 1}:`), savedD1);

  // Pre-midnight request resumes late with stale d0
  b.admit(d0, false);
  b.settle(d0);
  assert.equal(b.status(d1).day, day + 1);
  const afterStale = store.sql.exec(`SELECT value FROM settings WHERE key = 'budget_day'`).toArray()[0].value;
  assert.ok(afterStale.startsWith(`${day + 1}:`), `settings.budget_day must stay on day ${day + 1}: ${afterStale}`);
  const blocked = b.gateAddress('ip2', d1);
  assert.ok(blocked, 'addr map for day D+1 must not be wiped by stale d0');
  assert.equal(blocked.status, 429);

  // End-to-end app.handle with suspended body stream crossing UTC midnight
  const s = makeServer({ ADDR_DAILY: '1', BUDGET_SAVE_EVERY: '1', RATE_LIMIT_SCALE: '1' });
  s.clock.now = d0;
  let releaseBody;
  const bodyGate = new Promise((res) => { releaseBody = res; });
  const slowStream = new ReadableStream({
    async start(controller) {
      await bodyGate;
      controller.enqueue(new TextEncoder().encode(JSON.stringify(hostDesc())));
      controller.close();
    },
  });
  const pending = s.app.handle(new Request(`https://test.invalid/v1/rooms?${V}`, {
    method: 'POST',
    headers: { 'cf-connecting-ip': '198.51.100.10', 'user-agent': UA, 'content-type': 'application/json' },
    body: slowStream,
    duplex: 'half',
  }));
  await new Promise((r) => setTimeout(r, 15));
  s.clock.now = d1;
  assert.equal((await s.create(hostDesc(), { ip: '198.51.100.11' })).status, 201);
  releaseBody();
  await pending;
  assert.equal(s.app.status().budget.day, day + 1);
  assert.ok(s.setting('budget_day').startsWith(`${day + 1}:`), s.setting('budget_day'));
  const cap = await s.create(hostDesc(), { ip: '198.51.100.11' });
  assert.equal(cap.status, 429);
  assert.equal(cap.data.error.reason, 'daily_cap');
});
