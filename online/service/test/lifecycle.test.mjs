// Room lifecycle (PROTOCOL.md 1.3), poll answers, modes, relay addresses. The clock is injected.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, guestDesc, joined } from './helpers.mjs';

const err = (r, status, code) => { assert.equal(r.status, status, r.text); assert.equal(r.data.error.code, code, r.text); };
const sec = (n) => n * 1000;

test('create -> lookup -> join -> poll -> close(linked): the documented answers, the row is deleted at once', async () => {
  const s = makeServer();
  const c = await s.create();
  const { code, token: ht, punchKey } = c.data;
  let p = await s.poll(code, ht);
  assert.deepEqual(p.data, { state: 'open', mode: '', peer: null, relayPeer: '', reason: '', punchMs: 4000, pollMs: 2000 });
  s.advance(sec(60));
  const j = await s.join(code);
  assert.deepEqual(Object.keys(j.data).sort(), ['mode', 'peer', 'pollMs', 'punchKey', 'punchMs', 'token']);
  assert.equal(j.data.punchKey, punchKey);
  assert.equal(j.data.mode, 'punch');
  assert.deepEqual(j.data.peer, { nick: 'Alice', pub: '203.0.113.7:19019', lan: '', sym: 0 });
  assert.equal(j.data.pollMs, 1500);
  assert.equal(j.data.punchMs, 9000, 'the host polled every 5 s at that age (age 60 s): 5000 + 4000');
  p = await s.poll(code, ht);
  assert.deepEqual(p.data, { state: 'joined', mode: 'punch', peer: { nick: 'Bob', pub: '198.51.100.20:20020', lan: '', sym: 0 }, relayPeer: '', reason: '', punchMs: 4000, pollMs: 1500 });
  const g = await s.poll(code, j.data.token);
  assert.deepEqual(g.data.peer, { nick: 'Alice', pub: '203.0.113.7:19019', lan: '', sym: 0 });
  assert.equal(g.data.state, 'joined');
  assert.deepEqual((await s.close(code, ht, 'linked')).data, {});
  assert.equal(s.rows().length, 0);
  err(await s.call('GET', `/v1/rooms/${code}`), 404, 'not_found');
  err(await s.poll(code, ht), 404, 'not_found');
});

test('punchMs of a joiner: host poll interval at the time + 4000 (6000, 9000, 14000)', async () => {
  for (const [wait, want] of [[0, 6000], [29, 6000], [30, 9000], [119, 9000], [120, 14000], [300, 14000]]) {
    const s = makeServer();
    const c = await s.create();
    s.advance(sec(wait));
    assert.equal((await s.join(c.data.code)).data.punchMs, want, `after ${wait} s`);
  }
});

test('host poll cadence: 2000 under 30 s, 5000 under 120 s, 10000 after; 1500 once joined', async () => {
  const s = makeServer();
  const c = await s.create();
  const at = async (sc) => { s.clock.now = s.clock.now; return (await s.poll(c.data.code, c.data.token)).data.pollMs; };
  assert.equal(await at(), 2000);
  s.advance(sec(29)); assert.equal(await at(), 2000);
  s.advance(sec(1)); assert.equal(await at(), 5000);
  s.advance(sec(90)); assert.equal(await at(), 10000);
  await s.join(c.data.code);
  assert.equal(await at(), 1500);
});

test('an open room expires at 600 s: lookup and join are 404, the host poll reads closed/expired, deleted 30 s later', async () => {
  const s = makeServer();
  const c = await s.create();
  s.advance(sec(599));
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`)).status, 200);
  s.advance(sec(1));
  err(await s.call('GET', `/v1/rooms/${c.data.code}`), 404, 'not_found');
  err(await s.join(c.data.code), 404, 'not_found');
  const p = await s.poll(c.data.code, c.data.token);
  assert.equal(p.status, 200);
  assert.equal(p.data.state, 'closed');
  assert.equal(p.data.reason, 'expired');
  assert.equal(p.data.peer, null);
  s.advance(sec(29));
  assert.equal((await s.poll(c.data.code, c.data.token)).data.state, 'closed');
  s.advance(sec(2));
  err(await s.poll(c.data.code, c.data.token), 404, 'not_found');
  assert.equal(s.rows().length, 0);
});

test('a joined room must link within 120 s of the join: then closed/expired for both members', async () => {
  const s = makeServer();
  const r = await joined(s);
  s.advance(sec(119));
  assert.equal((await s.poll(r.code, r.ht)).data.state, 'joined');
  s.advance(sec(1));
  for (const t of [r.ht, r.gt]) {
    const p = await s.poll(r.code, t);
    assert.equal(p.data.state, 'closed');
    assert.equal(p.data.reason, 'expired');
  }
  err(await s.join(r.code), 404, 'not_found');
});

test('the 120 s counts from the join, not from the creation; a relay room has the same deadline', async () => {
  const s = makeServer();
  const c = await s.create();
  s.advance(sec(500));
  const j = await s.join(c.data.code);
  s.advance(sec(100));
  assert.equal((await s.endpoint(c.data.code, j.data.token, '198.18.0.2:3000')).status, 200);
  assert.equal((await s.poll(c.data.code, c.data.token)).data.state, 'relay');
  s.advance(sec(21));
  assert.equal((await s.poll(c.data.code, c.data.token)).data.state, 'closed');
});

test('a guest cancel / failed puts the room back to open with the old expiry; the same code works for another try', async () => {
  for (const reason of ['cancel', 'failed']) {
    const s = makeServer();
    const c = await s.create();
    s.advance(sec(100));
    const j = await s.join(c.data.code);
    s.advance(sec(10));
    assert.deepEqual((await s.close(c.data.code, j.data.token, reason)).data, {});
    assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`)).data.state, 'open');
    err(await s.poll(c.data.code, j.data.token), 401, 'bad_token');
    const hp = await s.poll(c.data.code, c.data.token);
    assert.equal(hp.data.state, 'open');
    assert.equal(hp.data.peer, null);
    const row = s.rows()[0];
    assert.equal(row.guest_token, null);
    assert.equal(row.guest_pub, '');
    assert.equal(row.expires_ms, row.created_ms + 600000);
    const j2 = await s.join(c.data.code, guestDesc({ nick: 'Carol' }));
    assert.equal(j2.status, 200);
    assert.equal((await s.poll(c.data.code, c.data.token)).data.peer.nick, 'Carol');
    s.advance(sec(121)); // the second join set a new 120 s deadline
    assert.equal((await s.poll(c.data.code, c.data.token)).data.state, 'closed');
  }
});

test('a guest cancel clears the relayed addresses too', async () => {
  const s = makeServer();
  const r = await joined(s);
  await s.endpoint(r.code, r.ht, '198.18.0.1:3000');
  await s.endpoint(r.code, r.gt, '198.18.0.2:3000');
  await s.close(r.code, r.gt, 'failed');
  const row = s.rows()[0];
  assert.equal(row.state, 'open'); assert.equal(row.host_relayed, ''); assert.equal(row.guest_relayed, '');
});

test('a guest\'s close(linked) changes nothing; the host\'s close(linked) deletes the row', async () => {
  const s = makeServer();
  const r = await joined(s);
  assert.equal((await s.close(r.code, r.gt, 'linked')).status, 200);
  assert.equal(s.rows()[0].state, 'joined');
  assert.equal((await s.close(r.code, r.ht, 'linked')).status, 200);
  assert.equal(s.rows().length, 0);
});

test('a host close(cancel/failed) leaves a 30 s tombstone: the guest\'s poll reads closed + reason, lookup and join answer 404', async () => {
  for (const reason of ['cancel', 'failed']) {
    const s = makeServer();
    const r = await joined(s);
    await s.close(r.code, r.ht, reason);
    const g = await s.poll(r.code, r.gt);
    assert.equal(g.status, 200);
    assert.equal(g.data.state, 'closed');
    assert.equal(g.data.reason, reason);
    assert.equal(g.data.peer, null);
    err(await s.call('GET', `/v1/rooms/${r.code}`), 404, 'not_found');
    err(await s.join(r.code), 404, 'not_found');
    assert.equal((await s.close(r.code, r.gt, 'cancel')).status, 200, 'closing a closed room is fine');
    s.advance(sec(31));
    err(await s.poll(r.code, r.gt), 404, 'not_found');
    assert.equal(s.rows().length, 0);
  }
});

test('a host close(cancel) of an open room: closed tombstone, then gone', async () => {
  const s = makeServer();
  const c = await s.create();
  await s.close(c.data.code, c.data.token, 'cancel');
  assert.equal((await s.poll(c.data.code, c.data.token)).data.reason, 'cancel');
  err(await s.call('GET', `/v1/rooms/${c.data.code}`), 404, 'not_found');
});

test('modes: punch by default; relay when either asked for it, either has no public address, or EITHER saw a symmetric NAT', async () => {
  const cases = [
    ['both open', {}, {}, 'punch'],
    ['host relayOnly', { relayOnly: 1 }, {}, 'relay'],
    ['guest relayOnly', {}, { relayOnly: 1 }, 'relay'],
    ['host sym', { sym: 1 }, {}, 'relay'],
    ['guest sym', {}, { sym: 1 }, 'relay'],
    ['sym and sym', { sym: 1 }, { sym: 1 }, 'relay'],
    ['host empty pub', { pub: '' }, {}, 'relay'],
    ['guest empty pub', {}, { pub: '' }, 'relay'],
  ];
  for (const [name, h, g, mode] of cases) {
    const s = makeServer();
    const r = await joined(s, h, g);
    assert.equal(r.joined.mode, mode, name);
    const hp = await s.poll(r.code, r.ht);
    assert.equal(hp.data.mode, mode, name);
    if (mode === 'relay') {
      assert.deepEqual([r.joined.peer.pub, r.joined.peer.lan, hp.data.peer.pub, hp.data.peer.lan], ['', '', '', ''], `${name}: relay mode hides pub and lan`);
      assert.ok(!JSON.stringify(hp.data).includes('198.51.100.20') && !JSON.stringify(r.joined).includes('203.0.113.7'));
    } else {
      assert.equal(r.joined.peer.pub, '203.0.113.7:19019');
      assert.equal(hp.data.peer.pub, '198.51.100.20:20020');
    }
  }
});

test('relayOnly: pub and lan are not even stored', async () => {
  const s = makeServer();
  const r = await joined(s, { relayOnly: 1 }, { relayOnly: 1 });
  const row = s.rows()[0];
  assert.deepEqual([row.host_pub, row.host_lan, row.guest_pub, row.guest_lan], ['', '', '', '']);
  assert.equal(r.joined.mode, 'relay');
});

test('lan is given only when both public addresses hold the same IP (same router)', async () => {
  const s = makeServer();
  const r = await joined(s, { pub: '203.0.113.7:19019' }, { pub: '203.0.113.7:30000', lan: '192.168.1.99:30000' });
  assert.equal(r.joined.mode, 'punch');
  assert.deepEqual(r.joined.peer, { nick: 'Alice', pub: '203.0.113.7:19019', lan: '192.168.1.23:19019', sym: 0 });
  assert.deepEqual((await s.poll(r.code, r.ht)).data.peer, { nick: 'Bob', pub: '203.0.113.7:30000', lan: '192.168.1.99:30000', sym: 0 });
  const t = await joined(makeServer(), {}, {});
  assert.equal(t.joined.peer.lan, '');
});

test('sym is passed on to the peer', async () => {
  const s = makeServer();
  const r = await joined(s, {}, { sym: 1 });
  assert.equal((await s.poll(r.code, r.ht)).data.peer.sym, 1);
});

test('the host\'s poll can carry a new pub while the room is open; a bad one is 400; once joined it is ignored', async () => {
  const s = makeServer();
  const c = await s.create();
  await s.poll(c.data.code, c.data.token, { pub: '203.0.113.99:40404' });
  assert.equal(s.rows()[0].host_pub, '203.0.113.99:40404');
  err(await s.poll(c.data.code, c.data.token, { pub: '10.0.0.1:5' }), 400, 'bad_request');
  err(await s.poll(c.data.code, c.data.token, { pub: 'nope' }), 400, 'bad_request');
  const j = await s.join(c.data.code);
  assert.equal(j.data.peer.pub, '203.0.113.99:40404');
  await s.poll(c.data.code, c.data.token, { pub: '203.0.113.1:1' });
  assert.equal(s.rows()[0].host_pub, '203.0.113.99:40404');
  // a guest\'s pub in a poll changes nothing either
  await s.poll(c.data.code, j.data.token, { pub: '203.0.113.2:2' });
  assert.equal(s.rows()[0].guest_pub, '198.51.100.20:20020');
});

test('polls write nothing (a changed pub aside)', async () => {
  const s = makeServer();
  const r = await joined(s);
  const before = JSON.stringify(s.rows());
  for (let i = 0; i < 5; i++) { await s.poll(r.code, r.ht); await s.poll(r.code, r.gt); await s.call('GET', `/v1/rooms/${r.code}`); }
  assert.equal(JSON.stringify(s.rows()), before);
});

test('route 7: a relayed address sets state relay, is stored, and the peer reads it as relayPeer', async () => {
  const s = makeServer();
  const r = await joined(s);
  let e = await s.endpoint(r.code, r.gt, '198.18.0.2:4000');
  assert.deepEqual(e.data, { relayPeer: '' });
  let hp = await s.poll(r.code, r.ht);
  assert.equal(hp.data.state, 'relay'); assert.equal(hp.data.mode, 'relay'); assert.equal(hp.data.relayPeer, '198.18.0.2:4000');
  assert.deepEqual(hp.data.peer, { nick: 'Bob', pub: '', lan: '', sym: 0 }, 'a relay room reveals no public address');
  e = await s.endpoint(r.code, r.ht, '198.18.0.1:3000');
  assert.deepEqual(e.data, { relayPeer: '198.18.0.2:4000' });
  assert.equal((await s.poll(r.code, r.gt)).data.relayPeer, '198.18.0.1:3000');
  // a member may post a new relayed address
  assert.equal((await s.endpoint(r.code, r.gt, '198.18.0.2:4001')).status, 200);
  assert.equal((await s.poll(r.code, r.ht)).data.relayPeer, '198.18.0.2:4001');
});

test('routes 6 and 7 need a second member: 403 relay_not_allowed in an open room', async () => {
  const s = makeServer();
  const c = await s.create();
  err(await s.relay(c.data.code, c.data.token), 403, 'relay_not_allowed');
  err(await s.endpoint(c.data.code, c.data.token, '198.18.0.1:3000'), 403, 'relay_not_allowed');
});

test('routes 6 and 7 on a closed room are 404', async () => {
  const s = makeServer();
  const r = await joined(s);
  await s.close(r.code, r.ht, 'cancel');
  err(await s.relay(r.code, r.gt), 404, 'not_found');
  err(await s.endpoint(r.code, r.gt, '198.18.0.2:4000'), 404, 'not_found');
});

test('MAX_ROOMS live rooms: the next create is 503 rooms_full; a closed room frees its place', async () => {
  const s = makeServer({ MAX_ROOMS: '3' });
  const made = [];
  for (let i = 0; i < 3; i++) made.push(await s.create());
  err(await s.create(), 503, 'rooms_full');
  await s.close(made[0].data.code, made[0].data.token, 'linked');
  assert.equal((await s.create()).status, 201);
});

test('expired rooms free their place at the next request', async () => {
  const s = makeServer({ MAX_ROOMS: '1' });
  await s.create();
  err(await s.create(), 503, 'rooms_full');
  s.advance(sec(601));
  assert.equal((await s.create()).status, 201);
});
