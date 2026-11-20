# playerkit: the real-network tests of pk_link (included by Makefile). Copyright CBOC-Vita. MIT licence (see LICENSE).
NETDRV    := ../gpsp/netdrv
NET_C     := $(NETDRV)/transport_lan.c $(NETDRV)/nat_hash.c $(NETDRV)/nat_stun.c $(NETDRV)/nat_turn.c src/pk_link_net_shim.c
NET_DEFS  := -DND_WINDOW=64 -DND_TXQ_CAP=256 -DND_RTO_MAX_US=2500000
LINK_SRC  := src/pk_link.cpp src/pk_link_net.cpp src/pk_util.cpp
LINK_HDR  := $(wildcard src/*.h) tests/link_sim.h $(wildcard $(NETDRV)/*.h)

$(B)/link_tests: tests/link_tests_main.cpp $(LINK_SRC) $(NET_C) $(LINK_HDR)
	@mkdir -p $(B)/linknet-asan
	for f in $(NET_C); do $(CC) -std=gnu11 -O2 -g -w $(SAN) $(NET_DEFS) -I$(NETDRV) -c $$f -o $(B)/linknet-asan/$$(basename $$f .c).o || exit 1; done
	$(CXX) $(CXXFLAGS) -Isrc -Itests -I$(NETDRV) tests/link_tests_main.cpp $(LINK_SRC) $(B)/linknet-asan/*.o -o $@ -pthread

$(B)/link_tsan: tests/link_tests_main.cpp $(LINK_SRC) $(NET_C) $(LINK_HDR)
	@mkdir -p $(B)/linknet-tsan
	for f in $(NET_C); do $(CC) -std=gnu11 -O1 -g -w -fsanitize=thread $(NET_DEFS) -I$(NETDRV) -c $$f -o $(B)/linknet-tsan/$$(basename $$f .c).o || exit 1; done
	$(CXX) $(TSAN_FLAGS) -Isrc -Itests -I$(NETDRV) tests/link_tests_main.cpp $(LINK_SRC) $(B)/linknet-tsan/*.o -o $@ -pthread

link-test: $(B)/link_tests $(B)/link_tsan
	@if pgrep -af 'build/[l]ink_(tests|tsan)' >/dev/null; then echo "another link_tests run is active: link tests share loopback ports, run them alone"; exit 1; fi
	$(B)/link_tests
	PK_LINK_QUICK=1 $(B)/link_tsan
.PHONY: link-test
