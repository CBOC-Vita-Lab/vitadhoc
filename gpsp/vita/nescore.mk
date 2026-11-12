# vita/nescore.mk -- the NES core (FCEUmm, nes/core) built into the app as an
# in-process core, the way TGB Dual is (corebuild.mk).  Included by
# corebuild.mk.  Needs CC LD_R NM OBJCOPY CORE_ARCH_FLAGS FE_FLAGS FE_INC B GP.
#
#   $(B)/nescore/nescore_linked.o   FCEUmm + nes/frontend/host.c as ONE
#                                   relocatable object whose only globals are
#                                   nes_host_* (FCEUmm defines hundreds of
#                                   generic names: RAM, X, PPU, crc32...)
#   $(B)/nescore/nescore.o          gpsp/nescore/nescore.c, the nescore_* API
#   $(B)/nescore/gbcore_mux.o       fe_host.c's gbcore_* calls routed to it
# NES_OBJS goes on the link line beside GB_OBJS.  GPL-2.0-or-later (FCEUmm).

NES_DIR := $(GP)/../nes
include $(NES_DIR)/core/sources.mk

NES_DEFS := -D__LIBRETRO__ -DPATH_MAX=1024 -DFCEU_VERSION_NUMERIC=9900 -DNDEBUG
NES_WK   := $(GP)/../widekit
NES_WK_SRC := wk_layers wk_map wk_compose wk_profile wk_util
NES_INC  := -I$(NES_WK)/include -I$(NES_DIR)/core/compat -I$(NES_DIR)/core/libretro-common/include \
            -I$(NES_DIR)/core -I$(NES_DIR)/core/input -I$(NES_DIR)/core/boards
NES_CFLAGS := $(CORE_ARCH_FLAGS) -g -std=gnu99 -w -fno-strict-aliasing $(NES_DEFS) $(NES_INC) -MMD -MP
NES_OPT    ?= -O3
NES_BOARD_OPT ?= -O1
NES_CORE_OBJS := $(addprefix $(B)/nescore/core/,$(CORE_SRCS:.c=.o))

$(B)/nescore/core/%.o: $(NES_DIR)/core/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(NES_CFLAGS) $(NES_OPT) -c -o $@ $<
$(B)/nescore/core/boards/%.o: $(NES_DIR)/core/boards/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(NES_CFLAGS) $(NES_BOARD_OPT) -c -o $@ $<
$(B)/nescore/host.o: $(NES_DIR)/frontend/host.c $(NES_DIR)/frontend/host.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(NES_CFLAGS) $(NES_OPT) -c -o $@ $<

# the wide view (nes/INTEGRATION.md, 12 Nov): wide.c and widekit go into the
# same object; no scout (SCOUT=0: its functions exist and do nothing)
$(B)/nescore/wide.o: $(NES_DIR)/frontend/wide.c $(NES_DIR)/frontend/host.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(NES_CFLAGS) $(NES_OPT) -DNESW_SCOUT=0 -c -o $@ $<
NES_WK_OBJS := $(addprefix $(B)/nescore/wk/,$(addsuffix .o,$(NES_WK_SRC)))
$(B)/nescore/wk/%.o: $(NES_WK)/src/%.cpp $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP $(CORE_ARCH_FLAGS) $(NES_OPT) -g -std=c++11 -w -fno-exceptions -fno-rtti \
	  -I$(NES_WK)/include -I$(NES_WK)/src -c -o $@ $<

$(B)/nescore/nescore_linked.o: $(NES_CORE_OBJS) $(B)/nescore/host.o $(B)/nescore/wide.o $(NES_WK_OBJS)
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='nes_host_*' $@.tmp $@
	@rm -f $@.tmp

$(B)/nescore/nescore.o: $(GP)/nescore/nescore.c $(GP)/nescore/nescore.h $(NES_DIR)/frontend/host.h $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) -MMD -MP -c -o $@ $<
$(B)/nescore/gbcore_mux.o: $(GP)/nescore/gbcore_mux.c $(GP)/nescore/gbcore_mux.h $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) -I$(GP)/gbcore -MMD -MP -c -o $@ $<

NES_OBJS := $(B)/nescore/nescore_linked.o $(B)/nescore/nescore.o $(B)/nescore/gbcore_mux.o
-include $(shell find $(B)/nescore -name '*.d' 2>/dev/null)
