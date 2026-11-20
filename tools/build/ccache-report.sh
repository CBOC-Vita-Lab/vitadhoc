#!/bin/sh
# ccache-report.sh CCACHE STATSLOG -- print this build's ccache hits and misses.
# Said plainly when ccache is not in use, so a cold build is never silent.
cc="$1"; log="$2"
if [ -z "$cc" ]; then
  echo "ccache: NOT USED (not installed, or USE_CCACHE=0): every object was compiled cold." >&2
  echo "        tools/build/ccache-setup.sh installs it and pins a persistent cache directory." >&2
  exit 0
fi
dir=$("$cc" -k cache_dir 2>/dev/null)
if [ -z "$log" ] || [ ! -s "$log" ]; then
  echo "ccache: in use (cache $dir), no compiles went through it in this build (everything was up to date)."
  exit 0
fi
# the stats log has one "<result>" line per ccache call; see `ccache --show-log-stats`
CCACHE_STATSLOG="$log" "$cc" --show-log-stats 2>/dev/null | awk -v d="$dir" '
  /^ *Hits:/ && !seenh { h = $2; seenh = 1 }
  /^ *Misses:/ && !seenm { m = $2; seenm = 1 }
  /^Uncacheable calls:/ { u = $3 }
  END { t = h + m; r = (t > 0) ? 100.0 * h / t : 0;
        printf "ccache: %d hits, %d misses (%.0f%% hit rate) of %d cacheable compiles, %d other calls (links, preprocessing); cache %s\n", h, m, r, t, u, d }'
