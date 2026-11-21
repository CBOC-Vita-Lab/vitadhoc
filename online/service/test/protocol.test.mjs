// The object: routes, field validation, strings, codes, error shape.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeServer, hostDesc, guestDesc, joined } from './helpers.mjs';

const err = (r, status, code) => { assert.equal(r.status, status, r.text); assert.equal(r.data.error.code, code, r.text); };

test('route table: every route answers, anything else is 404 not_found', async () => {
  const s = makeServer();
  assert.equal((await s.call('GET', '/v1/info')).status, 200);
  const c = await s.create();
  assert.equal(c.status, 201);
  const code = c.data.code;
  assert.equal((await s.call('GET', `/v1/rooms/${code}`)).status, 200);
  assert.equal((await s.join(code)).status, 200);
  for (const [m, p] of [['GET', '/'], ['POST', '/v1/rooms/'], ['GET', `/v1/rooms/${code}/join`], ['DELETE', `/v1/rooms/${code}`], ['POST', '/v1/info']]) {
    err(await s.call(m, p, m === 'POST' ? {} : undefined), 404, 'not_found');
  }
});

test('answers are JSON with cache-control no-store; the error shape carries code and message; retryAfterSec is also Retry-After', async () => {
  const s = makeServer({ RATE_LIMIT_SCALE: '1' });
  const r = await s.call('GET', '/v1/info');
  assert.match(r.headers.get('content-type'), /^application\/json/);
  assert.equal(r.headers.get('cache-control'), 'no-store');
  const e = await s.call('GET', '/v1/rooms/ABCDE');
  err(e, 404, 'not_found');
  assert.equal(typeof e.data.error.message, 'string');
  assert.deepEqual(Object.keys(e.data), ['error']);
  let last;
  for (let i = 0; i < 12; i++) last = await s.call('GET', '/v1/rooms/ABCDE', undefined, { ip: '198.51.100.200' });
  err(last, 429, 'rate_limited');
  assert.equal(last.headers.get('retry-after'), String(last.data.error.retryAfterSec));
});

test('info: the documented shape and values', async () => {
  const s = makeServer();
  assert.deepEqual((await s.call('GET', '/v1/info')).data, { service: 'vitadhoc', min: '2.0.0', latest: '2.0.0', relay: true, busy: false, roomTtlSec: 600 });
  assert.equal((await makeServer({ TURN_PROVIDER: '' }).call('GET', '/v1/info')).data.relay, false);
});

test('create: the documented answer; the code alphabet; the 16 hex punch key; the 22 character token', async () => {
  const s = makeServer();
  const r = await s.create();
  assert.equal(r.status, 201);
  assert.deepEqual(Object.keys(r.data).sort(), ['code', 'pollMs', 'punchKey', 'token', 'ttlSec']);
  assert.match(r.data.code, /^[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{5}$/);
  assert.match(r.data.token, /^[A-Za-z0-9_-]{22}$/);
  assert.match(r.data.punchKey, /^[0-9a-f]{16}$/);
  assert.equal(r.data.ttlSec, 600);
  assert.equal(r.data.pollMs, 2000);
});

test('10,000 codes: unique, only the 31 symbols, never I L O 0 1', async () => {
  const s = makeServer({ MAX_ROOMS: '20000', ROOMS_PER_ADDR: '100000', DAILY_REQUEST_BUDGET: '0', ADDR_DAILY: '0' });
  const seen = new Set();
  for (let i = 0; i < 10000; i++) {
    const r = await s.create(hostDesc(), { ip: `198.51.${(i >> 8) & 255}.${i & 255}` });
    assert.equal(r.status, 201, r.text);
    assert.ok(!seen.has(r.data.code), 'duplicate code');
    seen.add(r.data.code);
    assert.match(r.data.code, /^[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{5}$/);
  }
  assert.equal(seen.size, 10000);
  assert.ok(![...seen].some((c) => /[ILO01]/.test(c)));
  assert.equal(s.rows().length, 10000);
});

test('a code is upper-cased on input; a code with any other character, or the wrong length, is 404 like an unknown one', async () => {
  const s = makeServer();
  const c = await s.create();
  const low = c.data.code.toLowerCase();
  assert.equal((await s.call('GET', `/v1/rooms/${low}`)).status, 200);
  for (const bad of ['ABCDI', 'ABCD0', 'ABCD1', 'ABCDO', 'ABCDL', 'ABC', 'ABCDEF', 'ABC-E', 'AB%20E']) err(await s.call('GET', `/v1/rooms/${bad}`), 404, 'not_found');
  err(await s.call('POST', '/v1/rooms/ABCD0/join', guestDesc()), 404, 'not_found');
});

test('lookup: the documented card, state open then full', async () => {
  const s = makeServer();
  const c = await s.create();
  s.advance(42000);
  const card = (await s.call('GET', `/v1/rooms/${c.data.code}`)).data;
  assert.deepEqual(card, { code: c.data.code, kind: 1, console: 'gba', game: 'BPEE', rom: '9f3a01c2', title: 'Pokemon Emerald', nick: 'Alice', ver: '2.0.0', compat: '1a2b3c4d', state: 'open', ageSec: 42 });
  await s.join(c.data.code);
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`)).data.state, 'full');
});

test('body rules: JSON object, 1024 bytes, Content-Type, no unknown keys, an object', async () => {
  const s = makeServer();
  err(await s.call('POST', '/v1/rooms', undefined, { raw: '', noType: true }), 400, 'bad_request');
  err(await s.call('POST', '/v1/rooms', undefined, { raw: '{"nick":' }), 400, 'bad_request');
  err(await s.call('POST', '/v1/rooms', undefined, { raw: '[1,2]' }), 400, 'bad_request');
  err(await s.call('POST', '/v1/rooms', undefined, { raw: '"text"' }), 400, 'bad_request');
  err(await s.call('POST', '/v1/rooms', hostDesc(), { noType: true }), 400, 'bad_request');
  err(await s.call('POST', '/v1/rooms', { ...hostDesc(), extra: 1 }), 400, 'bad_request');
  err(await s.call('POST', '/v1/rooms', hostDesc({ title: 'x'.repeat(1100) }), { headers: { 'content-length': '100' } }), 413, 'too_large');
  const ok = await s.call('POST', '/v1/rooms', hostDesc({ title: 'x'.repeat(23) }));
  assert.equal(ok.status, 201);
});

test('create: every field validation, the message names the field', async () => {
  const s = makeServer({ RATE_LIMIT_SCALE: '1000' });
  const cases = [
    ['nick', { nick: '' }], ['nick', { nick: '   ' }], ['nick', { nick: 5 }], ['nick', { nick: undefined }],
    ['ver', { ver: '2.0.0-dev' }], ['ver', { ver: '' }], ['ver', { ver: 2 }], ['ver', { ver: undefined }],
    ['compat', { compat: '1A2B3C4D' }], ['compat', { compat: '1a2b3c4' }], ['compat', { compat: 'zzzzzzzz' }], ['compat', { compat: undefined }],
    ['kind', { kind: 3 }], ['kind', { kind: 6 }], ['kind', { kind: '1' }], ['kind', { kind: undefined }],
    ['console', { console: 'n64' }], ['console', { console: 'gg' }], ['console', { console: 'MD' }], ['console', { console: 'wsc' }], ['console', { console: 'GBA' }], ['console', { console: undefined }],
    ['game', { game: 'bpee' }], ['game', { game: '' }], ['game', { game: 'ABCDEFGHI' }], ['game', { game: 'AB-C' }], ['game', { game: undefined }],
    ['rom', { rom: '9F3A01C2' }], ['rom', { rom: '9f3a01c' }], ['rom', { rom: 7 }],
    ['title', { title: 7 }],
    ['sym', { sym: 2 }], ['sym', { sym: '1' }], ['relayOnly', { relayOnly: 2 }],
    ['pub', { pub: '203.0.113.7' }], ['pub', { pub: '203.0.113.7:0' }], ['pub', { pub: '203.0.113.7:65536' }], ['pub', { pub: '203.0.113.256:5' }], ['pub', { pub: '203.00.113.7:5' }],
    ['pub', { pub: 'example.com:5' }], ['pub', { pub: '[::1]:5' }], ['lan', { lan: '192.168.1.2' }],
  ];
  for (const [field, over] of cases) {
    const d = hostDesc();
    for (const [k, v] of Object.entries(over)) { if (v === undefined) delete d[k]; else d[k] = v; }
    const r = await s.create(d);
    err(r, 400, 'bad_request');
    assert.equal(r.data.error.field, field, JSON.stringify(over));
  }
  // allowed alternatives
  for (const over of [{ rom: '' }, { title: '' }, { console: 'ws' }, { kind: 0 }, { kind: 2 }, { kind: 4 }, { kind: 5 }, { pub: '' }, { lan: '' }, { game: 'A' }, { game: 'ABCDEFG9' }]) {
    assert.equal((await s.create(hostDesc(over))).status, 201, JSON.stringify(over));
  }
  // every console with Two players is accepted, and its card comes back with the console the host named
  for (const con of ['gba', 'gb', 'gbc', 'nes', 'snes', 'ws', 'md', 'sms', 'sg', 'scd']) {
    const c = await s.create(hostDesc({ console: con, kind: 5, game: 'A1B2C3D4' }));
    assert.equal(c.status, 201, con);
    const g = await s.call('GET', `/v1/rooms/${c.data.code}`);
    assert.equal(g.data.console, con, con);
    assert.equal((await s.join(c.data.code)).status, 200, 'join ' + con);
  }
  // rom, title, pub, lan, sym and relayOnly may be left out
  const slim = hostDesc(); delete slim.rom; delete slim.title; delete slim.pub; delete slim.lan; delete slim.sym; delete slim.relayOnly;
  assert.equal((await s.create(slim)).status, 201);
});

test('addresses: pub must be public, lan private, unless ALLOW_PRIVATE_ADDR=1', async () => {
  const s = makeServer();
  for (const pub of ['0.1.2.3:5', '10.0.0.1:5', '100.64.0.1:5', '100.127.255.254:5', '127.0.0.1:5', '169.254.1.1:5', '172.16.0.1:5', '172.31.255.1:5', '192.168.0.1:5', '224.0.0.1:5', '255.255.255.255:5']) {
    const r = await s.create(hostDesc({ pub }));
    err(r, 400, 'bad_request');
    assert.equal(r.data.error.field, 'pub', pub);
  }
  for (const pub of ['100.63.0.1:5', '100.128.0.1:5', '172.15.0.1:5', '172.32.0.1:5', '8.8.8.8:5', '198.18.0.1:5', '223.255.255.254:5', '192.169.0.1:5']) {
    assert.equal((await s.create(hostDesc({ pub }))).status, 201, pub);
  }
  for (const lan of ['8.8.8.8:5', '203.0.113.7:5', '127.0.0.1:5', '172.32.0.1:5', '100.64.0.1:5']) {
    const r = await s.create(hostDesc({ lan }));
    err(r, 400, 'bad_request');
    assert.equal(r.data.error.field, 'lan', lan);
  }
  for (const lan of ['10.1.2.3:5', '172.16.0.1:5', '172.31.9.9:5', '192.168.1.1:5', '169.254.3.3:5']) assert.equal((await s.create(hostDesc({ lan }))).status, 201, lan);
  const dev = makeServer({ ALLOW_PRIVATE_ADDR: '1' });
  assert.equal((await dev.create(hostDesc({ pub: '127.0.0.1:19019', lan: '8.8.8.8:5' }))).status, 201);
});

test('strings: nick 1..15 and title 0..23 are sanitised (other bytes -> ?, trimmed, cut); nothing stored or returned holds a quote, a backslash or a byte outside 0x20..0x7E', async () => {
  const s = makeServer();
  const c = await s.create(hostDesc({ nick: '  Al"ice\\\u00e9\u3042\nTabs\tAndMoreText  ', title: '  Pok\u00e9mon "Emerald" \\ Version Long Title Here  ' }));
  assert.equal(c.status, 201);
  const card = (await s.call('GET', `/v1/rooms/${c.data.code}`)).data;
  assert.equal(card.nick, 'Al?ice????Tabs?');
  assert.equal(card.nick.length, 15);
  assert.equal(card.title, 'Pok?mon ?Emerald? ? Ver');
  assert.equal(card.title.length, 23);
  const text = (await s.call('GET', `/v1/rooms/${c.data.code}`)).text;
  assert.ok(!text.includes('\\'), 'no escape in a body');
  assert.ok(!/[^\x20-\x7e]/.test(text));
  const j = await s.join(c.data.code, guestDesc({ nick: 'B\u00f6b\u0000' }));
  assert.equal(j.data.peer.nick, 'Al?ice????Tabs?');
  const p = await s.poll(c.data.code, c.data.token);
  assert.equal(p.data.peer.nick, 'B?b?');
  assert.ok(!p.text.includes('\\'));
  // a title of only exotic characters becomes question marks; an empty title is fine
  assert.equal((await s.call('GET', `/v1/rooms/${(await s.create(hostDesc({ title: '' }))).data.code}`)).data.title, '');
});

test('join: a joiner\'s kind, console, game, rom and title are ignored (even malformed); the room keeps the host\'s', async () => {
  const s = makeServer();
  const c = await s.create();
  const j = await s.join(c.data.code, { ...guestDesc(), kind: 99, console: 'x', game: 'zz', rom: 'nothex', title: 12 });
  assert.equal(j.status, 200, j.text);
  const room = s.rows()[0];
  assert.equal(room.kind, 1); assert.equal(room.game, 'BPEE'); assert.equal(room.title, 'Pokemon Emerald');
  err(await s.join((await s.create()).data.code, { nick: 'Bob', ver: '2.0.0' }), 400, 'bad_request');
  err(await s.join((await s.create()).data.code, { ...guestDesc(), extra: 1 }), 400, 'bad_request');
});

test('routes 5-8: token shape (400), unknown room (404), wrong token (401), unknown keys (400)', async () => {
  const s = makeServer();
  const r = await joined(s);
  for (const path of ['poll', 'relay', 'close']) {
    const extra = path === 'close' ? { reason: 'cancel' } : {};
    err(await s.call('POST', `/v1/rooms/${r.code}/${path}`, { token: 'short', ...extra }), 400, 'bad_request');
    err(await s.call('POST', `/v1/rooms/${r.code}/${path}`, {}), 400, 'bad_request');
    err(await s.call('POST', `/v1/rooms/${r.code}/${path}`, { token: 'A'.repeat(22), ...extra }), 401, 'bad_token');
    err(await s.call('POST', `/v1/rooms/ZZZZZ/${path}`, { token: r.ht, ...extra }), 404, 'not_found');
  }
  err(await s.call('POST', `/v1/rooms/${r.code}/poll`, { token: r.ht, extra: 1 }), 400, 'bad_request');
  err(await s.call('POST', `/v1/rooms/${r.code}/endpoint`, { token: r.ht }), 400, 'bad_request');
  err(await s.call('POST', `/v1/rooms/${r.code}/endpoint`, { token: r.ht, relay: '' }), 400, 'bad_request');
  err(await s.call('POST', `/v1/rooms/${r.code}/endpoint`, { token: r.ht, relay: '10.0.0.1:5' }), 400, 'bad_request');
  err(await s.call('POST', `/v1/rooms/${r.code}/close`, { token: r.ht, reason: 'bye' }), 400, 'bad_request');
  err(await s.call('POST', `/v1/rooms/${r.code}/close`, { token: r.ht }), 400, 'bad_request');
  // a token of another room is not a member
  const other = await s.create();
  err(await s.poll(r.code, other.data.token), 401, 'bad_token');
});

test('join: a second joiner is 409 room_full; a different compat is 409 version_mismatch with hostVer', async () => {
  const s = makeServer();
  const c = await s.create(hostDesc({ ver: '2.0.1' }));
  const mismatch = await s.join(c.data.code, guestDesc({ compat: 'deadbeef' }));
  err(mismatch, 409, 'version_mismatch');
  assert.equal(mismatch.data.error.hostVer, '2.0.1');
  assert.equal((await s.call('GET', `/v1/rooms/${c.data.code}`)).data.state, 'open', 'a refused join changes nothing');
  assert.equal((await s.join(c.data.code)).status, 200);
  err(await s.join(c.data.code, guestDesc({ nick: 'Carol' })), 409, 'room_full');
  err(await s.join(c.data.code, guestDesc({ compat: 'deadbeef' })), 409, 'room_full');
});

test('a server error never leaks its message', async () => {
  const s = makeServer();
  s.store.sql.exec('DROP TABLE rooms');
  const r = await s.call('GET', '/v1/rooms/ABCDE');
  err(r, 500, 'internal');
  assert.equal(r.data.error.message, 'internal error');
});
