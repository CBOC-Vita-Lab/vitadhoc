# Linux twin of the WonderSwan player: headless runner + test tools.
#   make -C wswan/twin -j4             builds build/wswan_twin and build/blend_test
# Copyright CBOC-Vita. MIT licence for this Makefile and main.cpp; the core in ../core keeps its own licence (GPL-2.0-or-later).
CORE := ../core
FE   := ../frontend
STB  := ../../snes/frontend/third_party/stb
B    := build
CC  ?= gcc
CXX ?= g++
OPT ?= -O2
CINC := -I$(CORE) -I$(CORE)/mednafen -I$(CORE)/mednafen/include -I$(CORE)/libretro-common/include -I$(FE)
CFLAGS := -MMD -MP $(OPT) -std=gnu99 -w -fno-strict-aliasing -DMEDNAFEN_VERSION_NUMERIC=0x00093501 $(CINC) $(EXTRA)
CXXFLAGS := -MMD -MP $(OPT) -std=gnu++14 -Wall -Wextra -fno-strict-aliasing $(CINC) -I$(STB) $(EXTRA)
include $(CORE)/sources.mk
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))
FE_OBJS := $(B)/host.o $(B)/blend.o $(B)/layout.o $(B)/safe_io.o $(B)/main.o

all: $(B)/wswan_twin $(B)/blend_test
$(B)/wswan_twin: $(CORE_OBJS) $(FE_OBJS)
	$(CXX) -o $@ $^ -lm
$(B)/blend_test: tests/blend_test.c $(FE)/blend.c
	@mkdir -p $(B)
	$(CC) $(OPT) -std=gnu99 -Wall -I$(FE) -o $@ $^ -lm
$(B)/core/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/core/mednafen/wswan/rtc.o: $(CORE)/mednafen/wswan/rtc.c $(FE)/time_shim.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include $(FE)/time_shim.h -c -o $@ $<
$(B)/host.o: $(FE)/host.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -fno-exceptions -fno-rtti -c -o $@ $<
$(B)/safe_io.o: $(FE)/safe_io.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/blend.o: $(FE)/blend.c
	@mkdir -p $(B)
	$(CC) $(OPT) -std=gnu99 -Wall -I$(FE) -c -o $@ $<
$(B)/layout.o: $(FE)/layout.c
	@mkdir -p $(B)
	$(CC) $(OPT) -std=gnu99 -Wall -I$(FE) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
-include $(shell find $(B) -name '*.d' 2>/dev/null)
clean:
	rm -rf $(B)
.PHONY: all clean
