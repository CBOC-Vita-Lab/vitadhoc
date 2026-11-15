// Idempotent create and join (request id `rid`), and the guest's relay credentials after a reopen.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, guestDesc, T0 } from './helpers.mjs';

const RID = '0123456789abcdef';
const RID2 = 'fedcba9876543210';
const err = (r, status, code) => { assert.equal(r.status, status, r.text); assert.equal(r.data.error.code, code, r.text); };

test('create with the same rid twice: the ORIGINAL answer, one room, the per-address limits not used up', async () => {
  const s = makeServer();
  const ip = { ip: '198.51.100.77' };
  const a = await s.create(hostDesc({ rid: RID }), ip);
  const b = await s.create(hostDesc({ rid: RID }), ip);
  assert.equal(a.status, 201);
  assert.equal(b.status, 201);
  assert.deepEqual(b.data, a.data, 'same code, token, punchKey');
  assert.equal(s.rows().length, 1, 'no orphan room');
  const c = await s.create(hostDesc({ rid: RID2 }), ip);
  assert.notEqual(c.data.code, a.data.code, 'another rid is another room');
  assert.equal(s.rows().length, 2);
});

test('a repeated create is answered even when the address already holds the most live rooms it may', async () => {
  const s = makeServer({ RATE_LIMIT_SCALE: '1', ROOMS_PER_ADDR: '4', CREATE_BURST: '20' });
  s.clock.now = T0;
  const ip = { ip: '198.51.100.78' };
  const first = await s.create(hostDesc({ rid: RID }), ip);
  for (let i = 0; i < 3; i++) { assert.equal((await s.create(hostDesc({ rid: `00000000000000a${i}` }), ip)).status, 201); }
  err(await s.create(hostDesc({ rid: RID2 }), ip), 429, 'rate_limited');
  assert.deepEqual((await s.create(hostDesc({ rid: RID }), ip)).data, first.data);
});

test('create without rid is not repeatable (two rooms); a malformed rid is 400', async () => {
  const s = makeServer();
  await s.create(); await s.create();
  assert.equal(s.rows().length, 2);
  for (const bad of ['xyz', 'ABCDEF0123456789', '0123456789abcde', 7, '0123456789abcdef0']) {
    err(await s.create(hostDesc({ rid: bad })), 400, 'bad_request');
  }
});

test('a closed or expired room is not answered again for its rid: a fresh room is made', async () => {
  const s = makeServer();
  const a = await s.create(hostDesc({ rid: RID }));
  await s.close(a.data.code, a.data.token, 'cancel');
  const b = await s.create(hostDesc({ rid: RID }));
  assert.equal(b.status, 201);
  assert.notEqual(b.data.code, a.data.code);
  s.advance(601 * 1000);
  const c = await s.create(hostDesc({ rid: RID }));
  assert.notEqual(c.data.code, b.data.code);
});

test('join with the same rid twice: the ORIGINAL token and answer, not 409 room_full, and no ghost second guest', async () => {
  const s = makeServer();
  const c = await s.create();
  const a = await s.join(c.data.code, guestDesc({ rid: RID }));
  const b = await s.join(c.data.code, guestDesc({ rid: RID }));
  assert.equal(a.status, 200);
  assert.equal(b.status, 200, b.text);
  assert.equal(b.data.token, a.data.token);
  assert.deepEqual(b.data, a.data);
  err(await s.join(c.data.code, guestDesc({ rid: RID2, nick: 'Carol' })), 409, 'room_full');
  err(await s.join(c.data.code, guestDesc({ nick: 'Dave' })), 409, 'room_full');
  assert.equal((await s.poll(c.data.code, a.data.token)).status, 200, 'the token works');
});

test('a join retry after the guest closed (cancel) does not hand out the stale token', async () => {
  const s = makeServer();
  const c = await s.create();
  const a = await s.join(c.data.code, guestDesc({ rid: RID }));
  await s.close(c.data.code, a.data.token, 'cancel');
  assert.equal(s.rows()[0].guest_req, null);
  const b = await s.join(c.data.code, guestDesc({ rid: RID }));
  assert.equal(b.status, 200);
  assert.notEqual(b.data.token, a.data.token);
});

test('a join with a rid does not reveal another room: a rid of room A does nothing in room B', async () => {
  const s = makeServer();
  const a = await s.create();
  const b = await s.create();
  const ja = await s.join(a.data.code, guestDesc({ rid: RID }));
  const jb = await s.join(b.data.code, guestDesc({ rid: RID }));
  assert.equal(jb.status, 200);
  assert.notEqual(jb.data.token, ja.data.token);
});

test('reopen resets the guest\'s mints: a third guest on one code can fetch relay credentials; uses never pass the credentials ever minted', async () => {
  const s = makeServer({ TURN_MONTHLY_BUDGET_MB: '100000' });
  const c = await s.create();
  const month = (T0 && new Date(T0).toISOString().slice(0, 7));
  let minted = 0;
  for (let g = 0; g < 4; g++) {
    const j = await s.join(c.data.code, guestDesc({ nick: `G${g}` }));
    assert.equal(j.status, 200, `guest ${g}: ${j.text}`);
    for (let k = 0; k < 2; k++) {
      const r = await s.relay(c.data.code, j.data.token);
      if (r.status === 200) minted++;
      else err(r, 429, 'rate_limited'); // the member cap: 2 per guest
    }
    assert.equal(s.rows()[0].guest_mints, 2, `guest ${g} got its own credentials`);
    for (let i = 0; i < 3; i++) await s.endpoint(c.data.code, j.data.token, `198.18.0.2:${4000 + g * 10 + i}`);
    await s.close(c.data.code, j.data.token, 'cancel');
    assert.equal(s.rows()[0].guest_mints, 0, 'reset on reopen');
  }
  assert.equal(minted, 8);
  const used = Number((s.setting('turn_month') || `${month}:0`).split(':')[1]);
  assert.ok(used <= minted, `uses ${used} never exceed the credentials minted ${minted}`);
  assert.ok(used >= 1);
});
