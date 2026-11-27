// The deploy and zone scripts, offline: nothing is contacted. npx (wrangler) and curl are stubs on PATH; the zone's API is a small stateful mock
// (test/fixtures/curl-mock.py). Checks the safety rules: no wrangler call outside wr(), only vitadhoc-online, no other service's name, the account
// pin, secrets only on stdin and never in a log / argument / environment, and that the zone script NEVER writes a rate-limiting rule.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, mkdirSync, cpSync, existsSync, readdirSync, chmodSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const ACCT = '0123456789abcdef0123456789abcdef';
const ZONE = 'fedcba9876543210fedcba9876543210';

function sh(args, { env = {}, cwd = root } = {}) {
  const r = spawnSync('bash', args, { cwd, env: { PATH: process.env.PATH, HOME: process.env.HOME, ...env }, encoding: 'utf8', timeout: 60000 });
  return { code: r.status, out: (r.stdout || '') + (r.stderr || '') };
}
function allFiles(dir) {
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...allFiles(p)); else out.push(p);
  }
  return out;
}

// ------------------------------------------------------------------------------------------------------------------ deploy script

test('deploy script: --self-check passes, --print-plan prints the plan and contacts nothing, vitadhoc-online only', () => {
  assert.equal(sh(['scripts/deploy-vitadhoc-online.sh', '--self-check']).code, 0);
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  mkdirSync(join(tmp, 'bin'));
  writeFileSync(join(tmp, 'bin/npx'), `#!/bin/bash\necho "$*" >> ${tmp}/npx.log\n`, { mode: 0o755 });
  writeFileSync(join(tmp, 'bin/curl'), `#!/bin/bash\necho "$*" >> ${tmp}/curl.log\n`, { mode: 0o755 });
  const r = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--print-plan', '--turn-key-id-file', '/nonexistent/a', '--turn-token-file', '/nonexistent/b'], { env: { PATH: `${tmp}/bin:${process.env.PATH}` } });
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /PLAN/);
  assert.match(r.out, /vitadhoc\.psvita\.dev/);
  assert.match(r.out, /secret put TURN_KEY_ID/);
  assert.match(r.out, /secret put TURN_KEY_API_TOKEN/);
  assert.match(r.out, /\*\*\*\*cdef/);
  assert.ok(!r.out.includes(ACCT), 'the account id is shown as its last 4 characters only');
  assert.ok(!/smg-online|msc-online|galaxy\.psvita|workers\.dev\//i.test(r.out.replace(/no workers\.dev/g, '')));
  assert.ok(!existsSync(join(tmp, 'npx.log')) && !existsSync(join(tmp, 'curl.log')), 'a plan runs nothing');
});

test('deploy script: refuses a wrong account id, another URL, a half secret pair, verify-only with secrets, and unknown flags', () => {
  const run = (...a) => sh(['scripts/deploy-vitadhoc-online.sh', ...a]);
  assert.notEqual(run('--print-plan').code, 0);
  assert.notEqual(run('--account-id', 'abc', '--print-plan').code, 0);
  assert.notEqual(run('--account-id', ACCT, '--url', 'https://galaxy.psvita.dev', '--print-plan').code, 0);
  assert.notEqual(run('--account-id', ACCT, '--url', 'https://vitadhoc-online.x.workers.dev', '--print-plan').code, 0);
  assert.notEqual(run('--account-id', ACCT, '--turn-key-id-file', '/x', '--print-plan').code, 0);
  assert.notEqual(run('--account-id', ACCT, '--verify-only', '--turn-key-id-file', '/x', '--turn-token-file', '/y', '--print-plan').code, 0);
  assert.notEqual(run('--account-id', ACCT, '--bogus').code, 0);
});

test('deploy script: refuses a config that names or mentions another service, or turns workers.dev on', () => {
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  const base = readFileSync(join(root, 'wrangler.toml'), 'utf8');
  const variants = {
    'a different name': base.replace('name = "vitadhoc-online"', 'name = "smg-online"'),
    'a mention of smg-online': `${base}\nFOO = "smg-online"\n`,
    'a mention of msc-online': `${base}\nFOO = "msc-online"\n`,
    'a mention of galaxy': `${base}\nFOO = "galaxy"\n`,
    'another domain': base.replace('pattern = "vitadhoc.psvita.dev"', 'pattern = "other.psvita.dev"'),
    'workers_dev on': base.replace('workers_dev = false', 'workers_dev = true'),
  };
  for (const [what, text] of Object.entries(variants)) {
    const dir = join(tmp, what.replace(/\W/g, '_'));
    mkdirSync(join(dir, 'scripts'), { recursive: true });
    cpSync(join(root, 'scripts/deploy-vitadhoc-online.sh'), join(dir, 'scripts/deploy-vitadhoc-online.sh'));
    writeFileSync(join(dir, 'wrangler.toml'), text);
    const r = sh([join(dir, 'scripts/deploy-vitadhoc-online.sh'), '--account-id', ACCT, '--print-plan'], { cwd: dir });
    assert.notEqual(r.code, 0, what);
    assert.match(r.out, /refusing/, what);
  }
  // comments in the config may mention other services (the shipped wrangler.toml does)
  assert.match(base, /^#.*other services/m);
});

function deployStubs(tmp, o = {}) {
  mkdirSync(join(tmp, 'bin'), { recursive: true });
  mkdirSync(join(tmp, 'home'), { recursive: true });
  writeFileSync(join(tmp, 'bin/npx'), `#!/bin/bash
echo "acct=\${CLOUDFLARE_ACCOUNT_ID:-NONE} npx $*" >> "${tmp}/npx.log"
case "$*" in *"secret put"*) cat > "${tmp}/stdin.$(echo "$*" | tr -c 'A-Z_' '_' | tr -s _)" ;; esac
case "$*" in *"wrangler whoami"*)
  echo "You are logged in with an OAuth Token, associated with the email owner@example.com."
  echo "| Account Name        | Account ID |"; echo "| Acme Hat Company    | \${STUB_WHOAMI_ID:-${ACCT}} |"; exit 0 ;; esac
case "$*" in *"deploy --dry-run"*|*"deploy --config"*) echo "Deployed vitadhoc-online ${o.deployExtra || ''}"; echo "  vitadhoc.psvita.dev (custom domain)"; exit 0 ;; esac
case "$*" in *"secret put"*) echo "Success! Uploaded secret"; exit 0 ;; esac
case "$*" in *"deployments list"*) exit \${STUB_DEPLOYMENTS_RC:-0} ;; esac
exit 0
`, { mode: 0o755 });
  // the DNS guard's lookup: the name resolves when STUB_DNS=1 (a record of that name exists), else it does not
  writeFileSync(join(tmp, 'bin/getent'), `#!/bin/bash
[ "\${STUB_DNS:-0}" = 1 ] && { echo "104.18.0.1 vitadhoc.psvita.dev"; exit 0; }
exit 2
`, { mode: 0o755 });
  writeFileSync(join(tmp, 'bin/curl'), `#!/usr/bin/env python3
import sys
a = sys.argv[1:]
url = a[-1]; out = a[a.index('-o') + 1]; hdr = a[a.index('-D') + 1]
open("${tmp}/curl.log", "a").write(url + "\\n")
path = url.split("vitadhoc.psvita.dev", 1)[1]
if path == "/v1/info": code, body = 200, '{"service":"vitadhoc","min":"${o.min || '2.0.0'}","latest":"${o.latest || '2.0.0'}","relay":${o.relay === false ? 'false' : 'true'},"busy":false,"roomTtlSec":600}'
elif path.startswith("/v1/rooms/ZZZZZ"): code, body = 404, '{"error":{"code":"not_found","message":"no such room"}}'
elif path == "/": code, body = ${o.rootCode || 403}, 'blocked'
else: code, body = 418, ''
open(out, "w").write(body); open(hdr, "w").write("HTTP/2 %d\\n" % code)
sys.stdout.write(str(code))
`, { mode: 0o755 });
  chmodSync(join(tmp, 'bin/curl'), 0o755);
  return { PATH: `${join(tmp, 'bin')}:${process.env.PATH}`, HOME: join(tmp, 'home') };
}

test('deploy script, offline run: tests, dry run, deploy, both secrets on stdin, live checks; secrets never in a log, argument or environment; verify-only deploys nothing', () => {
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  const env = deployStubs(tmp);
  const KEYID = 'keyid-0123456789abcdef';
  const TOK = `tok_${'x9Q'.repeat(12)}`;
  writeFileSync(join(tmp, 'keyid'), `${KEYID}\n`, { mode: 0o600 });
  writeFileSync(join(tmp, 'tok'), `${TOK}\n`, { mode: 0o600 });
  const r = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--skip-tests', '--turn-key-id-file', join(tmp, 'keyid'), '--turn-token-file', join(tmp, 'tok'), '--log-dir', join(tmp, 'log')], { env });
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /ALL CHECKS PASSED/);
  assert.match(r.out, /relay credentials: configured/);
  const calls = readFileSync(join(tmp, 'npx.log'), 'utf8').trim().split('\n');
  assert.ok(calls.every((c) => c.startsWith(`acct=${ACCT} `)), 'every call carries the account pin');
  assert.ok(calls.filter((c) => !c.includes('whoami')).every((c) => c.includes('--config wrangler.toml')), 'every wrangler call carries --config wrangler.toml');
  assert.ok(calls.some((c) => c.includes('deploy --dry-run')) && calls.some((c) => /wrangler deploy --config/.test(c)));
  assert.ok(calls.some((c) => c.includes('secret put TURN_KEY_ID')) && calls.some((c) => c.includes('secret put TURN_KEY_API_TOKEN')));
  const stdins = readdirSync(tmp).filter((f) => f.startsWith('stdin.'));
  assert.equal(stdins.length, 2);
  assert.deepEqual(stdins.map((f) => readFileSync(join(tmp, f), 'utf8')).sort(), [KEYID, TOK].sort(), 'the secrets reach wrangler on stdin');
  for (const f of allFiles(join(tmp, 'log'))) {
    const t = readFileSync(f, 'utf8');
    assert.ok(!t.includes(KEYID) && !t.includes(TOK), `${f} holds a secret`);
    assert.ok(!t.includes('owner@example.com') && !t.includes('Acme Hat'), `${f} holds whoami output`);
    assert.ok(!t.includes(ACCT), `${f} holds the account id`);
  }
  assert.ok(!r.out.includes(KEYID) && !r.out.includes(TOK) && !calls.join('\n').includes(TOK) && !readFileSync(join(tmp, 'curl.log'), 'utf8').includes(TOK));
  assert.ok(!/workers\.dev/.test(readFileSync(join(tmp, 'curl.log'), 'utf8')), 'only the custom domain is ever asked');
  // verify-only
  rmLog(tmp);
  const v = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--verify-only', '--log-dir', join(tmp, 'log2')], { env });
  assert.equal(v.code, 0, v.out);
  const vcalls = readFileSync(join(tmp, 'npx.log'), 'utf8').trim().split('\n');
  assert.ok(vcalls.every((c) => c.includes('whoami')), 'verify-only runs only whoami');
});
function rmLog(tmp) { writeFileSync(join(tmp, 'npx.log'), ''); }

test('deploy script: a wrong account stops before any deploy; an address wrangler should not report stops before the real deploy', () => {
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  const env = deployStubs(tmp);
  // the stub's whoami lists another account
  const wrong = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--skip-tests', '--log-dir', join(tmp, 'l1')], { env: { ...env, STUB_WHOAMI_ID: 'ffffffffffffffffffffffffffffffff' } });
  assert.notEqual(wrong.code, 0);
  assert.match(wrong.out, /does not list account id/);
  assert.ok(!readFileSync(join(tmp, 'npx.log'), 'utf8').includes('deploy'), 'nothing deployed on the wrong account');
  // wrangler reports a workers.dev address
  const tmp2 = mkdtempSync(join(tmpdir(), 'vad-'));
  const env2 = deployStubs(tmp2, { deployExtra: 'https://vitadhoc-online.someone.workers.dev' });
  const bad = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--skip-tests', '--log-dir', join(tmp2, 'l')], { env: env2 });
  assert.notEqual(bad.code, 0);
  assert.match(bad.out, /NOTHING WAS DEPLOYED/);
  const c2 = readFileSync(join(tmp2, 'npx.log'), 'utf8');
  assert.ok(!/wrangler deploy --config/.test(c2), 'the real deploy never ran');
});

test('deploy script: the first deploy stops when the hostname already resolves (a custom-domain publish could overwrite that DNS record); a re-deploy and --allow-existing-dns go on', () => {
  const run = (env, extra = []) => {
    const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
    const e = { ...deployStubs(tmp), ...env };
    const r = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--skip-tests', '--log-dir', join(tmp, 'l'), ...extra], { env: e });
    return { r, calls: readFileSync(join(tmp, 'npx.log'), 'utf8') };
  };
  // first deploy (no deployment yet), the name already resolves: refused before the dry run, nothing deployed
  const first = run({ STUB_DEPLOYMENTS_RC: '1', STUB_DNS: '1' });
  assert.notEqual(first.r.code, 0, first.r.out);
  assert.match(first.r.out, /already resolves/);
  assert.ok(!/deploy --dry-run|wrangler deploy --config/.test(first.calls), 'nothing was deployed or even dry-run');
  // the owner has looked: it goes on
  const forced = run({ STUB_DEPLOYMENTS_RC: '1', STUB_DNS: '1' }, ['--allow-existing-dns']);
  assert.equal(forced.r.code, 0, forced.r.out);
  assert.match(forced.r.out, /--allow-existing-dns given/);
  // first deploy, the name is free: the deploy creates it
  const free = run({ STUB_DEPLOYMENTS_RC: '1', STUB_DNS: '0' });
  assert.equal(free.r.code, 0, free.r.out);
  assert.match(free.r.out, /does not resolve yet/);
  // a re-deploy: the Worker exists, the name resolves (it is ours)
  const again = run({ STUB_DEPLOYMENTS_RC: '0', STUB_DNS: '1' });
  assert.equal(again.r.code, 0, again.r.out);
  assert.match(again.r.out, /already deployed/);
  // the plan names the guard
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  const plan = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--print-plan'], { env: deployStubs(tmp) });
  assert.match(plan.out, /DNS guard/);
});

test('deploy script: the live /v1/info must name min and latest 2.0.0 (the versions this build of the service is made for)', () => {
  for (const o of [{ min: '1.9.0' }, { latest: '2.1.0' }, { min: '2.0.1' }]) {
    const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
    const r = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--verify-only', '--log-dir', join(tmp, 'l')], { env: deployStubs(tmp, o) });
    assert.notEqual(r.code, 0, JSON.stringify(o));
    assert.match(r.out, /min|latest/, JSON.stringify(o));
  }
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  const ok = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--verify-only', '--log-dir', join(tmp, 'l')], { env: deployStubs(tmp) });
  assert.equal(ok.code, 0, ok.out);
  assert.match(ok.out, /min 2\.0\.0, latest 2\.0\.0/);
});

test('deploy script: live checks fail when the root is served (not 403/404)', () => {
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  const env = deployStubs(tmp, { rootCode: 200 });
  const r = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--verify-only', '--log-dir', join(tmp, 'l')], { env });
  assert.notEqual(r.code, 0);
  assert.match(r.out, /SOME CHECKS FAILED/);
  const tmp2 = mkdtempSync(join(tmpdir(), 'vad-'));
  const quiet = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--verify-only', '--log-dir', join(tmp2, 'l')], { env: deployStubs(tmp2, { relay: false }) });
  assert.notEqual(quiet.code, 0, 'verifying a deployed service: relay false is a failure');
  assert.match(quiet.out, /relay.*false|NOT configured/);
  // a deploy that was told to leave the secrets for later still reports it without failing
  const tmp3 = mkdtempSync(join(tmpdir(), 'vad-'));
  const later = sh(['scripts/deploy-vitadhoc-online.sh', '--account-id', ACCT, '--skip-tests', '--log-dir', join(tmp3, 'l')], { env: deployStubs(tmp3, { relay: false }) });
  assert.equal(later.code, 0, later.out);
  assert.match(later.out, /NOT configured yet/);
});

// ------------------------------------------------------------------------------------------------------------------ zone script

const RL_RULE = { id: 'rl1', ref: 'psvita-shared-ratelimit', description: 'shared', expression: '(starts_with(http.request.uri.path, "/v1/") or http.request.uri.path eq "/times")', action: 'block', enabled: true, ratelimit: { characteristics: ['cf.colo.id', 'ip.src'], period: 10, requests_per_period: 100, mitigation_timeout: 10 } };
const FOREIGN = (i) => ({ id: `x${i}`, ref: `other-${i}`, description: 'another host', expression: '(http.host eq "other.psvita.dev")', action: 'block', enabled: true });

function zoneEnv() {
  const tmp = mkdtempSync(join(tmpdir(), 'vad-'));
  mkdirSync(join(tmp, 'bin')); mkdirSync(join(tmp, 'seen')); mkdirSync(join(tmp, 'tmp'));
  cpSync(join(root, 'test/fixtures/curl-mock.py'), join(tmp, 'bin/curl'));
  chmodSync(join(tmp, 'bin/curl'), 0o755);
  const secret = `tok_${'k8Z'.repeat(12)}`;
  writeFileSync(join(tmp, 'token'), `${secret}\n`, { mode: 0o600 });
  const state = (over = {}) => writeFileSync(join(tmp, 'state.json'), JSON.stringify({ zone: { id: ZONE, status: 'active', account: ACCT }, phases: {}, n: 10, bot: false, bic: 'on', ...over }));
  const run = (...a) => {
    writeFileSync(join(tmp, 'calls.log'), '');
    const r = sh(['scripts/setup-vitadhoc-zone.sh', '--account-id', ACCT, '--token-file', join(tmp, 'token'), '--log-dir', join(tmp, `log${Math.random()}`), ...a],
      { env: { PATH: `${join(tmp, 'bin')}:${process.env.PATH}`, MOCK_DIR: tmp, MOCK_STATE: join(tmp, 'state.json'), TMPDIR: join(tmp, 'tmp') } });
    const calls = readFileSync(join(tmp, 'calls.log'), 'utf8').trim().split('\n').filter(Boolean);
    return { ...r, calls, writes: calls.filter((c) => /^(PUT|POST|PATCH|DELETE) /.test(c)), st: JSON.parse(readFileSync(join(tmp, 'state.json'), 'utf8')) };
  };
  return { tmp, secret, state, run };
}
const refs = (st, phase) => ((st.phases[phase] || {}).rules || []).map((r) => r.ref);

test('zone script: --self-check and --print-plan contact nothing; the plan has ONE WAF rule and a read-only look at the shared rate limit', () => {
  assert.equal(sh(['scripts/setup-vitadhoc-zone.sh', '--self-check']).code, 0);
  const r = sh(['scripts/setup-vitadhoc-zone.sh', '--account-id', ACCT, '--token-file', '/nonexistent', '--print-plan']);
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /vitadhoc-psvita-block-unserved/);
  assert.match(r.out, /http\.host eq "vitadhoc\.psvita\.dev"/);
  assert.match(r.out, /not starts_with\(http\.request\.uri\.path, "\/v1\/"\)/);
  assert.match(r.out, /acme-challenge/);
  assert.match(r.out, /pki-validation/);
  assert.match(r.out, /read only/);
  assert.ok(!/galaxy\.psvita|\/times/.test(r.out.replace(/"\/times"\)/g, '')), 'no other host, no /times');
  assert.ok(!r.out.includes(ACCT));
  assert.notEqual(sh(['scripts/setup-vitadhoc-zone.sh', '--token-file', '/x', '--print-plan']).code, 0);
});

test('zone script: creates ONE WAF rule for vitadhoc.psvita.dev, never writes the rate-limiting phase, a second run writes nothing', () => {
  const z = zoneEnv();
  z.state({ phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] } } });
  const r = z.run();
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /ALL CHECKS PASSED/);
  assert.deepEqual(refs(r.st, 'http_request_firewall_custom'), ['vitadhoc-psvita-block-unserved']);
  const waf = r.st.phases.http_request_firewall_custom.rules[0];
  assert.equal(waf.action, 'block');
  assert.ok(waf.expression.includes('http.host eq "vitadhoc.psvita.dev"') && !waf.expression.includes('galaxy') && !waf.expression.includes('/times'));
  assert.ok(r.writes.length >= 1);
  assert.ok(r.writes.every((w) => !/ratelimit/.test(w)), `a write touched the rate-limiting phase: ${r.writes}`);
  assert.deepEqual(r.st.phases.http_ratelimit.rules, [RL_RULE], 'the shared rule is untouched');
  assert.ok(r.out.indexOf('the plan') < r.out.indexOf('creating the'), 'the plan is printed before the first write');
  const again = z.run();
  assert.equal(again.code, 0, again.out);
  assert.equal(again.writes.length, 0, 'idempotent: a second run writes nothing');
  assert.ok(!readFileSync(join(z.tmp, 'seen/curl-args'), 'utf8').includes(z.secret) && !readFileSync(join(z.tmp, 'seen/curl-env'), 'utf8').includes(z.secret), 'the token is never an argument or an environment variable');
  for (const f of allFiles(z.tmp).filter((p) => /log/.test(p))) assert.ok(!readFileSync(f, 'utf8').includes(z.secret), f);
});

test('zone script: the live zone shape: rules under hex refs are found by their description (the shared rate limit, and ours on a second run)', () => {
  const z = zoneEnv();
  const liveRl = { ...RL_RULE, ref: '7ad74ab09438493587879bd717d7bdfa', description: 'psvita-shared-ratelimit' };
  const galaxy = { ...FOREIGN(1), ref: '4ebb3dd6627343eab717f6d8cd4e743a', description: 'galaxy-psvita-block-unserved', expression: '(http.host eq "galaxy.psvita.dev" and not starts_with(http.request.uri.path, "/v1/"))' };
  z.state({ phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [liveRl] }, http_request_firewall_custom: { id: 'rs_http_request_firewall_custom', rules: [galaxy] } } });
  const r = z.run();
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /ALL CHECKS PASSED/);
  assert.deepEqual(r.writes.map((w) => w.split(' ')[0]), ['POST']);
  assert.deepEqual(r.st.phases.http_ratelimit.rules, [liveRl], 'the shared rule is untouched');
  // the zone keeps our rule under an id of its own as ref: a second run still finds it (by description) and writes nothing
  const st = r.st;
  const ours = st.phases.http_request_firewall_custom.rules.find((x) => x.ref === 'vitadhoc-psvita-block-unserved');
  ours.ref = '0123456789abcdef0123456789abcdef';
  writeFileSync(join(z.tmp, 'state.json'), JSON.stringify(st));
  const again = z.run();
  assert.equal(again.code, 0, again.out);
  assert.equal(again.writes.length, 0, 'found by description: nothing written');
  assert.equal(again.st.phases.http_request_firewall_custom.rules[0].description, 'galaxy-psvita-block-unserved', "Galaxy's rule is kept");
});

test('zone script: stops with a clear message, writing NOTHING, when the shared rate limit is missing or does not cover /v1/*', () => {
  for (const [what, phases] of [
    ['no entrypoint', {}],
    ['an empty phase', { http_ratelimit: { id: 'rs_http_ratelimit', rules: [] } }],
    ['another rate rule', { http_ratelimit: { id: 'rs_http_ratelimit', rules: [{ ...RL_RULE, ref: 'someone-else' }] } }],
    ['a rule for other paths', { http_ratelimit: { id: 'rs_http_ratelimit', rules: [{ ...RL_RULE, expression: '(http.request.uri.path eq "/times")' }] } }],
    ['a disabled rule', { http_ratelimit: { id: 'rs_http_ratelimit', rules: [{ ...RL_RULE, enabled: false }] } }],
  ]) {
    const z = zoneEnv();
    z.state({ phases });
    const r = z.run();
    assert.notEqual(r.code, 0, what);
    assert.match(r.out, /psvita-shared-ratelimit/, what);
    assert.equal(r.writes.length, 0, `${what}: nothing written`);
    const v = z.run('--verify-only');
    assert.notEqual(v.code, 0, `${what} (verify-only)`);
    assert.equal(v.writes.length, 0);
  }
});

test('zone script: other rules in the WAF phase are kept; a changed rule is updated in place; --remove deletes only ours and never the rate limit', () => {
  const z = zoneEnv();
  z.state({ phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] }, http_request_firewall_custom: { id: 'rs_http_request_firewall_custom', rules: [FOREIGN(1), FOREIGN(2)] } } });
  let r = z.run();
  assert.equal(r.code, 0, r.out);
  assert.deepEqual(refs(r.st, 'http_request_firewall_custom'), ['other-1', 'other-2', 'vitadhoc-psvita-block-unserved']);
  assert.deepEqual(r.writes.map((w) => w.split(' ')[0]), ['POST']);
  // someone edited our rule
  const st = r.st;
  st.phases.http_request_firewall_custom.rules[2].expression = '(http.host eq "vitadhoc.psvita.dev")';
  writeFileSync(join(z.tmp, 'state.json'), JSON.stringify(st));
  r = z.run();
  assert.equal(r.code, 0, r.out);
  assert.deepEqual(r.writes.map((w) => w.split(' ')[0]), ['PATCH']);
  assert.deepEqual(refs(r.st, 'http_request_firewall_custom'), ['other-1', 'other-2', 'vitadhoc-psvita-block-unserved']);
  assert.ok(r.writes.every((w) => !/ratelimit/.test(w)));
  // rollback
  r = z.run('--remove');
  assert.equal(r.code, 0, r.out);
  assert.deepEqual(refs(r.st, 'http_request_firewall_custom'), ['other-1', 'other-2']);
  assert.deepEqual(r.writes.map((w) => w.split(' ')[0]), ['DELETE']);
  assert.deepEqual(r.st.phases.http_ratelimit.rules, [RL_RULE]);
});

test('zone script: Bot Fight Mode ON stops the run BEFORE any write (the rule is not added first and reported afterwards); --remove and --verify-only still work', () => {
  const z = zoneEnv();
  z.state({ bot: true, phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] } } });
  let r = z.run();
  assert.notEqual(r.code, 0);
  assert.match(r.out, /Bot Fight Mode is ON/);
  assert.equal(r.writes.length, 0, `nothing is written while Bot Fight Mode is on: ${r.writes}`);
  assert.deepEqual(refs(r.st, 'http_request_firewall_custom'), []);
  r = z.run('--verify-only');
  assert.notEqual(r.code, 0);
  assert.equal(r.writes.length, 0);
  // off: the same run writes the rule
  z.state({ bot: false, phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] } } });
  r = z.run();
  assert.equal(r.code, 0, r.out);
  assert.equal(r.writes.length, 1);
  // rollback is allowed whatever Bot Fight Mode says
  const st = r.st; st.bot = true; writeFileSync(join(z.tmp, 'state.json'), JSON.stringify(st));
  r = z.run('--remove');
  assert.deepEqual(r.writes.map((w) => w.split(' ')[0]), ['DELETE']);
});

test('zone script: stops when the zone already has 5 custom rules and none is ours (nothing is dropped)', () => {
  const z = zoneEnv();
  z.state({ phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] }, http_request_firewall_custom: { id: 'rs_http_request_firewall_custom', rules: [1, 2, 3, 4, 5].map(FOREIGN) } } });
  const r = z.run();
  assert.notEqual(r.code, 0);
  assert.match(r.out, /5 rules|allows 5/);
  assert.equal(r.writes.length, 0);
});

test('zone script: --verify-only writes nothing; the account and zone pins; a missing permission stops before any write', () => {
  const z = zoneEnv();
  z.state({ phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] } } });
  let r = z.run('--verify-only');
  assert.notEqual(r.code, 0, 'our rule is not there yet');
  assert.equal(r.writes.length, 0);
  z.run();
  r = z.run('--verify-only');
  assert.equal(r.code, 0, r.out);
  assert.equal(r.writes.length, 0);
  // the zone belongs to another account
  z.state({ zone: { id: ZONE, status: 'active', account: 'ffffffffffffffffffffffffffffffff' }, phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] } } });
  r = z.run();
  assert.notEqual(r.code, 0);
  assert.match(r.out, /does not belong to account/);
  assert.equal(r.writes.length, 0);
  // a token that may only read
  z.state({ rulesets_perm: 'read', phases: { http_ratelimit: { id: 'rs_http_ratelimit', rules: [RL_RULE] } } });
  r = z.run();
  assert.notEqual(r.code, 0);
  assert.match(r.out, /permission|Edit/);
});
