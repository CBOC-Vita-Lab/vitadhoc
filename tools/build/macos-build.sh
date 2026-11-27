#!/bin/sh
# macos-build.sh -- build the host twin of the app natively on macOS, step by step.
#
#     sh tools/build/macos-build.sh [--work DIR] [--no-tls] [--host-version V] [--jobs N] [--clean]
#
# The Mac is then the second player against a real Vita (docs/TESTING.md, "Second
# player on a Mac"; gpsp/vita/hostcore/mac_player.py).  Steps, each logged to
# WORK/logs/NN-name.log; the script STOPS AT THE FIRST FAILING STEP and shows the
# last 40 lines of its log (and the first compiler errors) so one paste is enough:
#
#   1 tools    Apple clang, make (3.81 is fine), python3, git, llvm-objcopy / llvm-nm
#              (Homebrew llvm: brew install llvm) and a probe that llvm-objcopy can
#              rename a symbol in a Mach-O object (what instance B of the core needs)
#   2 tls      Mbed TLS for macOS (tools/online/build-mbedtls.py --target macos): the
#              twin's HTTPS to the room service.  --no-tls builds without it (the twin
#              then cannot do Online; Same Wi-Fi still works).  Needs git access to GitHub.
#   3 twin     make -C gpsp/vita/hostcore ARCH=macos MBEDTLS=<prefix> HOST_VERSION=...
#   4 smoke    the binary starts, shows the app for 20 refreshes and exits 0
#
#   --work DIR        scratch and logs (default: build/macos-work in the checkout).  It must
#                     not be under /tmp or /var (symlinks on macOS; the TLS builder refuses them)
#   --host-version V  the string link rooms compare.  To join a REAL Vita's room it must be the
#                     Vita build's CORE_VERSION (see docs/TESTING.md); default host-<git revision>
#   --jobs N          make -j (default: the number of CPUs)
#   --clean           remove the twin's build directory first
#
# Nothing is installed and nothing outside the checkout is written.

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work="$root/build/macos-work"
tls=1
hostver=""
jobs=""
clean=0
while [ $# -gt 0 ]; do
  case "$1" in
    --work) work=$2; shift 2 ;;
    --no-tls) tls=0; shift ;;
    --host-version) hostver=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --clean) clean=1; shift ;;
    -h|--help) sed -n '2,/^# Nothing is installed/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
done

mkdir -p "$work/logs" || exit 1
work=$(cd "$work" && pwd)
logs="$work/logs"
summary="$work/summary.txt"
: > "$summary"
[ -n "$jobs" ] || jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
twindir="$root/gpsp/vita/hostcore"
exe="$twindir/build/macos/gbadhoc_host"

say() { echo "$*" | tee -a "$summary"; }

# step NAME LOGFILE command...   run a command, log it, stop the script when it fails
step() {
  name=$1; log=$2; shift 2
  say "== $name"
  echo "\$ $*" > "$log"
  "$@" >> "$log" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    say "FAILED: $name (exit $rc). Full log: $log"
    echo
    echo "---- last 40 lines of $log ----"
    tail -n 40 "$log"
    echo "---- first errors (up to 15) ----"
    grep -E "error|Error|undefined|duplicate symbol|not found|No such" "$log" | head -n 15
    echo "----"
    echo "Send the block above (or the log) back."
    exit $rc
  fi
  tail -n 2 "$log" | sed 's/^/   /'
}

# ---- 1 tools ---------------------------------------------------------------------
tools_check() {
  echo "system:  $(uname -srm)"
  [ "$(uname -s)" = Darwin ] || { echo "this script is for macOS (uname says $(uname -s)); on Linux use make ARCH=x86 or aarch64"; return 1; }
  echo "macOS:   $(sw_vers -productVersion 2>/dev/null)"
  command -v clang >/dev/null 2>&1 || { echo "no clang: xcode-select --install"; return 1; }
  clang --version | head -n 1
  clang++ --version | head -n 1
  make --version | head -n 1
  command -v python3 >/dev/null 2>&1 || { echo "no python3: xcode-select --install, or brew install python"; return 1; }
  python3 --version
  command -v git >/dev/null 2>&1 || { echo "no git: xcode-select --install"; return 1; }
  # llvm-objcopy and llvm-nm: PATH, then Homebrew's keg-only llvm
  llvm=""
  for d in "${LLVM_BIN:-}" "$(brew --prefix llvm 2>/dev/null)/bin" /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin \
           "$(dirname "$(command -v llvm-objcopy 2>/dev/null || echo /nonexistent/x)")"; do
    if [ -n "$d" ] && [ -x "$d/llvm-objcopy" ] && [ -x "$d/llvm-nm" ]; then llvm=$d; break; fi
  done
  if [ -z "$llvm" ]; then
    echo "llvm-objcopy / llvm-nm not found: brew install llvm   (or set LLVM_BIN=<llvm's bin directory>)"
    return 1
  fi
  echo "llvm:    $llvm ($("$llvm/llvm-objcopy" --version | sed -n 2p))"
  echo "$llvm" > "$work/llvm_bin"
  # the probe: rename a symbol of a Mach-O object, the way macho-group.sh does
  p="$work/probe"; rm -rf "$p"; mkdir -p "$p"
  printf 'int probe_data = 3;\nint probe_fn(void) { return probe_data; }\n' > "$p/p.c"
  clang -c "$p/p.c" -o "$p/p.o" || return 1
  printf '_probe_fn _renamed_fn\n_probe_data _renamed_data\n' > "$p/map"
  "$llvm/llvm-objcopy" --redefine-syms="$p/map" "$p/p.o" "$p/q.o" || { echo "llvm-objcopy cannot --redefine-syms a Mach-O object: update it (brew upgrade llvm)"; return 1; }
  "$llvm/llvm-nm" -g --defined-only "$p/q.o" | grep -q _renamed_fn || { echo "the renamed symbol is not in the object: this llvm-objcopy is too old"; return 1; }
  echo "probe:   llvm-objcopy renames Mach-O symbols: ok"
  echo "git:     $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
}
step "1 tools" "$logs/01-tools.log" tools_check
tail -n 12 "$logs/01-tools.log" | sed 's/^/   /' >> "$summary"
llvm_bin=$(cat "$work/llvm_bin")
export LLVM_BIN="$llvm_bin"

# ---- 2 tls -----------------------------------------------------------------------
mbed=""
if [ $tls -eq 1 ]; then
  step "2 tls (Mbed TLS, macOS)" "$logs/02-tls.log" \
    python3 "$root/tools/online/build-mbedtls.py" --target macos --work "$work/tls"
  mbed="$work/tls/mbedtls/macos"
  [ -f "$mbed/lib/libmbedtls.a" ] || { say "FAILED: $mbed/lib/libmbedtls.a is missing after step 2"; exit 1; }
else
  say "== 2 tls: skipped (--no-tls): the twin cannot do Online"
fi

# ---- 3 twin ----------------------------------------------------------------------
if [ $clean -eq 1 ]; then rm -rf "$twindir/build/macos"; fi
hv=""
[ -z "$hostver" ] || hv="HOST_VERSION=$hostver"
# shellcheck disable=SC2086
step "3 twin (ARCH=macos, -j$jobs)" "$logs/03-twin.log" \
  make -C "$twindir" ARCH=macos -j"$jobs" ${mbed:+MBEDTLS="$mbed"} $hv
[ -x "$exe" ] || { say "FAILED: $exe is missing after step 3"; exit 1; }

# ---- 4 smoke ---------------------------------------------------------------------
smoke() {
  rm -rf "$work/smoke"; mkdir -p "$work/smoke"
  "$exe" --root "$work/smoke" --frames 20 || return $?
}
step "4 smoke (start, 20 refreshes, exit)" "$logs/04-smoke.log" smoke
grep -q "host: exit 0" "$logs/04-smoke.log" || { say "FAILED: the twin ran but did not report a clean exit; log: $logs/04-smoke.log"; tail -n 20 "$logs/04-smoke.log"; exit 1; }

say ""
say "DONE. twin: $exe"
say "      TLS:  ${mbed:-none (plain http only)}"
say "      link version string: ${hostver:-host-$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
say "Second player: python3 $twindir/mac_player.py --exe $exe --rom <game.gba> --join <CODE>   (see --help)"
