// The dev server (the twins' fake outside service) and scripts/smoke.mjs, over real HTTP on loopback.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { hostDesc, guestDesc, UA } from './helpers.mjs';

const root = fileURLToPath(new URL('..', import.meta.url));

function startDev(args = []) {
  return new Promise((resolve, reject) => {
    const p = spawn(process.execPath, ['--no-warnings', 'dev/server.mjs', '--port', '0', ...args], { cwd: root, stdio: ['ignore', 'pipe', 'pipe'] });
    const lines = [];
    let buf = '';
    let stderr = '';
    p.stderr.on('data', (d) => { stderr += d; });
    p.stdout.on('data', (d) => {
      buf += d;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i); buf = buf.slice(i + 1);
        lines.push(line);
        const m = /^READY (\d+)$/.exec(line);
        if (m) resolve({ p, lines, port: Number(m[1]), base: `http://127.0.0.1:${m[1]}`, stop: () => new Promise((r) => { p.once('exit', r); p.kill(); }) });
      }
    });
    p.on('exit', (c) => reject(new Error(`dev server exited ${c}: ${stderr}`)));
    setTimeout(() => reject(new Error('no READY line')), 8000).unref();
  });
}
const post = (base, path, body, ip) => fetch(`${base}${path}?v=2.0.0`, { method: 'POST', headers: { 'content-type': 'application/json', 'user-agent': UA, ...(ip ? { 'x-test-ip': ip } : {}) }, body: JSON.stringify(body) }).then(async (r) => ({ status: r.status, data: await r.json() }));
const get = (base, path, ip) => fetch(`${base}${path}`, { headers: { 'user-agent': UA, ...(ip ? { 'x-test-ip': ip } : {}) } }).then(async (r) => ({ status: r.status, data: await r.json() }));

test('dev server: READY <port>, one REQ line per request (method, path, status, code), no address or token in any line, test defaults', async () => {
  const d = await startDev();
  try {
    assert.equal((await get(d.base, '/v1/info?v=2.0.0')).data.relay, true, 'the stub TURN provider is on by default');
    const c = await post(d.base, '/v1/rooms', hostDesc({ pub: '127.0.0.1:19019', lan: '10.9.8.7:19019' }));
    assert.equal(c.status, 201, 'private addresses are allowed by default');
    const j = await post(d.base, `/v1/rooms/${c.data.code}/join`, guestDesc({ pub: '127.0.0.1:19020', lan: '' }));
    assert.equal(j.status, 200);
    const rel = await post(d.base, `/v1/rooms/${c.data.code}/relay`, { token: j.data.token });
    assert.deepEqual(rel.data.servers[1].urls, ['turn:198.51.100.9:3478?transport=udp']);
    assert.equal((await get(d.base, '/v1/rooms/ZZZZZ?v=2.0.0')).status, 404);
    assert.equal((await get(d.base, '/nothing')).status, 404);
    await new Promise((r) => setTimeout(r, 100));
    const reqs = d.lines.filter((l) => l.startsWith('REQ '));
    assert.deepEqual(reqs, [
      'REQ GET /v1/info 200 -', 'REQ POST /v1/rooms 201 -', `REQ POST /v1/rooms/${c.data.code}/join 200 -`, `REQ POST /v1/rooms/${c.data.code}/relay 200 -`,
      'REQ GET /v1/rooms/ZZZZZ 404 not_found', 'REQ GET /nothing 404 not_found',
    ]);
    const text = d.lines.join('\n');
    assert.ok(!/\d+\.\d+\.\d+\.\d+/.test(text) && !text.includes(c.data.token) && !text.includes(j.data.token), text);
    // many rooms from one address: RATE_LIMIT_SCALE defaults to 1000 for the twins
    for (let i = 0; i < 10; i++) assert.equal((await post(d.base, '/v1/rooms', hostDesc({ pub: '127.0.0.1:1' }))).status, 201);
  } finally { await d.stop(); }
});

test('dev server: --env K=V (repeatable), RATE_LIMIT_SCALE=1 gives the real limits, x-test-ip picks the client address', async () => {
  const d = await startDev(['--env', 'RATE_LIMIT_SCALE=1', '--env', 'ALLOW_PRIVATE_ADDR=0', '--env', 'MIN_APP_VERSION=2.1.0']);
  try {
    assert.equal((await get(d.base, '/v1/info')).data.min, '2.1.0');
    const old = await post(d.base, '/v1/rooms', hostDesc());
    assert.equal(old.status, 426);
    assert.equal(old.data.error.min, '2.1.0');
    const ok = await fetch(`${d.base}/v1/rooms?v=2.1.0`, { method: 'POST', headers: { 'content-type': 'application/json', 'user-agent': UA, 'x-test-ip': '198.51.100.1' }, body: JSON.stringify(hostDesc()) });
    assert.equal(ok.status, 201);
    assert.equal((await post(d.base, '/v1/rooms', hostDesc({ pub: '127.0.0.1:5' }))).status, 426);
    const mk = (ip) => fetch(`${d.base}/v1/rooms?v=2.1.0`, { method: 'POST', headers: { 'content-type': 'application/json', 'user-agent': UA, 'x-test-ip': ip }, body: JSON.stringify(hostDesc()) }).then((r) => r.status);
    assert.deepEqual([await mk('198.51.100.2'), await mk('198.51.100.2'), await mk('198.51.100.2'), await mk('198.51.100.2')], [201, 201, 201, 429], 'burst 3 per address');
    assert.equal(await mk('198.51.100.3'), 201, 'another x-test-ip');
  } finally { await d.stop(); }
});

test('dev server: POST /_test/evict rebuilds the object over the same database; /_test/advance moves the clock; GET /_test/stats', async () => {
  const d = await startDev();
  try {
    const c = await post(d.base, '/v1/rooms', hostDesc());
    const ev = await fetch(`${d.base}/_test/evict`, { method: 'POST' });
    assert.equal(ev.status, 200);
    assert.equal((await get(d.base, `/v1/rooms/${c.data.code}?v=2.0.0`)).data.state, 'open', 'the room survived the eviction');
    const st = await get(d.base, '/_test/stats');
    assert.equal(st.data.rooms, 1);
    assert.equal(st.data.requests, 2);
    assert.ok(st.data.budget && st.data.turn);
    await post(d.base, '/_test/advance', { ms: 601000 });
    assert.equal((await get(d.base, `/v1/rooms/${c.data.code}?v=2.0.0`)).status, 404, 'expired by the moved clock');
    assert.equal((await get(d.base, '/_test/stats')).data.rooms, 0);
    assert.equal((await get(d.base, '/_test/nothing')).status, 404);
  } finally { await d.stop(); }
});

test('dev server: --clock-skew-ms moves the server clock from the start', async () => {
  const d = await startDev(['--clock-skew-ms', '86400000']);
  try {
    const c = await post(d.base, '/v1/rooms', hostDesc());
    const card = await get(d.base, `/v1/rooms/${c.data.code}?v=2.0.0`);
    assert.equal(card.data.ageSec, 0);
    const st = await get(d.base, '/_test/stats');
    assert.equal(st.data.budget.day, Math.floor((Date.now() + 86400000) / 86400000));
  } finally { await d.stop(); }
});

test('scripts/smoke.mjs passes against the dev server with ALLOW_PRIVATE_ADDR=0 and the real limits, and exits 1 against a dead address', async () => {
  const d = await startDev(['--env', 'ALLOW_PRIVATE_ADDR=0', '--env', 'RATE_LIMIT_SCALE=1']);
  try {
    const run = (args) => new Promise((res) => {
      const p = spawn(process.execPath, ['--no-warnings', 'scripts/smoke.mjs', ...args], { cwd: root });
      let out = ''; p.stdout.on('data', (x) => { out += x; }); p.stderr.on('data', (x) => { out += x; });
      p.on('exit', (code) => res({ code, out }));
    });
    const ok = await run([d.base, '--relay']);
    assert.equal(ok.code, 0, ok.out);
    assert.match(ok.out, /smoke OK/);
    assert.match(ok.out, /relay credentials/);
    const dead = await run(['http://127.0.0.1:1']);
    assert.equal(dead.code, 1, dead.out);
    assert.equal((await run([])).code, 2);
  } finally { await d.stop(); }
});
