# mdcore.o: Genesis Plus GX (md/core) + md/frontend/host.c as ONE relocatable object whose only global symbols are the
# md_host_* API of md/frontend/host.h (plus the undefined libc functions it calls). Same discipline as nes/frontend:
# the core defines hundreds of generic names (config, input, vdp, snd, cart, bitmap, m68k, z80 ...).
#
#   make -C md/core                      host build         -> build/host/mdcore.o
#   make -C md/core vita                 VitaSDK (arm-vita-eabi) -> build/vita/mdcore.o
#   make -C md/core armhf                arm-linux-gnueabihf     -> build/armhf/mdcore.o
#   make -C md/core check [TARGET=...]   list the globals, fail on anything but md_host_* and libc
# Variables: B (build dir), OPT (-O3), CPUFLAGS, CROSS, PROFILE=1 (no -fomit-frame-pointer, keeps -g for callgrind).
# Copyright CBOC-Vita. MIT licence for this Makefile; the sources keep the Genesis Plus GX licence (md/core/LICENSE.txt).

CORE := .
FE   := ../frontend
include $(CORE)/sources.mk

# LSB_FIRST ALIGN_LONG ALT_RENDERER: the flags GPGX's own Vita build uses (psp2/Makefile). USE_16BPP_RENDERING: the VDP
# writes RGB565 directly (the Vita's native format; no conversion pass). USE_DYNAMIC_ALLOC: the 10 MB cartridge area is
# allocated at load, not in .bss. HAVE_YM3438_CORE / HAVE_OPLL_CORE: the optional "enhanced" FM chips, never the default.
CORE_DEFS := -DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -DALIGN_LONG -DALT_RENDERER -DUSE_16BPP_RENDERING \
	-DUSE_DYNAMIC_ALLOC -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE -DMAXROMSIZE=10485760 -DMD_HOST -DNDEBUG -U_FORTIFY_SOURCE -fno-stack-protector
CINC := -I$(CORE) -I$(CORE)/m68k -I$(CORE)/z80 -I$(CORE)/sound -I$(CORE)/input_hw -I$(CORE)/cart_hw -I$(CORE)/cart_hw/svp \
	-I$(CORE)/cd_hw

OPT ?= -O3
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
ifeq ($(PROFILE),1)
FP := -g
else
FP := -g -fomit-frame-pointer
endif

# ccache (tools/build/ccache.mk): prefix inside CC/CXX, a no-op without ccache; USE_CCACHE=0 turns it off.
include ../../tools/build/ccache.mk
CC      := $(call ccw,$(CROSS)gcc)
LD_R    := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
NM      := $(CROSS)nm
SIZE    := $(CROSS)size
B       ?= build/$(TARGET)

CCORE := -MMD -MP $(CPUFLAGS) $(FP) -std=gnu99 -w -fno-strict-aliasing $(CORE_DEFS) $(CINC) $(EXTRA)
CHOST := -MMD -MP $(CPUFLAGS) $(OPT) $(FP) -std=gnu99 -Wall -Wextra -fno-strict-aliasing $(CORE_DEFS) $(CINC) -I$(FE) $(EXTRA)
CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.c=.o))

all: $(B)/mdcore.o
vita:
	$(MAKE) TARGET=vita all
armhf:
	$(MAKE) TARGET=armhf all
check:
	$(MAKE) TARGET=$(TARGET) $(B)/mdcore.o
	@$(NM) -g --defined-only $(B)/mdcore.o | awk '$$3 !~ /^md_host_/ { print "NOT LOCALISED: " $$3; bad = 1 } END { exit bad }'
	@echo "globals defined: $$($(NM) -g --defined-only $(B)/mdcore.o | wc -l) (all md_host_*)"
	@echo "undefined (must be libc / libgcc only):"; $(NM) -u $(B)/mdcore.o | awk '{ print "  " $$2 }' | tr '\n' ' '; echo
	@$(SIZE) $(B)/mdcore.o

$(B)/core/%.o: $(CORE)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCORE) $(OPT) -c -o $@ $<
$(B)/host.o: $(FE)/host.c $(FE)/host.h
	@mkdir -p $(B)
	$(CC) $(CHOST) -c -o $@ $<

$(B)/mdcore.o: $(CORE_OBJS) $(B)/host.o
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='md_host_*' $@.tmp $@
	@rm -f $@.tmp

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf build
.PHONY: all vita armhf check clean
