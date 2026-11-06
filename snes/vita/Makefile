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
INC := -I$(CORE) -I$(CORE)/apu -I$(CORE)/apu/bapu -I$(FE) -I$(FE)/third_party/stb -I$(V2D)/include
OPT ?= -O3
CXXFLAGS := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu++14 -w -fno-strict-aliasing -fno-rtti $(CORE_DEFS) $(INC)
CFLAGS   := -MMD -MP $(CPUFLAGS) -O2 -g -w -DVITA2D_NO_PGF -D__vita__ -I$(V2D)/include

CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.cpp=.o))
# Second, renamed copy of the renderer for the PPU thread (see ../core/rcopy.sh)
RCOPY_SRC := gfx tile tileimpl-h2x1 tileimpl-n1x1 tileimpl-n2x1
RCOPY_OBJS := $(addprefix $(B)/rcopy/,$(addsuffix .o,$(RCOPY_SRC)))
FE_OBJS   := $(B)/host.o $(B)/prof.o $(B)/main.o $(B)/font.o
V2D_OBJS  := $(B)/v2d/vita2d.o $(B)/v2d/vita2d_draw.o $(B)/v2d/vita2d_texture.o $(B)/v2d/utils.o
SHADERS   := $(wildcard $(V2D)/shader/*_gxp.o)

LIBS := -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceSysmodule_stub \
        -lSceCommonDialog_stub -lSceAppMgr_stub -lSceCtrl_stub -lSceTouch_stub \
        -lSceAppUtil_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
        -lSceSysmem_stub -lSceLibKernel_stub -lm

all: $(B)/snes.bin

$(B)/snes.elf: $(CORE_OBJS) $(RCOPY_OBJS) $(FE_OBJS) $(V2D_OBJS) $(SHADERS)
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
$(B)/main.o: main.cpp $(FE)/host.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/font.o: font.S
	@mkdir -p $(B)
	$(CC) $(CPUFLAGS) -I$(FE)/third_party/fonts -c -o $@ $<
$(B)/v2d/%.o: $(V2D)/source/%.c
	@mkdir -p $(B)/v2d
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean
