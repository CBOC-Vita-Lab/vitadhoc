# nescore.o: FCEUmm (nes/core) + host.c as ONE relocatable object whose only global symbols are the nes_host_*
# API of host.h (and the undefined libc functions it calls). For linking into a program that has its own globals
# (gpSP, TGB Dual): FCEUmm defines hundreds of generic names (RAM, X, PPU, GameInfo, crc32, FCEU_*...).
#
#   make -C nes/frontend                      host build         -> build/host/nescore.o
#   make -C nes/frontend vita                 VitaSDK (arm-vita-eabi) -> build/vita/nescore.o
#   make -C nes/frontend armhf                arm-linux-gnueabihf     -> build/armhf/nescore.o
#   make -C nes/frontend check [TARGET=...]   list the globals, fail on anything but nes_host_* and libc
# Variables: CROSS (tool prefix), CPUFLAGS, OPT (-O3), BOARD_OPT (-O1 for the ~400 small mapper files; core -O3).
# Copyright CBOC-Vita. MIT licence for this Makefile; ../core keeps the FCEUmm licence (GPL-2.0-or-later).

CORE := ../core
FE   := .
include $(CORE)/sources.mk

CORE_DEFS := -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DNDEBUG
CINC := -I$(CORE)/compat -I$(CORE)/libretro-common/include -I$(CORE) -I$(CORE)/input -I$(CORE)/boards
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
LD_R    := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm
SIZE    := $(CROSS)size
B       := build/$(TARGET)

CCORE := -MMD -MP $(CPUFLAGS) -g -std=gnu99 -w -fno-strict-aliasing $(CORE_DEFS) $(CINC)
CHOST := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu99 -Wall -Wextra -fno-strict-aliasing $(CORE_DEFS) $(CINC)
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))

all: $(B)/nescore.o
vita:
	$(MAKE) TARGET=vita all
armhf:
	$(MAKE) TARGET=armhf all
check:
	$(MAKE) TARGET=$(TARGET) $(B)/nescore.o
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
$(B)/nescore.o: $(CORE_OBJS) $(B)/host.o
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='nes_host_*' $@.tmp $@
	@rm -f $@.tmp

-include $(shell find build -name '*.d' 2>/dev/null)

clean:
	rm -rf build
.PHONY: all vita armhf check clean
