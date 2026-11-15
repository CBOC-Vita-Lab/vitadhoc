#!/bin/bash
# Offline test of create-turn-key.sh: `curl` is a stub on PATH (nothing is contacted). Run: bash scripts/create-turn-key.selftest.sh
# Not part of `npm test` (the node glob) on purpose: it is the owner-side script's own check.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
ACC=0123456789abcdef0123456789abcdef
TOKV=tok_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789
printf '%s\n' "$TOKV" > "$T/token"
ok=0; bad=0
check() { if [ "$2" = "$3" ]; then ok=$((ok+1)); else bad=$((bad+1)); echo "FAIL: $1 (got '$2', want '$3')"; fi; }
stub() { # $1 = reply body, $2 = http code
  printf '#!/bin/bash\ncat > "%s/stdin"\necho "$*" > "%s/args"\nprintf "%%s\\n%%s" %q %q\n' "$T" "$T" "$1" "$2" > "$T/bin/curl"; chmod +x "$T/bin/curl"; }
run() { rm -f "$T/k1" "$T/k2" "$T/out"; PATH="$T/bin:$PATH" bash "$HERE/create-turn-key.sh" --account-id $ACC --token-file "$T/token" --key-id-file "$T/k1" --turn-token-file "$T/k2" "$@" > "$T/out" 2>&1; echo $?; }

stub '{"success":true,"result":{"uid":"aaaabbbbccccddddeeeeffff00001111","key":"SECRETSECRETSECRETSECRETSECRET1234","name":"x"}}' 200
check "created rc" "$(run)" 0
check "uid file" "$(cat "$T/k1")" aaaabbbbccccddddeeeeffff00001111
check "secret file" "$(cat "$T/k2")" SECRETSECRETSECRETSECRETSECRET1234
check "modes" "$(stat -c %a "$T/k1" "$T/k2" | tr '\n' ' ')" "600 600 "
check "secret not printed" "$(grep -c -e SECRETSECRET -e aaaabbbbcccc "$T/out")" 0
check "token never in args" "$(grep -c "$TOKV" "$T/args")" 0
check "token on stdin" "$(grep -c "Bearer $TOKV" "$T/stdin")" 1
check "post to turn_keys" "$(grep -c 'calls/turn_keys' "$T/stdin")" 1
# a second run reuses
rc=$(PATH="$T/bin:$PATH" bash "$HERE/create-turn-key.sh" --account-id $ACC --token-file "$T/token" --key-id-file "$T/k1" --turn-token-file "$T/k2" >"$T/out" 2>&1; echo $?)
check "reuse rc" "$rc" 0; check "reuse says so" "$(grep -c reusing "$T/out")" 1
# half pair
rm -f "$T/k2"; rc=$(PATH="$T/bin:$PATH" bash "$HERE/create-turn-key.sh" --account-id $ACC --token-file "$T/token" --key-id-file "$T/k1" --turn-token-file "$T/k2" >"$T/out" 2>&1; echo $?)
check "half pair rc" "$rc" 3
# the API names the secret `secret`
stub '{"success":true,"result":{"uid":"aaaabbbbccccddddeeeeffff00001111","secret":"SECRETSECRETSECRETSECRETSECRET1234"}}' 200
check "secret field rc" "$(run)" 0
# billing
stub '{"success":false,"errors":[{"code":1,"message":"A payment method is required to use Realtime"}]}' 403
check "billing rc" "$(run)" 4; check "billing wrote nothing" "$(ls "$T/k1" "$T/k2" 2>/dev/null | wc -l)" 0
# missing permission
stub '{"success":false,"errors":[{"code":9109,"message":"Unauthorized to access requested resource"}]}' 403
check "permission rc" "$(run)" 7
# unreadable reply after success
stub '{"success":true,"result":{"uid":"aaaabbbbccccddddeeeeffff00001111"}}' 200
check "unreadable rc" "$(run)" 5; check "orphan uid hint" "$(grep -c '\*\*\*\*1111' "$T/out")" 1; check "unreadable wrote nothing" "$(ls "$T/k1" "$T/k2" 2>/dev/null | wc -l)" 0
# plan contacts nothing
rm -f "$T/stdin"; rc=$(PATH="$T/bin:$PATH" bash "$HERE/create-turn-key.sh" --account-id $ACC --token-file "$T/token" --key-id-file "$T/k1" --turn-token-file "$T/k2" --print-plan >"$T/out" 2>&1; echo $?)
check "plan rc" "$rc" 0; check "plan did not call curl" "$([ -e "$T/stdin" ] && echo yes || echo no)" no
check "plan hides account" "$(grep -c "$ACC" "$T/out")" 0
echo "$ok ok, $bad failed"; [ $bad -eq 0 ]
