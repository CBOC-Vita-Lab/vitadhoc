// Privacy: no address outlives the link or the room, nothing is logged.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, guestDesc, joined } from './helpers.mjs';

const sec = (n) => n * 1000;
const IP = /\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/;
const dump = (s) => JSON.stringify(s.store.sql.exec('SELECT * FROM rooms').toArray()) + JSON.stringify(s.store.sql.exec('SELECT * FROM settings').toArray());

test('after close(linked) the rooms table has no row and no address is anywhere in settings', async () => {
  const s = makeServer();
  const r = await joined(s);
  await s.relay(r.code, r.gt);
  await s.endpoint(r.code, r.gt, '198.18.0.2:4000');
  await s.endpoint(r.code, r.ht, '198.18.0.1:3000');
  assert.match(dump(s), IP, 'while the room lives, the addresses are in its row');
  await s.close(r.code, r.ht, 'linked');
  assert.equal(s.rows().length, 0);
  const settings = JSON.stringify(s.store.sql.exec('SELECT * FROM settings').toArray());
  assert.ok(!IP.test(settings), settings);
  assert.ok(!settings.includes(r.ht) && !settings.includes(r.gt));
  assert.deepEqual(s.store.sql.exec('SELECT key FROM settings ORDER BY key').toArray().map((x) => x.key), ['budget_day', 'mint_day', 'turn_day', 'turn_month', 'turn_used']);
});

test('after expiry and one request the row holds no address, nickname or creator key; 30 s later it is gone', async () => {
  const s = makeServer();
  const r = await joined(s);
  await s.endpoint(r.code, r.gt, '198.18.0.2:4000');
  s.advance(sec(121));
  await s.call('GET', '/v1/info');
  const rows = s.rows();
  assert.ok(rows.length <= 1);
  const d = dump(s);
  assert.ok(!IP.test(d), d);
  assert.ok(!d.includes('Alice') && !d.includes('Bob'));
  if (rows.length) { assert.equal(rows[0].state, 'closed'); assert.equal(rows[0].addr_key, ''); assert.equal(rows[0].punch_key, ''); }
  s.advance(sec(31));
  await s.call('GET', '/v1/info');
  assert.equal(s.rows().length, 0);
});

test('a host cancel and a guest give-up leave no address behind in the tombstone / the reopened row', async () => {
  const s = makeServer();
  const r = await joined(s);
  await s.close(r.code, r.ht, 'cancel');
  assert.ok(!IP.test(dump(s)), dump(s));
  const t = makeServer();
  const u = await joined(t);
  await t.close(u.code, u.gt, 'failed');
  const d = JSON.stringify(t.rows());
  assert.ok(!d.includes('198.51.100.20') && !d.includes('Bob'), 'the guest is gone from the row');
});

test('no address or token in any log line: nothing is written to the console or stdout/stderr while the service runs', async () => {
  const lines = [];
  const orig = {};
  for (const k of ['log', 'info', 'warn', 'error', 'debug']) { orig[k] = console[k]; console[k] = (...a) => lines.push(a.join(' ')); }
  const w = process.stdout.write.bind(process.stdout);
  const e = process.stderr.write.bind(process.stderr);
  const outs = [];
  // (node's test runner reads stdout for its own protocol: only observe, always forward)
  process.stdout.write = (c, ...rest) => { const t = String(c); if (!t.startsWith('TAP') && !/^\s*(ok|not ok|#|---|\.\.\.|duration_ms|type:)/.test(t)) outs.push(t); return w(c, ...rest); };
  process.stderr.write = (c, ...rest) => { outs.push(String(c)); return e(c, ...rest); };
  try {
    const s = makeServer({ RATE_LIMIT_SCALE: '1' });
    const r = await joined(s);
    await s.relay(r.code, r.gt);
    await s.endpoint(r.code, r.gt, '198.18.0.2:4000');
    await s.call('POST', '/v1/rooms/ZZZZZ/poll', { token: 'Z'.repeat(22) });
    await s.call('POST', '/v1/rooms', { broken: true });
    s.store.sql.exec('DROP TABLE rooms');
    await s.call('GET', '/v1/rooms/ABCDE'); // a 500
    await s.close(r.code, r.ht, 'linked');
    await new Promise((res) => setTimeout(res, 10));
    const all = [...lines, ...outs].join('\n');
    assert.ok(!IP.test(all) && !all.includes(r.ht) && !all.includes(r.gt) && !all.includes('Alice'), all);
    assert.equal(lines.length, 0, 'the core never logs');
  } finally {
    for (const k of Object.keys(orig)) console[k] = orig[k];
    process.stdout.write = w; process.stderr.write = e;
  }
});

test('the creator\'s address key is stored only while the room is live', async () => {
  const s = makeServer();
  const c = await s.create(hostDesc(), { ip: '198.51.100.7' });
  assert.equal(s.rows()[0].addr_key, '198.51.100.7');
  await s.close(c.data.code, c.data.token, 'cancel');
  assert.equal(s.rows()[0].addr_key, '');
});
