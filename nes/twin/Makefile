# Linux twin of the NES program: headless runner + test tools.
#   make -C nes/twin -j4             builds build/nes_twin
# Copyright CBOC-Vita. MIT licence for this Makefile and main.cpp; the FCEUmm core in
# ../core keeps its own licence (GPL-2.0-or-later).
CORE := ../core
FE   := ../frontend
STB  := ../../snes/frontend/third_party/stb
WK   := ../../widekit
WK_SRC := wk_layers wk_map wk_compose wk_profile wk_util
B    := build

CC  ?= gcc
CXX ?= g++
OPT ?= -O2
CORE_DEFS := -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DNDEBUG
CINC := -I$(WK)/include -I$(CORE)/compat -I$(CORE)/libretro-common/include -I$(CORE) -I$(CORE)/input -I$(CORE)/boards
CFLAGS   := -MMD -MP $(OPT) -std=gnu99 -w -fno-strict-aliasing $(CORE_DEFS) $(CINC) $(EXTRA)
HOSTCFLAGS := -MMD -MP $(OPT) -std=gnu99 -Wall -Wextra -fno-strict-aliasing $(CORE_DEFS) $(CINC) $(EXTRA)
CXXFLAGS := -MMD -MP $(OPT) -std=gnu++14 -Wall -Wextra -fno-strict-aliasing $(CORE_DEFS) $(CINC) -I$(FE) -I$(STB) $(EXTRA)

include $(CORE)/sources.mk
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))
WK_OBJS   := $(addprefix $(B)/wk/,$(addsuffix .o,$(WK_SRC)))
FE_OBJS   := $(B)/host.o $(B)/wide.o $(B)/host_files.o $(B)/safe_io.o $(B)/main.o $(WK_OBJS) $(B)/scoutcore.o

all: $(B)/nes_twin

$(B)/nes_twin: $(CORE_OBJS) $(FE_OBJS)
	$(CXX) -o $@ $^ -lm

$(B)/core/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/host.o: $(FE)/host.c
	@mkdir -p $(B)
	$(CC) $(HOSTCFLAGS) -c -o $@ $<
$(B)/wide.o: $(FE)/wide.c $(FE)/host.h
	@mkdir -p $(B)
	$(CC) $(HOSTCFLAGS) -DNESW_SCOUT=1 -c -o $@ $<
# the wide view's scout: a second machine = the same core objects + host.c (+ no-op wide hooks), every defined global
# renamed nesscout_* (what stays undefined, the C library, is shared)
$(B)/scout_stub.o: $(FE)/scout_stub.c
	@mkdir -p $(B)
	$(CC) $(HOSTCFLAGS) -c -o $@ $<
$(B)/scoutcore.o: $(CORE_OBJS) $(B)/host.o $(B)/scout_stub.o
	ld -r -o $@.tmp $^
	nm -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " nesscout_" $$3 }' > $@.syms
	objcopy --redefine-syms=$@.syms $@.tmp $@
	@rm -f $@.tmp
$(B)/wk/%.o: $(WK)/src/%.cpp
	@mkdir -p $(B)/wk
	$(CXX) -MMD -MP $(OPT) -std=c++11 -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti -I$(WK)/include -I$(WK)/src $(EXTRA) -c -o $@ $<
$(B)/%.o: $(FE)/%.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean
