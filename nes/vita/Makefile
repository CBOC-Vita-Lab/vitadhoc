# nes.bin: the NES program of VitAdhoc (FCEUmm core + our Vita frontend).
#   make -C nes/vita -j4                   builds build/nes.bin
# Run by VitAdhoc via sceAppMgrLoadExec; packed into the VPK by gpsp/vita/Makefile (see ../INTEGRATION.md).
# Copyright CBOC-Vita. MIT licence for this file; ../core keeps the FCEUmm licence (GPL-2.0-or-later),
# so the program nes.bin as a whole is distributed under the GPL.
# This Makefile builds nothing from gpsp/ or snes/ except the shared pacer and the third party libraries.

VITASDK ?= /opt/vitasdk
export VITASDK
PREFIX := $(VITASDK)/bin/arm-vita-eabi-
CC  := $(PREFIX)gcc
CXX := $(PREFIX)g++

CORE := ../core
FE   := ../frontend
SNES := ../../snes/frontend
TP   := $(SNES)/third_party
V2D  := $(TP)/vita2d
B    := build

include $(CORE)/sources.mk

CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
CORE_DEFS := -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DNDEBUG -D__vita__
CINC := -I$(CORE)/compat -I$(CORE)/libretro-common/include -I$(CORE) -I$(CORE)/input -I$(CORE)/boards
OPT ?= -O3
BOARD_OPT ?= -O1
# the core is third party code: compiled without its warnings; our own files with -Wall -Wextra
CCORE    := -MMD -MP $(CPUFLAGS) -g -std=gnu99 -w -fno-strict-aliasing $(CORE_DEFS) $(CINC)
HOSTCFLAGS := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu99 -Wall -Wextra -fno-strict-aliasing $(CORE_DEFS) $(CINC)
CXXFLAGS := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu++14 -Wall -Wextra -fno-strict-aliasing -fno-rtti $(CORE_DEFS) $(CINC) \
            -I$(FE) -I$(SNES) -isystem $(TP)/stb -isystem $(V2D)/include
CFLAGS   := -MMD -MP $(CPUFLAGS) -O2 -g -w -DVITA2D_NO_PGF -D__vita__ -I$(V2D)/include

CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))
FE_OBJS   := $(B)/host.o $(B)/host_files.o $(B)/safe_io.o $(B)/audio_pace.o $(B)/picture.o $(B)/main.o $(B)/font.o
V2D_OBJS  := $(B)/v2d/vita2d.o $(B)/v2d/vita2d_draw.o $(B)/v2d/vita2d_texture.o $(B)/v2d/utils.o
SHADERS   := $(wildcard $(V2D)/shader/*_gxp.o)

LIBS := -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceSysmodule_stub \
        -lSceCommonDialog_stub -lSceAppMgr_stub -lSceCtrl_stub -lSceTouch_stub \
        -lSceAppUtil_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
        -lSceSysmem_stub -lSceLibKernel_stub -lm

all: $(B)/nes.bin

$(B)/nes.elf: $(CORE_OBJS) $(FE_OBJS) $(V2D_OBJS) $(SHADERS)
	$(CXX) $(CPUFLAGS) -Wl,-q -o $@ $^ $(LIBS)

$(B)/nes.velf: $(B)/nes.elf
	$(VITASDK)/bin/vita-elf-create $< $@

$(B)/nes.bin: $(B)/nes.velf
	$(VITASDK)/bin/vita-make-fself -s -c $< $@

$(B)/core/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) $(OPT) -c -o $@ $<
# the ~400 mapper files are small and mostly run on cartridge register writes: -O1 keeps the build short
$(B)/core/boards/%.o: $(CORE)/boards/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) $(BOARD_OPT) -c -o $@ $<
$(B)/host.o: $(FE)/host.c
	@mkdir -p $(B)
	$(CC) $(HOSTCFLAGS) -c -o $@ $<
$(B)/%.o: $(FE)/%.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
# the pacer is shared with the SNES program (same file, not a copy)
$(B)/audio_pace.o: $(SNES)/audio_pace.cpp $(SNES)/audio_pace.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h $(FE)/picture.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/font.o: font.S
	@mkdir -p $(B)
	$(CC) $(CPUFLAGS) -I$(TP)/fonts -c -o $@ $<
$(B)/v2d/%.o: $(V2D)/source/%.c
	@mkdir -p $(B)/v2d
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean
