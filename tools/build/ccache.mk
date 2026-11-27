# tools/build/ccache.mk -- the one place that wires ccache into the Makefiles.
#
#   include <repo>/tools/build/ccache.mk        (from a Makefile that sets CC / CXX)
#   CC := $(call ccw,$(PREFIX)gcc)               (the compiler, prefixed when ccache exists)
#
# Nothing here depends on a symlink farm (/usr/lib/ccache/gcc and the like): the
# prefix is written into CC and CXX themselves, so a build can not silently
# fall back to cold compiles the way a wrapper-by-symlink setup does after a
# container restart.  When ccache is not installed, CCACHE is empty and every
# Makefile behaves exactly as before.  Whether it was used is printed by
# `make ccache-stats` and at the end of `make vpk`.
#
# Switches (command line or environment):
#   USE_CCACHE=0     never use it (a bisect, a doubt about the cache)
#   CCACHE=<path>    a specific binary (default: the one on PATH)
#   CCACHE_DIR       where the cache lives (ccache's own default is
#                    $XDG_CACHE_HOME/ccache or ~/.cache/ccache; tools/build/
#                    ccache-setup.sh can pin a persistent one)
#   CCACHE_MAXSIZE   default 5G here, when not already set
#
# Settings exported to ccache (each only when the caller did not set it):
#   CCACHE_BASEDIR   the parent of the checkout.  Absolute paths below it are
#                    rewritten to relative ones before hashing, so a second
#                    checkout or worktree beside this one (or a scratch build
#                    directory under it) shares hits with it.
#   CCACHE_NOHASHDIR=1  the current directory is not part of the hash.  The
#                    object code is the same wherever make runs; only the debug
#                    info's recorded working directory differs, and no shipped
#                    artifact carries it (vita-elf-create keeps loadable
#                    segments only; the host twins are test binaries).
#   No "sloppiness" is set: file times and macros stay in the hash, so a hit is
#   always what a cold compile would have produced.  (include_file_mtime /
#   time_macros would raise the hit rate after a fresh checkout; they are not
#   worth a stale object, and a rebuilt object is only seconds.)
#
# Per-build statistics: a top-level make (MAKELEVEL 0) starts a fresh stats log
# in the temporary directory; ccache appends one line per compile to it
# and `make ccache-stats` / ccache-summary read this build's hits and misses
# only, not the whole cache's lifetime counters (other builds on the same
# machine do not mix in).

ifndef CCACHE_MK_INCLUDED
CCACHE_MK_INCLUDED := 1

CCACHE_MK_DIR  := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
CCACHE_REPO    := $(abspath $(CCACHE_MK_DIR)/../..)

USE_CCACHE ?= 1
ifeq ($(USE_CCACHE),1)
ifeq ($(origin CCACHE),undefined)
CCACHE := $(shell command -v ccache 2>/dev/null)
endif
else
CCACHE :=
endif

# ccw: prefix a compiler command once (idempotent, empty CCACHE = unchanged)
ccw = $(if $(CCACHE),$(if $(filter $(CCACHE),$(firstword $(1))),$(1),$(CCACHE) $(1)),$(1))

# ccplain: the compiler command without the prefix (for flag stamps, so that turning ccache on or
# off does not make every object look stale)
ccplain = $(strip $(if $(CCACHE),$(patsubst $(CCACHE)%,%,$(1)),$(1)))

ifneq ($(CCACHE),)
export CCACHE_BASEDIR   ?= $(abspath $(CCACHE_REPO)/..)
export CCACHE_NOHASHDIR ?= 1
export CCACHE_MAXSIZE   ?= 5G
# one stats log per top-level build; sub-makes inherit it through the environment
ifeq ($(MAKELEVEL),0)
ifeq ($(origin CCACHE_STATSLOG),undefined)
export CCACHE_STATSLOG := $(or $(TMPDIR),/tmp)/ccache-stats.$(shell echo $$PPID).log
$(shell rm -f $(CCACHE_STATSLOG); find $(or $(TMPDIR),/tmp) -maxdepth 1 -name 'ccache-stats.*.log' -mtime +1 -delete 2>/dev/null)
endif
endif
endif

# $(call ccache_summary) -- a recipe line that prints this build's cache use, or
# says plainly that there was none.
define ccache_summary
@sh $(CCACHE_REPO)/tools/build/ccache-report.sh "$(CCACHE)" "$(CCACHE_STATSLOG)"
endef

# (the `ccache-stats` target is in ccache-stats.mk: a target in THIS file would
# become the includer's default goal.)

endif
