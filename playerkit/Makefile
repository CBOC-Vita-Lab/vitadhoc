# playerkit: portable player library (MIT). Linux twin with ASan + UBSan.
#
#   make test        build and run every unit test, plus the scripted menu walk (PNGs)
#   make tsan        the frame hand-over mailbox and the core watchdog under ThreadSanitizer (also part of test)
#   make model       the pacing model's tables (ARGS=standard|adaptive|all)
#   make shots       only the menu walk; PNGs go to SHOTS (default build/shots)
#   make vita        compile the library for the PS Vita (arm-vita-eabi-g++), objects only
#   make regen-font  regenerate src/pk_font_data.cpp from the OFL fonts in ../gpsp/assets/fonts
#
# Copyright CBOC-Vita. MIT licence (see LICENSE).

CXX      ?= g++
# ccache (tools/build/ccache.mk): a no-op without ccache; USE_CCACHE=0 turns it off.
include ../tools/build/ccache.mk
override CXX := $(call ccw,$(CXX))
SAN      ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O3 -g -fno-omit-frame-pointer $(SAN)
B        := build
SHOTS    ?= $(B)/shots

LIB_SRC  := src/pk_util.cpp src/pk_text.cpp src/pk_glyphs.cpp src/pk_font_data.cpp src/pk_icons.cpp src/pk_gfx_soft.cpp src/pk_states.cpp \
            src/pk_launch.cpp src/pk_patch.cpp src/pk_input.cpp src/pk_notice.cpp src/pk_menu.cpp src/pk_pace.cpp src/pk_handoff.cpp src/pk_look.cpp src/pk_cores.cpp src/pk_scale.cpp src/pk_rewind.cpp src/pk_ff.cpp src/pk_speed.cpp src/pk_link.cpp
LIB_OBJ  := $(patsubst src/%.cpp,$(B)/linux/%.o,$(LIB_SRC))
TEST_SRC := $(wildcard tests/test_*.cpp)
TEST_OBJ := $(patsubst tests/%.cpp,$(B)/linux/tests/%.o,$(TEST_SRC))

all: test

$(B)/linux/%.o: src/%.cpp $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

$(B)/linux/tests/%.o: tests/%.cpp $(wildcard src/*.h) $(wildcard tests/*.h) $(wildcard tools/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -Itools -c $< -o $@

$(B)/unit_tests: $(LIB_OBJ) $(TEST_OBJ) $(B)/linux/tests/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ -pthread

$(B)/linux/tests/main.o: tests/main.cpp tests/pk_test.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -c $< -o $@

$(B)/menu_walk: $(LIB_OBJ) $(B)/linux/tests/menu_walk.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(B)/linux/tests/menu_walk.o: tests/menu_walk.cpp tests/harness.h $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -c $< -o $@

test: $(B)/unit_tests $(B)/menu_walk $(B)/vita_gxm $(B)/handoff_tsan $(B)/cores_tsan $(B)/spin_tsan stdio_lock link-test ra-test
	TZ=UTC $(B)/unit_tests
	TZ=UTC $(B)/menu_walk $(SHOTS)
	$(B)/vita_gxm
	$(B)/handoff_tsan
	$(B)/cores_tsan
	$(B)/spin_tsan

# pk_link over the real UDP transport (loopback, the transport's own I/O threads): link_tests (ASan + UBSan, with the 12 s stall
# and the 20 s dead peer in real time) and link_tsan (ThreadSanitizer, quick). They use loopback ports and wall-clock time:
# run them ALONE (check `pgrep -af link_tests` first). The transport and NAT layer are the project's own files in ../gpsp/netdrv.
include link.mk

# pk_ra (RetroAchievements for the players) against the real rcheevos client and ra_http.c, talking to a fake server on the loopback
# (tests/fake_ra.h): sign-in with the app's token, a game found by its hash, an unlock toasted and sent, Hardcore, the account file never
# written, the token never in a log. ra.mk lists the sources; pk_ra_plat_posix.c is the thread layer. Uses a loopback port: run it ALONE.
RA_GP     := ../gpsp/vita
RA_OBJDIR := $(B)/ra
RA_CC     := $(CC)
RA_CFLAGS := -std=gnu11 -O1 -g -w $(SAN)
PK        := .
include ra.mk
$(B)/ra/pk_ra_plat_posix.o: ra/pk_ra_plat_posix.c src/pk_ra_plat.h
	@mkdir -p $(B)/ra
	$(CC) -std=gnu11 -O1 -g -w $(SAN) -Isrc -c -o $@ $<
$(B)/ra_tests: tests/ra_tests_main.cpp tests/fake_ra.h tests/pk_test.h src/pk_ra.cpp src/pk_ra.h $(RA_OBJS) $(B)/ra/pk_ra_plat_posix.o
	$(CXX) $(CXXFLAGS) -Isrc -Itests $(RA_INC) tests/ra_tests_main.cpp src/pk_ra.cpp \
	  $(RA_OBJS) $(B)/ra/pk_ra_plat_posix.o -o $@ -pthread
ra-test: $(B)/ra_tests
	@if pgrep -af 'build/[r]a_tests' >/dev/null; then echo "another ra_tests run is active"; exit 1; fi
	$(B)/ra_tests
.PHONY: ra-test

# the console's drawing path (pk_gfx_vita.cpp, unchanged) against a GXM-faithful fake of vita2d, with the real SDK headers
VITA2D_INC ?= ../snes/frontend/third_party/vita2d/include
VITASDK ?= /opt/vitasdk
$(B)/vita_gxm: $(LIB_OBJ) tests/vita_gxm/test_vita_gfx.cpp tests/vita_gxm/vita2d_fake.cpp tests/vita_gxm/vita2d_fake.h vita/pk_gfx_vita.cpp $(wildcard tests/*.h)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -Ivita -Itests/vita_gxm -I$(VITA2D_INC) -idirafter $(VITASDK)/arm-vita-eabi/include \
	  tests/vita_gxm/test_vita_gfx.cpp tests/vita_gxm/vita2d_fake.cpp vita/pk_gfx_vita.cpp $(LIB_OBJ) -o $@ -pthread

# the frame hand-over mailbox under ThreadSanitizer (a separate binary: it cannot share one with AddressSanitizer)
TSAN_FLAGS ?= -std=c++11 -Wall -Wextra -O1 -g -fsanitize=thread
$(B)/handoff_tsan: tests/handoff_tsan_main.cpp tests/handoff_stress.h src/pk_handoff.cpp src/pk_handoff.h src/pk_handoff_posix.h src/pk_pace.cpp
	@mkdir -p $(B)
	$(CXX) $(TSAN_FLAGS) -Isrc -Itests tests/handoff_tsan_main.cpp src/pk_handoff.cpp src/pk_pace.cpp -o $@ -pthread

# the core watchdog's heartbeat counters with real threads under ThreadSanitizer
$(B)/cores_tsan: tests/cores_tsan_main.cpp src/pk_cores.cpp src/pk_cores.h
	@mkdir -p $(B)
	$(CXX) $(TSAN_FLAGS) -Isrc tests/cores_tsan_main.cpp src/pk_cores.cpp -o $@ -pthread

# the spin lock (pk_util.h) with real threads under ThreadSanitizer
$(B)/spin_tsan: tests/spin_tsan_main.cpp src/pk_util.cpp src/pk_util.h
	@mkdir -p $(B)
	$(CXX) $(TSAN_FLAGS) -Isrc tests/spin_tsan_main.cpp src/pk_util.cpp -o $@ -pthread

# vita/pk_stdio_lock.c (the FILE-slot lock the console builds link with --wrap) on the host: the same
# unlocked "scan, claim" through a fake libc, unwrapped (double claims, shown) and wrapped (none); the wrapped one
# also under ThreadSanitizer.
STDIO_WRAP := -Wl,--wrap=__sfp -Wl,--wrap=__sfp_lock_acquire -Wl,--wrap=__sfp_lock_release
stdio_lock:
	@mkdir -p $(B)
	$(CC) -O1 -g -Itests/stdio_lock tests/stdio_lock/test_stdio_lock.c tests/stdio_lock/fake_sfp.c -o $(B)/stdio_raw -pthread
	$(B)/stdio_raw
	$(CC) -O1 -g -DWRAPPED -Itests/stdio_lock tests/stdio_lock/test_stdio_lock.c tests/stdio_lock/fake_sfp.c vita/pk_stdio_lock.c $(STDIO_WRAP) -o $(B)/stdio_wrapped -pthread
	$(B)/stdio_wrapped
	$(CC) -O1 -g -fsanitize=thread -DWRAPPED -Itests/stdio_lock tests/stdio_lock/test_stdio_lock.c tests/stdio_lock/fake_sfp.c vita/pk_stdio_lock.c $(STDIO_WRAP) -o $(B)/stdio_wrapped_tsan -pthread
	$(B)/stdio_wrapped_tsan
.PHONY: stdio_lock

tsan: $(B)/handoff_tsan $(B)/cores_tsan $(B)/spin_tsan
	$(B)/handoff_tsan
	$(B)/cores_tsan
	$(B)/spin_tsan

# the pacing model (virtual time, no console): make model ARGS="all"
$(B)/pacing_model: tools/pacing_model.cpp tools/pacing_model.h src/pk_pace.cpp src/pk_handoff.cpp src/pk_handoff.h
	@mkdir -p $(B)
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Isrc -Itools tools/pacing_model.cpp src/pk_pace.cpp src/pk_handoff.cpp -o $@

model: $(B)/pacing_model
	$(B)/pacing_model $(ARGS)

shots: $(B)/menu_walk
	TZ=UTC $(B)/menu_walk $(SHOTS)

# ---- PS Vita (compile only: nothing here can be run off the console) ----
VITASDK ?= /opt/vitasdk
VITACXX := $(call ccw,$(VITASDK)/bin/arm-vita-eabi-g++)
VITAFLAGS := -std=c++11 -Wall -Wextra -O2 -Isrc -I../snes/frontend/third_party/vita2d/include -D__vita__
VITA_SRC := $(filter-out src/pk_gfx_soft.cpp,$(LIB_SRC)) vita/pk_gfx_vita.cpp
VITA_OBJ := $(patsubst %.cpp,$(B)/vita/%.o,$(VITA_SRC))

$(B)/vita/%.o: %.cpp $(wildcard src/*.h) $(wildcard vita/*.h)
	@mkdir -p $(dir $@)
	$(VITACXX) $(VITAFLAGS) -c $< -o $@

vita: $(VITA_OBJ)
	@echo "vita objects: $(words $(VITA_OBJ))"
	$(VITASDK)/bin/arm-vita-eabi-ar rcs $(B)/vita/libplayerkit.a $(VITA_OBJ)
	@ls -l $(B)/vita/libplayerkit.a

regen-font: tools/gen_font.cpp
	@mkdir -p $(B)
	g++ -O1 -I../snes/frontend/third_party/stb tools/gen_font.cpp -o $(B)/gen_font
	$(B)/gen_font ../gpsp/assets/fonts src/pk_font_data.cpp

clean:
	rm -rf $(B)/linux $(B)/unit_tests $(B)/menu_walk $(B)/vita_gxm $(B)/vita $(B)/handoff_tsan $(B)/cores_tsan $(B)/pacing_model

.PHONY: all test shots vita regen-font clean tsan model
