#!/bin/bash
# Sets up the ONE security rule of the Cloudflare zone psvita.dev that belongs to the VitAdhoc online service https://vitadhoc.psvita.dev.
# THE ZONE IS SHARED: Galaxy and Strikers live on psvita.dev too, each on its own host and its own Worker. So this script
#   - manages ONE WAF custom rule, ref vitadhoc-psvita-block-unserved, always scoped to the host vitadhoc.psvita.dev, so it can never touch
#     another host: it blocks every path the Worker does not serve;
#   - NEVER creates, patches or deletes a rate-limiting rule. The Free plan has ONE rate-limiting rule for the whole zone and it is shared
#     (ref psvita-shared-ratelimit, 100 requests / 10 s per IP on /v1/*, set up by the Galaxy script): this script only READS the
#     http_ratelimit phase and stops with a clear message when that rule is missing or does not cover /v1/*.
# Same style and the same safety rules as deploy-vitadhoc-online.sh. Run AFTER the deploy (the custom domain exists). Nothing here touches the Worker.
# NEVER RUN BY AN AGENT: the owner (or the owner's Codex) runs it; the tests only run --print-plan and --self-check.
#
#   scripts/setup-vitadhoc-zone.sh --account-id <32 hex> --token-file ~/.cf-psvita-token [options]
#
#   --account-id ID        REQUIRED. The zone must belong to this account (32 hex characters; shown only as its last 4).
#   --token-file FILE      REQUIRED. The Cloudflare API token (one line). Read from the file, handed to curl on stdin (`curl -K -`), never printed,
#                          logged, written to a file or put on a command line. Needs: Zone > Zone > Read and Zone > Zone WAF > Edit, restricted to the
#                          zone psvita.dev (optional: a read permission for Bot Fight Mode, only for the --verify-only check). Nothing account-wide.
#   --print-plan           print the rule this script would manage and the calls it would make (nothing is contacted, the token file is not opened), exit 0
#   --self-check           check that curl is only called inside api() and the token is only used where it may be, then exit
#   --verify-only          read only: check that our rule exists exactly as planned, that the shared rate-limiting rule covers /v1/*, and (if the
#                          token may read it) that Bot Fight Mode is OFF; exit 1 if not
#   --remove               rollback: delete OUR WAF rule (found by its ref) and nothing else; other rules are never touched
#   --log-dir DIR          where the (summarised, never raw) replies are kept (default zone-logs/<UTC time>)
#
# What it manages (and ONLY this; the rule is found by its fixed `ref`, other rules of the phase are read and kept as they are):
#   phase http_request_firewall_custom   ref vitadhoc-psvita-block-unserved   Block every path on vitadhoc.psvita.dev (that host ONLY) the Worker does not serve
# What it only reads:
#   phase http_ratelimit                 ref psvita-shared-ratelimit          must exist and cover /v1/* (else: stop, nothing is written)
# Idempotent: a second run reads the rule, finds it equal and writes nothing. Before any write it prints the exact plan. It stops if the zone already
# has 5 custom rules and none of them is ours (the Free plan's limit; nothing is dropped).
set -euo pipefail
umask 077

ZONE_NAME=psvita.dev
HOST=vitadhoc.psvita.dev
API=https://api.cloudflare.com/client/v4
WAF_PHASE=http_request_firewall_custom
RL_PHASE=http_ratelimit
WAF_REF=vitadhoc-psvita-block-unserved
RL_REF=psvita-shared-ratelimit
WAF_MAX=5      # Free plan: 5 custom rules
ACCOUNT_ID=""
TOKEN_FILE=""
PRINT_PLAN=0
SELF_CHECK=0
VERIFY_ONLY=0
REMOVE=0
LOG_DIR=""
die() { echo "setup-vitadhoc-zone: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --account-id) ACCOUNT_ID=${2:-}; shift 2 ;;
    --token-file) TOKEN_FILE=${2:-}; shift 2 ;;
    --print-plan) PRINT_PLAN=1; shift ;;
    --self-check) SELF_CHECK=1; shift ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    --remove) REMOVE=1; shift ;;
    --log-dir) LOG_DIR=${2:-}; shift 2 ;;
    -h|--help) awk 'NR>1 { if ($0 ~ /^set -/) exit; print }' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# Self-check: curl runs only inside api(); the token variable appears only inside api() and on the lines that read and check it (marked tok-ok);
# nothing prints it. (api() is the function whose first line starts with "api()" and ends at the next line starting with "}".)
self_check() {
  awk 'BEGIN { c = "(^|[^A-Za-z0-9_./-])cur" "l( |$)"; t = "[$]TOK([^A-Za-z_]|$)|[$][{]TOK" }
       /^self_check\(\) *\{/ { skip = 1 }
       /^api\(\) *\{/ { inapi = 1 }
       { if (!skip && !inapi && $0 !~ /^[ \t]*#/ && $0 !~ /(die|note|echo) / && $0 ~ c) { print FILENAME ":" NR ": curl outside api(): " $0; bad = 1 }
         if (!skip && !inapi && $0 !~ /^[ \t]*#/ && $0 ~ t && $0 !~ /# tok-ok/) { print FILENAME ":" NR ": token used outside api(): " $0; bad = 1 } }
       /^\}/ { inapi = 0; skip = 0 }
       END { exit bad }' "$0"
}
if [ $SELF_CHECK -eq 1 ]; then
  self_check || die "curl or the token is used outside api() (lines above)"
  echo "self-check ok: curl runs only inside api(), the token is used only there"; exit 0
fi

[[ "$ACCOUNT_ID" =~ ^[0-9a-fA-F]{32}$ ]] || die "--account-id <32 hex characters> is required"
ACCOUNT_ID=$(printf '%s' "$ACCOUNT_ID" | tr A-F a-f)
[ -n "$TOKEN_FILE" ] || die "--token-file is required (the file holds the API token; it is never printed)"
[ $VERIFY_ONLY -eq 1 ] && [ $REMOVE -eq 1 ] && die "--verify-only and --remove exclude each other"
last4() { local v=$1; echo "****${v: -4}"; }

# ---------------------------------------------------------------- the rule we manage (compact JSON)
# Every path the Worker does not serve: the Worker serves /v1/* only (the .well-known paths are Cloudflare's certificate validation).
WAF_EXPR="(http.host eq \"$HOST\" and not starts_with(http.request.uri.path, \"/v1/\") and not starts_with(http.request.uri.path, \"/.well-known/acme-challenge/\") and not starts_with(http.request.uri.path, \"/.well-known/pki-validation/\"))"
mkjson() { # $1 ref  $2 description  $3 expression
  node -e 'const [ref,d,e]=process.argv.slice(1); process.stdout.write(JSON.stringify({ref,description:d,expression:e,action:"block",enabled:true}));' "$1" "$2" "$3"
}
WAF_RULE=$(mkjson "$WAF_REF" "vitadhoc-online ($HOST): block every path the Worker does not serve [v1]" "$WAF_EXPR")

print_rules() {
  echo "  WAF custom rule   (phase $WAF_PHASE, ref $WAF_REF), action block:"
  echo "      $WAF_EXPR"
  echo "  read only         (phase $RL_PHASE, ref $RL_REF): must exist and its expression must cover /v1/* (starts_with(http.request.uri.path, \"/v1/\")); this script never writes it"
}

if [ $PRINT_PLAN -eq 1 ]; then
  echo "PLAN (nothing is contacted, the token file is not opened). The account id is shown as its last 4 characters: $(last4 "$ACCOUNT_ID")"
  echo "1. GET $API/zones?name=$ZONE_NAME&account.id=<account>   (exactly one active zone, owned by the account)"
  echo "2. GET the entrypoints of $WAF_PHASE and $RL_PHASE of that zone (a missing entrypoint is an empty phase); other rules are kept as they are"
  echo "3. stop unless the shared rate-limiting rule $RL_REF exists and covers /v1/* (no rate-limiting rule is ever created, changed or deleted here)"
  echo "4. print the exact plan; stop here with --verify-only (it also looks at Bot Fight Mode: it must be OFF; this script never changes it)"
  if [ $REMOVE -eq 1 ]; then
    echo "5. --remove: DELETE only our WAF rule ($WAF_REF), when present"
  else
    echo "5. WAF phase: no entrypoint -> create it (POST /rulesets) with our one rule; entrypoint without our rule -> POST our rule to it (stop if it already holds $WAF_MAX rules); our rule differs -> PATCH it; equal -> nothing"
    echo "6. read the phases again and check our rule is there exactly as planned"
  fi
  echo "The rules:"; print_rules
  exit 0
fi

command -v curl >/dev/null || die "curl is needed"
command -v node >/dev/null || die "node is needed (JSON)"
if [ ! -r "$TOKEN_FILE" ]; then
  echo "setup-vitadhoc-zone: cannot read the token file (~/.cf-psvita-token). Nothing was contacted. To create it (the owner, once, in the dashboard):" >&2
  echo "  1. My Profile > API Tokens > Create Token > Create Custom Token." >&2
  echo "  2. Permissions: Zone > Zone > Read; Zone > Zone WAF > Edit. (Optional: Zone > Bot Management > Read, only to let --verify-only read Bot Fight Mode.)" >&2
  echo "  3. Zone Resources: Include > Specific zone > psvita.dev ONLY (no account-wide permission, nothing for any other zone)." >&2
  echo "  4. Create, copy the token once, save it as one line in ~/.cf-psvita-token (chmod 600). Then run this script again." >&2
  exit 1
fi
IFS= read -r TOK < "$TOKEN_FILE" || true   # tok-ok
[ ${#TOK} -ge 30 ] || die "the token file is shorter than 30 characters"   # tok-ok
[[ "$TOK" =~ ^[A-Za-z0-9._~+/=-]+$ ]] || die "the token file holds characters other than letters, digits and . _ ~ + / = -"   # tok-ok
LOG_DIR=${LOG_DIR:-zone-logs/$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$LOG_DIR"
STEP=0
note() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_DIR/summary.txt"; }

# Same filter as deploy-vitadhoc-online.sh: e-mail addresses and account-name lines go, every run of 32+ hex digits is shown as its last 4 only.
scrub() {
  LC_ALL=C sed -E -e '/[Aa]ccount [Nn]ame/s/.*/[account name line removed]/' \
         -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/[EMAIL]/g' \
         -e 's/[0-9A-Fa-f]{28,}([0-9A-Fa-f]{4})/[id ****\1]/g'
}

# THE ONLY place curl is run. $1 name, $2 method, $3 path (under /client/v4), $4 optional JSON body. Sets CODE (000 = no answer) and BODY (the raw reply,
# kept in memory only: it can hold the account id, name and e-mail; only summaries of it are ever printed or written, see errs() / jq-free node helpers).
# The token and the body reach curl as config lines on stdin (`curl -K -`), made by the printf builtin: never on a command line or in a file.
api() {
  STEP=$((STEP + 1))
  local base="$LOG_DIR/$(printf %02d $STEP)-$1" raw esc cfg
  echo "$2 $3" | scrub > "$base.request"
  cfg=$(printf 'header = "Authorization: Bearer %s"\nrequest = "%s"\nheader = "User-Agent: vitadhoc-zone-setup"\nurl = "%s%s"' "$TOK" "$2" "$API" "$3")
  if [ $# -ge 4 ]; then
    esc=$(printf '%s' "$4" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')
    cfg="$cfg"$(printf '\nheader = "Content-Type: application/json"\ndata-binary = "%s"' "$esc")
  fi
  raw=$(printf '%s\n' "$cfg" | curl -q -sS --max-time 30 -w '\n%{http_code}' -K - 2> "$base.curl-error") || true
  cfg=""
  CODE=${raw##*$'\n'}
  [[ "$CODE" =~ ^[0-9]{3}$ ]] || CODE=000
  BODY=${raw%$'\n'*}
  raw=""
  echo "$CODE" > "$base.status"
  LAST=$base
}

# summary of the errors of a reply: "code: message" lines (scrubbed)
errs() { printf '%s' "$BODY" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{const j=JSON.parse(s);for(const e of (j.errors||[]))console.log(e.code+": "+e.message);}catch{console.log("(no JSON in the reply)")}})' | scrub; }
# classify a refusal. 401 = the token is not valid or lacks the permission. 403 = a missing permission too, UNLESS the API's own message speaks of the
# plan / entitlement / an unavailable feature: then it is a refusal by the plan, not a permission problem. The API's message is always printed first.
perm_check() { # $1 what the call was, $2 the permission the token needs
  if [ "$CODE" = 401 ] || [ "$CODE" = 403 ]; then
    local m; m=$(errs | head -3 | tr '\n' ' ')
    note "$1: HTTP $CODE, the API says: $m"
    if [ "$CODE" = 403 ] && printf '%s' "$m" | grep -qiE 'plan|entitle|upgrade|not available|not allowed|not supported'; then
      note "!!! $1: refused by the PLAN or an entitlement, not (as far as the message shows) by a missing token permission. Nothing was changed by this call."
      die "refused by the plan: $1"
    fi
    note "!!! $1: the token lacks a permission or is not valid. Needed: $2."
    note "Add the permission to the token (dashboard > My Profile > API Tokens > the token > Edit), then run this script again; nothing was changed by this call."
    die "missing permission: $2"
  fi
}
# $1 = node expression over j (the parsed reply); prints its value
jv() { printf '%s' "$BODY" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{let j;try{j=JSON.parse(s)}catch{j={}};const r=('"$1"');process.stdout.write(r===undefined||r===null?"":String(r))})'; }

# ---------------------------------------------------------------- 1. the zone
api zone GET "/zones?name=$ZONE_NAME&account.id=$ACCOUNT_ID"
perm_check "zone lookup" "Zone > Zone > Read"
[ "$CODE" = 200 ] || { note "zone lookup: HTTP $CODE ($(errs | head -2 | tr '\n' ' ')); see $LAST.*"; die "zone lookup failed"; }
NZ=$(jv '(j.result||[]).length')
[ "$NZ" = 1 ] || die "expected exactly one zone named $ZONE_NAME in account $(last4 "$ACCOUNT_ID"), found ${NZ:-0}: is the domain added to this account?"
ZID=$(jv 'j.result[0].id')
ZSTATUS=$(jv 'j.result[0].status')
ZACC=$(jv 'j.result[0].account && j.result[0].account.id')
[[ "$ZID" =~ ^[0-9a-f]{32}$ ]] || die "the zone id has an unexpected shape"
[ "$ZACC" = "$ACCOUNT_ID" ] || die "the zone $ZONE_NAME does not belong to account $(last4 "$ACCOUNT_ID"): refusing"
[ "$ZSTATUS" = active ] || die "the zone $ZONE_NAME is not active (status: ${ZSTATUS:-unknown}): finish the domain's setup first"
note "zone $ZONE_NAME found: active, id $(last4 "$ZID"), belongs to account $(last4 "$ACCOUNT_ID")"


# ---------------------------------------------------------------- 2. read the two phases
# ---------------------------------------------------------------- 2. read the two phases
# state per phase: missing (no entrypoint yet) | absent (entrypoint, none of ours) | same | differs | ambiguous | foreign-full (no room for ours)
# RSET = ruleset id, RRULE = our rule's id, ROTHERS = number of other rules
read_phase() { # $1 phase $2 our ref $3 desired rule json $4 max rules
  api "read-$1" GET "/zones/$ZID/rulesets/phases/$1/entrypoint"
  perm_check "read of the $1 rules" "Zone > Zone WAF > Edit (or Read)"
  PSTATE=""; RSET=""; RRULE=""; ROTHERS=0; LIVE_EXPR=""
  if [ "$CODE" = 404 ]; then PSTATE=missing; return 0; fi
  [ "$CODE" = 200 ] || { note "read of $1: HTTP $CODE ($(errs | head -2 | tr '\n' ' ')); see $LAST.*"; die "cannot read the $1 rules"; }
  local out
  out=$(printf '%s' "$BODY" | node -e '
    const ref=process.argv[1], want=JSON.parse(process.argv[2]), max=+process.argv[3];
    let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{
      const j=JSON.parse(s), r=j.result||{}, rules=r.rules||[];
      // ours by ref, or by its description where the zone gave the rule an id of its own as ref (rules made in the dashboard or
      // by another script carry a hex ref; their description is the name the scripts agree on)
      const tag=String(want.description||"").split(" ")[0];
      const mine=rules.filter(x=>x.ref===ref||(tag&&String(x.description||"").split(" ")[0]===tag)), others=rules.length-mine.length;
      const norm=e=>String(e||"").replace(/\s+/g,"").replace(/^\((.*)\)$/,"$1");
      const pick=x=>JSON.stringify({d:x.description,e:norm(x.expression),a:x.action,on:x.enabled!==false,
        rl:x.ratelimit?{c:(x.ratelimit.characteristics||[]).slice().sort(),p:x.ratelimit.period,n:x.ratelimit.requests_per_period,m:x.ratelimit.mitigation_timeout}:null});
      const terms=e=>new Set([...String(e||"").matchAll(/starts_with\(http\.request\.uri\.path, *"([^"]*)"\)|http\.request\.uri\.path eq *"([^"]*)"/g)].map(m=>m[1]!==undefined?"sw:"+m[1]:"eq:"+m[2]));
      let st, live="";
      if(mine.length>1) st="ambiguous";
      else if(mine.length===1) {
        st=pick(mine[0])===pick(want)?"same":"differs";
        if(st==="differs"&&ref==="psvita-shared-ratelimit"){ live=String(mine[0].expression||"").replace(/\s+/g," "); const w=terms(want.expression); if([...terms(mine[0].expression)].some(t=>!w.has(t))) st="drops"; }
      }
      else st=rules.length>=max?"foreign-full":"absent";
      console.log([st,r.id||"-",mine.length?mine[0].id:"-",others].join(" "));
      console.log(live);
    });' "$2" "$3" "$4") || die "cannot read the reply of the $1 phase"
  read -r PSTATE RSET RRULE ROTHERS <<< "$(printf '%s\n' "$out" | head -1)"
  LIVE_EXPR=$(printf '%s\n' "$out" | sed -n 2p)
  [ "$RSET" = - ] && RSET=""; [ "$RRULE" = - ] && RRULE=""; return 0
}
read_phase "$WAF_PHASE" "$WAF_REF" "$WAF_RULE" "$WAF_MAX"; WAF_STATE=$PSTATE; WAF_SET=$RSET; WAF_RID=$RRULE; WAF_OTH=$ROTHERS

# The shared rate-limiting rule: READ ONLY. state: ok | missing | notv1 (exists but does not cover /v1/*)
api "read-$RL_PHASE" GET "/zones/$ZID/rulesets/phases/$RL_PHASE/entrypoint"
perm_check "read of the $RL_PHASE rules" "Zone > Zone WAF > Edit (or Read)"
RL_STATE=missing
if [ "$CODE" = 200 ]; then
  RL_STATE=$(printf '%s' "$BODY" | node -e '
    let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{
      let j;try{j=JSON.parse(s)}catch{j={}};
      // by ref, or by description: the live zone holds the shared rule under a hex ref with "psvita-shared-ratelimit" as its description
      const r=((j.result||{}).rules||[]).find(x=>x.ref===process.argv[1]||String(x.description||"").trim()===process.argv[1]);
      if(!r){console.log("missing");return}
      const covers=/starts_with\(\s*http\.request\.uri\.path\s*,\s*"\/v1\/"\s*\)/.test(String(r.expression||""))&&r.enabled!==false;
      console.log(covers?"ok":"notv1");
    });' "$RL_REF") || RL_STATE=missing
elif [ "$CODE" != 404 ]; then
  note "read of $RL_PHASE: HTTP $CODE ($(errs | head -2 | tr '\n' ' ')); see $LAST.*"; die "cannot read the $RL_PHASE rules"
fi
case "$RL_STATE" in
  ok) note "shared rate-limiting rule $RL_REF: present and covers /v1/* (read only)" ;;
  missing) RL_MSG="the shared rate-limiting rule $RL_REF does not exist on $ZONE_NAME" ;;
  notv1) RL_MSG="the shared rate-limiting rule $RL_REF exists but does not cover /v1/*" ;;
esac
RL_FAIL=0
if [ "$RL_STATE" != ok ]; then
  note "!!! $RL_MSG. VitAdhoc relies on it (100 requests / 10 s per IP on /v1/*) and this script never creates or changes a rate-limiting rule. Run the Galaxy zone script (it owns the shared rule) first, then run this one again."
  RL_FAIL=1
  [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ] && die "shared rate-limiting rule missing or not covering /v1/*: nothing was written"
fi

describe() { # state, others
  case "$1" in
    missing)      echo "WAF custom rule: no rules in this phase yet -> CREATE the phase's entrypoint with our rule" ;;
    absent)       echo "WAF custom rule: $2 other rule(s) there, kept as they are -> ADD our rule" ;;
    same)         echo "WAF custom rule: our rule is there exactly as planned -> UNCHANGED ($2 other rule(s) kept)" ;;
    differs)      echo "WAF custom rule: our rule is there but differs -> UPDATE it ($2 other rule(s) kept)" ;;
    ambiguous)    echo "WAF custom rule: MORE THAN ONE rule with our ref -> STOP" ;;
    foreign-full) echo "WAF custom rule: the phase already holds $2 rules, none ours, and the Free plan allows $WAF_MAX -> STOP (nothing is dropped)" ;;
  esac
}
note "the plan (nothing has been written yet):"
if [ $REMOVE -eq 1 ]; then
  note "  --remove: delete only our WAF rule ($WAF_PHASE): $([ -n "$WAF_RID" ] && echo "present -> DELETE" || echo "not there -> nothing"). The shared rate-limiting rule is not ours and is never touched"
else
  note "  $(describe "$WAF_STATE" "$WAF_OTH")"
  print_rules | sed 's/^/  /' | tee -a "$LOG_DIR/summary.txt"
fi

# Bot Fight Mode: read only, if the token may read it. It must be OFF (it cannot be skipped by rules and may challenge API clients).
BOT=unknown
api bot GET "/zones/$ZID/bot_management"
if [ "$CODE" = 200 ]; then
  case "$(jv 'j.result && j.result.fight_mode')" in true) BOT=ON ;; false) BOT=off ;; esac
fi

# Everything that can be checked is checked BEFORE the first write: Bot Fight Mode ON would challenge the game's API calls whatever our rule says, so
# adding the rule and only then reporting it would leave a half-done zone. (A rollback with --remove is always allowed.) An unreadable state
# (the token has no Bot Management read permission) is only a note here: it is checked by hand.
if [ "$BOT" = ON ] && [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ]; then
  note "!!! Bot Fight Mode is ON for $ZONE_NAME: it may challenge the game's API calls and rules cannot skip it. Turn it off (Security > Settings > Bot traffic) by hand, then run this script again. Nothing was written (this script never changes Bot Fight Mode)."
  die "Bot Fight Mode is ON: nothing was written"
fi

FAIL=0
if [ $VERIFY_ONLY -eq 1 ]; then
  if [ $REMOVE -eq 0 ]; then
    [ "$WAF_STATE" = same ] && note "WAF custom rule: as planned" || { note "WAF custom rule: NOT as planned ($WAF_STATE)"; FAIL=1; }
    [ "$RL_FAIL" -eq 0 ] || FAIL=1
  fi
  case "$BOT" in
    ON) note "!!! Bot Fight Mode is ON for $ZONE_NAME: it may challenge the game's API calls. Turn it off (Security > Settings > Bot traffic) by hand; this script never changes it."; FAIL=1 ;;
    off) note "Bot Fight Mode: off, as required" ;;
    *) note "Bot Fight Mode: could not be read with this token (needs a Bot Management read permission): check by hand that it is OFF" ;;
  esac
  [ $FAIL -eq 0 ] && note "ALL CHECKS PASSED" || note "SOME CHECKS FAILED: read $LOG_DIR/summary.txt"
  exit $FAIL
fi

# ---------------------------------------------------------------- 3. write (only what the plan says)
write_check() { # $1 name  (after a write call)
  case "$CODE" in
    200) note "$1: done" ;;
    *) note "$1: HTTP $CODE, the API says: $(errs | head -3 | tr '\n' ' ')(see $LAST.*)"
       perm_check "$1" "Zone > Zone WAF > Edit"
       note "(a refusal of the expression or the rule fields means this plan does not fit the zone's plan: nothing was widened or changed by this script; read the message)"
       die "$1 failed" ;;
  esac
}
if [ $REMOVE -eq 1 ]; then
  [ -n "$WAF_RID" ] && { api del-waf DELETE "/zones/$ZID/rulesets/$WAF_SET/rules/$WAF_RID"; write_check "delete of the WAF rule"; }
else
  case "$WAF_STATE" in ambiguous|foreign-full) die "$(describe "$WAF_STATE" "$WAF_OTH")" ;; esac
  case "$WAF_STATE" in
    missing) api new-waf POST "/zones/$ZID/rulesets" "{\"name\":\"vitadhoc-online $WAF_PHASE\",\"kind\":\"zone\",\"phase\":\"$WAF_PHASE\",\"rules\":[$WAF_RULE]}"; write_check "creating the WAF phase entrypoint with our rule" ;;
    absent)  api add-waf POST "/zones/$ZID/rulesets/$WAF_SET/rules" "$WAF_RULE"; write_check "adding the WAF rule" ;;
    differs) api upd-waf PATCH "/zones/$ZID/rulesets/$WAF_SET/rules/$WAF_RID" "$WAF_RULE"; write_check "updating the WAF rule" ;;
    same)    note "WAF rule: unchanged" ;;
  esac
fi

# ---------------------------------------------------------------- 4. read again and confirm
read_phase "$WAF_PHASE" "$WAF_REF" "$WAF_RULE" "$WAF_MAX"; W2=$PSTATE; WR2=$RRULE
if [ $REMOVE -eq 1 ]; then
  if [ -z "$WR2" ]; then note "our rule is gone; other rules were not touched"; else note "our rule is still there"; FAIL=1; fi
else
  [ "$W2" = same ] && note "WAF custom rule: confirmed as planned" || { note "WAF custom rule after the write: $W2 (expected same)"; FAIL=1; }
fi
[ "$BOT" = ON ] && { note "!!! Bot Fight Mode is ON for $ZONE_NAME (this script never changes it): turn it off by hand, it may challenge the game's API calls."; FAIL=1; }
[ $FAIL -eq 0 ] && note "ALL CHECKS PASSED" || note "SOME CHECKS FAILED: read $LOG_DIR/summary.txt"
exit $FAIL
