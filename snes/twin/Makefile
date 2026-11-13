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
CXXFLAGS := -MMD -MP $(OPT) -std=gnu++14 -w -fno-strict-aliasing $(CORE_DEFS) $(INC) $(EXTRA)

include $(CORE)/sources.mk
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.cpp=.o))
# Second copy of the renderer for the PPU thread: the same objects with every
# global symbol renamed (+_r) and the renderer's state variables redirected to
# the *_R copies defined in core/ppu_mt.cpp. See core/rcopy.sh.
RCOPY_SRC := gfx tile tileimpl-h2x1 tileimpl-n1x1 tileimpl-n2x1
RCOPY_OBJS := $(addprefix $(B)/rcopy/,$(addsuffix .o,$(RCOPY_SRC)))
OBJCOPY ?= objcopy
FE_OBJS   := $(B)/host.o $(B)/prof.o $(B)/audio_pace.o $(B)/audio_sim.o $(B)/main.o

all: $(B)/snes_twin $(B)/player_twin $(B)/snes_wide_twin

# snes_wide_twin: the wide view runner (adapter ../frontend/wide.cpp + widekit, MIT) for the proofs in wide_selftest.sh
WK       := ../../widekit
WK_SRC   := $(notdir $(basename $(wildcard $(WK)/src/*.cpp)))
WK_OBJS  := $(addprefix $(B)/wk/,$(addsuffix .o,$(WK_SRC)))
$(B)/wk/%.o: $(WK)/src/%.cpp $(wildcard $(WK)/include/*.h) $(wildcard $(WK)/src/*.h)
	@mkdir -p $(B)/wk
	$(CXX) $(OPT) -std=c++11 -Wall -fno-exceptions -fno-rtti -I$(WK)/include -c -o $@ $<
# player_twin: the Vita player's own logic (../vita/player.cpp) with playerkit's software renderer
PK       := ../../playerkit
PK_SRC   := pk_util pk_text pk_font_data pk_icons pk_gfx_soft pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff
PK_OBJS  := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC)))
PLAYER_OBJS := $(B)/player.o $(B)/picture.o $(B)/pipe.o $(B)/handoff_proof.o $(B)/player_twin.o
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) $(OPT) -std=gnu++14 -Wall -I$(PK)/src -c -o $@ $<
$(B)/player.o: ../vita/player.cpp ../vita/player.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/pipe.o: ../vita/pipe.cpp ../vita/pipe.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/handoff_proof.o: handoff_proof.cpp ../vita/player.h ../vita/pipe.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/picture.o: $(FE)/picture.cpp $(FE)/picture.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/player_twin.o: player_twin.cpp ../vita/player.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/player_twin: $(CORE_OBJS) $(RCOPY_OBJS) $(B)/host.o $(B)/prof.o $(B)/wide.o $(WK_OBJS) $(PLAYER_OBJS) $(PK_OBJS)
	$(CXX) -o $@ $^ -lm -pthread

$(B)/snes_twin: $(CORE_OBJS) $(RCOPY_OBJS) $(FE_OBJS)
	$(CXX) -o $@ $^ -lm -pthread


$(B)/wide.o: $(FE)/wide.cpp $(FE)/wide.h $(CORE)/widecap.h $(wildcard $(WK)/include/*.h)
	@mkdir -p $(B)
	$(CXX) $(OPT) -std=gnu++14 -Wall -I$(CORE) -I$(FE) -I$(WK)/include -c -o $@ $<
$(B)/wide_twin.o: wide_twin.cpp $(FE)/host.h $(FE)/wide.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/snes_wide_twin: $(CORE_OBJS) $(RCOPY_OBJS) $(B)/host.o $(B)/prof.o $(B)/wide.o $(B)/wide_twin.o $(WK_OBJS)
	$(CXX) -o $@ $^ -lm -pthread

$(B)/rcopy.map: $(addprefix $(B)/core/,$(addsuffix .o,$(RCOPY_SRC))) $(CORE)/rcopy.sh
	sh $(CORE)/rcopy.sh $@ $(filter %.o,$^)
$(B)/rcopy/%.o: $(B)/core/%.o $(B)/rcopy.map
	@mkdir -p $(B)/rcopy
	$(OBJCOPY) --redefine-syms=$(B)/rcopy.map $< $@

$(B)/core/%.o: $(CORE)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/host.o: $(FE)/host.cpp $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/prof.o: $(FE)/prof.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/audio_pace.o: $(FE)/audio_pace.cpp $(FE)/audio_pace.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/audio_sim.o: audio_sim.cpp audio_sim.h $(FE)/audio_pace.h $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h audio_sim.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean
