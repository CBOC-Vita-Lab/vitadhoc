# playerkit: portable player library (MIT). Linux twin with ASan + UBSan.
#
#   make test        build and run every unit test, plus the scripted menu walk (PNGs)
#   make shots       only the menu walk; PNGs go to SHOTS (default build/shots)
#   make vita        compile the library for the PS Vita (arm-vita-eabi-g++), objects only
#   make regen-font  regenerate src/pk_font_data.cpp from the OFL fonts in ../gpsp/assets/fonts
#
# Copyright CBOC-Vita. MIT licence (see LICENSE).

CXX      ?= g++
SAN      ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O1 -g -fno-omit-frame-pointer $(SAN)
B        := build
SHOTS    ?= $(B)/shots

LIB_SRC  := src/pk_util.cpp src/pk_text.cpp src/pk_font_data.cpp src/pk_icons.cpp src/pk_gfx_soft.cpp src/pk_states.cpp \
            src/pk_launch.cpp src/pk_patch.cpp src/pk_input.cpp src/pk_notice.cpp src/pk_menu.cpp
LIB_OBJ  := $(patsubst src/%.cpp,$(B)/linux/%.o,$(LIB_SRC))
TEST_SRC := $(wildcard tests/test_*.cpp)
TEST_OBJ := $(patsubst tests/%.cpp,$(B)/linux/tests/%.o,$(TEST_SRC))

all: test

$(B)/linux/%.o: src/%.cpp $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -c $< -o $@

$(B)/linux/tests/%.o: tests/%.cpp $(wildcard src/*.h) $(wildcard tests/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -c $< -o $@

$(B)/unit_tests: $(LIB_OBJ) $(TEST_OBJ) $(B)/linux/tests/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(B)/linux/tests/main.o: tests/main.cpp tests/pk_test.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -c $< -o $@

$(B)/menu_walk: $(LIB_OBJ) $(B)/linux/tests/menu_walk.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(B)/linux/tests/menu_walk.o: tests/menu_walk.cpp tests/harness.h $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Isrc -Itests -c $< -o $@

test: $(B)/unit_tests $(B)/menu_walk
	TZ=UTC $(B)/unit_tests
	TZ=UTC $(B)/menu_walk $(SHOTS)

shots: $(B)/menu_walk
	TZ=UTC $(B)/menu_walk $(SHOTS)

# ---- PS Vita (compile only: nothing here can be run off the console) ----
VITASDK ?= /opt/vitasdk
VITACXX := $(VITASDK)/bin/arm-vita-eabi-g++
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
	rm -rf $(B)/linux $(B)/unit_tests $(B)/menu_walk $(B)/vita

.PHONY: all test shots vita regen-font clean
