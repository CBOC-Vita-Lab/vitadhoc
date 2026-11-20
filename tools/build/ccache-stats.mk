# tools/build/ccache-stats.mk -- `make ccache-stats`: this build's ccache use (the stats log of the
# current make; empty when nothing was compiled) and the cache's size.  Include it AFTER the
# Makefile's default goal (it defines a target).  Needs tools/build/ccache.mk included first.
ccache-stats:
	$(call ccache_summary)
	@if [ -n "$(CCACHE)" ]; then $(CCACHE) -s | grep -E 'Cache directory|Max size|Cache size|Hits:|Misses:' | head -8; fi
.PHONY: ccache-stats
