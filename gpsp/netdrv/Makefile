# netdrv/Makefile — desktop build of the netpacket driver + unit tests.
#
#   make          -> libnetdrv.a (netdrv core + UDP transport)
#   make test     -> build and RUN the unit-test suite (desktop, no SDL)
#   make clean
#
# The netdrv core (netdrv.c) is transport-agnostic plain C and is also
# compiled into the PSP frontend later (Phase 4) against transport_adhoc.
# transport_udp.c is desktop-only (POSIX sockets). Run under WSL/Linux.

CC      ?= gcc
CFLAGS  := -O2 -g -Wall -Wextra -I.

LIB     := libnetdrv.a
OBJS    := obj/netdrv.o obj/transport_udp.o

all: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)

obj/%.o: %.c netdrv.h netdrv_wire.h transport_udp.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj/test_netdrv.o: tests/test_netdrv.c netdrv.h netdrv_wire.h transport_udp.h
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

test_netdrv: obj/test_netdrv.o $(LIB)
	$(CC) -o $@ obj/test_netdrv.o $(LIB)

test: test_netdrv
	./test_netdrv

# ---- transport_lan: the Vita port's Wi-Fi / direct-IP transport -----------
# Host-testable (BSD sockets + pthreads).  `make test-lan` runs it on real
# loopback sockets in real time (~40 s); SAN=address or SAN=thread rebuilds
# everything it links with that sanitizer into obj-$(SAN)/.
#   make test-lan                 all tests
#   make test-lan LAN_TESTS=nat   only some (names: tests/test_lan.c)
#   make test-lan SAN=address     under AddressSanitizer + UBSan
#   make test-lan SAN=thread      under ThreadSanitizer
#   make bench-lan                the ARQ-profile table (not a test)
LAN_SAN_address := -fsanitize=address,undefined -fno-omit-frame-pointer
LAN_SAN_thread  := -fsanitize=thread
LAN_OBJ    := obj$(if $(SAN),-$(SAN))
LAN_BIN    := test_lan$(if $(SAN),-$(SAN))
LAN_CFLAGS := $(CFLAGS) -std=gnu11 -pthread $(LAN_SAN_$(SAN))
LAN_HDRS   := netdrv.h netdrv_wire.h nd_thread.h transport_lan.h

$(LAN_OBJ)/lan_%.o: %.c $(LAN_HDRS)
	@mkdir -p $(LAN_OBJ)
	$(CC) $(LAN_CFLAGS) -c -o $@ $<

$(LAN_OBJ)/lan_test_lan.o: tests/test_lan.c $(LAN_HDRS)
	@mkdir -p $(LAN_OBJ)
	$(CC) $(LAN_CFLAGS) -c -o $@ $<

$(LAN_BIN): $(LAN_OBJ)/lan_test_lan.o $(LAN_OBJ)/lan_netdrv.o $(LAN_OBJ)/lan_transport_lan.o
	$(CC) $(LAN_CFLAGS) -o $@ $^

test-lan: $(LAN_BIN)
	./$(LAN_BIN) $(LAN_TESTS)

bench-lan: $(LAN_BIN)
	./$(LAN_BIN) bench

# The same tests as a 32-bit ARMv7 hard-float Linux program under qemu-arm
# (the Vita's CPU family; proves the 32-bit build and that the ring's
# barriers are emitted -- qemu on an x86 host cannot prove their ORDERING).
test-lan-arm:
	arm-linux-gnueabihf-gcc -O2 -g -std=gnu11 -Wall -Wextra -march=armv7-a \
	   -mfpu=neon -mfloat-abi=hard -marm -pthread -I. -o test_lan-arm \
	   tests/test_lan.c netdrv.c transport_lan.c
	qemu-arm -L /usr/arm-linux-gnueabihf ./test_lan-arm $(LAN_TESTS)

# ---- transport_vadhoc: the Vita ad hoc transport, against a fake stack -----
# The real file, built with -DVADHOC_HOST_FAKE against tests/vadhoc_fake.c
# (the SCE calls it makes, over UDP loopback).  Proves the transport's own
# logic; says nothing about what a console's firmware does.
VAD_BIN := test_vadhoc$(if $(SAN),-$(SAN))
VAD_SRC := tests/test_vadhoc.c tests/vadhoc_fake.c transport_vadhoc.c netdrv.c
VAD_HDR := $(LAN_HDRS) transport_vadhoc.h tests/vadhoc_fake.h

$(VAD_BIN): $(VAD_SRC) $(VAD_HDR)
	$(CC) $(LAN_CFLAGS) -DVADHOC_HOST_FAKE -Itests -o $@ $(VAD_SRC)

test-vadhoc: $(VAD_BIN)
	./$(VAD_BIN)

# ---- upnp_igd: the port-mapping client, against a pretend router ------------
# tests/fake_igd.py (python3) is started per scenario by the test itself.
IGD_BIN := test_igd$(if $(SAN),-$(SAN))

$(IGD_BIN): tests/test_igd.c upnp_igd.c upnp_igd.h nd_thread.h
	$(CC) $(LAN_CFLAGS) -DUPNP_IGD_TEST -o $@ tests/test_igd.c upnp_igd.c

test-igd: $(IGD_BIN)
	./$(IGD_BIN) tests/fake_igd.py

# ---- the real link session over transport_lan --------------------------------
# Two complete fe_gblink stacks (tools/gblink/build.sh) over netdrv over
# transport_lan on loopback sockets, in real time, with generated test
# cartridges.  ~2 minutes; needs python3 and binutils.
test-session:
	python3 tests/run_sesslan.py sesslan-obj

# ---- the Vita build of all of it: compile, link, resolve imports -------------
# Needs vitasdk (VITASDK=/opt/vitasdk).  Compiles the network files exactly
# as gpsp/vita/net.mk tells the Vita Makefile to, links tests/vita_linkcheck.c
# (a reference to every entry point) against the stub libraries, and lets
# vita-elf-create turn each imported SCE function into a NID: a function
# that does not exist under that name fails here, not on a console.
VITASDK   ?= /opt/vitasdk
VITA_CC   := $(VITASDK)/bin/arm-vita-eabi-gcc
VITA_FLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -DVITA -marm -mcpu=cortex-a9 \
              -mfloat-abi=hard -I. -I../vita \
              -DND_WINDOW=64 -DND_TXQ_CAP=256 -DND_RTO_MAX_US=2500000
VITA_SRCS := netdrv.c transport_lan.c transport_vadhoc.c upnp_igd.c \
             ../vita/net_vita.c tests/vita_linkcheck.c
VITA_LIBS := -lScePspnetAdhoc_stub -lSceCommonDialog_stub -lSceNetCtl_stub \
             -lSceNet_stub -lSceSysmodule_stub

vita-check:
	@mkdir -p obj-vita
	@for f in $(VITA_SRCS); do \
	   echo "  VITA-CC $$f"; \
	   $(VITA_CC) $(VITA_FLAGS) -c -o obj-vita/$$(basename $$f .c).o $$f || exit 1; \
	done
	$(VITA_CC) -Wl,-q -o obj-vita/linkcheck.elf obj-vita/*.o $(VITA_LIBS)
	$(VITASDK)/bin/vita-elf-create obj-vita/linkcheck.elf obj-vita/linkcheck.velf
	@echo "vita-check: compiled, linked, imports resolved"

clean:
	rm -rf obj obj-address obj-thread obj-vita sesslan-obj $(LIB) test_netdrv test_lan \
	       test_lan-address test_lan-thread test_vadhoc test_vadhoc-address \
	       test_vadhoc-thread test_igd test_igd-address test_igd-thread

.PHONY: all test test-lan test-lan-arm bench-lan test-vadhoc test-igd test-session vita-check clean
