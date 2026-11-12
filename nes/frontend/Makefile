# nescore.o: FCEUmm (nes/core) + host.c as ONE relocatable object whose only global symbols are the nes_host_*
# API of host.h (and the undefined libc functions it calls). For linking into a program that has its own globals
# (gpSP, TGB Dual): FCEUmm defines hundreds of generic names (RAM, X, PPU, GameInfo, crc32, FCEU_*...).
#
#   make -C nes/frontend                      host build         -> build/host/nescore.o
#   make -C nes/frontend vita                 VitaSDK (arm-vita-eabi) -> build/vita/nescore.o
#   make -C nes/frontend armhf                arm-linux-gnueabihf     -> build/armhf/nescore.o
#   make -C nes/frontend check [TARGET=...]   list the globals, fail on anything but nes_host_* and libc
#   make -C nes/frontend SCOUT=1 [vita]       with the wide view's scout (a second, renamed copy of core + host inside
#                                             the object: about twice the code and bss) -> build/<target>-scout/nescore.o
# Variables: CROSS (tool prefix), CPUFLAGS, OPT (-O3), BOARD_OPT (-O1 for the ~400 small mapper files; core -O3).
# Copyright CBOC-Vita. MIT licence for this Makefile; ../core keeps the FCEUmm licence (GPL-2.0-or-later).

CORE := ../core
FE   := .
WK   := ../../widekit
WK_SRC := wk_layers wk_map wk_compose wk_profile wk_util
include $(CORE)/sources.mk

CORE_DEFS := -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DNDEBUG
CINC := -I$(WK)/include -I$(CORE)/compat -I$(CORE)/libretro-common/include -I$(CORE) -I$(CORE)/input -I$(CORE)/boards
OPT ?= -O3
BOARD_OPT ?= -O1

# per target settings
CROSS ?=
CPUFLAGS ?=
TARGET ?= host
ifeq ($(TARGET),vita)
VITASDK ?= /opt/vitasdk
CROSS := $(VITASDK)/bin/arm-vita-eabi-
CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
CORE_DEFS += -D__vita__
endif
ifeq ($(TARGET),armhf)
CROSS := arm-linux-gnueabihf-
CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
endif

CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
LD_R    := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm
SIZE    := $(CROSS)size
SCOUT   ?= 0
B       := build/$(TARGET)$(if $(filter 1,$(SCOUT)),-scout)

CCORE := -MMD -MP $(CPUFLAGS) -g -std=gnu99 -w -fno-strict-aliasing $(CORE_DEFS) $(CINC)
CHOST := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu99 -Wall -Wextra -fno-strict-aliasing $(CORE_DEFS) $(CINC)
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))
WK_OBJS := $(addprefix $(B)/wk/,$(addsuffix .o,$(WK_SRC)))

all: $(B)/nescore.o
vita:
	$(MAKE) TARGET=vita SCOUT=$(SCOUT) all
armhf:
	$(MAKE) TARGET=armhf SCOUT=$(SCOUT) all
check:
	$(MAKE) TARGET=$(TARGET) SCOUT=$(SCOUT) $(B)/nescore.o
	@$(NM) -g --defined-only $(B)/nescore.o | awk '$$3 !~ /^nes_host_/ { print "NOT LOCALISED: " $$3; bad = 1 } END { exit bad }'
	@echo "globals defined: $$($(NM) -g --defined-only $(B)/nescore.o | wc -l) (all nes_host_*)"
	@echo "undefined (must be libc / libgcc only):"; $(NM) -u $(B)/nescore.o | awk '{ print "  " $$2 }' | tr '\n' ' '; echo
	@$(SIZE) $(B)/nescore.o

# core: one rule for the root files, one for the mapper files
$(B)/core/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) $(OPT) -c -o $@ $<
$(B)/core/boards/%.o: $(CORE)/boards/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) $(BOARD_OPT) -c -o $@ $<
$(B)/host.o: host.c host.h
	@mkdir -p $(B)
	$(CC) $(CHOST) -c -o $@ $<

# everything local but the API. The names are listed by objcopy's pattern (nes_host_*).
$(B)/wide.o: wide.c host.h
	@mkdir -p $(B)
	$(CC) $(CHOST) -DNESW_SCOUT=$(SCOUT) -c -o $@ $<
# the scout's machine: core + host.c + no-op wide hooks, every defined global renamed nesscout_* (the C library stays shared)
$(B)/scout_stub.o: scout_stub.c
	@mkdir -p $(B)
	$(CC) $(CHOST) -c -o $@ $<
$(B)/scoutcore.o: $(CORE_OBJS) $(B)/host.o $(B)/scout_stub.o
	$(LD_R) -r -o $@.tmp $^
	$(NM) -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " nesscout_" $$3 }' > $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $@.tmp $@
	@rm -f $@.tmp
SCOUT_OBJ := $(if $(filter 1,$(SCOUT)),$(B)/scoutcore.o)
# widekit (MIT, C++11, no exceptions / RTTI / C++ runtime): the wide view's layer analysis, learned map and compositor
$(B)/wk/%.o: $(WK)/src/%.cpp
	@mkdir -p $(B)/wk
	$(CXX) -MMD -MP $(CPUFLAGS) $(OPT) -g -std=c++11 -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti -I$(WK)/include -I$(WK)/src -c -o $@ $<

$(B)/nescore.o: $(CORE_OBJS) $(B)/host.o $(B)/wide.o $(WK_OBJS) $(SCOUT_OBJ)
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='nes_host_*' $@.tmp $@
	@rm -f $@.tmp

-include $(shell find build -name '*.d' 2>/dev/null)

clean:
	rm -rf build
.PHONY: all vita armhf check clean
