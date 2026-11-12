# widekit - MIT licence.  Portable C++11 library + tests.
#   make test        build and run the tests (Linux, ASan + UBSan)
#   make bench       optimised timing run (host)
#   make vita        compile the library for the Vita (must be warning free)
#   make arm-check   NEON == scalar blend check under qemu-arm
#   make arm-count   dynamic instruction count of one bench run under qemu-arm
#   make shots       (re)write the PNG strips (also done by `make test`)
CXX      ?= g++
SHOTS    ?= build/shots
SRC       = src/wk_layers.cpp src/wk_map.cpp src/wk_compose.cpp src/wk_profile.cpp src/wk_util.cpp
HDR       = $(wildcard include/*.h) src/wk_util.h
WARN      = -Wall -Wextra -Wshadow -Wno-unused-parameter
CXXFLAGS_T= -std=c++11 -g -O1 $(WARN) -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Iinclude -Isrc
TESTS     = tests/test_main.cpp tests/synth.cpp tests/png.cpp

build/test_widekit: $(SRC) $(TESTS) $(HDR) tests/synth.h tests/png.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS_T) $(SRC) $(TESTS) -o $@

test: build/test_widekit
	WK_SHOTS=$(SHOTS) ./build/test_widekit

build/bench: $(SRC) tests/bench.cpp tests/synth.cpp tests/png.cpp $(HDR) tests/synth.h
	@mkdir -p build
	$(CXX) -std=c++11 -O2 $(WARN) -Iinclude -Isrc $(SRC) tests/bench.cpp tests/synth.cpp tests/png.cpp -o $@

bench: build/bench
	./build/bench

VITA_CXX = /opt/vitasdk/bin/arm-vita-eabi-g++
VITA_FLAGS = -std=c++11 -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard $(WARN) -fno-exceptions -fno-rtti -Iinclude -Isrc

vita: $(SRC) $(HDR)
	@mkdir -p build/vita
	@for f in $(SRC); do $(VITA_CXX) $(VITA_FLAGS) -c $$f -o build/vita/$$(basename $$f .cpp).o || exit 1; done
	@echo "vita: all objects compiled without warnings"

ARM_CXX = arm-linux-gnueabihf-g++
ARM_FLAGS = -std=c++11 -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static $(WARN) -Iinclude -Isrc -Itests

build/arm_check: $(SRC) tests/arm_check.cpp $(HDR)
	@mkdir -p build
	$(ARM_CXX) $(ARM_FLAGS) $(SRC) tests/arm_check.cpp -o $@

arm-check: build/arm_check build/arm_frames build/host_frames
	qemu-arm build/arm_check
	@qemu-arm build/arm_frames > build/frames_arm.txt
	@./build/host_frames > build/frames_host.txt
	@cat build/frames_arm.txt
	@cmp build/frames_arm.txt build/frames_host.txt && echo "whole-frame hashes: NEON build == scalar host build"

build/arm_frames: $(SRC) tests/arm_frames.cpp tests/synth.cpp tests/png.cpp $(HDR) tests/synth.h
	@mkdir -p build
	$(ARM_CXX) $(ARM_FLAGS) $(SRC) tests/arm_frames.cpp tests/synth.cpp tests/png.cpp -Itests -o $@

build/host_frames: $(SRC) tests/arm_frames.cpp tests/synth.cpp tests/png.cpp $(HDR) tests/synth.h
	@mkdir -p build
	$(CXX) -std=c++11 -O2 $(WARN) -Iinclude -Isrc -Itests $(SRC) tests/arm_frames.cpp tests/synth.cpp tests/png.cpp -o $@

build/arm_bench: $(SRC) tests/bench.cpp tests/synth.cpp tests/png.cpp $(HDR) tests/synth.h
	@mkdir -p build
	$(ARM_CXX) $(ARM_FLAGS) $(SRC) tests/bench.cpp tests/synth.cpp tests/png.cpp -o $@

arm-count: build/arm_bench
	tests/count_insns.sh build/arm_bench

clean:
	rm -rf build

.PHONY: test bench vita arm-check arm-count clean
