# Linux twin of the SNES program: headless runner + measurement tools.
#   make -C snes/twin -j1            builds build/snes_twin
# Copyright CBOC-Vita. MIT licence for this Makefile and main.cpp; the Snes9x
# core in ../core keeps its own licence.
CORE := ../core
FE   := ../frontend
B    := build

CXX ?= g++
OPT ?= -O2
CORE_DEFS := -DHAVE_STDINT_H -DNDEBUG -DS9X_PROF -DUSE_SELF_THREADED=0
INC  := -I$(CORE) -I$(CORE)/apu -I$(CORE)/apu/bapu -I$(FE) -I$(FE)/third_party/stb
CXXFLAGS := $(OPT) -std=gnu++14 -w -fno-strict-aliasing $(CORE_DEFS) $(INC) $(EXTRA)

include $(CORE)/sources.mk
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.cpp=.o))
FE_OBJS   := $(B)/host.o $(B)/prof.o $(B)/main.o

all: $(B)/snes_twin

$(B)/snes_twin: $(CORE_OBJS) $(FE_OBJS)
	$(CXX) -o $@ $^ -lm

$(B)/core/%.o: $(CORE)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/host.o: $(FE)/host.cpp $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/prof.o: $(FE)/prof.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(B)
.PHONY: all clean
