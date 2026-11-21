#!/bin/bash
# Creates the Cloudflare Realtime TURN key of the VitAdhoc online service and writes its two values to two private files, which
# deploy-vitadhoc-online.sh then reads (--turn-key-id-file / --turn-token-file) and sets as the Worker's two secrets over stdin.
# NEVER RUN BY AN AGENT: the owner (or the owner's Codex) runs it. The tests run only --print-plan and --self-check, which contact nothing.
#
#   scripts/create-turn-key.sh --account-id <32 hex> --token-file ~/.cf-vad-calls-token \
#                              --key-id-file ~/.vad-turn-key-id --turn-token-file ~/.vad-turn-token [--name NAME]
#
#   --account-id ID        REQUIRED. The account the key is created in (32 hex characters; shown only as its last 4).
#   --token-file FILE      REQUIRED. A Cloudflare API token, one line, with the account permission "Calls Write" (dashboard: Account > Calls (Realtime) >
#                          Edit), for THIS account only. Handed to curl on stdin (`curl -K -`), never printed, never on a command line.
#   --key-id-file FILE     REQUIRED. Where the key's `uid` goes (it becomes the secret TURN_KEY_ID).
#   --turn-token-file FILE REQUIRED. Where the key's secret goes (the response field `key`; `secret` if that is the name the API uses; it becomes
#                          the secret TURN_KEY_API_TOKEN). The API shows it ONCE.
#   --name NAME            the key's label in the dashboard (default vitadhoc-online)
#   --print-plan           print what would be done and exit 0 (nothing is contacted, no file is opened or written)
#   --self-check           check that curl runs only inside api() and the token is used only there, then exit
#
# What it does: POST https://api.cloudflare.com/client/v4/accounts/<account>/calls/turn_keys {"name": NAME}. ONE call, one key.
#   - If BOTH output files already exist and are non-empty it creates nothing ("reusing") and exits 0: a key is never created twice by accident.
#   - If only ONE of them exists it stops (a half pair is a lost key: look at it by hand).
#   - It never overwrites a file. Both files are written (mode 600) only after the reply parsed completely; the reply itself is never printed
#     or kept. If the reply cannot be read after the key was created, the script prints the last 4 characters of the key's uid so the owner can
#     delete that one orphan key in the dashboard (Realtime > TURN), and exits 5.
#   - If the account needs a payment method (or a plan) for Realtime, the API says so: the script prints its message and exits 4. Add nothing,
#     change nothing, tell the owner.
# Exit codes: 0 done / reusing, 1 bad use, 3 half pair of files, 4 refused by plan or billing, 5 created but unreadable, 6 other API failure,
#   7 the token lacks the permission or is not valid.
set -euo pipefail
umask 077

API=https://api.cloudflare.com/client/v4
ACCOUNT_ID=""; TOKEN_FILE=""; KEYID_FILE=""; TURNTOK_FILE=""; NAME=vitadhoc-online; PRINT_PLAN=0; SELF_CHECK=0
die() { echo "create-turn-key: $*" >&2; exit "${RC:-1}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --account-id) ACCOUNT_ID=${2:-}; shift 2 ;;
    --token-file) TOKEN_FILE=${2:-}; shift 2 ;;
    --key-id-file) KEYID_FILE=${2:-}; shift 2 ;;
    --turn-token-file) TURNTOK_FILE=${2:-}; shift 2 ;;
    --name) NAME=${2:-}; shift 2 ;;
    --print-plan) PRINT_PLAN=1; shift ;;
    --self-check) SELF_CHECK=1; shift ;;
    -h|--help) awk 'NR>1 { if ($0 ~ /^set -/) exit; print }' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# Self-check: curl runs only inside api(); the token variable appears only inside api() and on the lines that read and check it (marked tok-ok).
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

last4() { local v=$1; echo "****${v: -4}"; }
[[ "$ACCOUNT_ID" =~ ^[0-9a-fA-F]{32}$ ]] || die "--account-id <32 hex characters> is required"
ACCOUNT_ID=$(printf '%s' "$ACCOUNT_ID" | tr A-F a-f)
[ -n "$TOKEN_FILE" ] && [ -n "$KEYID_FILE" ] && [ -n "$TURNTOK_FILE" ] || die "--token-file, --key-id-file and --turn-token-file are all required"
[[ "$NAME" =~ ^[A-Za-z0-9._-]{1,60}$ ]] || die "--name: 1 to 60 letters, digits, . _ -"

if [ $PRINT_PLAN -eq 1 ]; then
  echo "PLAN (nothing is contacted, no file is opened or written). Account: $(last4 "$ACCOUNT_ID")"
  echo "1. if both output files exist and are non-empty: create nothing, exit 0 (reusing); if only one exists: stop"
  echo "2. read the API token from its file (never printed; handed to curl on stdin)"
  echo "3. POST $API/accounts/<account>/calls/turn_keys   body {\"name\":\"$NAME\"}    (ONE call: one key; needs the permission Calls Write)"
  echo "4. parse result.uid and result.key (or result.secret); write the uid to the key-id file and the secret to the token file, mode 600, only if both parsed; print only the uid's last 4 characters"
  exit 0
fi

command -v curl >/dev/null || die "curl is needed"
command -v node >/dev/null || die "node is needed (JSON)"
have() { [ -s "$1" ]; }
if have "$KEYID_FILE" && have "$TURNTOK_FILE"; then echo "create-turn-key: both key files already exist: reusing them, no key is created."; exit 0; fi
if [ -e "$KEYID_FILE" ] || [ -e "$TURNTOK_FILE" ]; then RC=3 die "only one of the two key files exists (or one is empty): a half pair means a key that cannot be used. Look at both by hand; nothing was created."; fi
[ -r "$TOKEN_FILE" ] || die "cannot read the token file (it needs the account permission Calls Write, for this account only). Nothing was contacted."
IFS= read -r TOK < "$TOKEN_FILE" || true   # tok-ok
[ ${#TOK} -ge 30 ] || die "the token file is shorter than 30 characters"   # tok-ok
[[ "$TOK" =~ ^[A-Za-z0-9._~+/=-]+$ ]] || die "the token file holds characters other than letters, digits and . _ ~ + / = -"   # tok-ok

# THE ONLY place curl is run. $1 method, $2 path (under /client/v4), $3 JSON body. Sets CODE (000 = no answer) and BODY (the raw reply: kept in memory
# only; it holds the new key's secret on success, so it is never printed or written). The token and the body reach curl on stdin (`curl -K -`).
api() {
  local raw esc cfg
  esc=$(printf '%s' "$3" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')
  cfg=$(printf 'header = "Authorization: Bearer %s"\nrequest = "%s"\nheader = "User-Agent: vitadhoc-turn-key"\nurl = "%s%s"\nheader = "Content-Type: application/json"\ndata-binary = "%s"' "$TOK" "$1" "$API" "$2" "$esc")
  raw=$(printf '%s\n' "$cfg" | curl -q -sS --max-time 30 -w '\n%{http_code}' -K - 2>/dev/null) || true
  cfg=""
  CODE=${raw##*$'\n'}
  [[ "$CODE" =~ ^[0-9]{3}$ ]] || CODE=000
  BODY=${raw%$'\n'*}
  raw=""
}
# error lines of a reply, with e-mail addresses and long hex runs masked
errs() { printf '%s' "$BODY" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{const j=JSON.parse(s);for(const e of (j.errors||[]))console.log(e.code+": "+e.message);}catch{console.log("(no JSON in the reply)")}})' \
  | LC_ALL=C sed -E -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/[EMAIL]/g' -e 's/[0-9A-Fa-f]{28,}([0-9A-Fa-f]{4})/[id ****\1]/g'; }

api POST "/accounts/$ACCOUNT_ID/calls/turn_keys" "{\"name\":\"$NAME\"}"
TOK=""
if [ "$CODE" != 200 ] && [ "$CODE" != 201 ]; then
  m=$(errs | head -3 | tr '\n' ' ')
  echo "create-turn-key: HTTP $CODE, the API says: $m" >&2
  if printf '%s' "$m" | grep -qiE 'payment|billing|subscription|plan|entitle|upgrade|not available|credit card'; then
    RC=4 die "refused by the plan or the billing of the account: STOP. Do not add a payment method. No key was created. Tell the owner."
  fi
  if [ "$CODE" = 401 ] || [ "$CODE" = 403 ]; then RC=7 die "the token lacks the permission 'Calls Write' for this account, or is not valid. No key was created."; fi
  RC=6 die "the key was not created (HTTP $CODE)."
fi
# parse: prints "OK <uid> <secret>" or "BAD <last 4 of the uid, if any>"; values are checked for shape before anything is written
PAIR=$(printf '%s' "$BODY" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{let r;try{const j=JSON.parse(s);r=(j&&j.result)||j||{}}catch{r={}};
  const uid=String(r.uid||""), sec=String(r.key||r.secret||"");
  if(/^[A-Za-z0-9._-]{8,64}$/.test(uid)&&/^[A-Za-z0-9._~+\/=-]{16,}$/.test(sec)) process.stdout.write("OK "+uid+" "+sec);
  else process.stdout.write("BAD "+uid.slice(-4));})') || PAIR="BAD "
BODY=""
if [ "${PAIR%% *}" != OK ]; then
  u=${PAIR#BAD }; PAIR=""
  echo "create-turn-key: the key WAS created (HTTP 200) but the reply could not be read${u:+; its uid ends ****$u}. Delete that one key in the dashboard (Realtime > TURN, the key named $NAME), then run this script again." >&2
  RC=5 die "no file was written."
fi
PAIR=${PAIR#OK }
UID_V=${PAIR%% *}; SEC_V=${PAIR#* }; PAIR=""
printf '%s\n' "$UID_V" > "$KEYID_FILE.new.$$"; printf '%s\n' "$SEC_V" > "$TURNTOK_FILE.new.$$"
chmod 600 "$KEYID_FILE.new.$$" "$TURNTOK_FILE.new.$$"
mv "$KEYID_FILE.new.$$" "$KEYID_FILE"; mv "$TURNTOK_FILE.new.$$" "$TURNTOK_FILE"
echo "create-turn-key: key created (uid ends $(last4 "$UID_V")); wrote the key id and the secret to the two files (mode 600). The secret is not shown again by Cloudflare: keep the files until the deploy has set the secrets."
SEC_V=""
