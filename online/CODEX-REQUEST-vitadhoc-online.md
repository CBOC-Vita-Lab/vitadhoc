# Codex request: deploy the VitAdhoc 2.0 online service to https://vitadhoc.psvita.dev (15 Nov 2026)

You are doing ONE job, in this order:

- (A) checks that contact nothing;
- (B) create the relay (TURN) key, then deploy the new Worker `vitadhoc-online` on the custom domain `vitadhoc.psvita.dev` with its two secrets;
- (C) add ONE host-scoped security rule to the Cloudflare zone `psvita.dev`;
- (D) verify and write a report.

Nothing else. Read all of this before you start.

**The zone `psvita.dev` is SHARED.** Two other services live in it, each on its own host, each its own Worker. They are not part of this
request. Never touch them, list them, contact them or put their names in a command: see "Hard rules".

---

## Package (made by the person who prepared this request; you only check it)

* File: `vitadhoc-online-server-20261115a.tgz`, in the same folder as this prompt.
* SHA-256: `<<SHA256 OF THE TGZ: filled in by the coordinator at release time; do not run this request while this line still shows a placeholder>>`.
  Compute it yourself first (`shasum -a 256` on macOS). If it differs, or the line above is still a placeholder, STOP and report.
* Unpack it into a NEW empty folder whose path contains a space (for example `~/vitadhoc deploy 20261115/`) and work from `<that folder>/server`.
  It holds the Worker (`wrangler.toml`, `src/`), `test/`, `dev/`, `scripts/` and the docs `README.md`, `PROTOCOL.md`, `DEPLOY.txt`.
* Network you may use, and nothing else: the npm registry (for `npm install`); `api.cloudflare.com` and Cloudflare's wrangler endpoints, only
  through the three scripts and wrangler as written below; `https://vitadhoc.psvita.dev` for the `curl`, `openssl s_client` and smoke checks of part D.
  Never `*.workers.dev` (this Worker has no workers.dev address by design).

The three scripts you will run (all in `scripts/`, all read by you before use, see A3): `create-turn-key.sh`, `deploy-vitadhoc-online.sh`,
`setup-vitadhoc-zone.sh`. Always run them as `bash scripts/<name> ...` (the files are not marked executable).

*How this package was made (for the person preparing it, not for you):* from the repository root at the release commit,
`W=$(mktemp -d); git archive --format=tar --prefix=server/ HEAD:online/service | tar -x -C "$W"; (cd "$W" && tar --sort=name --mtime='UTC 2026-01-01' --owner=0 --group=0 --numeric-owner -cf - server | gzip -n -9) > vitadhoc-online-server-20261115a.tgz; shasum -a 256 vitadhoc-online-server-20261115a.tgz`.
The result depends only on the content of `online/service`, so the same tree always gives the same hash.

## Account id (do not echo it)

The Cloudflare account id (32 hex characters) is read from the file `~/.vad-account-id` (one line, mode 600), which the owner keeps for this.
Read it into a shell variable WITHOUT printing it:

    ACCOUNT_ID=$(tr -d ' \r\n' < ~/.vad-account-id)
    [ "${#ACCOUNT_ID}" = 32 ] && printf '%s' "$ACCOUNT_ID" | grep -qE '^[0-9a-f]{32}$' || echo "ACCOUNT ID NOT USABLE: STOP"

* STOP if the file is missing or the value is not exactly 32 lower-case hex characters. Never get it from `wrangler whoami` (that prints an e-mail address).
* Every command below uses `"$ACCOUNT_ID"`, never the literal id. The scripts show it only as its last 4 characters; never print it yourself.
* The variable may not survive between your commands: run the whole request in ONE shell session, or start every command that needs it with the
  assignment line above. If a script says `--account-id <32 hex characters> is required`, the variable was empty: read it again. Never type the id by hand.

## Files and tokens (the owner prepares these once; none of them is ever printed, copied or put on a command line)

| file (mode 600, one line) | what | needed by |
|---|---|---|
| `~/.vad-account-id` | the account id | everything |
| `wrangler login` (done by the owner) | the owner's session | part B deploy |
| `~/.cf-vad-calls-token` | an API token with the ACCOUNT permission **Calls Write** (dashboard: My Profile > API Tokens > Create Custom Token > Account > Calls (Realtime) > Edit), account resources: this account ONLY | B1 only (creating the relay key) |
| `~/.cf-psvita-token` | an API token with Zone > Zone > Read and Zone > Zone WAF > Edit, zone resources: **psvita.dev ONLY**. Optional but wanted: Zone > Bot Management > Read (so the script can read Bot Fight Mode). It may already exist from the owner's earlier zone work. | part C and the zone check of D |

**The relay key: two ways, the owner decides (say which you used in the report).**

1. *Create a new key* (this request, B1): needs `~/.cf-vad-calls-token`. The script `create-turn-key.sh` makes ONE key named `vitadhoc-online`
   and writes its two values to `~/.vad-turn-key-id` and `~/.vad-turn-token` (mode 600). Cloudflare shows the key's secret ONCE, so the script
   never overwrites those files and creates nothing if both already exist.
2. *Reuse a key the owner already has*: the owner puts the key's id and its API token in `~/.vad-turn-key-id` and `~/.vad-turn-token` (one line each,
   mode 600), and no Calls token is needed; B1 then says "reusing" and creates nothing. (A reused key is shared with whatever else uses it; the
   service only mints credentials from it.)

**If a file the part needs is missing**, do not stop the whole request: finish every part that does not need it, mark the part PENDING, and put
this, verbatim, at the top of the report and in your final message, naming only the file(s) that are missing:
> PENDING: create `<file>` once (steps above) and tell Codex "rerun part <B1 | C>".

## Hard rules

* Touch ONLY the Worker `vitadhoc-online`, the hostname `vitadhoc.psvita.dev`, the one TURN key of this service and the ONE zone rule with ref
  `vitadhoc-psvita-block-unserved`.
* **MUST NOT BE TOUCHED, LISTED, CONTACTED OR NAMED IN COMMANDS:** the other services of the zone and account (the Workers `smg-online` and
  `msc-online`, their hostnames, routes, secrets, Durable Objects and zone rules); **the zone's SHARED rate-limiting rule
  `psvita-shared-ratelimit`** (this request adds NO rate-limiting rule: the Free plan allows one per zone and the shared one already covers `/v1/*`;
  the zone script only READS it); **Bot Fight Mode** (it must stay OFF; you never change it); Browser Integrity Check (read only); DNS records
  (the deploy creates the one for the custom domain by itself; you add none); any other zone setting.
* Never read anything in the owner's private folders, and never open `cloudflare-*.txt` files.
* Never print, log, commit or paste a token, a TURN key value, the full account id, or an e-mail address. Keep only the scripts' own summaries.
* Do not loosen any check in the scripts. Do not run `wrangler` or call the Cloudflare API yourself outside the scripts; the other network use is the
  plain `curl`, `openssl s_client` and `node scripts/smoke.mjs` of part D, against `https://vitadhoc.psvita.dev` only.
* Never turn anything off, never delete anything, in this request. Undo steps are at the end and run only if the owner asks.
* If a step fails or a check is not as described: STOP, change nothing further, report exactly what you saw (scrubbed). Do not work around a refusal.

## A. Checks before touching Cloudflare (nothing is contacted except the npm registry)

1. Tools: `node` (version 22.5 or newer: the service tests use `node:sqlite`), `npm`, `npx`, `curl`, `python3`, `openssl`, `bash`. A missing or too old
   tool is a STOP; name it. Compute the package's SHA-256 and compare. Read the account id as above. Check that the files of the table above that
   parts B and C need exist (check existence only; do not open or print the token files; refer to them as `~/.cf-psvita-token` and so on).
2. Unpack, `cd` into `server`, run `npm install --no-audit --no-fund` (this package has no lock file; it installs wrangler 4 beside the scripts, so
   that `npx wrangler` is the installed one and not a fresh download), then `npx wrangler --version` (must start with `4.`).
3. **Read the three scripts** (`create-turn-key.sh`, `deploy-vitadhoc-online.sh`, `setup-vitadhoc-zone.sh`) line by line before running any. They
   follow one set of safety rules; what you will find, so that you do not have to guess:
   * `create-turn-key.sh`: curl runs only inside `api()`, the token and the body reach curl on stdin (`-K -`), the reply (which holds the new secret) is
     never printed or written, only the uid's last 4 characters are shown, nothing is overwritten.
   * `deploy-vitadhoc-online.sh`: every wrangler call goes through `wr()` (account pinned, `--config wrangler.toml`, output through `scrub()`, no
     wrangler debug log); it refuses a config that names another service or any address but `vitadhoc.psvita.dev`; secrets go to wrangler on stdin.
   * `setup-vitadhoc-zone.sh`: curl runs only inside `api()`; replies are kept as summaries with ids shown as their last 4 characters; it prints only
     the expression of OUR rule (never the expression of a rule that is not ours: it does not write the shared rate-limit rule at all, so it has
     nothing to merge, drop or update there); the API's raw reply is never printed.
   * Known limits, which this request compensates by its own order of steps (they are not a reason to stop): the zone script reads Bot Fight Mode
     and fails on it only AFTER its writes, so C0 runs the read-only `--verify-only` first and C2 runs only if that shows Bot Fight Mode is not ON; the
     deploy script's live check proves only "200 with `service` vitadhoc, an unknown room is 404, `/` is not served", so part D adds the explicit
     checks of the version numbers and the relay flag with `curl`.
   * If your own reading finds a line that could leak a token, a TURN secret, the account id or an e-mail address, STOP and name the line.
4. Run `bash scripts/deploy-vitadhoc-online.sh --self-check`, `bash scripts/setup-vitadhoc-zone.sh --self-check`,
   `bash scripts/create-turn-key.sh --self-check` (each prints `self-check ok`), and `bash scripts/create-turn-key.selftest.sh` (ends `21 ok, 0 failed`).
5. `node --test "test/*.test.mjs"` (the quotes matter: Node 22 does not expand a folder). Expect `# pass 110`, `# fail 0`, `# skipped 0`. Any other
   number or any failure: STOP (it is the wrong package or a broken one).
6. Print the three plans and read them; they contact nothing:
   * `bash scripts/create-turn-key.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-vad-calls-token --key-id-file ~/.vad-turn-key-id --turn-token-file ~/.vad-turn-token --print-plan`
   * `bash scripts/deploy-vitadhoc-online.sh --account-id "$ACCOUNT_ID" --turn-key-id-file ~/.vad-turn-key-id --turn-token-file ~/.vad-turn-token --print-plan`
   * `bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token --print-plan`

   The deploy plan must show the custom domain `https://vitadhoc.psvita.dev`, workers.dev OFF, and only `npx wrangler whoami`, `deploy --dry-run`,
   `deploy` and the two `secret put` calls. The zone plan must show exactly ONE written rule (`vitadhoc-psvita-block-unserved`, phase
   `http_request_firewall_custom`, host `vitadhoc.psvita.dev` only) and the shared rate-limit rule as READ ONLY. Anything else: STOP.

## B. The relay key and the deploy. Always runs (B1 needs `~/.cf-vad-calls-token` unless the key files already exist).

**B1. The relay key.**

    bash scripts/create-turn-key.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-vad-calls-token --key-id-file ~/.vad-turn-key-id --turn-token-file ~/.vad-turn-token

* Expected: `key created (uid ends ****xxxx); wrote the key id and the secret to the two files (mode 600)`, exit 0; or `both key files already exist: reusing them`, exit 0.
* **If the account needs a payment method (or a plan) for Realtime, the script prints the API's message and exits 4: STOP the whole request there.**
  Nothing was created and nothing was deployed. Report the message and say "payment method needed for Realtime". Do NOT add a payment method.
* Exit 7: the Calls token lacks the permission: mark B PENDING with the one-line PENDING text; do not go on to B2 (the deploy needs the files).
* Exit 5: the key was created but its reply could not be read; the script printed the uid's last 4 characters. STOP and report them; the owner
  deletes that one orphan key in the dashboard (Realtime > TURN).
* **Never run B1 a second time to "retry" after a success or after exit 5 without the owner's say-so** (every run would create another key).
  If B2 fails later, the two files stay and B2 is simply repeated.

**B2. The deploy (creates ONLY the Worker `vitadhoc-online` and its custom domain; sets the two secrets).**

    bash scripts/deploy-vitadhoc-online.sh --account-id "$ACCOUNT_ID" --turn-key-id-file ~/.vad-turn-key-id --turn-token-file ~/.vad-turn-token

* Use no `--url` and no other option. The script: checks the login lists the account (`whoami`, output never stored), runs the local suite, a dry run
  (stops unless wrangler reports no address but `vitadhoc.psvita.dev`), the deploy, puts `TURN_KEY_ID` and `TURN_KEY_API_TOKEN` on stdin to
  `wrangler secret put`, then its live checks, and ends `ALL CHECKS PASSED`.
* On a first deploy the custom domain's DNS record and certificate can take minutes: while there is NO answer at all the script asks again every 10 s for up
  to 10 minutes. That is normal. It does not retry a wrong answer. If ONLY that wait ran out (everything else fine, and the deploy itself reported
  `vitadhoc.psvita.dev`), do NOT deploy again: wait a few minutes, then run D1 once.
* Likely failures (report them, do not improvise): the zone `psvita.dev` is not active on this account; a DNS record named `vitadhoc` already exists in the
  zone (report it, do not delete it); wrangler cannot create the custom domain with the login (report the message).

## C. The zone's ONE rule (after B passed; needs `~/.cf-psvita-token`: if missing, mark C PENDING and go to D)

**C0. Read-only look, BEFORE any write:**

    bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token --verify-only

Before our rule exists this ends `SOME CHECKS FAILED` with `WAF custom rule: NOT as planned (missing)` or `(absent)`: that part is expected. Read the
other lines and decide:

* `the shared rate-limiting rule psvita-shared-ratelimit does not exist` or `does not cover /v1/*`: STOP part C (nothing was written). Report it and add:
  "the shared rate-limit rule of the zone is created by the other service's zone step; run that first, then rerun part C". Do NOT create or change it.
* `Bot Fight Mode is ON`: STOP part C (nothing was written). Report it. Do NOT change it; the owner turns it off (Security > Settings > Bot traffic) and says "rerun part C".
* `Bot Fight Mode: could not be read with this token`: not a stop, but write it in the report as "Bot Fight Mode NOT READ, the owner must confirm it is OFF".
* `missing permission: ...` / HTTP 401 or 403: report the name; mark C PENDING with the one-line PENDING text.

**C1.** `bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token --print-plan` again, then **C2**, only if C0 raised no STOP:

    bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token

How it works: it finds the zone by name (exactly one, active, in this account), reads both phases, keeps every rule that is not ours, prints the exact
plan, and only then writes ONE rule, found by its ref `vitadhoc-psvita-block-unserved`, action Block:

    (http.host eq "vitadhoc.psvita.dev" and not starts_with(http.request.uri.path, "/v1/") and not starts_with(http.request.uri.path, "/.well-known/acme-challenge/") and not starts_with(http.request.uri.path, "/.well-known/pki-validation/"))

Allowed reactions, always after the script printed the API's own message:
* **missing permission** (HTTP 401, or 403 without a plan message): report the name; the owner fixes the token; "rerun part C". A second run writes only what is missing.
* **refused by the plan** (403 whose message names the plan or an entitlement): STOP and report the message.
* **The API refuses the expression** (`starts_with`, `http.host`): STOP and report. Do not rewrite it.
* **The zone already holds 5 custom rules and none is ours**: the script stops by itself and writes nothing. Report the count and say it. Remove nothing.
* **The script reports "after the write ... Bot Fight Mode is ON"** (it only learns this late if C0 could not read it): report it, change nothing; the owner turns it off.

**C3. A second run** must write nothing and print `WAF rule: unchanged` for our rule:
`bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token` (again; it ends `ALL CHECKS PASSED`). Keep the summary line.

## D. Verify, then report

The checks below are what the request means by "verified". The scripts' own verifiers are narrower (see A3); do each numbered check yourself and write
down its result, pass or fail, even where a script printed `ALL CHECKS PASSED`.

1. `bash scripts/deploy-vitadhoc-online.sh --account-id "$ACCOUNT_ID" --verify-only`: must end `ALL CHECKS PASSED`. (It proves: `/v1/info` is 200 with
   `"service":"vitadhoc"`; an unknown room answers 404 `not_found`; `/` is 403 or 404; no wrangler debug log appeared. It does NOT prove the version
   numbers or the relay flag: items 3 and 4 do.)
2. If part C ran: `bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token --verify-only`: must end `ALL CHECKS PASSED`
   (our rule exactly as planned; the shared rate-limit rule present and covering `/v1/*`). Report the Bot Fight Mode line as printed (`off`, or `could not be read`).
3. Plain `curl` checks, no token; keep only status lines and headers, and save the output in your folder. Send `User-Agent: VitAdhoc/2.0.0 (PlayStation Vita)`
   (the app's own) unless a line says otherwise:
   * `https://vitadhoc.psvita.dev/v1/info?v=2.0.0` -> 200, JSON with `"service":"vitadhoc"`, `"min":"2.0.0"`, `"latest":"2.0.0"`, `"relay":true`,
     `"roomTtlSec":600`; header `cache-control: no-store`. **`"relay":false` is a FAIL** (the secrets are not in effect): report it.
   * `https://vitadhoc.psvita.dev/v1/rooms/ZZZZZ?v=2.0.0` -> 404 with `"code":"not_found"` (the Worker, the Durable Object and the code path are alive).
   * `https://vitadhoc.psvita.dev/v1/rooms/ZZZZZ` (no `v`) -> 400 `bad_version`.
   * `https://vitadhoc.psvita.dev/v1/rooms/ZZZZZ?v=1.9.0` -> 426 `update_required` with `"min":"2.0.0"`.
   * `https://vitadhoc.psvita.dev/v1/info` with `User-Agent: curl-check` -> the status Cloudflare gives it (200 expected). If it is 403, report it with
     whether Browser Integrity Check is on; the app's own User-Agent above must still be 200 on `/v1/info`.
   * Only if part C ran: `https://vitadhoc.psvita.dev/` and `https://vitadhoc.psvita.dev/robots.txt` -> **403 from Cloudflare** (the zone rule: an HTML or plain Cloudflare
     block page, NOT the Worker's JSON `{"error":{"code":"not_found",...}}`). If they answer 404 with the Worker's JSON, the rule is not in effect: report it. If C is
     pending, 404 from the Worker is expected; say so.
4. `node scripts/smoke.mjs https://vitadhoc.psvita.dev --relay` -> must end `smoke OK`. It plays a whole session with two fake members (create, lookup,
   poll, join, poll, close, gone) and, with `--relay`, mints ONE real relay credential: the line `ok N relay credentials (a UDP turn: url with a username)`
   proves the TURN key works. Run it ONCE (do not loop it: it counts against the service's daily limits). A `relay_unavailable` or `turn_upstream` answer is a FAIL: report it.
5. Save the certificate chain the app will see, for the owner's TLS test:
   `openssl s_client -showcerts -connect vitadhoc.psvita.dev:443 -servername vitadhoc.psvita.dev </dev/null` -> keep only the PEM blocks, in `served-chain.pem`
   next to your report (public data), and write the subject, issuer and `notAfter` of each block into the report
   (`openssl x509 -noout -subject -issuer -enddate` on each). The app trusts eight roots (Google Trust Services R1/R4, ISRG X1/X2, four SSL.com); say whether
   the last certificate in the chain is issued by one of them or what else it is.
6. Write `CODEX-REPORT-vitadhoc-online-20261115.md` next to your working folder. Include: the package SHA-256 as verified; which relay-key way was used
   (created / reused) and the uid's last 4 characters; the Wrangler version id of the deploy; the exit code and last lines of every summary; the test
   counts (110 pass); the result of every numbered check D1 to D5 (pass / fail / not run); the Bot Fight Mode and Browser Integrity Check readings
   as the scripts printed them; whether parts B1, B2 and C are DONE or PENDING (with the PENDING line); what you did NOT do (no rate-limit rule, no other
   service, no Bot Fight change, no DNS, no workers.dev, no notifications: the Free plan has none for Workers or Durable Objects); anything unexpected.
   **Never include**: a token, a TURN key value, the account id (last 4 characters at most), an e-mail address, an account name.
   Send back: the report, the three `--print-plan` outputs, `served-chain.pem` and the curl status lines.

## Undo (only if the owner asks)

* **Quickest, no change to Cloudflare resources:** the owner sets the Worker variable `KILL_SWITCH` to `all` in the dashboard (Workers > `vitadhoc-online`
  > Settings > Variables): the apps then say "Online play is busy", nothing is deleted. Clear it to switch back on.
* **The zone rule:** `bash scripts/setup-vitadhoc-zone.sh --account-id "$ACCOUNT_ID" --token-file ~/.cf-psvita-token --remove` deletes ONLY our rule
  `vitadhoc-psvita-block-unserved`. The shared rate-limit rule and every other rule are never touched by it.
* **The Worker and its custom domain:** the owner deletes the Worker `vitadhoc-online` in the dashboard (Workers & Pages > `vitadhoc-online` > Settings >
  Delete). That deletes its Durable Object and every room in it (rooms live at most 10 minutes anyway), and detaches `vitadhoc.psvita.dev`. Then look at
  the zone's DNS: a leftover record named `vitadhoc` is removed by the owner, not by you. (If the owner prefers wrangler: the ONE exception to
  "no wrangler outside the scripts" is `CLOUDFLARE_ACCOUNT_ID="$ACCOUNT_ID" WRANGLER_WRITE_LOGS=false WRANGLER_SEND_METRICS=false npx wrangler delete --config wrangler.toml`,
  and only on the owner's explicit say-so; show nothing of its output but its last line.)
* **The relay key:** the owner deletes the key named `vitadhoc-online` in the dashboard (Realtime > TURN). Do it only after the Worker is gone or switched
  off: without the key, every relay request answers 503 `relay_unavailable` while direct links keep working.
* **A bad new version of the Worker** (not the first deploy): `CLOUDFLARE_ACCOUNT_ID="$ACCOUNT_ID" WRANGLER_WRITE_LOGS=false WRANGLER_SEND_METRICS=false npx wrangler rollback --config wrangler.toml`,
  again only if the owner asks.
