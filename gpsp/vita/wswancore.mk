# vita/wswancore.mk -- the WonderSwan core (Beetle WonderSwan, wswan/core) built
# into the app as an in-process core, the way the NES one is (nescore.mk;
# wswan/INTEGRATION.md has the flags).  Included by corebuild.mk.
# Needs CC CXX LD_R OBJCOPY CORE_ARCH_FLAGS FE_FLAGS FE_INC B GP.
#
#   $(B)/wswan/wswancore.o   Beetle + wswan/frontend/host.cpp as ONE relocatable
#                            object whose only globals are wswan_host_*
#   $(B)/wswan/blend.o       the 75.47 Hz -> refresh temporal blend
#   $(B)/wswan/layout.o      rotation and the Vita pad map
#   $(B)/wswan/wscore.o      gpsp/wscore/wscore.c, the wscore_* API
# WS_OBJS goes on the link line beside GB_OBJS.  GPL-2.0-or-later (Beetle).

WS_DIR := $(GP)/../wswan
include $(WS_DIR)/core/sources.mk
WS_CORE_SRCS := $(CORE_SRCS)

WS_INC    := -I$(WS_DIR)/core -I$(WS_DIR)/core/mednafen -I$(WS_DIR)/core/mednafen/include \
             -I$(WS_DIR)/core/libretro-common/include -I$(WS_DIR)/frontend
WS_CFLAGS := $(CORE_ARCH_FLAGS) -g -std=gnu99 -w -fno-strict-aliasing \
             -DMEDNAFEN_VERSION_NUMERIC=0x00093501 $(WS_INC) -MMD -MP
WS_OPT    ?= -O3
WS_CORE_OBJS := $(addprefix $(B)/wswan/core/,$(WS_CORE_SRCS:.c=.o))

$(B)/wswan/core/%.o: $(WS_DIR)/core/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(WS_CFLAGS) $(WS_OPT) -c -o $@ $<
$(B)/wswan/core/mednafen/wswan/rtc.o: $(WS_DIR)/core/mednafen/wswan/rtc.c $(WS_DIR)/frontend/time_shim.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(WS_CFLAGS) $(WS_OPT) -include $(WS_DIR)/frontend/time_shim.h -c -o $@ $<
$(B)/wswan/host.o: $(WS_DIR)/frontend/host.cpp $(WS_DIR)/frontend/host.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_ARCH_FLAGS) $(WS_OPT) -g -std=gnu++11 -fno-strict-aliasing -fno-exceptions \
	  -fno-rtti -fno-threadsafe-statics $(WS_INC) -MMD -MP -c -o $@ $<

$(B)/wswan/wswancore.o: $(WS_CORE_OBJS) $(B)/wswan/host.o
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='wswan_host_*' $@.tmp $@
	@rm -f $@.tmp

$(B)/wswan/blend.o: $(WS_DIR)/frontend/blend.c $(WS_DIR)/frontend/blend.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_ARCH_FLAGS) -O2 -g -std=gnu99 -MMD -MP -c -o $@ $<
$(B)/wswan/layout.o: $(WS_DIR)/frontend/layout.c $(WS_DIR)/frontend/layout.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_ARCH_FLAGS) -O2 -g -std=gnu99 -MMD -MP -c -o $@ $<
$(B)/wswan/wscore.o: $(GP)/wscore/wscore.c $(GP)/wscore/wscore.h $(WS_DIR)/frontend/host.h $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) -MMD -MP -c -o $@ $<

WS_OBJS := $(B)/wswan/wswancore.o $(B)/wswan/blend.o $(B)/wswan/layout.o $(B)/wswan/wscore.o
-include $(shell find $(B)/wswan -name '*.d' 2>/dev/null)
