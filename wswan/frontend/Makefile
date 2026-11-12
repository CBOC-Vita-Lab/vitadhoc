# wswancore.o: the Beetle WonderSwan core (wswan/core) + host.cpp as ONE relocatable object whose only global symbols are
# the wswan_host_* API of host.h (and undefined libc/libgcc names). For linking into a program with its own globals.
#
#   make -C wswan/frontend                      host build          -> build/host/wswancore.o
#   make -C wswan/frontend vita                 VitaSDK (arm-vita-eabi) -> build/vita/wswancore.o
#   make -C wswan/frontend armhf                arm-linux-gnueabihf -> build/armhf/wswancore.o
#   make -C wswan/frontend check [TARGET=...]   list the globals, fail on anything but wswan_host_*
# Variables: CROSS, CPUFLAGS, OPT (-O3).
# Copyright CBOC-Vita. MIT licence for this Makefile; ../core keeps its licence (GPL-2.0-or-later).

CORE := ../core
include $(CORE)/sources.mk

OPT ?= -O3
CINC := -I$(CORE) -I$(CORE)/mednafen -I$(CORE)/mednafen/include -I$(CORE)/libretro-common/include -I.
CROSS ?=
CPUFLAGS ?=
TARGET ?= host
ifeq ($(TARGET),vita)
VITASDK ?= /opt/vitasdk
CROSS := $(VITASDK)/bin/arm-vita-eabi-
CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
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
B       := build/$(TARGET)

CCORE := -DMEDNAFEN_VERSION_NUMERIC=0x00093501 -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu99 -w -fno-strict-aliasing $(CINC)
CXXHOST := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu++11 -Wall -Wextra -fno-strict-aliasing -fno-exceptions -fno-rtti -fno-threadsafe-statics $(CINC)
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))

all: $(B)/wswancore.o
vita:
	$(MAKE) TARGET=vita all
armhf:
	$(MAKE) TARGET=armhf all
check:
	$(MAKE) TARGET=$(TARGET) $(B)/wswancore.o
	@$(NM) -g --defined-only $(B)/wswancore.o | awk '$$3 !~ /^wswan_host_/ { print "NOT LOCALISED: " $$3; bad = 1 } END { exit bad }'
	@echo "globals defined: $$($(NM) -g --defined-only $(B)/wswancore.o | wc -l) (all wswan_host_*)"
	@echo "undefined (must be libc / libgcc only):"; $(NM) -u $(B)/wswancore.o | awk '{ print "  " $$2 }' | tr '\n' ' '; echo
	@$(SIZE) $(B)/wswancore.o

$(B)/core/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) -c -o $@ $<
# rtc.c: time(NULL) goes to the host's clock source
$(B)/core/mednafen/wswan/rtc.o: $(CORE)/mednafen/wswan/rtc.c time_shim.h
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) -include time_shim.h -c -o $@ $<
$(B)/host.o: host.cpp host.h time_shim.h
	@mkdir -p $(B)
	$(CXX) $(CXXHOST) -c -o $@ $<

$(B)/wswancore.o: $(CORE_OBJS) $(B)/host.o
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='wswan_host_*' $@.tmp $@
	@rm -f $@.tmp

-include $(shell find build -name '*.d' 2>/dev/null)

clean:
	rm -rf build
.PHONY: all vita armhf check clean
