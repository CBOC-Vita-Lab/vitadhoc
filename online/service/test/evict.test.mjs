// The reviewer's test: an idle Durable Object leaves memory after about 10 s and comes back empty. Rooms are rows, so nothing is lost.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, guestDesc, joined } from './helpers.mjs';

const sec = (n) => n * 1000;

test('create a room, rebuild the object: the lookup still resolves, the join works, the host\'s token still polls', async () => {
  const s = makeServer();
  const c = await s.create();
  s.rebuild();
  s.advance(sec(60));
  const card = await s.call('GET', `/v1/rooms/${c.data.code}`);
  assert.equal(card.status, 200);
  assert.equal(card.data.state, 'open');
  assert.equal(card.data.ageSec, 60);
  assert.equal(card.data.title, 'Pokemon Emerald');
  s.rebuild();
  assert.equal((await s.poll(c.data.code, c.data.token)).data.state, 'open');
  s.rebuild();
  const j = await s.join(c.data.code);
  assert.equal(j.status, 200);
  assert.equal(j.data.punchKey, c.data.punchKey);
  s.rebuild();
  const hp = await s.poll(c.data.code, c.data.token);
  assert.equal(hp.data.state, 'joined');
  assert.equal(hp.data.peer.nick, 'Bob');
  assert.equal((await s.poll(c.data.code, j.data.token)).data.peer.nick, 'Alice');
});

test('the guest\'s relayed address survives a second rebuild', async () => {
  const s = makeServer();
  const r = await joined(s);
  s.rebuild();
  s.advance(sec(60));
  assert.equal((await s.relay(r.code, r.gt)).status, 200);
  assert.equal((await s.endpoint(r.code, r.gt, '198.18.0.2:4000')).status, 200);
  s.rebuild();
  s.advance(sec(10));
  s.rebuild();
  const hp = await s.poll(r.code, r.ht);
  assert.equal(hp.data.state, 'relay');
  assert.equal(hp.data.relayPeer, '198.18.0.2:4000');
  assert.equal((await s.endpoint(r.code, r.ht, '198.18.0.1:3000')).data.relayPeer, '198.18.0.2:4000');
});

test('the relay credential counters (per member, per room) are rows: a rebuild does not reset them', async () => {
  const s = makeServer({ RATE_LIMIT_SCALE: '1' });
  const r = await joined(s);
  s.advance(sec(2));
  assert.equal((await s.relay(r.code, r.gt)).status, 200);
  s.rebuild();
  assert.equal((await s.relay(r.code, r.gt)).status, 200);
  s.rebuild();
  const third = await s.relay(r.code, r.gt);
  assert.equal(third.status, 429);
  assert.equal(third.data.error.reason, 'relay_limit');
});

test('a room past its time is gone at the first request after a rebuild; the tombstone is then removed after 30 s', async () => {
  const s = makeServer();
  const c = await s.create();
  s.rebuild();
  s.advance(sec(601));
  s.rebuild();
  const r = await s.call('GET', `/v1/rooms/${c.data.code}`);
  assert.equal(r.status, 404);
  const row = s.rows()[0];
  assert.equal(row.state, 'closed');
  assert.equal(row.reason, 'expired');
  s.advance(sec(31));
  s.rebuild();
  await s.call('GET', '/v1/info');
  assert.equal(s.rows().length, 0, 'the first request after a start sweeps');
});

test('the first request after a rebuild sweeps even when the last sweep was seconds ago', async () => {
  const s = makeServer();
  const c = await s.create();
  s.advance(sec(600));
  s.rebuild();
  await s.call('GET', '/v1/info');
  assert.equal(s.rows()[0].state, 'closed');
  assert.equal(s.rows()[0].host_pub, '', 'addresses are wiped when the room closes');
  void c;
});

test('the budget count survives a rebuild (saved every BUDGET_SAVE_EVERY and at the first request after a start)', async () => {
  const s = makeServer({ BUDGET_SAVE_EVERY: '20' });
  for (let i = 0; i < 45; i++) await s.call('GET', '/v1/info', undefined, { ip: `198.51.100.${i + 1}` });
  const used = s.app.status().budget.objectRequests;
  assert.equal(used, 45);
  s.rebuild();
  await s.call('GET', '/v1/info');
  const st = s.app.status().budget;
  assert.ok(st.objectRequests >= 41, `restored ${st.objectRequests}`);
  assert.ok(st.objectRequests <= 46);
  // the first request after the rebuild saved at once
  assert.equal(Number(s.setting('budget_day').split(':')[4]), st.objectRequests);
});
