# snes.bin: the SNES program of VitAdhoc (Snes9x core + our Vita frontend).
#   make -C snes/vita -j1                  builds build/snes.bin
# Run by VitAdhoc via sceAppMgrLoadExec; packed into the VPK by gpsp/vita/Makefile.
# Copyright CBOC-Vita. MIT licence for this file; ../core keeps the Snes9x licence.
# This Makefile builds nothing from gpsp/: the programs are separate.

VITASDK ?= /opt/vitasdk
export VITASDK
PREFIX := $(VITASDK)/bin/arm-vita-eabi-
CC  := $(PREFIX)gcc
CXX := $(PREFIX)g++

CORE := ../core
FE   := ../frontend
V2D  := $(FE)/third_party/vita2d
B    := build

include $(CORE)/sources.mk

CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
# S9X_PROF compiles the timing hooks in; they cost nothing while g_s9xp.on is 0
# (the app switches them on for one second in four to keep the log cheap).
CORE_DEFS := -DHAVE_STDINT_H -DNDEBUG -DS9X_PROF -DUSE_SELF_THREADED=0 -D__vita__
PK  := ../../playerkit
WK  := ../../widekit
INC := -I$(WK)/include -I$(CORE) -I$(CORE)/apu -I$(CORE)/apu/bapu -I$(FE) -I$(FE)/third_party/stb -I$(V2D)/include -I$(PK)/src -I$(PK)/vita
OPT ?= -O3
CXXFLAGS := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu++14 -w -fno-strict-aliasing -fno-rtti $(CORE_DEFS) $(INC)
CFLAGS   := -MMD -MP $(CPUFLAGS) -O2 -g -w -DVITA2D_NO_PGF -D__vita__ -I$(V2D)/include

CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.cpp=.o))
# Second, renamed copy of the renderer for the PPU thread (see ../core/rcopy.sh)
RCOPY_SRC := gfx tile tileimpl-h2x1 tileimpl-n1x1 tileimpl-n2x1
RCOPY_OBJS := $(addprefix $(B)/rcopy/,$(addsuffix .o,$(RCOPY_SRC)))
FE_OBJS   := $(B)/host.o $(B)/prof.o $(B)/audio_pace.o $(B)/picture.o $(B)/wide.o $(B)/player.o $(B)/pipe.o $(B)/main.o
# widekit (MIT, ../../widekit): the wide view's layer analysis, learned map and composition
WK_SRC    := $(notdir $(basename $(wildcard $(WK)/src/*.cpp)))
WK_OBJS   := $(addprefix $(B)/wk/,$(addsuffix .o,$(WK_SRC)))
# playerkit (MIT): the in-game menu, states, input mapping, launch protocol, patches, notices, pacing
PK_SRC    := pk_util pk_text pk_font_data pk_icons pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff pk_cores
PK_OBJS   := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC))) $(B)/pk/pk_gfx_vita.o
V2D_OBJS  := $(B)/v2d/vita2d.o $(B)/v2d/vita2d_draw.o $(B)/v2d/vita2d_texture.o $(B)/v2d/utils.o
SHADERS   := $(wildcard $(V2D)/shader/*_gxp.o)

LIBS := -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceSysmodule_stub \
        -lSceCommonDialog_stub -lSceAppMgr_stub -lSceCtrl_stub -lSceTouch_stub \
        -lSceAppUtil_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
        -lSceSysmem_stub -lSceLibKernel_stub -lm

all: $(B)/snes.bin

$(B)/snes.elf: $(CORE_OBJS) $(RCOPY_OBJS) $(FE_OBJS) $(WK_OBJS) $(PK_OBJS) $(V2D_OBJS) $(SHADERS)
	$(CXX) $(CPUFLAGS) -Wl,-q -o $@ $^ $(LIBS)

$(B)/snes.velf: $(B)/snes.elf
	$(VITASDK)/bin/vita-elf-create $< $@

$(B)/snes.bin: $(B)/snes.velf
	$(VITASDK)/bin/vita-make-fself -s -c $< $@

$(B)/rcopy.map: $(addprefix $(B)/core/,$(addsuffix .o,$(RCOPY_SRC))) $(CORE)/rcopy.sh
	NM=$(PREFIX)nm sh $(CORE)/rcopy.sh $@ $(filter %.o,$^)
$(B)/rcopy/%.o: $(B)/core/%.o $(B)/rcopy.map
	@mkdir -p $(B)/rcopy
	$(PREFIX)objcopy --redefine-syms=$(B)/rcopy.map $< $@

$(B)/core/%.o: $(CORE)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/%.o: $(FE)/%.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h player.h pipe.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/pipe.o: pipe.cpp pipe.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/player.o: player.cpp player.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/wk/%.o: $(WK)/src/%.cpp $(wildcard $(WK)/include/*.h) $(wildcard $(WK)/src/*.h)
	@mkdir -p $(B)/wk
	$(CXX) $(CPUFLAGS) $(OPT) -g -std=gnu++11 -fno-exceptions -fno-rtti -I$(WK)/include -c -o $@ $<
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/pk/pk_gfx_vita.o: $(PK)/vita/pk_gfx_vita.cpp $(wildcard $(PK)/vita/*.h) $(PK)/src/pk_gfx.h
	@mkdir -p $(B)/pk
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/v2d/%.o: $(V2D)/source/%.c
	@mkdir -p $(B)/v2d
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean
