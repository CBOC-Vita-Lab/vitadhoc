#!/bin/sh
# ccache-setup.sh [CACHE_DIR] -- make sure ccache is installed and the cache lives somewhere persistent.
#
# Run it once per machine, and again after a container or VM is rebuilt (a
# fresh image has neither the package nor the machine-wide setting; the cache
# directory itself survives when it is on a persistent disk).  Safe to repeat.
#   - installs the ccache package when it is missing (Debian/Ubuntu, needs root)
#   - when CACHE_DIR is given and the user is root, writes /etc/ccache.conf so
#     every shell, make and script on the machine uses that directory and the
#     5 GB cap, without any environment variable or symlink
# Nothing in the repository requires this: without ccache the Makefiles build
# as they always did, and say so at the end of `make vpk`.
set -u
dir="${1:-}"
if ! command -v ccache >/dev/null 2>&1; then
  echo "ccache not found; installing"
  if [ "$(id -u)" = 0 ] && command -v apt-get >/dev/null 2>&1; then
    apt-get install -y ccache >/dev/null 2>&1 || { apt-get update >/dev/null 2>&1; apt-get install -y ccache; } || exit 1
  else
    echo "install the ccache package with your package manager, then run this again" >&2
    exit 1
  fi
fi
if [ -n "$dir" ]; then
  mkdir -p "$dir" || exit 1
  if [ "$(id -u)" = 0 ]; then
    printf 'cache_dir = %s\nmax_size = 5G\n' "$dir" > /etc/ccache.conf
    echo "wrote /etc/ccache.conf (cache_dir = $dir, max_size = 5G)"
  else
    echo "not root: export CCACHE_DIR=$dir in your shell profile instead"
  fi
fi
ccache --version | head -1
echo "cache directory: $(CCACHE_DIR="${dir:-${CCACHE_DIR:-}}" ccache -k cache_dir)"
