# Linux twin of the SNES program: headless runner + measurement tools.
#   make -C snes/twin -j1            builds build/snes_twin
# Copyright CBOC-Vita. MIT licence for this Makefile and main.cpp; the Snes9x
# core in ../core keeps its own licence.
CORE := ../core
FE   := ../frontend
B    := build

CXX ?= g++
# ccache (tools/build/ccache.mk): prefix inside CC/CXX, a no-op without ccache; USE_CCACHE=0 turns it off.
include ../../tools/build/ccache.mk
override CC := $(call ccw,$(CC))
override CXX := $(call ccw,$(CXX))
OPT ?= -O2
CORE_DEFS := -DHAVE_STDINT_H -DNDEBUG -DS9X_PROF -DUSE_SELF_THREADED=0
CORE_DEFS += -DSA1_FAST_RUN=15
INC  := -I$(CORE) -I$(CORE)/apu -I$(CORE)/apu/bapu -I$(FE) -I$(FE)/third_party/stb
CXXFLAGS := -MMD -MP $(OPT) -std=gnu++14 -w -fno-strict-aliasing $(CORE_DEFS) $(INC) $(EXTRA)

include $(CORE)/sources.mk
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.cpp=.o))
# Second copy of the renderer for the PPU thread: the same objects with every
# global symbol renamed (+_r) and the renderer's state variables redirected to
# the *_R copies defined in core/ppu_mt.cpp. See core/rcopy.sh.
RCOPY_SRC := gfx tile tileimpl-h2x1 tileimpl-n1x1 tileimpl-n2x1
RCOPY_OBJS := $(addprefix $(B)/rcopy/,$(addsuffix .o,$(RCOPY_SRC)))
# Third copy (suffix _w, state *_W): the wide view's margin thread, see core/ppu_mt.cpp
RCOPY_OBJS += $(addprefix $(B)/wcopy/,$(addsuffix .o,$(RCOPY_SRC)))
OBJCOPY ?= objcopy
FE_OBJS   := $(B)/host.o $(B)/prof.o $(B)/audio_pace.o $(B)/audio_sim.o $(B)/main.o

all: $(B)/snes_twin $(B)/player_twin $(B)/snes_wide_twin $(B)/scale_proof

# snes_wide_twin: the wide view runner (adapter ../frontend/wide.cpp + widekit, MIT) for the proofs in wide_selftest.sh
WK       := ../../widekit
WK_SRC   := $(notdir $(basename $(wildcard $(WK)/src/*.cpp)))
WK_OBJS  := $(addprefix $(B)/wk/,$(addsuffix .o,$(WK_SRC)))
$(B)/wk/%.o: $(WK)/src/%.cpp $(wildcard $(WK)/include/*.h) $(wildcard $(WK)/src/*.h)
	@mkdir -p $(B)/wk
	$(CXX) $(OPT) -std=c++11 -Wall -fno-exceptions -fno-rtti -I$(WK)/include -c -o $@ $<
# player_twin: the Vita player's own logic (../vita/player.cpp) with playerkit's software renderer
PK       := ../../playerkit
# RetroAchievements (playerkit/ra.mk): pk_ra, the rcheevos client and ra_http.c, the thread layer of the twin
RA_CC     := $(call ccw,gcc)
RA_GP     := ../../gpsp/vita
RA_OBJDIR := $(B)/ra
RA_CFLAGS := -std=gnu11 -O2 -g -w
include $(PK)/ra.mk
.DEFAULT_GOAL := all
$(B)/ra/pk_ra_plat_posix.o: $(PK)/ra/pk_ra_plat_posix.c $(PK)/src/pk_ra_plat.h
	@mkdir -p $(B)/ra
	$(RA_CC) $(RA_CFLAGS) -I$(PK)/src -c -o $@ $<
$(B)/pk/pk_ra.o: PK_EXTRA_INC := $(RA_INC)
PK_SRC   := pk_util pk_text pk_font_data pk_icons pk_gfx_soft pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff pk_look pk_rewind pk_ff pk_speed pk_ra
PK_OBJS  := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC))) $(B)/ra/pk_ra_plat_posix.o $(RA_OBJS)
PLAYER_OBJS := $(B)/player.o $(B)/picture.o $(B)/pipe.o $(B)/handoff_proof.o $(B)/player_twin.o
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) $(OPT) -std=gnu++14 -Wall -I$(PK)/src $(PK_EXTRA_INC) -c -o $@ $<
$(B)/player.o: ../vita/player.cpp ../vita/player.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/pipe.o: ../vita/pipe.cpp ../vita/pipe.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/handoff_proof.o: handoff_proof.cpp ../vita/player.h ../vita/pipe.h $(wildcard $(PK)/src/*.h)
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -c -o $@ $<
$(B)/picture.o: $(FE)/picture.cpp $(FE)/picture.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/player_twin.o: player_twin.cpp ../vita/player.h $(wildcard $(PK)/src/*.h) $(PK)/tests/fake_ra.h
	$(CXX) $(CXXFLAGS) -Wall -I../vita -I$(PK)/src -I$(PK)/tests -c -o $@ $<
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

# scale_proof: the pixel art scaler (playerkit pk_scale) on the pictures of real runs, normal and wide, against its own definition
$(B)/pk/pk_scale.o: $(PK)/src/pk_scale.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) $(OPT) -std=gnu++14 -Wall -I$(PK)/src -c -o $@ $<
$(B)/scale_proof.o: scale_proof.cpp $(FE)/host.h $(FE)/wide.h $(PK)/src/pk_scale.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -I$(PK)/src -c -o $@ $<
$(B)/scale_proof: $(CORE_OBJS) $(RCOPY_OBJS) $(B)/host.o $(B)/prof.o $(B)/wide.o $(B)/scale_proof.o $(B)/pk/pk_scale.o $(WK_OBJS)
	$(CXX) -o $@ $^ -lm -pthread

# snes_wide_twin_cg: the same with Valgrind's client requests (--cg A,B instruments frames A..B only; run under valgrind --tool=callgrind --instr-atstart=no)
$(B)/wide_twin_cg.o: wide_twin.cpp $(FE)/host.h $(FE)/wide.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -DWIDE_CALLGRIND -c -o $@ $<
$(B)/snes_wide_twin_cg: $(CORE_OBJS) $(RCOPY_OBJS) $(B)/host.o $(B)/prof.o $(B)/wide.o $(B)/wide_twin_cg.o $(WK_OBJS)
	$(CXX) -o $@ $^ -lm -pthread

$(B)/rcopy.map: $(addprefix $(B)/core/,$(addsuffix .o,$(RCOPY_SRC))) $(CORE)/rcopy.sh
	sh $(CORE)/rcopy.sh $@ $(filter %.o,$^)
$(B)/rcopy/%.o: $(B)/core/%.o $(B)/rcopy.map
	@mkdir -p $(B)/rcopy
	$(OBJCOPY) --redefine-syms=$(B)/rcopy.map $< $@
$(B)/wcopy.map: $(addprefix $(B)/core/,$(addsuffix .o,$(RCOPY_SRC))) $(CORE)/rcopy.sh
	RCOPY_SUF=w sh $(CORE)/rcopy.sh $@ $(filter %.o,$^)
$(B)/wcopy/%.o: $(B)/core/%.o $(B)/wcopy.map
	@mkdir -p $(B)/wcopy
	$(OBJCOPY) --redefine-syms=$(B)/wcopy.map $< $@

# Super FX speed-ups (exact, identity-tested on the 15 ROMs): merged run-loop test, cache range
# test, byte-per-line cache fill flags. The Vita Makefile uses the same defines for these two files.
GSU_FAST := -DFX_LIMIT -DFX_FAST -DFX_LINEFLAGS
$(B)/core/fxinst.o $(B)/core/fxemu.o: CXXFLAGS += $(GSU_FAST)

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
