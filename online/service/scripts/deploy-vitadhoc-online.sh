#!/bin/bash
# Deploys (or only checks) the VitAdhoc online service, vitadhoc-online. See ../DEPLOY.txt. NEVER RUN BY AN AGENT: the owner (or the
# owner's Codex) runs it; the tests only run --print-plan and --self-check, which contact nothing.
#
#   scripts/deploy-vitadhoc-online.sh --account-id <32 hex> [options]
#
#   --account-id ID        REQUIRED. The Cloudflare account to act on (32 hex characters). Exported as CLOUDFLARE_ACCOUNT_ID for every wrangler call;
#                          refused unless `wrangler whoami` lists it. This pin is what prevents acting on a wrong account.
#   --url URL              the service's public address. Only https://vitadhoc.psvita.dev (the default: the Worker custom domain declared in
#                          wrangler.toml) is accepted. workers.dev is OFF (workers_dev = false) and is never used or checked.
#   --turn-key-id-file F   set the secret TURN_KEY_ID from the first line of F (Cloudflare Realtime TURN key id; never printed)
#   --turn-token-file F    set the secret TURN_KEY_API_TOKEN from the first line of F (the TURN key's API token; never printed)
#                          (both together, or neither; without them the service answers 503 relay_unavailable on route 6 until they are set)
#   --print-plan           print what would be run (the account id shown as its last 4 characters; no token file is opened) and exit 0; contacts and writes nothing
#   --self-check           check that no wrangler call exists outside the wr() function, then exit
#   --verify-only          deploy nothing, set no secret: run the live checks against https://vitadhoc.psvita.dev
#   --skip-tests           do not run the local test suite first (it contacts nothing)
#   --allow-existing-dns   the FIRST deploy of the Worker is refused when the name vitadhoc.psvita.dev already resolves: a Worker custom
#                          domain publish can take over (overwrite) an existing DNS record of that name without asking. Pass this only after
#                          looking at the zone's DNS records and deciding that the record may be replaced.
#   --log-dir DIR          where every reply is kept (default deploy-logs/<UTC time>)
#
# NOT DONE BY THIS SCRIPT: the zone's security rule (scripts/setup-vitadhoc-zone.sh does it). The shared rate-limiting rule of psvita.dev
# belongs to the Galaxy script. Bot Fight Mode stays OFF on psvita.dev. Nothing in the Worker can stop requests that reach it: the platform
# has counted them already.
#
# Rules this script keeps: every wrangler call carries --config wrangler.toml and goes through wr() (config, account env, output filter);
# it refuses to run when the config or --url mention smg-online, msc-online or galaxy, so it can never act on the Galaxy or Strikers
# services; `wrangler whoami` output is never stored (only OK or the failure reason; only the account ids are read from it); every other
# wrangler output passes scrub() (e-mail addresses and account-name rows removed) before it reaches a file; no secret is ever printed,
# logged, written to disk or put on a command line (it goes to wrangler on stdin); every HTTP reply is kept (status, headers, body) next to
# the request that produced it.
set -euo pipefail
umask 077
# wrangler's own debug logger writes every message (whoami's account name, e-mail and full ids included) to a file under
# ~/.wrangler/logs or ./.wrangler/logs BEFORE any console filtering, and wrangler sends usage metrics: both are switched off
# for every call made from here, and the end of the script fails if a wrangler log file appeared all the same.
export WRANGLER_WRITE_LOGS=false WRANGLER_SEND_METRICS=false

CONFIG=wrangler.toml
URL=""
CUSTOM_URL="https://vitadhoc.psvita.dev"
HOSTNAME_OK="vitadhoc.psvita.dev"
ACCOUNT_ID=""
PRINT_PLAN=0
SELF_CHECK=0
WHOAMI_IDS=""
WR_URLS=""
WR_HOSTS=""
VERIFY_ONLY=0
SKIP_TESTS=0
ALLOW_DNS=0
KEYID_FILE=""
TURNTOK_FILE=""
LOG_DIR=""
CUSTOM_WAIT_SEC=${DEPLOY_CUSTOM_WAIT_SEC:-600}   # a brand-new custom domain (DNS, certificate) can take minutes: "no answer" is retried this long

die() { echo "deploy-vitadhoc-online: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --url) URL=${2:-}; shift 2 ;;
    --account-id) ACCOUNT_ID=${2:-}; shift 2 ;;
    --turn-key-id-file) KEYID_FILE=${2:-}; shift 2 ;;
    --turn-token-file) TURNTOK_FILE=${2:-}; shift 2 ;;
    --print-plan) PRINT_PLAN=1; shift ;;
    --self-check) SELF_CHECK=1; shift ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    --skip-tests) SKIP_TESTS=1; shift ;;
    --allow-existing-dns) ALLOW_DNS=1; shift ;;
    --log-dir) LOG_DIR=${2:-}; shift 2 ;;
    -h|--help) awk 'NR>1 { if ($0 ~ /^set -/) exit; print }' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# Self-check: outside wr() no line may run wrangler (comments and quoted messages do not count: a call is `npx wrangler` or a bare command word).
self_check() {
  awk 'BEGIN { p1 = "npx +(-y +)?wran" "gler"; p2 = "(^|[^A-Za-z0-9_./-])wran" "gler +[a-z]" }
       /^wr\(\) *\{/ { inwr = 1 }
       { if (!inwr && $0 !~ /^[ \t]*#/ && $0 !~ /(die|note|echo) / && ($0 ~ p1 || $0 ~ p2)) { print FILENAME ":" NR ": " $0; bad = 1 } }
       /^\}/ { inwr = 0 }
       END { exit bad }' "$0"
}
if [ $SELF_CHECK -eq 1 ]; then
  self_check || die "wrangler is called outside the wr() function (lines above)"
  echo "self-check ok: every wrangler call goes through wr()"; exit 0
fi

cd "$(dirname "$0")/.."
[ -f "$CONFIG" ] || die "no $CONFIG here ($(pwd))"
grep -q '^name = "vitadhoc-online"' "$CONFIG" || die "$CONFIG does not name the worker vitadhoc-online: refusing"
if grep -v '^[[:space:]]*#' "$CONFIG" | grep -qiE 'smg-online|msc-online|galaxy|strikers'; then die "$CONFIG mentions smg-online, msc-online, galaxy or strikers: refusing (this script deploys vitadhoc-online ONLY)"; fi
grep -q '^routes = \[{ pattern = "vitadhoc.psvita.dev", custom_domain = true }\]' "$CONFIG" || die "$CONFIG does not declare the custom domain vitadhoc.psvita.dev (routes = ...custom_domain = true): refusing"
grep -q '^workers_dev = false' "$CONFIG" || die "$CONFIG must set workers_dev = false (no workers.dev address: a WAF rule and the rate limit cannot sit in front of it): refusing"
URL=${URL:-$CUSTOM_URL}
[ "$URL" = "$CUSTOM_URL" ] || die "--url must be $CUSTOM_URL (not $URL)"
[[ "$ACCOUNT_ID" =~ ^[0-9a-fA-F]{32}$ ]] || die "--account-id <32 hex characters> is required (the Cloudflare account the service belongs to; wrong format: refusing)"
ACCOUNT_ID=$(printf '%s' "$ACCOUNT_ID" | tr A-F a-f)
export CLOUDFLARE_ACCOUNT_ID=$ACCOUNT_ID
{ [ -n "$KEYID_FILE" ] && [ -z "$TURNTOK_FILE" ]; } && die "--turn-key-id-file and --turn-token-file go together"
{ [ -z "$KEYID_FILE" ] && [ -n "$TURNTOK_FILE" ]; } && die "--turn-key-id-file and --turn-token-file go together"
SET_SECRETS=0
EXPECT_APP_VERSION=2.0.0   # the app version this service build is made for: /v1/info must report it as min and latest
[ -n "$KEYID_FILE" ] && SET_SECRETS=1
[ $SET_SECRETS -eq 1 ] && [ $VERIFY_ONLY -eq 1 ] && die "--verify-only sets no secrets: no secret options with it"
command -v curl >/dev/null || die "curl is needed"
command -v npx >/dev/null || die "npx (Node >= 22.5) is needed"

LOG_DIR=${LOG_DIR:-deploy-logs/$(date -u +%Y%m%dT%H%M%SZ)}
STEP=0
WR_LOG_DIRS=("${HOME:-/nonexistent}/.wrangler/logs" "./.wrangler/logs" "${XDG_CONFIG_HOME:-${HOME:-/nonexistent}/.config}/.wrangler/logs")
wr_log_files() { local d; for d in "${WR_LOG_DIRS[@]}"; do [ -d "$d" ] && find "$d" -type f 2>/dev/null; done | sort; return 0; }
WR_LOGS_BEFORE=""
note() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_DIR/summary.txt"; }
last4() { local v=$1; echo "****${v: -4}"; }

# Filter for everything wrangler (or a test run) prints before it reaches a file: e-mail addresses and account-name lines and account table
# rows (name | id) are removed, every run of 28+ hex digits is shown as its last 4 only.
scrub() {
  LC_ALL=C sed -E -e '/(\||│).*[0-9A-Fa-f]{32}/s/.*/[account table row removed]/' \
         -e '/[Aa]ccount [Nn]ame/s/.*/[account name line removed]/' \
         -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/[EMAIL]/g' \
         -e 's/[0-9A-Fa-f]{28,}([0-9A-Fa-f]{4})/[id ****\1]/g'
}

# THE ONLY place wrangler is run (the self-check greps for any other). Always: CLOUDFLARE_ACCOUNT_ID (exported above) and, except for whoami,
# --config wrangler.toml. $1 = a name for the log file, the rest = wrangler arguments. With PRINT_PLAN it only prints the command.
# whoami: output is NEVER stored or printed; only its exit code and the 32-hex account ids are used (WHOAMI_IDS).
# A secret put reads the value from stdin (the caller pipes it in); the output of every call goes through scrub() into the log; any
# workers.dev URL or *.psvita.dev name it contains is left in WR_URLS / WR_HOSTS (in memory) for the address check.
wr() {
  local name=$1; shift
  local cfg=(--config "$CONFIG")
  [ "$name" = whoami ] && cfg=()
  if [ $PRINT_PLAN -eq 1 ]; then echo "   CLOUDFLARE_ACCOUNT_ID=$(last4 "$ACCOUNT_ID") npx wrangler $* ${cfg[*]-}"; return 0; fi
  STEP=$((STEP + 1))
  local out="$LOG_DIR/$(printf %02d $STEP)-wrangler-$name.txt" raw rc=0
  WR_URLS=""; WR_HOSTS=""
  if [ "$name" = whoami ]; then
    raw=$(npx wrangler "$@" </dev/null 2>&1) || rc=$?
    if [ $rc -ne 0 ]; then raw=""; echo "FAILED: wrangler whoami exit code $rc (not logged in?)" > "$out"; note "wrangler whoami: FAILED ($out)"; return 1; fi
    WHOAMI_IDS=$(printf '%s\n' "$raw" | grep -oiE '[0-9a-f]{32}' | tr A-F a-f | sort -u || true)
    raw=""
    echo OK > "$out"; note "wrangler whoami: ok (output not stored)"; return 0
  fi
  echo "\$ npx wrangler $* ${cfg[*]}" > "$out"
  case "$name" in
    secret-*) raw=$(npx wrangler "$@" "${cfg[@]}" 2>&1) || rc=$? ;;   # (stdin is the caller's pipe: the secret value)
    *) raw=$(npx wrangler "$@" "${cfg[@]}" </dev/null 2>&1) || rc=$? ;;
  esac
  printf '%s\n' "$raw" | scrub >> "$out"
  WR_URLS=$(printf '%s\n' "$raw" | grep -oE 'https://[A-Za-z0-9.-]+\.workers\.dev' | sort -u || true)
  WR_HOSTS=$(printf '%s\n' "$raw" | grep -oE '[A-Za-z0-9-]+\.psvita\.dev' | tr A-Z a-z | sort -u || true)
  raw=""
  if [ $rc -eq 0 ]; then note "wrangler $name: ok ($out)"; return 0; fi
  note "wrangler $name: FAILED ($out)"; return 1
}

# wrangler must report NO workers.dev address (workers_dev = false) and only the name vitadhoc.psvita.dev among the psvita.dev names.
# $1 = dry | deploy. After the real deploy the custom domain must be among the reported names.
check_urls() {
  local u n=0 bad="" h
  for u in $WR_URLS; do n=$((n + 1)); bad="$bad $u"; done
  for h in $WR_HOSTS; do n=$((n + 1)); [ "$h" = "$HOSTNAME_OK" ] || bad="$bad $h"; done
  if [ -n "$bad" ]; then
    if [ "$1" = dry ]; then die "the dry run reports$bad, not only $HOSTNAME_OK: NOTHING WAS DEPLOYED"; fi
    note "!!! WRONG ADDRESS: the deploy reported$bad, expected only $HOSTNAME_OK. The deploy has already happened. Stop; do not set secrets; tell the owner."
    die "wrong address after the deploy"
  fi
  if [ "$1" = dry ] && [ $n -eq 0 ]; then note "the dry run reported no address (it does not always); the addresses are checked on the real deploy"; return 0; fi
  if [ "$1" = deploy ] && ! printf '%s\n' "$WR_HOSTS" | grep -qx "$HOSTNAME_OK"; then
    note "!!! the deploy did not report the custom domain $HOSTNAME_OK: cannot confirm it. Stop and check by hand (zone psvita.dev active on this account? an old DNS record named vitadhoc?)."
    die "custom domain not confirmed after the deploy"
  fi
  note "wrangler reported only the expected address${WR_HOSTS:+ (custom domain $WR_HOSTS)}"
}

# One HTTP request; its reply (status, headers, body) is kept under one step number with the request line. Sets CODE (000 = no answer)
# and LAST (the path of the kept files without extension). Not called in $(...): the step counter must survive.
BASE=$CUSTOM_URL
req() { # $1 name, $2 path (with query), $3 optional method
  STEP=$((STEP + 1))
  local base="$LOG_DIR/$(printf %02d $STEP)-$1" m=${3:-GET}
  echo "$m $2" > "$base.request"
  CODE=$(curl -q -sS -X "$m" -o "$base.body" -D "$base.headers" -w '%{http_code}' --max-time 20 -H 'User-Agent: VitAdhoc-deploy-check/2.0' "$BASE$2" 2> "$base.curl-error") || true
  [[ "$CODE" =~ ^[0-9]{3}$ ]] || CODE=000
  echo "$CODE" > "$base.status"
  LAST=$base
}

if [ $PRINT_PLAN -eq 1 ]; then
  echo "PLAN (nothing is contacted, nothing is written, no token file is opened). The account id is shown as its last 4 characters."
  echo "account pin: CLOUDFLARE_ACCOUNT_ID=$(last4 "$ACCOUNT_ID") exported for every wrangler call; the service: $CUSTOM_URL (workers.dev is off)"
  echo "1. login check (only the exit code and the account ids are used; the output is never stored or shown); refuse unless id $(last4 "$ACCOUNT_ID") is listed:"
  wr whoami whoami
  if [ $VERIFY_ONLY -eq 0 ]; then
    [ $SKIP_TESTS -eq 0 ] && echo "2. local suite: node --test \"test/*.test.mjs\"   (contacts nothing)"
    echo "2b. DNS guard: if the Worker is not deployed yet (wrangler deployments list fails) and $HOSTNAME_OK already resolves, stop before the dry run"
    echo "    (a custom-domain publish can overwrite an existing DNS record silently); --allow-existing-dns overrides after the owner has looked"
    wr deployments deployments list
    echo "3. dry run; wrangler must report no workers.dev address and no psvita.dev name but $HOSTNAME_OK, else stop before the deploy:"
    wr dry-run deploy --dry-run --outdir "<log-dir>/bundle"
    echo "4. the deploy; the addresses it reports must be as above and include the custom domain $HOSTNAME_OK, else stop at once:"
    wr deploy deploy
    if [ $SET_SECRETS -eq 1 ]; then
      echo "5. set the two relay secrets; each value goes on stdin from its file (never on a command line, never printed):"
      wr secret-put-keyid secret put TURN_KEY_ID
      wr secret-put-token secret put TURN_KEY_API_TOKEN
    fi
  else
    echo "2. verify-only: no deploy, no secret"
  fi
  echo "6. live checks, each reply kept (status, headers, body) with its request, on $CUSTOM_URL only:"
  echo "   curl $CUSTOM_URL/v1/info                    expect 200 with \"service\":\"vitadhoc\" (retried while there is no answer at all: a new custom domain)"
  echo "   curl $CUSTOM_URL/v1/rooms/ZZZZZ?v=2.0.0       expect 404 not_found (the object and the code path are alive)"
  echo "   curl $CUSTOM_URL/                            expect 403 (the zone's WAF rule) or 404 (the Worker): never 200"
  echo "   curl $CUSTOM_URL/v1/info?v=2.0.0             \"min\":\"$EXPECT_APP_VERSION\", \"latest\":\"$EXPECT_APP_VERSION\" and \"relay\":true (a deploy run without secrets only reports relay false)"
  exit 0
fi

mkdir -p "$LOG_DIR"
SECRET_FAIL=0
if [ $SET_SECRETS -eq 1 ]; then
  [ -r "$KEYID_FILE" ] || die "cannot read the TURN key id file"
  [ -r "$TURNTOK_FILE" ] || die "cannot read the TURN token file"
  IFS= read -r KEYID < "$KEYID_FILE" || true
  IFS= read -r TURNTOK < "$TURNTOK_FILE" || true
  [[ "$KEYID" =~ ^[A-Za-z0-9._-]{8,64}$ ]] || die "the TURN key id file does not hold a plausible key id (8 to 64 letters, digits, . _ -)"
  [[ "$TURNTOK" =~ ^[A-Za-z0-9._~+/=-]{16,}$ ]] || die "the TURN token file does not hold a plausible API token (at least 16 characters of letters, digits and . _ ~ + / = -)"
fi

note "vitadhoc-online at $CUSTOM_URL; account $(last4 "$ACCOUNT_ID"); mode: $([ $VERIFY_ONLY -eq 1 ] && echo verify-only || echo deploy); replies kept in $LOG_DIR"

# ---------------------------------------------------------------- 1. local checks
WR_LOGS_BEFORE=$(wr_log_files)
wr whoami whoami || die "not logged in to Cloudflare (run the wrangler login first)"
if ! printf '%s\n' "$WHOAMI_IDS" | grep -qx "$ACCOUNT_ID"; then
  echo "FAILED: the logged-in login does not list account id $(last4 "$ACCOUNT_ID")" > "$(ls "$LOG_DIR"/*-wrangler-whoami.txt | head -1)"
  die "wrangler whoami does not list account id $(last4 "$ACCOUNT_ID"): refusing to act on another account"
fi
note "account $(last4 "$ACCOUNT_ID") is listed by wrangler whoami"
if [ $SKIP_TESTS -eq 0 ] && [ $VERIFY_ONLY -eq 0 ]; then
  trc=0; traw=$(node --test "test/*.test.mjs" 2>&1) || trc=$?
  printf '%s\n' "$traw" | scrub > "$LOG_DIR/00-tests.txt"; traw=""
  if [ $trc -eq 0 ]; then note "tests: ok ($(grep -E '^# (pass|fail)' "$LOG_DIR/00-tests.txt" | tr '\n' ' '))"
  else note "tests: FAILED ($LOG_DIR/00-tests.txt)"; die "the test suite failed: nothing deployed"; fi
fi

# ---------------------------------------------------------------- 2. deploy
# A Worker custom-domain publish creates the DNS record of the hostname and, if one of that name exists already, may take it over without asking.
# A Worker that is already deployed owns its record (a re-deploy); for the FIRST deploy the name must not resolve yet. getent / host / dig
# answer; with none of them the guard cannot look and says so.
dns_resolves() { # 0 = resolves, 1 = does not, 2 = cannot tell
  if command -v getent >/dev/null; then getent hosts "$HOSTNAME_OK" >/dev/null 2>&1 && return 0 || return 1; fi
  if command -v dig >/dev/null; then [ -n "$(dig +short "$HOSTNAME_OK" 2>/dev/null)" ] && return 0 || return 1; fi
  if command -v host >/dev/null; then host "$HOSTNAME_OK" >/dev/null 2>&1 && return 0 || return 1; fi
  return 2
}
dns_guard() {
  local rc=0
  if wr deployments deployments list; then note "DNS guard: the Worker is already deployed, so $HOSTNAME_OK is its own record"; return 0; fi
  dns_resolves || rc=$?
  case $rc in
    0) if [ $ALLOW_DNS -eq 1 ]; then note "DNS guard: $HOSTNAME_OK already resolves; --allow-existing-dns given: the deploy may replace that record"; return 0; fi
       die "first deploy, but $HOSTNAME_OK already resolves: a custom-domain publish could overwrite that DNS record. Nothing was deployed. Look at the zone's DNS records for the name vitadhoc; remove it, or pass --allow-existing-dns" ;;
    1) note "DNS guard: first deploy and $HOSTNAME_OK does not resolve yet: the deploy will create it" ;;
    *) note "DNS guard: cannot look ($HOSTNAME_OK: no getent, dig or host here). Check the zone's DNS records for the name vitadhoc by hand before going on" ;;
  esac
}
if [ $VERIFY_ONLY -eq 0 ]; then
  dns_guard
  wr dry-run deploy --dry-run --outdir "$LOG_DIR/bundle" || die "the bundle does not build: nothing deployed"
  check_urls dry
  wr deploy deploy || die "wrangler deploy failed (see the log above); check with --verify-only before trying again"
  check_urls deploy
  if [ $SET_SECRETS -eq 1 ]; then
    printf '%s' "$KEYID" | wr secret-put-keyid secret put TURN_KEY_ID || { note "secret TURN_KEY_ID: FAILED"; SECRET_FAIL=1; }
    printf '%s' "$TURNTOK" | wr secret-put-token secret put TURN_KEY_API_TOKEN || { note "secret TURN_KEY_API_TOKEN: FAILED"; SECRET_FAIL=1; }
  fi
  KEYID=""; TURNTOK=""
fi

# ---------------------------------------------------------------- 3. checks against the live service
FAIL=$SECRET_FAIL
req info "/v1/info"
if [ "$CODE" = 000 ]; then
  # first deploy: the custom domain may not resolve or have its certificate yet (a zone registered today: minutes). Retry ONLY while there is no
  # answer at all (DNS / TLS failure); any answer, right or wrong, ends the wait.
  cstart=$(date +%s); ctries=1
  note "$BASE: no answer yet (a new custom domain needs DNS and a certificate: expected on the first deploy); asking again every 10 s for up to ${CUSTOM_WAIT_SEC} s"
  while [ "$CODE" = 000 ] && [ $(( $(date +%s) - cstart )) -lt "$CUSTOM_WAIT_SEC" ]; do
    sleep 10; ctries=$((ctries + 1)); req "info-try$ctries" "/v1/info"
  done
  note "$BASE: ${ctries} tries for /v1/info, last answer $CODE"
fi
if [ "$CODE" = 200 ] && grep -q '"service":"vitadhoc"' "$LAST.body"; then note "GET /v1/info: 200, service vitadhoc"
else note "GET /v1/info: $CODE (expected 200 with service vitadhoc; see $LAST.*)"; FAIL=1; fi
if [ "$CODE" = 200 ]; then
  # What this build of the service must say: the app version it is made for (wrangler.toml MIN_APP_VERSION / LATEST_APP_VERSION) and a working relay.
  INFO_MIN=$(grep -oE '"min":"[^"]*"' "$LAST.body" | head -1 | cut -d'"' -f4 || true)
  INFO_LATEST=$(grep -oE '"latest":"[^"]*"' "$LAST.body" | head -1 | cut -d'"' -f4 || true)
  if [ "$INFO_MIN" = "$EXPECT_APP_VERSION" ] && [ "$INFO_LATEST" = "$EXPECT_APP_VERSION" ]; then note "/v1/info: min $INFO_MIN, latest $INFO_LATEST, as expected"
  else note "/v1/info: min '${INFO_MIN:-?}', latest '${INFO_LATEST:-?}', expected both $EXPECT_APP_VERSION (MIN_APP_VERSION / LATEST_APP_VERSION of the deployed config; see $LAST.*)"; FAIL=1; fi
  if grep -q '"relay":true' "$LAST.body"; then note "relay credentials: configured (relay true)"
  elif [ $VERIFY_ONLY -eq 0 ] && [ $SET_SECRETS -eq 0 ]; then note "relay credentials: NOT configured yet (relay false): route 6 answers 503 relay_unavailable until the TURN secrets are set (this run was told to leave them)"
  else note "relay credentials: relay is not true in /v1/info (NOT configured yet?): route 6 answers 503 relay_unavailable; set the TURN secrets (see $LAST.*)"; FAIL=1; fi
fi
req lookup "/v1/rooms/ZZZZZ?v=2.0.0"
if [ "$CODE" = 404 ] && grep -q '"code":"not_found"' "$LAST.body"; then note "GET /v1/rooms/ZZZZZ: 404 not_found (the Worker and the object answer)"
else note "GET /v1/rooms/ZZZZZ: $CODE (expected 404 not_found; see $LAST.*)"; FAIL=1; fi
req root "/"
if [ "$CODE" = 403 ] || [ "$CODE" = 404 ]; then note "GET /: $CODE (not served, as intended)"
else note "GET /: $CODE (expected 403 from the zone's WAF rule or 404 from the Worker; see $LAST.*)"; FAIL=1; fi

# no wrangler debug log may have been written by this run (it would hold account details)
WR_LOGS_AFTER=$(wr_log_files)
if [ "$WR_LOGS_AFTER" != "$WR_LOGS_BEFORE" ]; then
  newlogs=$(comm -13 <(printf '%s\n' "$WR_LOGS_BEFORE") <(printf '%s\n' "$WR_LOGS_AFTER") | sed '/^$/d' | wc -l | tr -d ' ')
  note "wrangler wrote $newlogs debug log file(s) although WRANGLER_WRITE_LOGS=false: they may hold account details: look in ~/.wrangler/logs and ./.wrangler/logs and remove them"
  FAIL=1
fi
if [ $FAIL -eq 0 ]; then note "ALL CHECKS PASSED"; else note "SOME CHECKS FAILED: read $LOG_DIR/summary.txt and the kept replies"; fi
exit $FAIL
